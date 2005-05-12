/*
 * Implementation of the policy database.
 *
 * Author : Stephen Smalley, <sds@epoch.ncsc.mil>
 */

/* Updated: Frank Mayer <mayerf@tresys.com> and Karl MacMillan <kmacmillan@tresys.com>
 *
 * 	Added conditional policy language extensions
 *
 * Copyright (C) 2003 - 2004 Tresys Technology, LLC
 *	This program is free software; you can redistribute it and/or modify
 *  	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, version 2.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include "security.h"

#include "policydb.h"
#include "conditional.h"
#include "mls.h"

#define _DEBUG_HASHES

#ifdef DEBUG_HASHES
static char *symtab_name[SYM_NUM] = {
	"common prefixes",
	"classes",
	"roles",
	"types",
	"users",
	mls_symtab_names
	"bools"
};
#endif

static unsigned int symtab_sizes[SYM_NUM] = {
	2,
	32,
	16,
	512,
	128,
	mls_symtab_sizes
	16
};

struct policydb_compat_info {
	int version;
	int sym_num;
	int ocon_num;
};

/* These need to be updated if SYM_NUM or OCON_NUM changes */
static struct policydb_compat_info policydb_compat[] = {
	{
		.version        = POLICYDB_VERSION_BASE,
		.sym_num        = SYM_NUM - 1,
		.ocon_num       = OCON_NUM - 1,
	},
	{
		.version        = POLICYDB_VERSION_BOOL,
		.sym_num        = SYM_NUM,
		.ocon_num       = OCON_NUM - 1,
	},
	{
		.version        = POLICYDB_VERSION_IPV6,
		.sym_num        = SYM_NUM,
		.ocon_num       = OCON_NUM,
	},
	{
		.version        = POLICYDB_VERSION_NLCLASS,
		.sym_num        = SYM_NUM,
		.ocon_num       = OCON_NUM,
	},
};

static struct policydb_compat_info *policydb_lookup_compat(int version)
{
	int i;
	struct policydb_compat_info *info = NULL;

	for (i = 0; i < sizeof(policydb_compat)/sizeof(*info); i++) {
		if (policydb_compat[i].version == version) {
			info = &policydb_compat[i];
			break;
		}
	}
	return info;
}

/*
 * Initialize the role table.
 */
int roles_init(struct policydb *p)
{
	char *key = NULL;
	int rc;
	struct role_datum *role;

	role = kmalloc(sizeof(*role), GFP_KERNEL);
	if (!role) {
		rc = -ENOMEM;
		goto out;
	}
	memset(role, 0, sizeof(*role));
	role->value = ++p->p_roles.nprim;
	if (role->value != OBJECT_R_VAL) {
		rc = -EINVAL;
		goto out_free_role;
	}
	key = kmalloc(strlen(OBJECT_R)+1,GFP_KERNEL);
	if (!key) {
		rc = -ENOMEM;
		goto out_free_role;
	}
	strcpy(key, OBJECT_R);
	rc = hashtab_insert(p->p_roles.table, key, role);
	if (rc)
		goto out_free_key;
out:
	return rc;

out_free_key:
	kfree(key);
out_free_role:
	kfree(role);
	goto out;
}

/*
 * Initialize a policy database structure.
 */
int policydb_init(struct policydb *p)
{
	int i, rc;

	memset(p, 0, sizeof(*p));

	for (i = 0; i < SYM_NUM; i++) {
		rc = symtab_init(&p->symtab[i], symtab_sizes[i]);
		if (rc)
			goto out_free_symtab;
	}

	rc = avtab_init(&p->te_avtab);
	if (rc)
		goto out_free_symtab;

	rc = roles_init(p);
	if (rc)
		goto out_free_avtab;

	rc = cond_policydb_init(p);
	if (rc)
		goto out_free_avtab;

out:
	return rc;

out_free_avtab:
	avtab_destroy(&p->te_avtab);

out_free_symtab:
	for (i = 0; i < SYM_NUM; i++)
		hashtab_destroy(p->symtab[i].table);
	goto out;
}

/*
 * The following *_index functions are used to
 * define the val_to_name and val_to_struct arrays
 * in a policy database structure.  The val_to_name
 * arrays are used when converting security context
 * structures into string representations.  The
 * val_to_struct arrays are used when the attributes
 * of a class, role, or user are needed.
 */

static int common_index(void *key, void *datum, void *datap)
{
	struct policydb *p;
	struct common_datum *comdatum;

	comdatum = datum;
	p = datap;
	if (!comdatum->value || comdatum->value > p->p_commons.nprim)
		return -EINVAL;
	p->p_common_val_to_name[comdatum->value - 1] = key;
	return 0;
}

static int class_index(void *key, void *datum, void *datap)
{
	struct policydb *p;
	struct class_datum *cladatum;

	cladatum = datum;
	p = datap;
	if (!cladatum->value || cladatum->value > p->p_classes.nprim)
		return -EINVAL;
	p->p_class_val_to_name[cladatum->value - 1] = key;
	p->class_val_to_struct[cladatum->value - 1] = cladatum;
	return 0;
}

static int role_index(void *key, void *datum, void *datap)
{
	struct policydb *p;
	struct role_datum *role;

	role = datum;
	p = datap;
	if (!role->value || role->value > p->p_roles.nprim)
		return -EINVAL;
	p->p_role_val_to_name[role->value - 1] = key;
	p->role_val_to_struct[role->value - 1] = role;
	return 0;
}

static int type_index(void *key, void *datum, void *datap)
{
	struct policydb *p;
	struct type_datum *typdatum;

	typdatum = datum;
	p = datap;

	if (typdatum->primary) {
		if (!typdatum->value || typdatum->value > p->p_types.nprim)
			return -EINVAL;
		p->p_type_val_to_name[typdatum->value - 1] = key;
	}

	return 0;
}

static int user_index(void *key, void *datum, void *datap)
{
	struct policydb *p;
	struct user_datum *usrdatum;

	usrdatum = datum;
	p = datap;
	if (!usrdatum->value || usrdatum->value > p->p_users.nprim)
		return -EINVAL;
	p->p_user_val_to_name[usrdatum->value - 1] = key;
	p->user_val_to_struct[usrdatum->value - 1] = usrdatum;
	return 0;
}

static int (*index_f[SYM_NUM]) (void *key, void *datum, void *datap) =
{
	common_index,
	class_index,
	role_index,
	type_index,
	user_index,
	mls_index_f
	cond_index_bool
};

/*
 * Define the common val_to_name array and the class
 * val_to_name and val_to_struct arrays in a policy
 * database structure.
 *
 * Caller must clean up upon failure.
 */
int policydb_index_classes(struct policydb *p)
{
	int rc;

	p->p_common_val_to_name =
		kmalloc(p->p_commons.nprim * sizeof(char *), GFP_KERNEL);
	if (!p->p_common_val_to_name) {
		rc = -ENOMEM;
		goto out;
	}

	rc = hashtab_map(p->p_commons.table, common_index, p);
	if (rc)
		goto out;

	p->class_val_to_struct =
		kmalloc(p->p_classes.nprim * sizeof(*(p->class_val_to_struct)), GFP_KERNEL);
	if (!p->class_val_to_struct) {
		rc = -ENOMEM;
		goto out;
	}

	p->p_class_val_to_name =
		kmalloc(p->p_classes.nprim * sizeof(char *), GFP_KERNEL);
	if (!p->p_class_val_to_name) {
		rc = -ENOMEM;
		goto out;
	}

	rc = hashtab_map(p->p_classes.table, class_index, p);
out:
	return rc;
}

#ifdef DEBUG_HASHES
static void symtab_hash_eval(struct symtab *s)
{
	int i;

	for (i = 0; i < SYM_NUM; i++) {
		struct hashtab *h = s[i].table;
		struct hashtab_info info;

		hashtab_stat(h, &info);
		printk(KERN_INFO "%s:  %d entries and %d/%d buckets used, "
		       "longest chain length %d\n", symtab_name[i], h->nel,
		       info.slots_used, h->size, info.max_chain_len);
	}
}
#endif

/*
 * Define the other val_to_name and val_to_struct arrays
 * in a policy database structure.
 *
 * Caller must clean up on failure.
 */
int policydb_index_others(struct policydb *p)
{
	int i, rc = 0;

	printk(KERN_INFO "security:  %d users, %d roles, %d types, %d bools",
	       p->p_users.nprim, p->p_roles.nprim, p->p_types.nprim, p->p_bools.nprim);
	mls_policydb_index_others(p);
	printk("\n");

	printk(KERN_INFO "security:  %d classes, %d rules\n",
	       p->p_classes.nprim, p->te_avtab.nel);

#ifdef DEBUG_HASHES
	avtab_hash_eval(&p->te_avtab, "rules");
	symtab_hash_eval(p->symtab);
#endif

	p->role_val_to_struct =
		kmalloc(p->p_roles.nprim * sizeof(*(p->role_val_to_struct)),
		        GFP_KERNEL);
	if (!p->role_val_to_struct) {
		rc = -ENOMEM;
		goto out;
	}

	p->user_val_to_struct =
		kmalloc(p->p_users.nprim * sizeof(*(p->user_val_to_struct)),
		        GFP_KERNEL);
	if (!p->user_val_to_struct) {
		rc = -ENOMEM;
		goto out;
	}

	if (cond_init_bool_indexes(p)) {
		rc = -ENOMEM;
		goto out;
	}

	for (i = SYM_ROLES; i < SYM_NUM; i++) {
		p->sym_val_to_name[i] =
			kmalloc(p->symtab[i].nprim * sizeof(char *), GFP_KERNEL);
		if (!p->sym_val_to_name[i]) {
			rc = -ENOMEM;
			goto out;
		}
		rc = hashtab_map(p->symtab[i].table, index_f[i], p);
		if (rc)
			goto out;
	}

out:
	return rc;
}

/*
 * The following *_destroy functions are used to
 * free any memory allocated for each kind of
 * symbol data in the policy database.
 */

static int perm_destroy(void *key, void *datum, void *p)
{
	kfree(key);
	kfree(datum);
	return 0;
}

static int common_destroy(void *key, void *datum, void *p)
{
	struct common_datum *comdatum;

	kfree(key);
	comdatum = datum;
	hashtab_map(comdatum->permissions.table, perm_destroy, NULL);
	hashtab_destroy(comdatum->permissions.table);
	kfree(datum);
	return 0;
}

static int class_destroy(void *key, void *datum, void *p)
{
	struct class_datum *cladatum;
	struct constraint_node *constraint, *ctemp;
	struct constraint_expr *e, *etmp;

	kfree(key);
	cladatum = datum;
	hashtab_map(cladatum->permissions.table, perm_destroy, NULL);
	hashtab_destroy(cladatum->permissions.table);
	constraint = cladatum->constraints;
	while (constraint) {
		e = constraint->expr;
		while (e) {
			ebitmap_destroy(&e->names);
			etmp = e;
			e = e->next;
			kfree(etmp);
		}
		ctemp = constraint;
		constraint = constraint->next;
		kfree(ctemp);
	}
	kfree(cladatum->comkey);
	kfree(datum);
	return 0;
}

static int role_destroy(void *key, void *datum, void *p)
{
	struct role_datum *role;

	kfree(key);
	role = datum;
	ebitmap_destroy(&role->dominates);
	ebitmap_destroy(&role->types);
	kfree(datum);
	return 0;
}

static int type_destroy(void *key, void *datum, void *p)
{
	kfree(key);
	kfree(datum);
	return 0;
}

static int user_destroy(void *key, void *datum, void *p)
{
	struct user_datum *usrdatum;

	kfree(key);
	usrdatum = datum;
	ebitmap_destroy(&usrdatum->roles);
	mls_user_destroy(usrdatum);
	kfree(datum);
	return 0;
}

static int (*destroy_f[SYM_NUM]) (void *key, void *datum, void *datap) =
{
	common_destroy,
	class_destroy,
	role_destroy,
	type_destroy,
	user_destroy,
	mls_destroy_f
	cond_destroy_bool
};

void ocontext_destroy(struct ocontext *c, int i)
{
	context_destroy(&c->context[0]);
	context_destroy(&c->context[1]);
	if (i == OCON_ISID || i == OCON_FS ||
	    i == OCON_NETIF || i == OCON_FSUSE)
		kfree(c->u.name);
	kfree(c);
}

/*
 * Free any memory allocated by a policy database structure.
 */
void policydb_destroy(struct policydb *p)
{
	struct ocontext *c, *ctmp;
	struct genfs *g, *gtmp;
	int i;

	for (i = 0; i < SYM_NUM; i++) {
		hashtab_map(p->symtab[i].table, destroy_f[i], NULL);
		hashtab_destroy(p->symtab[i].table);
	}

	for (i = 0; i < SYM_NUM; i++) {
		if (p->sym_val_to_name[i])
			kfree(p->sym_val_to_name[i]);
	}

	if (p->class_val_to_struct)
		kfree(p->class_val_to_struct);
	if (p->role_val_to_struct)
		kfree(p->role_val_to_struct);
	if (p->user_val_to_struct)
		kfree(p->user_val_to_struct);

	avtab_destroy(&p->te_avtab);

	for (i = 0; i < OCON_NUM; i++) {
		c = p->ocontexts[i];
		while (c) {
			ctmp = c;
			c = c->next;
			ocontext_destroy(ctmp,i);
		}
	}

	g = p->genfs;
	while (g) {
		kfree(g->fstype);
		c = g->head;
		while (c) {
			ctmp = c;
			c = c->next;
			ocontext_destroy(ctmp,OCON_FSUSE);
		}
		gtmp = g;
		g = g->next;
		kfree(gtmp);
	}

	cond_policydb_destroy(p);

	return;
}

/*
 * Load the initial SIDs specified in a policy database
 * structure into a SID table.
 */
int policydb_load_isids(struct policydb *p, struct sidtab *s)
{
	struct ocontext *head, *c;
	int rc;

	rc = sidtab_init(s);
	if (rc) {
		printk(KERN_ERR "security:  out of memory on SID table init\n");
		goto out;
	}

	head = p->ocontexts[OCON_ISID];
	for (c = head; c; c = c->next) {
		if (!c->context[0].user) {
			printk(KERN_ERR "security:  SID %s was never "
			       "defined.\n", c->u.name);
			rc = -EINVAL;
			goto out;
		}
		if (sidtab_insert(s, c->sid[0], &c->context[0])) {
			printk(KERN_ERR "security:  unable to load initial "
			       "SID %s.\n", c->u.name);
			rc = -EINVAL;
			goto out;
		}
	}
out:
	return rc;
}

/*
 * Return 1 if the fields in the security context
 * structure `c' are valid.  Return 0 otherwise.
 */
int policydb_context_isvalid(struct policydb *p, struct context *c)
{
	struct role_datum *role;
	struct user_datum *usrdatum;

	if (!c->role || c->role > p->p_roles.nprim)
		return 0;

	if (!c->user || c->user > p->p_users.nprim)
		return 0;

	if (!c->type || c->type > p->p_types.nprim)
		return 0;

	if (c->role != OBJECT_R_VAL) {
		/*
		 * Role must be authorized for the type.
		 */
		role = p->role_val_to_struct[c->role - 1];
		if (!ebitmap_get_bit(&role->types,
				     c->type - 1))
			/* role may not be associated with type */
			return 0;

		/*
		 * User must be authorized for the role.
		 */
		usrdatum = p->user_val_to_struct[c->user - 1];
		if (!usrdatum)
			return 0;

		if (!ebitmap_get_bit(&usrdatum->roles,
				     c->role - 1))
			/* user may not be associated with role */
			return 0;
	}

	if (!mls_context_isvalid(p, c))
		return 0;

	return 1;
}

/*
 * Read and validate a security context structure
 * from a policydb binary representation file.
 */
static int context_read_and_validate(struct context *c,
				     struct policydb *p,
				     void *fp)
{
	u32 buf[3];
	int rc;

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0) {
		printk(KERN_ERR "security: context truncated\n");
		goto out;
	}
	c->user = le32_to_cpu(buf[0]);
	c->role = le32_to_cpu(buf[1]);
	c->type = le32_to_cpu(buf[2]);
	if (mls_read_range(c, fp)) {
		printk(KERN_ERR "security: error reading MLS range of "
		       "context\n");
		rc = -EINVAL;
		goto out;
	}

	if (!policydb_context_isvalid(p, c)) {
		printk(KERN_ERR "security:  invalid security context\n");
		context_destroy(c);
		rc = -EINVAL;
	}
out:
	return rc;
}

/*
 * The following *_read functions are used to
 * read the symbol data from a policy database
 * binary representation file.
 */

static int perm_read(struct policydb *p, struct hashtab *h, void *fp)
{
	char *key = NULL;
	struct perm_datum *perdatum;
	int rc;
	u32 buf[2], len;

	perdatum = kmalloc(sizeof(*perdatum), GFP_KERNEL);
	if (!perdatum) {
		rc = -ENOMEM;
		goto out;
	}
	memset(perdatum, 0, sizeof(*perdatum));

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0)
		goto bad;

	len = le32_to_cpu(buf[0]);
	perdatum->value = le32_to_cpu(buf[1]);
	rc = mls_read_perm(perdatum, fp);
	if (rc)
		goto bad;

	key = kmalloc(len + 1,GFP_KERNEL);
	if (!key) {
		rc = -ENOMEM;
		goto bad;
	}
	rc = next_entry(key, fp, len);
	if (rc < 0)
		goto bad;
	key[len] = 0;

	rc = hashtab_insert(h, key, perdatum);
	if (rc)
		goto bad;
out:
	return rc;
bad:
	perm_destroy(key, perdatum, NULL);
	goto out;
}

static int common_read(struct policydb *p, struct hashtab *h, void *fp)
{
	char *key = NULL;
	struct common_datum *comdatum;
	u32 buf[4], len, nel;
	int i, rc;

	comdatum = kmalloc(sizeof(*comdatum), GFP_KERNEL);
	if (!comdatum) {
		rc = -ENOMEM;
		goto out;
	}
	memset(comdatum, 0, sizeof(*comdatum));

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0)
		goto bad;

	len = le32_to_cpu(buf[0]);
	comdatum->value = le32_to_cpu(buf[1]);

	rc = symtab_init(&comdatum->permissions, PERM_SYMTAB_SIZE);
	if (rc)
		goto bad;
	comdatum->permissions.nprim = le32_to_cpu(buf[2]);
	nel = le32_to_cpu(buf[3]);

	key = kmalloc(len + 1,GFP_KERNEL);
	if (!key) {
		rc = -ENOMEM;
		goto bad;
	}
	rc = next_entry(key, fp, len);
	if (rc < 0)
		goto bad;
	key[len] = 0;

	for (i = 0; i < nel; i++) {
		rc = perm_read(p, comdatum->permissions.table, fp);
		if (rc)
			goto bad;
	}

	rc = hashtab_insert(h, key, comdatum);
	if (rc)
		goto bad;
out:
	return rc;
bad:
	common_destroy(key, comdatum, NULL);
	goto out;
}

static int class_read(struct policydb *p, struct hashtab *h, void *fp)
{
	char *key = NULL;
	struct class_datum *cladatum;
	struct constraint_node *c, *lc;
	struct constraint_expr *e, *le;
	u32 buf[6], len, len2, ncons, nexpr, nel;
	int i, j, depth, rc;

	cladatum = kmalloc(sizeof(*cladatum), GFP_KERNEL);
	if (!cladatum) {
		rc = -ENOMEM;
		goto out;
	}
	memset(cladatum, 0, sizeof(*cladatum));

	rc = next_entry(buf, fp, sizeof(u32)*6);
	if (rc < 0)
		goto bad;

	len = le32_to_cpu(buf[0]);
	len2 = le32_to_cpu(buf[1]);
	cladatum->value = le32_to_cpu(buf[2]);

	rc = symtab_init(&cladatum->permissions, PERM_SYMTAB_SIZE);
	if (rc)
		goto bad;
	cladatum->permissions.nprim = le32_to_cpu(buf[3]);
	nel = le32_to_cpu(buf[4]);

	ncons = le32_to_cpu(buf[5]);

	key = kmalloc(len + 1,GFP_KERNEL);
	if (!key) {
		rc = -ENOMEM;
		goto bad;
	}
	rc = next_entry(key, fp, len);
	if (rc < 0)
		goto bad;
	key[len] = 0;

	if (len2) {
		cladatum->comkey = kmalloc(len2 + 1,GFP_KERNEL);
		if (!cladatum->comkey) {
			rc = -ENOMEM;
			goto bad;
		}
		rc = next_entry(cladatum->comkey, fp, len2);
		if (rc < 0)
			goto bad;
		cladatum->comkey[len2] = 0;

		cladatum->comdatum = hashtab_search(p->p_commons.table,
						    cladatum->comkey);
		if (!cladatum->comdatum) {
			printk(KERN_ERR "security:  unknown common %s\n",
			       cladatum->comkey);
			rc = -EINVAL;
			goto bad;
		}
	}
	for (i = 0; i < nel; i++) {
		rc = perm_read(p, cladatum->permissions.table, fp);
		if (rc)
			goto bad;
	}

	lc = NULL;
	for (i = 0; i < ncons; i++) {
		c = kmalloc(sizeof(*c), GFP_KERNEL);
		if (!c) {
			rc = -ENOMEM;
			goto bad;
		}
		memset(c, 0, sizeof(*c));

		if (lc) {
			lc->next = c;
		} else {
			cladatum->constraints = c;
		}

		rc = next_entry(buf, fp, sizeof(u32)*2);
		if (rc < 0)
			goto bad;
		c->permissions = le32_to_cpu(buf[0]);
		nexpr = le32_to_cpu(buf[1]);
		le = NULL;
		depth = -1;
		for (j = 0; j < nexpr; j++) {
			e = kmalloc(sizeof(*e), GFP_KERNEL);
			if (!e) {
				rc = -ENOMEM;
				goto bad;
			}
			memset(e, 0, sizeof(*e));

			if (le) {
				le->next = e;
			} else {
				c->expr = e;
			}

			rc = next_entry(buf, fp, sizeof(u32)*3);
			if (rc < 0)
				goto bad;
			e->expr_type = le32_to_cpu(buf[0]);
			e->attr = le32_to_cpu(buf[1]);
			e->op = le32_to_cpu(buf[2]);

			rc = -EINVAL;
			switch (e->expr_type) {
			case CEXPR_NOT:
				if (depth < 0)
					goto bad;
				break;
			case CEXPR_AND:
			case CEXPR_OR:
				if (depth < 1)
					goto bad;
				depth--;
				break;
			case CEXPR_ATTR:
				if (depth == (CEXPR_MAXDEPTH-1))
					goto bad;
				depth++;
				break;
			case CEXPR_NAMES:
				if (depth == (CEXPR_MAXDEPTH-1))
					goto bad;
				depth++;
				if (ebitmap_read(&e->names, fp))
					goto bad;
				break;
			default:
				goto bad;
			}
			le = e;
		}
		if (depth != 0)
			goto bad;
		lc = c;
	}

	rc = mls_read_class(cladatum, fp);
	if (rc)
		goto bad;

	rc = hashtab_insert(h, key, cladatum);
	if (rc)
		goto bad;

	rc = 0;
out:
	return rc;
bad:
	class_destroy(key, cladatum, NULL);
	goto out;
}

static int role_read(struct policydb *p, struct hashtab *h, void *fp)
{
	char *key = NULL;
	struct role_datum *role;
	int rc;
	u32 buf[2], len;

	role = kmalloc(sizeof(*role), GFP_KERNEL);
	if (!role) {
		rc = -ENOMEM;
		goto out;
	}
	memset(role, 0, sizeof(*role));

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0)
		goto bad;

	len = le32_to_cpu(buf[0]);
	role->value = le32_to_cpu(buf[1]);

	key = kmalloc(len + 1,GFP_KERNEL);
	if (!key) {
		rc = -ENOMEM;
		goto bad;
	}
	rc = next_entry(key, fp, len);
	if (rc < 0)
		goto bad;
	key[len] = 0;

	rc = ebitmap_read(&role->dominates, fp);
	if (rc)
		goto bad;

	rc = ebitmap_read(&role->types, fp);
	if (rc)
		goto bad;

	if (strcmp(key, OBJECT_R) == 0) {
		if (role->value != OBJECT_R_VAL) {
			printk(KERN_ERR "Role %s has wrong value %d\n",
			       OBJECT_R, role->value);
			rc = -EINVAL;
			goto bad;
		}
		rc = 0;
		goto bad;
	}

	rc = hashtab_insert(h, key, role);
	if (rc)
		goto bad;
out:
	return rc;
bad:
	role_destroy(key, role, NULL);
	goto out;
}

static int type_read(struct policydb *p, struct hashtab *h, void *fp)
{
	char *key = NULL;
	struct type_datum *typdatum;
	int rc;
	u32 buf[3], len;

	typdatum = kmalloc(sizeof(*typdatum),GFP_KERNEL);
	if (!typdatum) {
		rc = -ENOMEM;
		return rc;
	}
	memset(typdatum, 0, sizeof(*typdatum));

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0)
		goto bad;

	len = le32_to_cpu(buf[0]);
	typdatum->value = le32_to_cpu(buf[1]);
	typdatum->primary = le32_to_cpu(buf[2]);

	key = kmalloc(len + 1,GFP_KERNEL);
	if (!key) {
		rc = -ENOMEM;
		goto bad;
	}
	rc = next_entry(key, fp, len);
	if (rc < 0)
		goto bad;
	key[len] = 0;

	rc = hashtab_insert(h, key, typdatum);
	if (rc)
		goto bad;
out:
	return rc;
bad:
	type_destroy(key, typdatum, NULL);
	goto out;
}

static int user_read(struct policydb *p, struct hashtab *h, void *fp)
{
	char *key = NULL;
	struct user_datum *usrdatum;
	int rc;
	u32 buf[2], len;


	usrdatum = kmalloc(sizeof(*usrdatum), GFP_KERNEL);
	if (!usrdatum) {
		rc = -ENOMEM;
		goto out;
	}
	memset(usrdatum, 0, sizeof(*usrdatum));

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0)
		goto bad;

	len = le32_to_cpu(buf[0]);
	usrdatum->value = le32_to_cpu(buf[1]);

	key = kmalloc(len + 1,GFP_KERNEL);
	if (!key) {
		rc = -ENOMEM;
		goto bad;
	}
	rc = next_entry(key, fp, len);
	if (rc < 0)
		goto bad;
	key[len] = 0;

	rc = ebitmap_read(&usrdatum->roles, fp);
	if (rc)
		goto bad;

	rc = mls_read_user(usrdatum, fp);
	if (rc)
		goto bad;

	rc = hashtab_insert(h, key, usrdatum);
	if (rc)
		goto bad;
out:
	return rc;
bad:
	user_destroy(key, usrdatum, NULL);
	goto out;
}

static int (*read_f[SYM_NUM]) (struct policydb *p, struct hashtab *h, void *fp) =
{
	common_read,
	class_read,
	role_read,
	type_read,
	user_read,
	mls_read_f
	cond_read_bool
};

#define mls_config(x) \
       ((x) & POLICYDB_CONFIG_MLS) ? "mls" : "no_mls"

/*
 * Read the configuration data from a policy database binary
 * representation file into a policy database structure.
 */
int policydb_read(struct policydb *p, void *fp)
{
	struct role_allow *ra, *lra;
	struct role_trans *tr, *ltr;
	struct ocontext *l, *c, *newc;
	struct genfs *genfs_p, *genfs, *newgenfs;
	int i, j, rc;
	u32 buf[8], len, len2, config, nprim, nel, nel2;
	char *policydb_str;
	struct policydb_compat_info *info;

	config = 0;
	mls_set_config(config);

	rc = policydb_init(p);
	if (rc)
		goto out;

	/* Read the magic number and string length. */
	rc = next_entry(buf, fp, sizeof(u32)* 2);
	if (rc < 0)
		goto bad;

	for (i = 0; i < 2; i++)
		buf[i] = le32_to_cpu(buf[i]);

	if (buf[0] != POLICYDB_MAGIC) {
		printk(KERN_ERR "security:  policydb magic number 0x%x does "
		       "not match expected magic number 0x%x\n",
		       buf[0], POLICYDB_MAGIC);
		goto bad;
	}

	len = buf[1];
	if (len != strlen(POLICYDB_STRING)) {
		printk(KERN_ERR "security:  policydb string length %d does not "
		       "match expected length %Zu\n",
		       len, strlen(POLICYDB_STRING));
		goto bad;
	}
	policydb_str = kmalloc(len + 1,GFP_KERNEL);
	if (!policydb_str) {
		printk(KERN_ERR "security:  unable to allocate memory for policydb "
		       "string of length %d\n", len);
		rc = -ENOMEM;
		goto bad;
	}
	rc = next_entry(policydb_str, fp, len);
	if (rc < 0) {
		printk(KERN_ERR "security:  truncated policydb string identifier\n");
		kfree(policydb_str);
		goto bad;
	}
	policydb_str[len] = 0;
	if (strcmp(policydb_str, POLICYDB_STRING)) {
		printk(KERN_ERR "security:  policydb string %s does not match "
		       "my string %s\n", policydb_str, POLICYDB_STRING);
		kfree(policydb_str);
		goto bad;
	}
	/* Done with policydb_str. */
	kfree(policydb_str);
	policydb_str = NULL;

	/* Read the version, config, and table sizes. */
	rc = next_entry(buf, fp, sizeof(u32)*4);
	if (rc < 0)
		goto bad;
	for (i = 0; i < 4; i++)
		buf[i] = le32_to_cpu(buf[i]);

	p->policyvers = buf[0];
	if (p->policyvers < POLICYDB_VERSION_MIN ||
	    p->policyvers > POLICYDB_VERSION_MAX) {
	    	printk(KERN_ERR "security:  policydb version %d does not match "
	    	       "my version range %d-%d\n",
	    	       buf[0], POLICYDB_VERSION_MIN, POLICYDB_VERSION_MAX);
	    	goto bad;
	}

	if (buf[1] != config) {
		printk(KERN_ERR "security:  policydb configuration (%s) does "
		       "not match my configuration (%s)\n",
		       mls_config(buf[1]),
		       mls_config(config));
		goto bad;
	}


	info = policydb_lookup_compat(p->policyvers);
	if (!info) {
		printk(KERN_ERR "security:  unable to find policy compat info "
		       "for version %d\n", p->policyvers);
		goto bad;
	}

	if (buf[2] != info->sym_num || buf[3] != info->ocon_num) {
		printk(KERN_ERR "security:  policydb table sizes (%d,%d) do "
		       "not match mine (%d,%d)\n", buf[2], buf[3],
		       info->sym_num, info->ocon_num);
		goto bad;
	}

	rc = mls_read_nlevels(p, fp);
	if (rc)
		goto bad;

	for (i = 0; i < info->sym_num; i++) {
		rc = next_entry(buf, fp, sizeof(u32)*2);
		if (rc < 0)
			goto bad;
		nprim = le32_to_cpu(buf[0]);
		nel = le32_to_cpu(buf[1]);
		for (j = 0; j < nel; j++) {
			rc = read_f[i](p, p->symtab[i].table, fp);
			if (rc)
				goto bad;
		}

		p->symtab[i].nprim = nprim;
	}

	rc = avtab_read(&p->te_avtab, fp, config);
	if (rc)
		goto bad;

	if (p->policyvers >= POLICYDB_VERSION_BOOL) {
		rc = cond_read_list(p, fp);
		if (rc)
			goto bad;
	}

	rc = next_entry(buf, fp, sizeof(u32));
	if (rc < 0)
		goto bad;
	nel = le32_to_cpu(buf[0]);
	ltr = NULL;
	for (i = 0; i < nel; i++) {
		tr = kmalloc(sizeof(*tr), GFP_KERNEL);
		if (!tr) {
			rc = -ENOMEM;
			goto bad;
		}
		memset(tr, 0, sizeof(*tr));
		if (ltr) {
			ltr->next = tr;
		} else {
			p->role_tr = tr;
		}
		rc = next_entry(buf, fp, sizeof(u32)*3);
		if (rc < 0)
			goto bad;
		tr->role = le32_to_cpu(buf[0]);
		tr->type = le32_to_cpu(buf[1]);
		tr->new_role = le32_to_cpu(buf[2]);
		ltr = tr;
	}

	rc = next_entry(buf, fp, sizeof(u32));
	if (rc < 0)
		goto bad;
	nel = le32_to_cpu(buf[0]);
	lra = NULL;
	for (i = 0; i < nel; i++) {
		ra = kmalloc(sizeof(*ra), GFP_KERNEL);
		if (!ra) {
			rc = -ENOMEM;
			goto bad;
		}
		memset(ra, 0, sizeof(*ra));
		if (lra) {
			lra->next = ra;
		} else {
			p->role_allow = ra;
		}
		rc = next_entry(buf, fp, sizeof(u32)*2);
		if (rc < 0)
			goto bad;
		ra->role = le32_to_cpu(buf[0]);
		ra->new_role = le32_to_cpu(buf[1]);
		lra = ra;
	}

	rc = policydb_index_classes(p);
	if (rc)
		goto bad;

	rc = policydb_index_others(p);
	if (rc)
		goto bad;

	for (i = 0; i < info->ocon_num; i++) {
		rc = next_entry(buf, fp, sizeof(u32));
		if (rc < 0)
			goto bad;
		nel = le32_to_cpu(buf[0]);
		l = NULL;
		for (j = 0; j < nel; j++) {
			c = kmalloc(sizeof(*c), GFP_KERNEL);
			if (!c) {
				rc = -ENOMEM;
				goto bad;
			}
			memset(c, 0, sizeof(*c));
			if (l) {
				l->next = c;
			} else {
				p->ocontexts[i] = c;
			}
			l = c;
			rc = -EINVAL;
			switch (i) {
			case OCON_ISID:
				rc = next_entry(buf, fp, sizeof(u32));
				if (rc < 0)
					goto bad;
				c->sid[0] = le32_to_cpu(buf[0]);
				rc = context_read_and_validate(&c->context[0], p, fp);
				if (rc)
					goto bad;
				break;
			case OCON_FS:
			case OCON_NETIF:
				rc = next_entry(buf, fp, sizeof(u32));
				if (rc < 0)
					goto bad;
				len = le32_to_cpu(buf[0]);
				c->u.name = kmalloc(len + 1,GFP_KERNEL);
				if (!c->u.name) {
					rc = -ENOMEM;
					goto bad;
				}
				rc = next_entry(c->u.name, fp, len);
				if (rc < 0)
					goto bad;
				c->u.name[len] = 0;
				rc = context_read_and_validate(&c->context[0], p, fp);
				if (rc)
					goto bad;
				rc = context_read_and_validate(&c->context[1], p, fp);
				if (rc)
					goto bad;
				break;
			case OCON_PORT:
				rc = next_entry(buf, fp, sizeof(u32)*3);
				if (rc < 0)
					goto bad;
				c->u.port.protocol = le32_to_cpu(buf[0]);
				c->u.port.low_port = le32_to_cpu(buf[1]);
				c->u.port.high_port = le32_to_cpu(buf[2]);
				rc = context_read_and_validate(&c->context[0], p, fp);
				if (rc)
					goto bad;
				break;
			case OCON_NODE:
				rc = next_entry(buf, fp, sizeof(u32)* 2);
				if (rc < 0)
					goto bad;
				c->u.node.addr = le32_to_cpu(buf[0]);
				c->u.node.mask = le32_to_cpu(buf[1]);
				rc = context_read_and_validate(&c->context[0], p, fp);
				if (rc)
					goto bad;
				break;
			case OCON_FSUSE:
				rc = next_entry(buf, fp, sizeof(u32)*2);
				if (rc < 0)
					goto bad;
				c->v.behavior = le32_to_cpu(buf[0]);
				if (c->v.behavior > SECURITY_FS_USE_NONE)
					goto bad;
				len = le32_to_cpu(buf[1]);
				c->u.name = kmalloc(len + 1,GFP_KERNEL);
				if (!c->u.name) {
					rc = -ENOMEM;
					goto bad;
				}
				rc = next_entry(c->u.name, fp, len);
				if (rc < 0)
					goto bad;
				c->u.name[len] = 0;
				rc = context_read_and_validate(&c->context[0], p, fp);
				if (rc)
					goto bad;
				break;
			case OCON_NODE6: {
				int k;

				rc = next_entry(buf, fp, sizeof(u32) * 8);
				if (rc < 0)
					goto bad;
				for (k = 0; k < 4; k++)
					c->u.node6.addr[k] = le32_to_cpu(buf[k]);
				for (k = 0; k < 4; k++)
					c->u.node6.mask[k] = le32_to_cpu(buf[k+4]);
				if (context_read_and_validate(&c->context[0], p, fp))
					goto bad;
				break;
			}
			}
		}
	}

	rc = next_entry(buf, fp, sizeof(u32));
	if (rc < 0)
		goto bad;
	nel = le32_to_cpu(buf[0]);
	genfs_p = NULL;
	rc = -EINVAL;
	for (i = 0; i < nel; i++) {
		rc = next_entry(buf, fp, sizeof(u32));
		if (rc < 0)
			goto bad;
		len = le32_to_cpu(buf[0]);
		newgenfs = kmalloc(sizeof(*newgenfs), GFP_KERNEL);
		if (!newgenfs) {
			rc = -ENOMEM;
			goto bad;
		}
		memset(newgenfs, 0, sizeof(*newgenfs));

		newgenfs->fstype = kmalloc(len + 1,GFP_KERNEL);
		if (!newgenfs->fstype) {
			rc = -ENOMEM;
			kfree(newgenfs);
			goto bad;
		}
		rc = next_entry(newgenfs->fstype, fp, len);
		if (rc < 0) {
			kfree(newgenfs->fstype);
			kfree(newgenfs);
			goto bad;
		}
		newgenfs->fstype[len] = 0;
		for (genfs_p = NULL, genfs = p->genfs; genfs;
		     genfs_p = genfs, genfs = genfs->next) {
			if (strcmp(newgenfs->fstype, genfs->fstype) == 0) {
				printk(KERN_ERR "security:  dup genfs "
				       "fstype %s\n", newgenfs->fstype);
				kfree(newgenfs->fstype);
				kfree(newgenfs);
				goto bad;
			}
			if (strcmp(newgenfs->fstype, genfs->fstype) < 0)
				break;
		}
		newgenfs->next = genfs;
		if (genfs_p)
			genfs_p->next = newgenfs;
		else
			p->genfs = newgenfs;
		rc = next_entry(buf, fp, sizeof(u32));
		if (rc < 0)
			goto bad;
		nel2 = le32_to_cpu(buf[0]);
		for (j = 0; j < nel2; j++) {
			rc = next_entry(buf, fp, sizeof(u32));
			if (rc < 0)
				goto bad;
			len = le32_to_cpu(buf[0]);

			newc = kmalloc(sizeof(*newc), GFP_KERNEL);
			if (!newc) {
				rc = -ENOMEM;
				goto bad;
			}
			memset(newc, 0, sizeof(*newc));

			newc->u.name = kmalloc(len + 1,GFP_KERNEL);
			if (!newc->u.name) {
				rc = -ENOMEM;
				goto bad_newc;
			}
			rc = next_entry(newc->u.name, fp, len);
			if (rc < 0)
				goto bad_newc;
			newc->u.name[len] = 0;
			rc = next_entry(buf, fp, sizeof(u32));
			if (rc < 0)
				goto bad_newc;
			newc->v.sclass = le32_to_cpu(buf[0]);
			if (context_read_and_validate(&newc->context[0], p, fp))
				goto bad_newc;
			for (l = NULL, c = newgenfs->head; c;
			     l = c, c = c->next) {
				if (!strcmp(newc->u.name, c->u.name) &&
				    (!c->v.sclass || !newc->v.sclass ||
				     newc->v.sclass == c->v.sclass)) {
					printk(KERN_ERR "security:  dup genfs "
					       "entry (%s,%s)\n",
					       newgenfs->fstype, c->u.name);
					goto bad_newc;
				}
				len = strlen(newc->u.name);
				len2 = strlen(c->u.name);
				if (len > len2)
					break;
			}

			newc->next = c;
			if (l)
				l->next = newc;
			else
				newgenfs->head = newc;
		}
	}

	rc = mls_read_trusted(p, fp);
	if (rc)
		goto bad;

	rc = 0;
out:
	return rc;
bad_newc:
	ocontext_destroy(newc,OCON_FSUSE);
bad:
	if (!rc)
		rc = -EINVAL;
	policydb_destroy(p);
	goto out;
}
