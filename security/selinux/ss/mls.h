/*
 * Multi-level security (MLS) policy operations.
 *
 * Author : Stephen Smalley, <sds@epoch.ncsc.mil>
 */
#ifndef _SS_MLS_H_
#define _SS_MLS_H_

#include "context.h"
#include "policydb.h"

#ifdef CONFIG_SECURITY_SELINUX_MLS

void mls_compute_av(struct context *scontext,
		    struct context *tcontext,
		    struct class_datum *tclass,
		    u32 *allowed);

int mls_compute_context_len(struct context *context);
int mls_sid_to_context(struct context *context, char **scontext);
int mls_context_isvalid(struct policydb *p, struct context *c);

int mls_context_to_sid(char oldc,
	               char **scontext,
		       struct context *context);

int mls_convert_context(struct policydb *oldp,
			struct policydb *newp,
			struct context *context);

int mls_compute_sid(struct context *scontext,
		    struct context *tcontext,
		    u16 tclass,
		    u32 specified,
		    struct context *newcontext);

int sens_index(void *key, void *datum, void *datap);
int cat_index(void *key, void *datum, void *datap);
int sens_destroy(void *key, void *datum, void *p);
int cat_destroy(void *key, void *datum, void *p);
int sens_read(struct policydb *p, struct hashtab *h, void *fp);
int cat_read(struct policydb *p, struct hashtab *h, void *fp);

#define mls_for_user_ranges(user, usercon) { \
struct mls_range_list *__ranges; \
for (__ranges = user->ranges; __ranges; __ranges = __ranges->next) { \
usercon.range = __ranges->range;

#define mls_end_user_ranges } }

#define mls_symtab_names  "levels", "categories",
#define mls_symtab_sizes  16, 16,
#define mls_index_f sens_index, cat_index,
#define mls_destroy_f sens_destroy, cat_destroy,
#define mls_read_f sens_read, cat_read,
#define mls_write_f sens_write, cat_write,
#define mls_policydb_index_others(p) printk(", %d levels", p->nlevels);

#define mls_set_config(config) config |= POLICYDB_CONFIG_MLS

void mls_user_destroy(struct user_datum *usrdatum);
int mls_read_range(struct context *c, void *fp);
int mls_read_perm(struct perm_datum *perdatum, void *fp);
int mls_read_class(struct class_datum *cladatum,  void *fp);
int mls_read_user(struct user_datum *usrdatum, void *fp);
int mls_read_nlevels(struct policydb *p, void *fp);
int mls_read_trusted(struct policydb *p, void *fp);

#else

#define	mls_compute_av(scontext, tcontext, tclass_datum, allowed)
#define mls_compute_context_len(context) 0
#define	mls_sid_to_context(context, scontextpp)
#define mls_context_isvalid(p, c) 1
#define	mls_context_to_sid(oldc, context_str, context) 0
#define mls_convert_context(oldp, newp, c) 0
#define mls_compute_sid(scontext, tcontext, tclass, specified, newcontextp) 0
#define mls_for_user_ranges(user, usercon)
#define mls_end_user_ranges
#define mls_symtab_names
#define mls_symtab_sizes
#define mls_index_f
#define mls_destroy_f
#define mls_read_f
#define mls_write_f
#define mls_policydb_index_others(p)
#define mls_set_config(config)
#define mls_user_destroy(usrdatum)
#define mls_read_range(c, fp) 0
#define mls_read_perm(p, fp) 0
#define mls_read_class(c, fp) 0
#define mls_read_user(u, fp) 0
#define mls_read_nlevels(p, fp) 0
#define mls_read_trusted(p, fp) 0

#endif

#endif	/* _SS_MLS_H */

