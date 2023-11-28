/*
 * GPLv2 or MIT License
 *
 * Copyright (c) 2023 Cryspen
 *
 */

#include "hacl_rsa.h"

#include <linux/fips.h>
#include <linux/module.h>
#include <linux/mpi.h>
#include <crypto/internal/rsa.h>
#include <crypto/internal/akcipher.h>
#include <crypto/akcipher.h>
#include <crypto/algapi.h>

/**
RSA Key data structure
**/

struct hacl_rsa_key {
    uint32_t modBits;
    uint32_t eBits;
    uint32_t dBits;
    uint8_t *nb;
    uint8_t *eb;
    uint8_t *db;
}

static inline struct hacl_rsa_key *rsa_get_key(struct crypto_akcipher *tfm)
{
	return akcipher_tfm_ctx(tfm);
}

static int rsa_enc(struct akcipher_request *req)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	const struct hacl_rsa_key *pkey = rsa_get_key(tfm);
	int ret = 0;

	if (unlikely(!pkey->nb || !pkey->eb)) {
		ret = -EINVAL;
		goto done;
	}
	unsigned int plain_len = (pkey->modBits - 1)/8 + 1;
	unsigned int cipher_len = (pkey->modBits - 2)/8 + 1;

	if (req->src_len != plain_len || req->dst_len != cipher_len) {
		ret = -EINVAL;
		goto done;
	}

	uint64_t *pk = HACL_RSA_new_rsa_load_pkey(pkey->modBits,pkey->eBits,pkey->nb,pkey->eb);

	if (!pk) {
		ret = -EINVAL;
		goto done;
	}
	
	ret = HACL_RSA_rsa_enc(pkey->modBits,pkey->eBits,pk, req->src, req->dst);

	if (!ret)
	         ret = -EBADMSG;

	free(pk);

 done:	return ret;
}

static int rsa_dec(struct akcipher_request *req)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	const struct hacl_rsa_key *skey = rsa_get_key(tfm);
	int ret = 0;

	if (unlikely(!skey->nb || !skey->db)) {
		ret = -EINVAL;
		goto done;
	}
	unsigned int plain_len = (skey->modBits - 1)/8 + 1;
	unsigned int cipher_len = (skey->modBits - 2)/8 + 1;

	if (req->src_len != cipher_len || req->dst_len != plain_len) {
		ret = -EINVAL;
		goto done;
	}

	uint64_t *sk = HACL_RSA_new_rsa_load_skey(skey->modBits,skey->eBits,skey->dBits,skey->nb,skey->eb,skey->db);

	if (!sk) {
		ret = -EINVAL;
		goto done;
	}
	
	ret = HACL_RSA_rsa_dec(skey->modBits,skey->eBits,skey->dBits,sk,req->src,req->dst);

	if (!ret)
	         ret = -EBADMSG;

	free(sk);

 done:	return ret;
}

static void rsa_free_key(struct hacl_rsa_key *key)
{
	free(key->db);
	free(key->eb);
	free(key->nb);
	key->db = NULL;
	key->eb = NULL;
	key->nb = NULL;
}

static int rsa_check_key_length(unsigned int len)
{
	switch (len) {
	case 512:
	case 1024:
	case 1536:
		if (fips_enabled)
			return -EINVAL;
		fallthrough;
	case 2048:
	case 3072:
	case 4096:
		return 0;
	}

	return -EINVAL;
}


static int rsa_set_pub_key(struct crypto_akcipher *tfm, const void *key,
			   unsigned int keylen)
{
	struct hacl_rsa_key *pkey = rsa_get_key(tfm);
	struct rsa_key raw_key = {0};
	
	int ret = 0;

	/* Free the old MPI key if any */
	rsa_free_key(pkey);

	ret = rsa_parse_pub_key(&raw_key, key, keylen);
	if (ret)
		return ret;

	pkey->modBits = raw_key.n_sz * 8;
	pkey->eBits = raw_key.e_sz * 8;
	pkey->nb = raw_key.n;
	pkey->eb = raw_key.e;

	if (!pkey->nb || !pkey->eb)
		goto err;

	return ret;
	
err:
	rsa_free_key(pkey);
	return -ENOMEM;
}

static int rsa_set_priv_key(struct crypto_akcipher *tfm, const void *key,
			   unsigned int keylen)
{
	struct hacl_rsa_key *skey = rsa_get_key(tfm);
	struct rsa_key raw_key = {0};
	
	int ret = 0;

	/* Free the old MPI key if any */
	rsa_free_key(skey);

	ret = rsa_parse_priv_key(&raw_key, key, keylen);
	if (ret)
		return ret;

	skey->modBits = raw_key.n_sz * 8;
	skey->eBits = raw_key.e_sz * 8;
       	skey->dBits = raw_key.d_sz * 8;
	skey->nb = raw_key.n;
	skey->eb = raw_key.e;
	skey->db = raw_key.d;

	if (!skey->nb || !skey->eb || !skey->db)
		goto err;

	return ret;
	
err:
	rsa_free_key(skey);
	return -ENOMEM;
}

static unsigned int rsa_max_size(struct crypto_akcipher *tfm)
{
	struct rsa_mpi_key *pkey = akcipher_tfm_ctx(tfm);

	return mpi_get_size(pkey->modBits);
}

static void rsa_exit_tfm(struct crypto_akcipher *tfm)
{
	struct rsa_mpi_key *pkey = akcipher_tfm_ctx(tfm);

	rsa_free_key(pkey);
}

static struct akcipher_alg hacl_rsa = {
	.encrypt = rsa_enc,
	.decrypt = rsa_dec,
	.set_priv_key = rsa_set_priv_key,
	.set_pub_key = rsa_set_pub_key,
	.max_size = rsa_max_size,
	.exit = rsa_exit_tfm,
	.base = {
		.cra_name = "rsa",
		.cra_driver_name = "rsa-hacl",
		.cra_priority = 100,
		.cra_module = THIS_MODULE,
		.cra_ctxsize = sizeof(struct hacl_rsa_key),
	},
};

static int __init hacl_rsa_init(void)
{
	int err;

	err = crypto_register_akcipher(&rsa);
	if (err)
		return err;

	return 0;
}

static void __exit hacl_rsa_exit(void)
{
	crypto_unregister_akcipher(&rsa);
}

subsys_initcall(hacl_rsa_init);
module_exit(hacl_rsa_exit);
MODULE_ALIAS_CRYPTO("rsa");
MODULE_ALIAS_CRYPTO("rsa-hacl");
MODULE_LICENSE("GPLv2 or MIT");
MODULE_DESCRIPTION("Formally Verified RSA algorithm from HACL*");
