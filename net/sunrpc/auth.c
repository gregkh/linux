/*
 * linux/net/sunrpc/auth.c
 *
 * Generic RPC client authentication API.
 *
 * Copyright (C) 1996, Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/socket.h>
#include <linux/sunrpc/clnt.h>
#include <linux/spinlock.h>

#ifdef RPC_DEBUG
# define RPCDBG_FACILITY	RPCDBG_AUTH
#endif

static struct rpc_authops *	auth_flavors[RPC_AUTH_MAXFLAVOR] = {
	&authnull_ops,		/* AUTH_NULL */
	&authunix_ops,		/* AUTH_UNIX */
	NULL,			/* others can be loadable modules */
};

static u32
pseudoflavor_to_flavor(u32 flavor) {
	if (flavor >= RPC_AUTH_MAXFLAVOR)
		return RPC_AUTH_GSS;
	return flavor;
}

int
rpcauth_register(struct rpc_authops *ops)
{
	rpc_authflavor_t flavor;

	if ((flavor = ops->au_flavor) >= RPC_AUTH_MAXFLAVOR)
		return -EINVAL;
	if (auth_flavors[flavor] != NULL)
		return -EPERM;		/* what else? */
	auth_flavors[flavor] = ops;
	return 0;
}

int
rpcauth_unregister(struct rpc_authops *ops)
{
	rpc_authflavor_t flavor;

	if ((flavor = ops->au_flavor) >= RPC_AUTH_MAXFLAVOR)
		return -EINVAL;
	if (auth_flavors[flavor] != ops)
		return -EPERM;		/* what else? */
	auth_flavors[flavor] = NULL;
	return 0;
}

struct rpc_auth *
rpcauth_create(rpc_authflavor_t pseudoflavor, struct rpc_clnt *clnt)
{
	struct rpc_auth		*auth;
	struct rpc_authops	*ops;
	u32			flavor = pseudoflavor_to_flavor(pseudoflavor);

	if (flavor >= RPC_AUTH_MAXFLAVOR || !(ops = auth_flavors[flavor]))
		return NULL;
	if (!try_module_get(ops->owner))
		return NULL;
	auth = ops->create(clnt, pseudoflavor);
	if (!auth)
		return NULL;
	atomic_set(&auth->au_count, 1);
	if (clnt->cl_auth)
		rpcauth_destroy(clnt->cl_auth);
	clnt->cl_auth = auth;
	return auth;
}

void
rpcauth_destroy(struct rpc_auth *auth)
{
	if (!atomic_dec_and_test(&auth->au_count))
		return;
	auth->au_ops->destroy(auth);
	module_put(auth->au_ops->owner);
	kfree(auth);
}

static DEFINE_SPINLOCK(rpc_credcache_lock);

/*
 * Initialize RPC credential cache
 */
void
rpcauth_init_credcache(struct rpc_auth *auth)
{
	int i;
	for (i = 0; i < RPC_CREDCACHE_NR; i++)
		INIT_LIST_HEAD(&auth->au_credcache[i]);
	auth->au_nextgc = jiffies + (auth->au_expire >> 1);
}

/*
 * Destroy an unreferenced credential
 */
static inline void
rpcauth_crdestroy(struct rpc_cred *cred)
{
#ifdef RPC_DEBUG
	BUG_ON(cred->cr_magic != RPCAUTH_CRED_MAGIC ||
			atomic_read(&cred->cr_count) ||
			!list_empty(&cred->cr_hash));
	cred->cr_magic = 0;
#endif
	cred->cr_ops->crdestroy(cred);
}

/*
 * Destroy a list of credentials
 */
static inline
void rpcauth_destroy_credlist(struct list_head *head)
{
	struct rpc_cred *cred;

	while (!list_empty(head)) {
		cred = list_entry(head->next, struct rpc_cred, cr_hash);
		list_del_init(&cred->cr_hash);
		rpcauth_crdestroy(cred);
	}
}

/*
 * Clear the RPC credential cache, and delete those credentials
 * that are not referenced.
 */
void
rpcauth_free_credcache(struct rpc_auth *auth)
{
	LIST_HEAD(free);
	struct list_head *pos, *next;
	struct rpc_cred	*cred;
	int		i;

	spin_lock(&rpc_credcache_lock);
	for (i = 0; i < RPC_CREDCACHE_NR; i++) {
		list_for_each_safe(pos, next, &auth->au_credcache[i]) {
			cred = list_entry(pos, struct rpc_cred, cr_hash);
			cred->cr_auth = NULL;
			list_del_init(&cred->cr_hash);
			if (atomic_read(&cred->cr_count) == 0)
				list_add(&cred->cr_hash, &free);
		}
	}
	spin_unlock(&rpc_credcache_lock);
	rpcauth_destroy_credlist(&free);
}

static inline int
rpcauth_prune_expired(struct rpc_cred *cred, struct list_head *free)
{
	if (atomic_read(&cred->cr_count) != 0)
	       return 0;
	if (time_before(jiffies, cred->cr_expire))
		return 0;
	cred->cr_auth = NULL;
	list_del(&cred->cr_hash);
	list_add(&cred->cr_hash, free);
	return 1;
}

/*
 * Remove stale credentials. Avoid sleeping inside the loop.
 */
static void
rpcauth_gc_credcache(struct rpc_auth *auth, struct list_head *free)
{
	struct list_head *pos, *next;
	struct rpc_cred	*cred;
	int		i;

	dprintk("RPC: gc'ing RPC credentials for auth %p\n", auth);
	for (i = 0; i < RPC_CREDCACHE_NR; i++) {
		list_for_each_safe(pos, next, &auth->au_credcache[i]) {
			cred = list_entry(pos, struct rpc_cred, cr_hash);
			rpcauth_prune_expired(cred, free);
		}
	}
	auth->au_nextgc = jiffies + auth->au_expire;
}

/*
 * Look up a process' credentials in the authentication cache
 */
struct rpc_cred *
rpcauth_lookup_credcache(struct rpc_auth *auth, struct auth_cred * acred,
		int taskflags)
{
	LIST_HEAD(free);
	struct list_head *pos, *next;
	struct rpc_cred	*new = NULL,
			*cred = NULL;
	int		nr = 0;

	if (!(taskflags & RPC_TASK_ROOTCREDS))
		nr = acred->uid & RPC_CREDCACHE_MASK;
retry:
	spin_lock(&rpc_credcache_lock);
	if (time_before(auth->au_nextgc, jiffies))
		rpcauth_gc_credcache(auth, &free);
	list_for_each_safe(pos, next, &auth->au_credcache[nr]) {
		struct rpc_cred *entry;
	       	entry = list_entry(pos, struct rpc_cred, cr_hash);
		if (rpcauth_prune_expired(entry, &free))
			continue;
		if (entry->cr_ops->crmatch(acred, entry, taskflags)) {
			list_del(&entry->cr_hash);
			cred = entry;
			break;
		}
	}
	if (new) {
		if (cred)
			list_add(&new->cr_hash, &free);
		else
			cred = new;
	}
	if (cred) {
		list_add(&cred->cr_hash, &auth->au_credcache[nr]);
		cred->cr_auth = auth;
		get_rpccred(cred);
	}
	spin_unlock(&rpc_credcache_lock);

	rpcauth_destroy_credlist(&free);

	if (!cred) {
		new = auth->au_ops->crcreate(auth, acred, taskflags);
		if (new) {
#ifdef RPC_DEBUG
			new->cr_magic = RPCAUTH_CRED_MAGIC;
#endif
			goto retry;
		}
	}

	return (struct rpc_cred *) cred;
}

struct rpc_cred *
rpcauth_lookupcred(struct rpc_auth *auth, int taskflags)
{
	struct auth_cred acred;
	struct rpc_cred *ret;

	get_group_info(current->group_info);
	acred.uid = current->fsuid;
	acred.gid = current->fsgid;
	acred.group_info = current->group_info;

	dprintk("RPC:     looking up %s cred\n",
		auth->au_ops->au_name);
	ret = rpcauth_lookup_credcache(auth, &acred, taskflags);
	put_group_info(current->group_info);
	return ret;
}

struct rpc_cred *
rpcauth_bindcred(struct rpc_task *task)
{
	struct rpc_auth *auth = task->tk_auth;
	struct auth_cred acred;
	struct rpc_cred *ret;

	get_group_info(current->group_info);
	acred.uid = current->fsuid;
	acred.gid = current->fsgid;
	acred.group_info = current->group_info;

	dprintk("RPC: %4d looking up %s cred\n",
		task->tk_pid, task->tk_auth->au_ops->au_name);
	task->tk_msg.rpc_cred = rpcauth_lookup_credcache(auth, &acred, task->tk_flags);
	if (task->tk_msg.rpc_cred == 0)
		task->tk_status = -ENOMEM;
	ret = task->tk_msg.rpc_cred;
	put_group_info(current->group_info);
	return ret;
}

void
rpcauth_holdcred(struct rpc_task *task)
{
	dprintk("RPC: %4d holding %s cred %p\n",
		task->tk_pid, task->tk_auth->au_ops->au_name, task->tk_msg.rpc_cred);
	if (task->tk_msg.rpc_cred)
		get_rpccred(task->tk_msg.rpc_cred);
}

void
put_rpccred(struct rpc_cred *cred)
{
	if (!atomic_dec_and_lock(&cred->cr_count, &rpc_credcache_lock))
		return;

	if (list_empty(&cred->cr_hash)) {
		spin_unlock(&rpc_credcache_lock);
		rpcauth_crdestroy(cred);
		return;
	}
	cred->cr_expire = jiffies + cred->cr_auth->au_expire;
	spin_unlock(&rpc_credcache_lock);
}

void
rpcauth_unbindcred(struct rpc_task *task)
{
	struct rpc_auth	*auth = task->tk_auth;
	struct rpc_cred	*cred = task->tk_msg.rpc_cred;

	dprintk("RPC: %4d releasing %s cred %p\n",
		task->tk_pid, auth->au_ops->au_name, cred);

	put_rpccred(cred);
	task->tk_msg.rpc_cred = NULL;
}

u32 *
rpcauth_marshcred(struct rpc_task *task, u32 *p)
{
	struct rpc_auth	*auth = task->tk_auth;
	struct rpc_cred	*cred = task->tk_msg.rpc_cred;

	dprintk("RPC: %4d marshaling %s cred %p\n",
		task->tk_pid, auth->au_ops->au_name, cred);
	return cred->cr_ops->crmarshal(task, p,
				task->tk_flags & RPC_CALL_REALUID);
}

u32 *
rpcauth_checkverf(struct rpc_task *task, u32 *p)
{
	struct rpc_auth	*auth = task->tk_auth;
	struct rpc_cred	*cred = task->tk_msg.rpc_cred;

	dprintk("RPC: %4d validating %s cred %p\n",
		task->tk_pid, auth->au_ops->au_name, cred);
	return cred->cr_ops->crvalidate(task, p);
}

int
rpcauth_wrap_req(struct rpc_task *task, kxdrproc_t encode, void *rqstp,
		u32 *data, void *obj)
{
	struct rpc_cred *cred = task->tk_msg.rpc_cred;

	dprintk("RPC: %4d using %s cred %p to wrap rpc data\n",
			task->tk_pid, cred->cr_auth->au_ops->au_name, cred);
	if (cred->cr_ops->crwrap_req)
		return cred->cr_ops->crwrap_req(task, encode, rqstp, data, obj);
	/* By default, we encode the arguments normally. */
	return encode(rqstp, data, obj);
}

int
rpcauth_unwrap_resp(struct rpc_task *task, kxdrproc_t decode, void *rqstp,
		u32 *data, void *obj)
{
	struct rpc_cred *cred = task->tk_msg.rpc_cred;

	dprintk("RPC: %4d using %s cred %p to unwrap rpc data\n",
			task->tk_pid, cred->cr_auth->au_ops->au_name, cred);
	if (cred->cr_ops->crunwrap_resp)
		return cred->cr_ops->crunwrap_resp(task, decode, rqstp,
						   data, obj);
	/* By default, we decode the arguments normally. */
	return decode(rqstp, data, obj);
}

int
rpcauth_refreshcred(struct rpc_task *task)
{
	struct rpc_auth	*auth = task->tk_auth;
	struct rpc_cred	*cred = task->tk_msg.rpc_cred;

	dprintk("RPC: %4d refreshing %s cred %p\n",
		task->tk_pid, auth->au_ops->au_name, cred);
	task->tk_status = cred->cr_ops->crrefresh(task);
	return task->tk_status;
}

void
rpcauth_invalcred(struct rpc_task *task)
{
	dprintk("RPC: %4d invalidating %s cred %p\n",
		task->tk_pid, task->tk_auth->au_ops->au_name, task->tk_msg.rpc_cred);
	spin_lock(&rpc_credcache_lock);
	if (task->tk_msg.rpc_cred)
		task->tk_msg.rpc_cred->cr_flags &= ~RPCAUTH_CRED_UPTODATE;
	spin_unlock(&rpc_credcache_lock);
}

int
rpcauth_uptodatecred(struct rpc_task *task)
{
	return !(task->tk_msg.rpc_cred) ||
		(task->tk_msg.rpc_cred->cr_flags & RPCAUTH_CRED_UPTODATE);
}
