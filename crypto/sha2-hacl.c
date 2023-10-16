/*
 * GPLv2 or MIT License
 *
 * Copyright (c) 2023 Cryspen
 *
 */

#include <crypto/sha2.h>
#include <crypto/sha512_base.h>

#include "hacl_hash.h"
#include "hacl_lib.h"

int hacl_sha256_update(struct shash_desc *desc, const u8 *data,
                       unsigned int len)
{
        struct sha256_state *sctx = shash_desc_ctx(desc);
        struct Hacl_Streaming_MD_state_32_s st;
        st.block_state = sctx->state;
        st.buf = sctx->buf;
        st.total_len = sctx->count;
        uint8_t res = Hacl_Streaming_SHA2_update_256(&st, (u8 *)data, len);
        sctx->count = st.total_len;
        return res;
}
EXPORT_SYMBOL(hacl_sha256_update);

static int hacl_sha256_final(struct shash_desc *desc, u8 *out)
{
        struct sha256_state *sctx = shash_desc_ctx(desc);
        struct Hacl_Streaming_MD_state_32_s st;
        st.block_state = sctx->state;
        st.buf = sctx->buf;
        st.total_len = sctx->count;
        if (crypto_shash_digestsize(desc->tfm) == SHA224_DIGEST_SIZE)
                Hacl_Streaming_SHA2_finish_224(&st, out);
        else
                Hacl_Streaming_SHA2_finish_256(&st, out);
        return 0;
}

int hacl_sha256_finup(struct shash_desc *desc, const u8 *data, unsigned int len,
                      u8 *hash)
{
        struct sha256_state *sctx = shash_desc_ctx(desc);
        struct Hacl_Streaming_MD_state_32_s st;
        st.block_state = sctx->state;
        st.buf = sctx->buf;
        st.total_len = sctx->count;
        Hacl_Streaming_SHA2_update_256(&st, (u8 *)data, len);
        if (crypto_shash_digestsize(desc->tfm) == SHA224_DIGEST_SIZE)
                Hacl_Streaming_SHA2_finish_224(&st, hash);
        else
                Hacl_Streaming_SHA2_finish_256(&st, hash);
        return 0;
}
EXPORT_SYMBOL(hacl_sha256_finup);

int hacl_sha512_update(struct shash_desc *desc, const u8 *data,
                       unsigned int len)
{
        struct sha512_state *sctx = shash_desc_ctx(desc);
        struct Hacl_Streaming_MD_state_64_s st;
        st.block_state = sctx->state;
        st.buf = sctx->buf;
        st.total_len = sctx->count[0];
        uint8_t res = Hacl_Streaming_SHA2_update_512(&st, (u8 *)data, len);
        sctx->count[0] = st.total_len;
        return res;
}
EXPORT_SYMBOL(hacl_sha512_update);

static int hacl_sha512_final(struct shash_desc *desc, u8 *hash)
{
        struct sha512_state *sctx = shash_desc_ctx(desc);
        struct Hacl_Streaming_MD_state_64_s st;
        st.block_state = sctx->state;
        st.buf = sctx->buf;
        st.total_len = sctx->count[0];
        if (crypto_shash_digestsize(desc->tfm) == SHA384_DIGEST_SIZE)
                Hacl_Streaming_SHA2_finish_384(&st, hash);
        else
                Hacl_Streaming_SHA2_finish_512(&st, hash);
        return 0;
}

int hacl_sha512_finup(struct shash_desc *desc, const u8 *data, unsigned int len,
                      u8 *hash)
{
        struct sha512_state *sctx = shash_desc_ctx(desc);
        struct Hacl_Streaming_MD_state_64_s st;
        st.block_state = sctx->state;
        st.buf = sctx->buf;
        st.total_len = sctx->count[0];
        uint8_t res = Hacl_Streaming_SHA2_update_512(&st, (u8 *)data, len);
        if (res == 0) {
                if (crypto_shash_digestsize(desc->tfm) == SHA384_DIGEST_SIZE)
                        Hacl_Streaming_SHA2_finish_384(&st, hash);
                else
                        Hacl_Streaming_SHA2_finish_512(&st, hash);
                return 0;
        } else {
                return res;
        }
}
EXPORT_SYMBOL(hacl_sha512_finup);

static struct shash_alg sha2_hacl_algs[4] = { {
                .digestsize = SHA256_DIGEST_SIZE,
                .init = sha256_base_init,
                .update = hacl_sha256_update,
                .final = hacl_sha256_final,
                .finup = hacl_sha256_finup,
                .descsize = sizeof(struct sha256_state),
                .base = {
                        .cra_name = "sha256",
                        .cra_driver_name = "sha256-hacl",
                        .cra_priority = 100,
                        .cra_blocksize = SHA256_BLOCK_SIZE,
                        .cra_module = THIS_MODULE,
                }
        }, {
                .digestsize = SHA224_DIGEST_SIZE,
                .init = sha224_base_init,
                .update = hacl_sha256_update,
                .final = hacl_sha256_final,
                .finup = hacl_sha256_finup,
                .descsize = sizeof(struct sha256_state),
                .base = {
                        .cra_name = "sha224",
                        .cra_driver_name = "sha224-hacl",
                        .cra_priority = 100,
                        .cra_blocksize = SHA224_BLOCK_SIZE,
                        .cra_module = THIS_MODULE,
                }
        }, {
                .digestsize = SHA384_DIGEST_SIZE,
                .init = sha384_base_init,
                .update = hacl_sha512_update,
                .final = hacl_sha512_final,
                .finup = hacl_sha512_finup,
                .descsize = sizeof(struct sha512_state),
                .base = {
                        .cra_name = "sha384",
                        .cra_driver_name = "sha384-hacl",
                        .cra_priority = 100,
                        .cra_blocksize = SHA384_BLOCK_SIZE,
                        .cra_module = THIS_MODULE,
                }
        }, {
                .digestsize = SHA512_DIGEST_SIZE,
                .init = sha512_base_init,
                .update = hacl_sha512_update,
                .final = hacl_sha512_final,
                .finup = hacl_sha512_finup,
                .descsize = sizeof(struct sha512_state),
                .base = {
                        .cra_name = "sha512",
                        .cra_driver_name = "sha512-hacl",
                        .cra_priority = 100,
                        .cra_blocksize = SHA512_BLOCK_SIZE,
                        .cra_module = THIS_MODULE,
                }
        }
};

static int __init sha2_hacl_mod_init(void)
{
        return crypto_register_shashes(sha2_hacl_algs,
                                       ARRAY_SIZE(sha2_hacl_algs));
}

static void __exit sha2_hacl_mod_fini(void)
{
        crypto_unregister_shashes(sha2_hacl_algs, ARRAY_SIZE(sha2_hacl_algs));
}

subsys_initcall(sha2_hacl_mod_init);
module_exit(sha2_hacl_mod_fini);

MODULE_LICENSE("GPLv2 or MIT");
MODULE_DESCRIPTION("Formally Verified SHA-2 Secure Hash Algorithm from HACL*");

MODULE_ALIAS_CRYPTO("sha224");
MODULE_ALIAS_CRYPTO("sha224-hacl");
MODULE_ALIAS_CRYPTO("sha256");
MODULE_ALIAS_CRYPTO("sha256-hacl");
MODULE_ALIAS_CRYPTO("sha384");
MODULE_ALIAS_CRYPTO("sha384-hacl");
MODULE_ALIAS_CRYPTO("sha512");
MODULE_ALIAS_CRYPTO("sha512-hacl");
