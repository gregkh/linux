/*
 * linux/net/sunrpc/auth_null.c
 *
 * AUTH_NULL authentication. Really :-)
 *
 * Copyright (C) 1996, Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/module.h>
#include <linux/in.h>
#include <linux/utsname.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sched.h>

#ifdef RPC_DEBUG
# define RPCDBG_FACILITY	RPCDBG_AUTH
#endif

static struct rpc_credops	null_credops;

static struct rpc_auth *
nul_create(struct rpc_clnt *clnt, rpc_authflavor_t flavor)
{
	struct rpc_auth	*auth;

	dprintk("RPC: creating NULL authenticator for client %p\n", clnt);
	if (!(auth = (struct rpc_auth *) kmalloc(sizeof(*auth),GFP_KERNEL)))
		return NULL;
	auth->au_cslack = 4;
	auth->au_rslack = 2;
	auth->au_ops = &authnull_ops;
	auth->au_expire = 1800 * HZ;
	rpcauth_init_credcache(auth);

	return (struct rpc_auth *) auth;
}

static void
nul_destroy(struct rpc_auth *auth)
{
	dprintk("RPC: destroying NULL authenticator %p\n", auth);
	rpcauth_free_credcache(auth);
}

/*
 * Create NULL creds for current process
 */
static struct rpc_cred *
nul_create_cred(struct rpc_auth *auth, struct auth_cred *acred, int flags)
{
	struct rpc_cred	*cred;

	if (!(cred = (struct rpc_cred *) kmalloc(sizeof(*cred),GFP_KERNEL)))
		return NULL;
	atomic_set(&cred->cr_count, 0);
	cred->cr_flags = RPCAUTH_CRED_UPTODATE;
	cred->cr_uid = acred->uid;
	cred->cr_ops = &null_credops;

	return cred;
}

/*
 * Destroy cred handle.
 */
static void
nul_destroy_cred(struct rpc_cred *cred)
{
	kfree(cred);
}

/*
 * Match cred handle against current process
 */
static int
nul_match(struct auth_cred *acred, struct rpc_cred *cred, int taskflags)
{
	return 1;
}

/*
 * Marshal credential.
 */
static u32 *
nul_marshal(struct rpc_task *task, u32 *p, int ruid)
{
	*p++ = htonl(RPC_AUTH_NULL);
	*p++ = 0;
	*p++ = htonl(RPC_AUTH_NULL);
	*p++ = 0;

	return p;
}

/*
 * Refresh credential. This is a no-op for AUTH_NULL
 */
static int
nul_refresh(struct rpc_task *task)
{
	task->tk_msg.rpc_cred->cr_flags |= RPCAUTH_CRED_UPTODATE;
	return 0;
}

static u32 *
nul_validate(struct rpc_task *task, u32 *p)
{
	rpc_authflavor_t	flavor;
	u32			size;

	flavor = ntohl(*p++);
	if (flavor != RPC_AUTH_NULL) {
		printk("RPC: bad verf flavor: %u\n", flavor);
		return NULL;
	}

	size = ntohl(*p++);
	if (size != 0) {
		printk("RPC: bad verf size: %u\n", size);
		return NULL;
	}

	return p;
}

struct rpc_authops	authnull_ops = {
	.owner		= THIS_MODULE,
	.au_flavor	= RPC_AUTH_NULL,
#ifdef RPC_DEBUG
	.au_name	= "NULL",
#endif
	.create		= nul_create,
	.destroy	= nul_destroy,
	.crcreate	= nul_create_cred,
};

static
struct rpc_credops	null_credops = {
	.crdestroy	= nul_destroy_cred,
	.crmatch	= nul_match,
	.crmarshal	= nul_marshal,
	.crrefresh	= nul_refresh,
	.crvalidate	= nul_validate,
};
