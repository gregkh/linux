// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2021 IBM Corporation
 */

#include <linux/module.h>
#include <crypto/internal/akcipher.h>
#include <crypto/internal/ecc.h>
#include <crypto/rng.h>
#include <crypto/drbg.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <crypto/akcipher.h>
#include <crypto/ecdh.h>
#include <linux/asn1_decoder.h>
#include <linux/scatterlist.h>
#include <linux/oid_registry.h>

#include "ecprivkey.asn1.h"
#include "ecdsasignature.asn1.h"

#include "hacl_p256.h"

struct ecc_ctx {
	unsigned int curve_id;
	const struct ecc_curve *curve;
	bool key_set;
	bool is_private;

	bool pub_key_set;
	u64 d[ECC_MAX_DIGITS]; /* privkey  big integer */
	u64 x[ECC_MAX_DIGITS]; /* pub key x and y coordinates */
	u64 y[ECC_MAX_DIGITS];
	struct ecc_point pub_key;
};

struct ecdsa_signature_ctx {
	const struct ecc_curve *curve;
	u64 r[ECC_MAX_DIGITS];
	u64 s[ECC_MAX_DIGITS];
};

/*
 * Get the r and s components of a signature from the X509 certificate.
 */
static int ecdsa_get_signature_rs(u64 *dest, size_t hdrlen, unsigned char tag,
				  const void *value, size_t vlen, unsigned int ndigits)
{
	size_t keylen = ndigits * sizeof(u64);
	ssize_t diff = vlen - keylen;
	const char *d = value;
	u8 rs[ECC_MAX_BYTES];

	if (!value || !vlen)
		return -EINVAL;

	/* diff = 0: 'value' has exacly the right size
	 * diff > 0: 'value' has too many bytes; one leading zero is allowed that
	 *           makes the value a positive integer; error on more
	 * diff < 0: 'value' is missing leading zeros, which we add
	 */
	if (diff > 0) {
		/* skip over leading zeros that make 'value' a positive int */
		if (*d == 0) {
			vlen -= 1;
			diff--;
			d++;
		}
		if (diff)
			return -EINVAL;
	}
	if (-diff >= keylen)
		return -EINVAL;

	if (diff) {
		/* leading zeros not given in 'value' */
		memset(rs, 0, -diff);
	}

	memcpy(&rs[-diff], d, vlen);

	ecc_swap_digits((u64 *)rs, dest, ndigits);

	return 0;
}

int ecdsa_get_signature_r(void *context, size_t hdrlen, unsigned char tag,
			  const void *value, size_t vlen)
{
	struct ecdsa_signature_ctx *sig = context;

	return ecdsa_get_signature_rs(sig->r, hdrlen, tag, value, vlen,
				      sig->curve->g.ndigits);
}

int ecdsa_get_signature_s(void *context, size_t hdrlen, unsigned char tag,
			  const void *value, size_t vlen)
{
	struct ecdsa_signature_ctx *sig = context;

	return ecdsa_get_signature_rs(sig->s, hdrlen, tag, value, vlen,
				      sig->curve->g.ndigits);
}

static int _ecdsa_verify(struct ecc_ctx *ctx, const u64 *hash, const u64 *r, const u64 *s)
{
	const struct ecc_curve *curve = ctx->curve;
	unsigned int ndigits = curve->g.ndigits;
	u64 s1[ECC_MAX_DIGITS];
	u64 u1[ECC_MAX_DIGITS];
	u64 u2[ECC_MAX_DIGITS];
	u64 x1[ECC_MAX_DIGITS];
	u64 y1[ECC_MAX_DIGITS];
	struct ecc_point res = ECC_POINT_INIT(x1, y1, ndigits);

	/* 0 < r < n  and 0 < s < n */
	if (vli_is_zero(r, ndigits) || vli_cmp(r, curve->n, ndigits) >= 0 ||
	    vli_is_zero(s, ndigits) || vli_cmp(s, curve->n, ndigits) >= 0)
		return -EBADMSG;

	/* hash is given */
	pr_devel("hash : %016llx %016llx ... %016llx\n",
		 hash[ndigits - 1], hash[ndigits - 2], hash[0]);

	/* s1 = (s^-1) mod n */
	vli_mod_inv(s1, s, curve->n, ndigits);
	/* u1 = (hash * s1) mod n */
	vli_mod_mult_slow(u1, hash, s1, curve->n, ndigits);
	/* u2 = (r * s1) mod n */
	vli_mod_mult_slow(u2, r, s1, curve->n, ndigits);
	/* res = u1*G + u2 * pub_key */
	ecc_point_mult_shamir(&res, u1, &curve->g, u2, &ctx->pub_key, curve);

	/* res.x = res.x mod n (if res.x > order) */
	if (unlikely(vli_cmp(res.x, curve->n, ndigits) == 1))
		/* faster alternative for NIST p384, p256 & p192 */
		vli_sub(res.x, res.x, curve->n, ndigits);

	if (!vli_cmp(res.x, r, ndigits))
		return 0;

	return -EKEYREJECTED;
}

/*
 * Verify an ECDSA signature.
 */
static int ecdsa_verify(struct akcipher_request *req)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct ecc_ctx *ctx = akcipher_tfm_ctx(tfm);
	size_t keylen = ctx->curve->g.ndigits * sizeof(u64);
	struct ecdsa_signature_ctx sig_ctx = {
		.curve = ctx->curve,
	};
	u8 rawhash[ECC_MAX_BYTES];
	u64 hash[ECC_MAX_DIGITS];
	unsigned char *buffer;
	ssize_t diff;
	int ret;

	if (unlikely(!ctx->key_set))
		return -EINVAL;

	buffer = kmalloc(req->src_len + req->dst_len, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	sg_pcopy_to_buffer(req->src,
		sg_nents_for_len(req->src, req->src_len + req->dst_len),
		buffer, req->src_len + req->dst_len, 0);

	ret = asn1_ber_decoder(&ecdsasignature_decoder, &sig_ctx,
			       buffer, req->src_len);
	if (ret < 0)
		goto error;

	/* if the hash is shorter then we will add leading zeros to fit to ndigits */
	diff = keylen - req->dst_len;
	if (diff >= 0) {
		if (diff)
			memset(rawhash, 0, diff);
		memcpy(&rawhash[diff], buffer + req->src_len, req->dst_len);
	} else if (diff < 0) {
		/* given hash is longer, we take the left-most bytes */
		memcpy(&rawhash, buffer + req->src_len, keylen);
	}

	if (strncmp(ctx->curve->name, "nist_256", 8) == 0) {
		u8 pk[64];
		u8 r[32];
		u8 s[32];
		ecc_swap_digits(ctx->x, (u64*)pk, 4);
		ecc_swap_digits(ctx->y, (u64*)(pk + 32), 4);
		ecc_swap_digits(sig_ctx.r, (u64*)r, ctx->curve->g.ndigits);
		ecc_swap_digits(sig_ctx.s, (u64*)s, ctx->curve->g.ndigits);
		if (Hacl_P256_ecdsa_verif_without_hash(req->dst_len, rawhash, pk, r, s)) {
			ret = 0;
		} else {
			ret = -EKEYREJECTED;
		}
	} else {
		ecc_swap_digits((u64 *)rawhash, hash, ctx->curve->g.ndigits);
		ret = _ecdsa_verify(ctx, hash, sig_ctx.r, sig_ctx.s);
	}

error:
	kfree(buffer);

	return ret;
}

static int _ecdsa_sign(struct ecc_ctx *ctx, const u64 *hash, const u64 *k,
		       struct ecdsa_signature_ctx *sig_ctx)
{
	unsigned int ndigits = ctx->curve->g.ndigits;
	u64 rd_h[ECC_MAX_DIGITS];
	u64 kinv[ECC_MAX_DIGITS];
	/* we can use s as y coordinate here as we're discarding it anyway later */
	struct ecc_point K = ECC_POINT_INIT(sig_ctx->r, sig_ctx->s, ndigits);

	ecc_point_mult(&K, &ctx->curve->g, k, NULL, ctx->curve, ndigits);

	if (vli_cmp(sig_ctx->r, ctx->curve->n, ndigits) >= 0)
		vli_sub(sig_ctx->r, sig_ctx->r, ctx->curve->n, ndigits);

	if (vli_is_zero(sig_ctx->r, ndigits))
		return -EAGAIN;

	vli_mod_mult_slow(rd_h, sig_ctx->r, ctx->d, ctx->curve->n, ndigits);
	vli_mod_add(rd_h, rd_h, hash, ctx->curve->n, ndigits);
	vli_mod_inv(kinv, k, ctx->curve->n, ndigits);
	vli_mod_mult_slow(sig_ctx->s, kinv, rd_h, ctx->curve->n, ndigits);

	if (vli_is_zero(sig_ctx->s, ndigits))
		return -EAGAIN;

	memzero_explicit(rd_h, sizeof(rd_h));
	memzero_explicit(kinv, sizeof(kinv));
	return 0;
}

/* RFC 6979 p. 3.1.1 selects the same hash function that was used to
 * process the input message. However, we don't have this information in
 * the context and can only guess based on the size of the hash. This is
 * OK, because p. 3.6 states that a different function may be used of the
 * same (or higher) strength. Therefore, we pick SHA-512 as the default
 * case. The only disadvantage would be that the KAT vectors from the RFC
 * will not be verifiable. Userspace should not depend on it anyway as any
 * higher priority ECDSA crypto drivers may actually not implement
 * deterministic signatures
 */
static struct crypto_rng *rfc6979_alloc_rng(struct ecc_ctx *ctx,
					    size_t hash_size, u8 *rawhash)
{
	u64 seed[2 * ECC_MAX_DIGITS];
	unsigned int ndigits = ctx->curve->g.ndigits;
	struct drbg_string entropy, pers = {0};
	struct drbg_test_data seed_data;
	const char *alg;
	struct crypto_rng *rng;
	int err;

	switch (hash_size) {
	case SHA1_DIGEST_SIZE:
		alg = "drbg_nopr_hmac_sha1";
		break;
	case SHA256_DIGEST_SIZE:
		alg = "drbg_nopr_hmac_sha256";
		break;
	case SHA384_DIGEST_SIZE:
		alg = "drbg_nopr_hmac_sha384";
		break;
	default:
		alg = "drbg_nopr_hmac_sha512";
	}

	rng = crypto_alloc_rng(alg, 0, 0);
	if (IS_ERR(rng))
		return rng;

	ecc_swap_digits(ctx->d, seed, ndigits);
	memcpy(seed + ndigits, rawhash, ndigits << ECC_DIGITS_TO_BYTES_SHIFT);
	drbg_string_fill(&entropy, (u8 *)seed, (ndigits * 2) << ECC_DIGITS_TO_BYTES_SHIFT);
	seed_data.testentropy = &entropy;
	err = crypto_drbg_reset_test(rng, &pers, &seed_data);
	if (err) {
		crypto_free_rng(rng);
		return ERR_PTR(err);
	}

	return rng;
}

static int rfc6979_gen_k(struct ecc_ctx *ctx, struct crypto_rng *rng, u64 *k)
{
	unsigned int ndigits = ctx->curve->g.ndigits;
	u8 K[ECC_MAX_BYTES];
	int ret;

	do {
		ret = crypto_rng_get_bytes(rng, K, ndigits << ECC_DIGITS_TO_BYTES_SHIFT);
		if (ret)
			return ret;

		ecc_swap_digits((u64 *)K, k, ndigits);
	} while (vli_cmp(k, ctx->curve->n, ndigits) >= 0);

	memzero_explicit(K, sizeof(K));
	return 0;
}

static int rfc6979_gen_k_hacl(struct ecc_ctx *ctx, struct crypto_rng *rng, u8 *k)
{
	unsigned int ndigits = ctx->curve->g.ndigits;
	int ret;

	do {
		ret = crypto_rng_get_bytes(rng, k, ndigits << ECC_DIGITS_TO_BYTES_SHIFT);
		if (ret)
			return ret;

	} while (!Hacl_P256_validate_private_key(k));

	return 0;
}

/* scratch buffer should be at least ECC_MAX_BYTES */
static int asn1_encode_signature_sg(struct akcipher_request *req,
				    struct ecdsa_signature_ctx *sig_ctx,
				    u8 *scratch)
{
	unsigned int ndigits = sig_ctx->curve->g.ndigits;
	unsigned int r_bits = vli_num_bits(sig_ctx->r, ndigits);
	unsigned int s_bits = vli_num_bits(sig_ctx->s, ndigits);
	struct sg_mapping_iter miter;
	unsigned int nents;
	u8 *buf, *p;
	size_t needed = 2; /* tag and len for the top ASN1 sequence */

	needed += 2; /* tag and len for r as an ASN1 integer */
	needed += BITS_TO_BYTES(r_bits);
	if (r_bits % 8 == 0)
		/* leftmost bit is set, so need another byte for 0x00 to make the
		 * integer positive
		 */
		needed++;

	needed += 2; /* tag and len for s as an ASN1 integer */
	needed += BITS_TO_BYTES(s_bits);
	if (s_bits % 8 == 0)
		/* leftmost bit is set, so need another byte for 0x00 to make the
		 * integer positive
		 */
		needed++;

	if (req->dst_len < needed) {
		req->dst_len = needed;
		return -EOVERFLOW;
	}

	nents = sg_nents_for_len(req->dst, needed);
	if (nents == 1) {
		sg_miter_start(&miter, req->dst, nents, SG_MITER_ATOMIC | SG_MITER_TO_SG);
		sg_miter_next(&miter);
		buf = miter.addr;
	} else {
		buf = kmalloc(needed, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
	}

	/* we will begin from the end */
	ecc_swap_digits(sig_ctx->s, (u64 *)scratch, ndigits);
	p = buf + needed - BITS_TO_BYTES(s_bits);
	memcpy(p, scratch +
	       (ndigits << ECC_DIGITS_TO_BYTES_SHIFT) - BITS_TO_BYTES(s_bits),
	       BITS_TO_BYTES(s_bits));
	if (s_bits % 8 == 0) {
		p--;
		*p = 0;
	}
	p -= 2;
	p[0] = ASN1_INT;
	p[1] = (s_bits % 8 == 0) ? BITS_TO_BYTES(s_bits) + 1 : BITS_TO_BYTES(s_bits);

	ecc_swap_digits(sig_ctx->r, (u64 *)scratch, ndigits);
	p -= BITS_TO_BYTES(r_bits);
	memcpy(p, scratch +
	       (ndigits << ECC_DIGITS_TO_BYTES_SHIFT) - BITS_TO_BYTES(r_bits),
	       BITS_TO_BYTES(r_bits));
	if (r_bits % 8 == 0) {
		p--;
		*p = 0;
	}
	p -= 2;
	p[0] = ASN1_INT;
	p[1] = (r_bits % 8 == 0) ? BITS_TO_BYTES(r_bits) + 1 : BITS_TO_BYTES(r_bits);

	buf[0] = ASN1_CONS_BIT | ASN1_SEQ;
	buf[1] = (needed - 2) & 0xff;

	if (nents == 1)
		sg_miter_stop(&miter);
	else {
		sg_copy_from_buffer(req->dst, nents, buf, needed);
		kfree(buf);
	}
	req->dst_len = needed;

	return 0;
}

static int ecdsa_sign(struct akcipher_request *req)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct ecc_ctx *ctx = akcipher_tfm_ctx(tfm);
	size_t keylen = ctx->curve->g.ndigits << ECC_DIGITS_TO_BYTES_SHIFT;
	u8 rawhash_k[ECC_MAX_BYTES];
	u64 hash[ECC_MAX_DIGITS];
	struct ecdsa_signature_ctx sig_ctx = {
		.curve = ctx->curve,
	};
	struct crypto_rng *rng;
	ssize_t diff;
	int ret;

	/* if the hash is shorter then we will add leading zeros to fit to ndigits */
	diff = keylen - req->src_len;
	if (diff >= 0) {
		if (diff)
			memset(rawhash_k, 0, diff);
		sg_copy_to_buffer(req->src, sg_nents_for_len(req->src, req->src_len),
				  &rawhash_k[diff], req->src_len);
	} else if (diff < 0) {
		/* given hash is longer, we take the left-most bytes */
		sg_copy_to_buffer(req->src, sg_nents_for_len(req->src, req->src_len),
				  rawhash_k, req->src_len);
	}

	rng = rfc6979_alloc_rng(ctx, req->src_len, rawhash_k);
	if (IS_ERR(rng))
		return PTR_ERR(rng);

	if (strncmp(ctx->curve->name, "nist_256", 8) == 0) {
		u8 private_key[32];
		u8 signature[64];
		u8 nonce[32];
		ecc_swap_digits(ctx->d, (u64*)private_key, 2);
		ret = rfc6979_gen_k_hacl(ctx, rng, nonce);
		if (ret) {
			goto alloc_rng;
		}
		/* The signing function also checks that the scalars are valid. */
		/* XXX: Is the value blinded already or should this be done here? */
		do {
			if (Hacl_P256_ecdsa_sign_p256_without_hash(signature, req->dst_len,
													   rawhash_k, private_key, nonce)) {
				ret = 0;
			} else {
				ret = -EAGAIN;
			}
		} while (ret == -EAGAIN);
		/* Encode the signature. Note that this could be more efficient when
		   done directly and not first converting it to u64s. */
		ecc_swap_digits(signature, sig_ctx.r, 2);
		ecc_swap_digits(signature + 32, sig_ctx.s, 2);
		ret = asn1_encode_signature_sg(req, &sig_ctx, rawhash_k);
	} else {
		ecc_swap_digits((u64 *)rawhash_k, hash, ctx->curve->g.ndigits);
		do {
			ret = rfc6979_gen_k(ctx, rng, (u64 *)rawhash_k);
			if (ret)
				goto alloc_rng;

			ret = _ecdsa_sign(ctx, hash, (u64 *)rawhash_k, &sig_ctx);
		} while (ret == -EAGAIN);
		memzero_explicit(rawhash_k, sizeof(rawhash_k));

		ret = asn1_encode_signature_sg(req, &sig_ctx, rawhash_k);
	}

alloc_rng:
	crypto_free_rng(rng);
	return ret;
}

static int ecdsa_ecc_ctx_init(struct ecc_ctx *ctx, unsigned int curve_id)
{
	ctx->curve_id = curve_id;
	ctx->curve = ecc_get_curve(curve_id);
	if (!ctx->curve)
		return -EINVAL;

	return 0;
}


static void ecdsa_ecc_ctx_deinit(struct ecc_ctx *ctx)
{
	ctx->key_set = false;
	if (ctx->is_private)
		memzero_explicit(ctx->d, sizeof(ctx->d));
}

static int ecdsa_ecc_ctx_reset(struct ecc_ctx *ctx)
{
	unsigned int curve_id = ctx->curve_id;
	int ret;

	ecdsa_ecc_ctx_deinit(ctx);
	ret = ecdsa_ecc_ctx_init(ctx, curve_id);
	if (ret == 0)
		ctx->pub_key = ECC_POINT_INIT(ctx->x, ctx->y,
					      ctx->curve->g.ndigits);
	return ret;
}

/*
 * Set the public key given the raw uncompressed key data from an X509
 * certificate. The key data contain the concatenated X and Y coordinates of
 * the public key.
 */
static int ecdsa_set_pub_key(struct crypto_akcipher *tfm, const void *key, unsigned int keylen)
{
	struct ecc_ctx *ctx = akcipher_tfm_ctx(tfm);
	const unsigned char *d = key;
	const u64 *digits = (const u64 *)&d[1];
	unsigned int ndigits;
	int ret;

	ret = ecdsa_ecc_ctx_reset(ctx);
	if (ret < 0)
		return ret;

	if (keylen < 1 || (((keylen - 1) >> 1) % sizeof(u64)) != 0)
		return -EINVAL;
	/* we only accept uncompressed format indicated by '4' */
	if (d[0] != 4)
		return -EINVAL;

	keylen--;
	ndigits = (keylen >> 1) / sizeof(u64);
	if (ndigits != ctx->curve->g.ndigits)
		return -EINVAL;

	ecc_swap_digits(digits, ctx->pub_key.x, ndigits);
	ecc_swap_digits(&digits[ndigits], ctx->pub_key.y, ndigits);
	ret = ecc_is_pubkey_valid_full(ctx->curve, &ctx->pub_key);

	ctx->key_set = ret == 0;
	ctx->is_private = false;

	return ret;
}

int ecc_get_priv_key(void *context, size_t hdrlen, unsigned char tag,
		     const void *value, size_t vlen)
{
	struct ecc_ctx *ctx = context;
	size_t dlen = ctx->curve->g.ndigits * sizeof(u64);
	ssize_t diff = vlen - dlen;
	const char *d = value;
	u8 priv[ECC_MAX_BYTES];

	/* diff = 0: 'value' has exacly the right size
	 * diff > 0: 'value' has too many bytes; one leading zero is allowed that
	 *           makes the value a positive integer; error on more
	 * diff < 0: 'value' is missing leading zeros, which we add
	 */
	if (diff > 0) {
		/* skip over leading zeros that make 'value' a positive int */
		if (*d == 0) {
			vlen -= 1;
			diff--;
			d++;
		}
		if (diff)
			return -EINVAL;
	}
	if (-diff >= dlen)
		return -EINVAL;

	if (diff) {
		/* leading zeros not given in 'value' */
		memset(priv, 0, -diff);
	}

	memcpy(&priv[-diff], d, vlen);

	ecc_swap_digits((u64 *)priv, ctx->d, ctx->curve->g.ndigits);
	memzero_explicit(priv, sizeof(priv));
	return ecc_is_key_valid(ctx->curve_id, ctx->curve->g.ndigits, ctx->d, dlen);
}

int ecc_get_priv_params(void *context, size_t hdrlen, unsigned char tag,
			const void *value, size_t vlen)
{
	struct ecc_ctx *ctx = context;

	switch (look_up_OID(value, vlen)) {
	case OID_id_prime192v1:
		return (ctx->curve_id == ECC_CURVE_NIST_P192) ? 0 : -EINVAL;
	case OID_id_prime256v1:
		return (ctx->curve_id == ECC_CURVE_NIST_P256) ? 0 : -EINVAL;
	case OID_id_ansip384r1:
		return (ctx->curve_id == ECC_CURVE_NIST_P384) ? 0 : -EINVAL;
	default:
		break;
	}

	return -EINVAL;
}

int ecc_get_priv_version(void *context, size_t hdrlen, unsigned char tag,
			 const void *value, size_t vlen)
{
	if (vlen == 1) {
		if (*((u8 *)value) == 1)
			return 0;
	}

	return -EINVAL;
}

static int ecdsa_set_priv_key(struct crypto_akcipher *tfm, const void *key,
			      unsigned int keylen)
{
	struct ecc_ctx *ctx = akcipher_tfm_ctx(tfm);
	int ret;

	ret = ecdsa_ecc_ctx_reset(ctx);
	if (ret < 0)
		return ret;

	ret = asn1_ber_decoder(&ecprivkey_decoder, ctx, key, keylen);
	if (ret)
		return ret;

	ecc_point_mult(&ctx->pub_key, &ctx->curve->g, ctx->d, NULL, ctx->curve,
		       ctx->curve->g.ndigits);
	ret = ecc_is_pubkey_valid_full(ctx->curve, &ctx->pub_key);
	if (ret)
		return ret;

	ctx->key_set = ret == 0;
	ctx->is_private = true;
 
 	return ret;
 }

static void ecdsa_exit_tfm(struct crypto_akcipher *tfm)
{
	struct ecc_ctx *ctx = akcipher_tfm_ctx(tfm);

	ecdsa_ecc_ctx_deinit(ctx);
}

static unsigned int ecdsa_max_size(struct crypto_akcipher *tfm)
{
	struct ecc_ctx *ctx = akcipher_tfm_ctx(tfm);

	if (!ctx->key_set)
		return 0;

	if (ctx->is_private) {
		/* see ecdsasignature.asn1
		 * for a max 384 bit curve we would only need 1 byte length
		 * ASN1 encoding for the top level sequence and r,s integers
		 * 1 byte sequence tag + 1 byte sequence length (max 102 for 384
		 * bit curve) + 2 (for r and s) * (1 byte integer tag + 1 byte
		 * integer length (max 49 for 384 bit curve) + 1 zero byte (if r
		 * or s has leftmost bit set) + sizeof(r or s)
		 */
		return 2 + 2 * (3 + (ctx->curve->g.ndigits << ECC_DIGITS_TO_BYTES_SHIFT));
	}

	return ctx->curve->g.ndigits << ECC_DIGITS_TO_BYTES_SHIFT;
}

static int ecdsa_nist_p384_init_tfm(struct crypto_akcipher *tfm)
{
	struct ecc_ctx *ctx = akcipher_tfm_ctx(tfm);

	return ecdsa_ecc_ctx_init(ctx, ECC_CURVE_NIST_P384);
}

static struct akcipher_alg ecdsa_nist_p384 = {
	.sign = ecdsa_sign,
	.verify = ecdsa_verify,
	.set_priv_key = ecdsa_set_priv_key,
	.set_pub_key = ecdsa_set_pub_key,
	.max_size = ecdsa_max_size,
	.init = ecdsa_nist_p384_init_tfm,
	.exit = ecdsa_exit_tfm,
	.base = {
		.cra_name = "ecdsa-nist-p384",
		.cra_driver_name = "ecdsa-nist-p384-generic",
		.cra_priority = 100,
		.cra_module = THIS_MODULE,
		.cra_ctxsize = sizeof(struct ecc_ctx),
	},
};

static int ecdsa_nist_p256_init_tfm(struct crypto_akcipher *tfm)
{
	struct ecc_ctx *ctx = akcipher_tfm_ctx(tfm);

	return ecdsa_ecc_ctx_init(ctx, ECC_CURVE_NIST_P256);
}

static struct akcipher_alg ecdsa_nist_p256 = {
	.sign = ecdsa_sign,
	.verify = ecdsa_verify,
	.set_priv_key = ecdsa_set_priv_key,
	.set_pub_key = ecdsa_set_pub_key,
	.max_size = ecdsa_max_size,
	.init = ecdsa_nist_p256_init_tfm,
	.exit = ecdsa_exit_tfm,
	.base = {
		.cra_name = "ecdsa-nist-p256",
		.cra_driver_name = "ecdsa-nist-p256-generic",
		.cra_priority = 100,
		.cra_module = THIS_MODULE,
		.cra_ctxsize = sizeof(struct ecc_ctx),
	},
};

static int ecdsa_nist_p192_init_tfm(struct crypto_akcipher *tfm)
{
	struct ecc_ctx *ctx = akcipher_tfm_ctx(tfm);

	return ecdsa_ecc_ctx_init(ctx, ECC_CURVE_NIST_P192);
}

static struct akcipher_alg ecdsa_nist_p192 = {
	.sign = ecdsa_sign,
	.verify = ecdsa_verify,
	.set_priv_key = ecdsa_set_priv_key,
	.set_pub_key = ecdsa_set_pub_key,
	.max_size = ecdsa_max_size,
	.init = ecdsa_nist_p192_init_tfm,
	.exit = ecdsa_exit_tfm,
	.base = {
		.cra_name = "ecdsa-nist-p192",
		.cra_driver_name = "ecdsa-nist-p192-generic",
		.cra_priority = 100,
		.cra_module = THIS_MODULE,
		.cra_ctxsize = sizeof(struct ecc_ctx),
	},
};
static bool ecdsa_nist_p192_registered;

static int __init ecdsa_init(void)
{
	int ret;

	/* NIST p192 may not be available in FIPS mode */
	ret = crypto_register_akcipher(&ecdsa_nist_p192);
	ecdsa_nist_p192_registered = ret == 0;

	ret = crypto_register_akcipher(&ecdsa_nist_p256);
	if (ret)
		goto nist_p256_error;

	ret = crypto_register_akcipher(&ecdsa_nist_p384);
	if (ret)
		goto nist_p384_error;

	return 0;

nist_p384_error:
	crypto_unregister_akcipher(&ecdsa_nist_p256);

nist_p256_error:
	if (ecdsa_nist_p192_registered)
		crypto_unregister_akcipher(&ecdsa_nist_p192);
	return ret;
}

static void __exit ecdsa_exit(void)
{
	if (ecdsa_nist_p192_registered)
		crypto_unregister_akcipher(&ecdsa_nist_p192);
	crypto_unregister_akcipher(&ecdsa_nist_p256);
	crypto_unregister_akcipher(&ecdsa_nist_p384);
}

subsys_initcall(ecdsa_init);
module_exit(ecdsa_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stefan Berger <stefanb@linux.ibm.com>");
MODULE_DESCRIPTION("ECDSA generic algorithm");
MODULE_ALIAS_CRYPTO("ecdsa-generic");
