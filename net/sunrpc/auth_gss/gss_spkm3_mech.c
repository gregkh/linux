/*
 *  linux/net/sunrpc/gss_spkm3_mech.c
 *
 *  Copyright (c) 2003 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Andy Adamson <andros@umich.edu>
 *  J. Bruce Fields <bfields@umich.edu>
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 *  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/sunrpc/auth.h>
#include <linux/in.h>
#include <linux/sunrpc/svcauth_gss.h>
#include <linux/sunrpc/gss_spkm3.h>
#include <linux/sunrpc/xdr.h>
#include <linux/crypto.h>

#ifdef RPC_DEBUG
# define RPCDBG_FACILITY	RPCDBG_AUTH
#endif

static inline int
get_bytes(char **ptr, const char *end, void *res, int len)
{
	char *p, *q;
	p = *ptr;
	q = p + len;
	if (q > end || q < p)
		return -1;
	memcpy(res, p, len);
	*ptr = q;
	return 0;
}

static inline int
get_netobj(char **ptr, const char *end, struct xdr_netobj *res)
{
	char *p, *q;
	p = *ptr;
	if (get_bytes(&p, end, &res->len, sizeof(res->len)))
		return -1;
	q = p + res->len;
	if(res->len == 0)
		goto out_nocopy;
	if (q > end || q < p)
		return -1;
	if (!(res->data = kmalloc(res->len, GFP_KERNEL)))
		return -1;
	memcpy(res->data, p, res->len);
out_nocopy:
	*ptr = q;
	return 0;
}

static inline int
get_key(char **p, char *end, struct crypto_tfm **res, int *resalg)
{
	struct xdr_netobj	key = {
		.len = 0,
		.data = NULL,
	};
	int			alg_mode,setkey = 0;
	char			*alg_name;

	if (get_bytes(p, end, resalg, sizeof(int)))
		goto out_err;
	if ((get_netobj(p, end, &key)))
		goto out_err;

	switch (*resalg) {
		case NID_des_cbc:
			alg_name = "des";
			alg_mode = CRYPTO_TFM_MODE_CBC;
			setkey = 1;
			break;
		case NID_md5:
			if (key.len == 0) {
				dprintk("RPC: SPKM3 get_key: NID_md5 zero Key length\n");
			}
			alg_name = "md5";
			alg_mode = 0;
			setkey = 0;
			break;
		case NID_cast5_cbc:
			dprintk("RPC: SPKM3 get_key: case cast5_cbc, UNSUPPORTED \n");
			goto out_err;
			break;
		default:
			dprintk("RPC: SPKM3 get_key: unsupported algorithm %d", *resalg);
			goto out_err_free_key;
	}
	if (!(*res = crypto_alloc_tfm(alg_name, alg_mode)))
		goto out_err_free_key;
	if (setkey) {
		if (crypto_cipher_setkey(*res, key.data, key.len))
			goto out_err_free_tfm;
	}

	if(key.len > 0)
		kfree(key.data);
	return 0;

out_err_free_tfm:
	crypto_free_tfm(*res);
out_err_free_key:
	if(key.len > 0)
		kfree(key.data);
out_err:
	return -1;
}

static u32
gss_import_sec_context_spkm3(struct xdr_netobj *inbuf,
				struct gss_ctx *ctx_id)
{
	char	*p = inbuf->data;
	char	*end = inbuf->data + inbuf->len;
	struct	spkm3_ctx *ctx;

	if (!(ctx = kmalloc(sizeof(*ctx), GFP_KERNEL)))
		goto out_err;
	memset(ctx, 0, sizeof(*ctx));

	if (get_netobj(&p, end, &ctx->ctx_id))
		goto out_err_free_ctx;

	if (get_bytes(&p, end, &ctx->qop, sizeof(ctx->qop)))
		goto out_err_free_ctx_id;

	if (get_netobj(&p, end, &ctx->mech_used))
		goto out_err_free_mech;

	if (get_bytes(&p, end, &ctx->ret_flags, sizeof(ctx->ret_flags)))
		goto out_err_free_mech;

	if (get_bytes(&p, end, &ctx->req_flags, sizeof(ctx->req_flags)))
		goto out_err_free_mech;

	if (get_netobj(&p, end, &ctx->share_key))
		goto out_err_free_s_key;

	if (get_key(&p, end, &ctx->derived_conf_key, &ctx->conf_alg)) {
		dprintk("RPC: SPKM3 confidentiality key will be NULL\n");
	}

	if (get_key(&p, end, &ctx->derived_integ_key, &ctx->intg_alg)) {
		dprintk("RPC: SPKM3 integrity key will be NULL\n");
	}

	if (get_bytes(&p, end, &ctx->owf_alg, sizeof(ctx->owf_alg)))
		goto out_err_free_s_key;

	if (get_bytes(&p, end, &ctx->owf_alg, sizeof(ctx->owf_alg)))
		goto out_err_free_s_key;

	if (p != end)
		goto out_err_free_s_key;

	ctx_id->internal_ctx_id = ctx;

	dprintk("Succesfully imported new spkm context.\n");
	return 0;

out_err_free_s_key:
	kfree(ctx->share_key.data);
out_err_free_mech:
	kfree(ctx->mech_used.data);
out_err_free_ctx_id:
	kfree(ctx->ctx_id.data);
out_err_free_ctx:
	kfree(ctx);
out_err:
	return GSS_S_FAILURE;
}

static void
gss_delete_sec_context_spkm3(void *internal_ctx) {
	struct spkm3_ctx *sctx = internal_ctx;

	if(sctx->derived_integ_key)
		crypto_free_tfm(sctx->derived_integ_key);
	if(sctx->derived_conf_key)
		crypto_free_tfm(sctx->derived_conf_key);
	if(sctx->share_key.data)
		kfree(sctx->share_key.data);
	if(sctx->mech_used.data)
		kfree(sctx->mech_used.data);
	kfree(sctx);
}

static u32
gss_verify_mic_spkm3(struct gss_ctx		*ctx,
			struct xdr_buf		*signbuf,
			struct xdr_netobj	*checksum,
			u32		*qstate) {
	u32 maj_stat = 0;
	int qop_state = 0;
	struct spkm3_ctx *sctx = ctx->internal_ctx_id;

	dprintk("RPC: gss_verify_mic_spkm3 calling spkm3_read_token\n");
	maj_stat = spkm3_read_token(sctx, checksum, signbuf, &qop_state,
				   SPKM_MIC_TOK);

	if (!maj_stat && qop_state)
	    *qstate = qop_state;

	dprintk("RPC: gss_verify_mic_spkm3 returning %d\n", maj_stat);
	return maj_stat;
}

static u32
gss_get_mic_spkm3(struct gss_ctx	*ctx,
		     u32		qop,
		     struct xdr_buf	*message_buffer,
		     struct xdr_netobj	*message_token) {
	u32 err = 0;
	struct spkm3_ctx *sctx = ctx->internal_ctx_id;

	dprintk("RPC: gss_get_mic_spkm3\n");

	err = spkm3_make_token(sctx, qop, message_buffer,
			      message_token, SPKM_MIC_TOK);
	return err;
}

static struct gss_api_ops gss_spkm3_ops = {
	.gss_import_sec_context	= gss_import_sec_context_spkm3,
	.gss_get_mic		= gss_get_mic_spkm3,
	.gss_verify_mic		= gss_verify_mic_spkm3,
	.gss_delete_sec_context	= gss_delete_sec_context_spkm3,
};

static struct pf_desc gss_spkm3_pfs[] = {
	{RPC_AUTH_GSS_SPKM, 0, RPC_GSS_SVC_NONE, "spkm3"},
	{RPC_AUTH_GSS_SPKMI, 0, RPC_GSS_SVC_INTEGRITY, "spkm3i"},
};

static struct gss_api_mech gss_spkm3_mech = {
	.gm_name	= "spkm3",
	.gm_owner	= THIS_MODULE,
	.gm_ops		= &gss_spkm3_ops,
	.gm_pf_num	= ARRAY_SIZE(gss_spkm3_pfs),
	.gm_pfs		= gss_spkm3_pfs,
};

static int __init init_spkm3_module(void)
{
	int status;

	status = gss_mech_register(&gss_spkm3_mech);
	if (status)
		printk("Failed to register spkm3 gss mechanism!\n");
	return 0;
}

static void __exit cleanup_spkm3_module(void)
{
	gss_mech_unregister(&gss_spkm3_mech);
}

MODULE_LICENSE("GPL");
module_init(init_spkm3_module);
module_exit(cleanup_spkm3_module);
