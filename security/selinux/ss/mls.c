/*
 * Implementation of the multi-level security (MLS) policy.
 *
 * Author : Stephen Smalley, <sds@epoch.ncsc.mil>
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include "mls.h"
#include "policydb.h"
#include "services.h"

/*
 * Remove any permissions from `allowed' that are
 * denied by the MLS policy.
 */
void mls_compute_av(struct context *scontext,
		    struct context *tcontext,
		    struct class_datum *tclass,
		    u32 *allowed)
{
	unsigned int rel[2];
	int l;

	for (l = 0; l < 2; l++)
		rel[l] = mls_level_relation(scontext->range.level[l],
					    tcontext->range.level[l]);

	if (rel[1] != MLS_RELATION_EQ) {
		if (rel[1] != MLS_RELATION_DOM &&
		    !ebitmap_get_bit(&policydb.trustedreaders, scontext->type - 1) &&
		    !ebitmap_get_bit(&policydb.trustedobjects, tcontext->type - 1)) {
			/* read(s,t) = (s.high >= t.high) = False */
			*allowed = (*allowed) & ~(tclass->mlsperms.read);
		}
		if (rel[1] != MLS_RELATION_DOMBY &&
		    !ebitmap_get_bit(&policydb.trustedreaders, tcontext->type - 1) &&
		    !ebitmap_get_bit(&policydb.trustedobjects, scontext->type - 1)) {
			/* readby(s,t) = read(t,s) = False */
			*allowed = (*allowed) & ~(tclass->mlsperms.readby);
		}
	}
	if (((rel[0] != MLS_RELATION_DOMBY && rel[0] != MLS_RELATION_EQ) ||
	    ((!mls_level_eq(tcontext->range.level[0],
			    tcontext->range.level[1])) &&
	     (rel[1] != MLS_RELATION_DOM && rel[1] != MLS_RELATION_EQ))) &&
	    !ebitmap_get_bit(&policydb.trustedwriters, scontext->type - 1) &&
	    !ebitmap_get_bit(&policydb.trustedobjects, tcontext->type - 1)) {
		/*
		 * write(s,t) = ((s.low <= t.low = t.high) or (s.low
		 * <= t.low <= t.high <= s.high)) = False
		 */
		*allowed = (*allowed) & ~(tclass->mlsperms.write);
	}

	if (((rel[0] != MLS_RELATION_DOM && rel[0] != MLS_RELATION_EQ) ||
	    ((!mls_level_eq(scontext->range.level[0],
			    scontext->range.level[1])) &&
	     (rel[1] != MLS_RELATION_DOMBY && rel[1] != MLS_RELATION_EQ))) &&
	    !ebitmap_get_bit(&policydb.trustedwriters, tcontext->type - 1) &&
	    !ebitmap_get_bit(&policydb.trustedobjects, scontext->type - 1)) {
		/* writeby(s,t) = write(t,s) = False */
		*allowed = (*allowed) & ~(tclass->mlsperms.writeby);
	}
}

/*
 * Return the length in bytes for the MLS fields of the
 * security context string representation of `context'.
 */
int mls_compute_context_len(struct context * context)
{
	int i, l, len;


	len = 0;
	for (l = 0; l < 2; l++) {
		len += strlen(policydb.p_sens_val_to_name[context->range.level[l].sens - 1]) + 1;

		for (i = 1; i <= ebitmap_length(&context->range.level[l].cat); i++)
			if (ebitmap_get_bit(&context->range.level[l].cat, i - 1))
				len += strlen(policydb.p_cat_val_to_name[i - 1]) + 1;

		if (mls_level_relation(context->range.level[0], context->range.level[1])
				== MLS_RELATION_EQ)
			break;
	}

	return len;
}

/*
 * Write the security context string representation of
 * the MLS fields of `context' into the string `*scontext'.
 * Update `*scontext' to point to the end of the MLS fields.
 */
int mls_sid_to_context(struct context *context,
		       char **scontext)
{
	char *scontextp;
	int i, l;

	scontextp = *scontext;

	for (l = 0; l < 2; l++) {
		strcpy(scontextp,
		       policydb.p_sens_val_to_name[context->range.level[l].sens - 1]);
		scontextp += strlen(policydb.p_sens_val_to_name[context->range.level[l].sens - 1]);
		*scontextp = ':';
		scontextp++;
		for (i = 1; i <= ebitmap_length(&context->range.level[l].cat); i++)
			if (ebitmap_get_bit(&context->range.level[l].cat, i - 1)) {
				strcpy(scontextp, policydb.p_cat_val_to_name[i - 1]);
				scontextp += strlen(policydb.p_cat_val_to_name[i - 1]);
				*scontextp = ',';
				scontextp++;
			}
		if (mls_level_relation(context->range.level[0], context->range.level[1])
				!= MLS_RELATION_EQ) {
			scontextp--;
			sprintf(scontextp, "-");
			scontextp++;

		} else {
			break;
		}
	}

	*scontext = scontextp;
	return 0;
}

/*
 * Return 1 if the MLS fields in the security context
 * structure `c' are valid.  Return 0 otherwise.
 */
int mls_context_isvalid(struct policydb *p, struct context *c)
{
	unsigned int relation;
	struct level_datum *levdatum;
	struct user_datum *usrdatum;
	struct mls_range_list *rnode;
	int i, l;

	/*
	 * MLS range validity checks: high must dominate low, low level must
	 * be valid (category set <-> sensitivity check), and high level must
	 * be valid (category set <-> sensitivity check)
	 */
	relation = mls_level_relation(c->range.level[1],
				      c->range.level[0]);
	if (!(relation & (MLS_RELATION_DOM | MLS_RELATION_EQ)))
		/* High does not dominate low. */
		return 0;

	for (l = 0; l < 2; l++) {
		if (!c->range.level[l].sens || c->range.level[l].sens > p->p_levels.nprim)
			return 0;
		levdatum = hashtab_search(p->p_levels.table,
			p->p_sens_val_to_name[c->range.level[l].sens - 1]);
		if (!levdatum)
			return 0;

		for (i = 1; i <= ebitmap_length(&c->range.level[l].cat); i++) {
			if (ebitmap_get_bit(&c->range.level[l].cat, i - 1)) {
				if (i > p->p_cats.nprim)
					return 0;
				if (!ebitmap_get_bit(&levdatum->level->cat, i - 1))
					/*
					 * Category may not be associated with
					 * sensitivity in low level.
					 */
					return 0;
			}
		}
	}

	if (c->role == OBJECT_R_VAL)
		return 1;

	/*
	 * User must be authorized for the MLS range.
	 */
	if (!c->user || c->user > p->p_users.nprim)
		return 0;
	usrdatum = p->user_val_to_struct[c->user - 1];
	for (rnode = usrdatum->ranges; rnode; rnode = rnode->next) {
		if (mls_range_contains(rnode->range, c->range))
			break;
	}
	if (!rnode)
		/* user may not be associated with range */
		return 0;

	return 1;
}


/*
 * Set the MLS fields in the security context structure
 * `context' based on the string representation in
 * the string `*scontext'.  Update `*scontext' to
 * point to the end of the string representation of
 * the MLS fields.
 *
 * This function modifies the string in place, inserting
 * NULL characters to terminate the MLS fields.
 */
int mls_context_to_sid(char oldc,
		       char **scontext,
		       struct context *context)
{

	char delim;
	char *scontextp, *p;
	struct level_datum *levdatum;
	struct cat_datum *catdatum;
	int l, rc = -EINVAL;

	if (!oldc) {
		/* No MLS component to the security context.  Try
		   to use a default 'unclassified' value. */
		levdatum = hashtab_search(policydb.p_levels.table,
		                          "unclassified");
		if (!levdatum)
			goto out;
		context->range.level[0].sens = levdatum->level->sens;
		context->range.level[1].sens = context->range.level[0].sens;
		rc = 0;
		goto out;
	}

	/* Extract low sensitivity. */
	scontextp = p = *scontext;
	while (*p && *p != ':' && *p != '-')
		p++;

	delim = *p;
	if (delim != 0)
		*p++ = 0;

	for (l = 0; l < 2; l++) {
		levdatum = hashtab_search(policydb.p_levels.table, scontextp);
		if (!levdatum)
			goto out;

		context->range.level[l].sens = levdatum->level->sens;

		if (delim == ':') {
			/* Extract low category set. */
			while (1) {
				scontextp = p;
				while (*p && *p != ',' && *p != '-')
					p++;
				delim = *p;
				if (delim != 0)
					*p++ = 0;

				catdatum = hashtab_search(policydb.p_cats.table,
				                          scontextp);
				if (!catdatum)
					goto out;

				rc = ebitmap_set_bit(&context->range.level[l].cat,
				                     catdatum->value - 1, 1);
				if (rc)
					goto out;
				if (delim != ',')
					break;
			}
		}
		if (delim == '-') {
			/* Extract high sensitivity. */
			scontextp = p;
			while (*p && *p != ':')
				p++;

			delim = *p;
			if (delim != 0)
				*p++ = 0;
		} else
			break;
	}

	if (l == 0) {
		context->range.level[1].sens = context->range.level[0].sens;
		rc = ebitmap_cpy(&context->range.level[1].cat,
				 &context->range.level[0].cat);
		if (rc)
			goto out;
	}
	*scontext = ++p;
	rc = 0;
out:
	return rc;
}

/*
 * Copies the MLS range from `src' into `dst'.
 */
static inline int mls_copy_context(struct context *dst,
				   struct context *src)
{
	int l, rc = 0;

	/* Copy the MLS range from the source context */
	for (l = 0; l < 2; l++) {

		dst->range.level[l].sens = src->range.level[l].sens;
		rc = ebitmap_cpy(&dst->range.level[l].cat,
				 &src->range.level[l].cat);
		if (rc)
			break;
	}

	return rc;
}

/*
 * Convert the MLS fields in the security context
 * structure `c' from the values specified in the
 * policy `oldp' to the values specified in the policy `newp'.
 */
int mls_convert_context(struct policydb *oldp,
			struct policydb *newp,
			struct context *c)
{
	struct level_datum *levdatum;
	struct cat_datum *catdatum;
	struct ebitmap bitmap;
	int l, i;

	for (l = 0; l < 2; l++) {
		levdatum = hashtab_search(newp->p_levels.table,
			oldp->p_sens_val_to_name[c->range.level[l].sens - 1]);

		if (!levdatum)
			return -EINVAL;
		c->range.level[l].sens = levdatum->level->sens;

		ebitmap_init(&bitmap);
		for (i = 1; i <= ebitmap_length(&c->range.level[l].cat); i++) {
			if (ebitmap_get_bit(&c->range.level[l].cat, i - 1)) {
				int rc;

				catdatum = hashtab_search(newp->p_cats.table,
				         	oldp->p_cat_val_to_name[i - 1]);
				if (!catdatum)
					return -EINVAL;
				rc = ebitmap_set_bit(&bitmap, catdatum->value - 1, 1);
				if (rc)
					return rc;
			}
		}
		ebitmap_destroy(&c->range.level[l].cat);
		c->range.level[l].cat = bitmap;
	}

	return 0;
}

int mls_compute_sid(struct context *scontext,
		    struct context *tcontext,
		    u16 tclass,
		    u32 specified,
		    struct context *newcontext)
{
	switch (specified) {
	case AVTAB_TRANSITION:
	case AVTAB_CHANGE:
		/* Use the process MLS attributes. */
		return mls_copy_context(newcontext, scontext);
	case AVTAB_MEMBER:
		/* Only polyinstantiate the MLS attributes if
		   the type is being polyinstantiated */
		if (newcontext->type != tcontext->type) {
			/* Use the process MLS attributes. */
			return mls_copy_context(newcontext, scontext);
		} else {
			/* Use the related object MLS attributes. */
			return mls_copy_context(newcontext, tcontext);
		}
	default:
		return -EINVAL;
	}
	return -EINVAL;
}

void mls_user_destroy(struct user_datum *usrdatum)
{
	struct mls_range_list *rnode, *rtmp;
	rnode = usrdatum->ranges;
	while (rnode) {
		rtmp = rnode;
		rnode = rnode->next;
		ebitmap_destroy(&rtmp->range.level[0].cat);
		ebitmap_destroy(&rtmp->range.level[1].cat);
		kfree(rtmp);
	}
}

int mls_read_perm(struct perm_datum *perdatum, void *fp)
{
	u32 buf[1];
	int rc;

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0)
		return -EINVAL;
	perdatum->base_perms = le32_to_cpu(buf[0]);
	return 0;
}

/*
 * Read a MLS level structure from a policydb binary
 * representation file.
 */
struct mls_level *mls_read_level(void *fp)
{
	struct mls_level *l;
	u32 buf[1];
	int rc;

	l = kmalloc(sizeof(*l), GFP_ATOMIC);
	if (!l) {
		printk(KERN_ERR "security: mls: out of memory\n");
		return NULL;
	}
	memset(l, 0, sizeof(*l));

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0) {
		printk(KERN_ERR "security: mls: truncated level\n");
		goto bad;
	}
	l->sens = cpu_to_le32(buf[0]);

	if (ebitmap_read(&l->cat, fp)) {
		printk(KERN_ERR "security: mls:  error reading level "
		       "categories\n");
		goto bad;
	}
	return l;

bad:
	kfree(l);
	return NULL;
}


/*
 * Read a MLS range structure from a policydb binary
 * representation file.
 */
static int mls_read_range_helper(struct mls_range *r, void *fp)
{
	u32 buf[2], items;
	int rc;

	rc = next_entry(buf, fp, sizeof(u32));
	if (rc < 0)
		goto out;

	items = le32_to_cpu(buf[0]);
	if (items > ARRAY_SIZE(buf)) {
		printk(KERN_ERR "security: mls:  range overflow\n");
		rc = -EINVAL;
		goto out;
	}
	rc = next_entry(buf, fp, sizeof(u32) * items);
	if (rc < 0) {
		printk(KERN_ERR "security: mls:  truncated range\n");
		goto out;
	}
	r->level[0].sens = le32_to_cpu(buf[0]);
	if (items > 1) {
		r->level[1].sens = le32_to_cpu(buf[1]);
	} else {
		r->level[1].sens = r->level[0].sens;
	}

	rc = ebitmap_read(&r->level[0].cat, fp);
	if (rc) {
		printk(KERN_ERR "security: mls:  error reading low "
		       "categories\n");
		goto out;
	}
	if (items > 1) {
		rc = ebitmap_read(&r->level[1].cat, fp);
		if (rc) {
			printk(KERN_ERR "security: mls:  error reading high "
			       "categories\n");
			goto bad_high;
		}
	} else {
		rc = ebitmap_cpy(&r->level[1].cat, &r->level[0].cat);
		if (rc) {
			printk(KERN_ERR "security: mls:  out of memory\n");
			goto bad_high;
		}
	}

	rc = 0;
out:
	return rc;
bad_high:
	ebitmap_destroy(&r->level[0].cat);
	goto out;
}

int mls_read_range(struct context *c, void *fp)
{
	return mls_read_range_helper(&c->range, fp);
}


/*
 * Read a MLS perms structure from a policydb binary
 * representation file.
 */
int mls_read_class(struct class_datum *cladatum, void *fp)
{
	struct mls_perms *p = &cladatum->mlsperms;
	u32 buf[4];
	int rc;

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0) {
		printk(KERN_ERR "security: mls:  truncated mls permissions\n");
		return -EINVAL;
	}
	p->read = le32_to_cpu(buf[0]);
	p->readby = le32_to_cpu(buf[1]);
	p->write = le32_to_cpu(buf[2]);
	p->writeby = le32_to_cpu(buf[3]);
	return 0;
}

int mls_read_user(struct user_datum *usrdatum, void *fp)
{
	struct mls_range_list *r, *l;
	int rc;
	u32 nel, i;
	u32 buf[1];

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0)
		goto out;
	nel = le32_to_cpu(buf[0]);
	l = NULL;
	for (i = 0; i < nel; i++) {
		r = kmalloc(sizeof(*r), GFP_ATOMIC);
		if (!r) {
			rc = -ENOMEM;
			goto out;
		}
		memset(r, 0, sizeof(*r));

		rc = mls_read_range_helper(&r->range, fp);
		if (rc) {
			kfree(r);
			goto out;
		}

		if (l)
			l->next = r;
		else
			usrdatum->ranges = r;
		l = r;
	}
out:
	return rc;
}

int mls_read_nlevels(struct policydb *p, void *fp)
{
	u32 buf[1];
	int rc;

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0)
		return -EINVAL;
	p->nlevels = le32_to_cpu(buf[0]);
	return 0;
}

int mls_read_trusted(struct policydb *p, void *fp)
{
	int rc = 0;

	rc = ebitmap_read(&p->trustedreaders, fp);
	if (rc)
		goto out;
	rc = ebitmap_read(&p->trustedwriters, fp);
	if (rc)
		goto bad;
	rc = ebitmap_read(&p->trustedobjects, fp);
	if (rc)
		goto bad2;
out:
	return rc;
bad2:
	ebitmap_destroy(&p->trustedwriters);
bad:
	ebitmap_destroy(&p->trustedreaders);
	goto out;
}

int sens_index(void *key, void *datum, void *datap)
{
	struct policydb *p;
	struct level_datum *levdatum;


	levdatum = datum;
	p = datap;

	if (!levdatum->isalias)
		p->p_sens_val_to_name[levdatum->level->sens - 1] = key;

	return 0;
}

int cat_index(void *key, void *datum, void *datap)
{
	struct policydb *p;
	struct cat_datum *catdatum;


	catdatum = datum;
	p = datap;


	if (!catdatum->isalias)
		p->p_cat_val_to_name[catdatum->value - 1] = key;

	return 0;
}

int sens_destroy(void *key, void *datum, void *p)
{
	struct level_datum *levdatum;

	kfree(key);
	levdatum = datum;
	if (!levdatum->isalias) {
		ebitmap_destroy(&levdatum->level->cat);
		kfree(levdatum->level);
	}
	kfree(datum);
	return 0;
}

int cat_destroy(void *key, void *datum, void *p)
{
	kfree(key);
	kfree(datum);
	return 0;
}

int sens_read(struct policydb *p, struct hashtab *h, void *fp)
{
	char *key = NULL;
	struct level_datum *levdatum;
	int rc;
	u32 buf[2], len;

	levdatum = kmalloc(sizeof(*levdatum), GFP_ATOMIC);
	if (!levdatum) {
		rc = -ENOMEM;
		goto out;
	}
	memset(levdatum, 0, sizeof(*levdatum));

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0)
		goto bad;

	len = le32_to_cpu(buf[0]);
	levdatum->isalias = le32_to_cpu(buf[1]);

	key = kmalloc(len + 1,GFP_ATOMIC);
	if (!key) {
		rc = -ENOMEM;
		goto bad;
	}
	rc = next_entry(key, fp, len);
	if (rc < 0)
		goto bad;
	key[len] = 0;

	levdatum->level = mls_read_level(fp);
	if (!levdatum->level) {
		rc = -EINVAL;
		goto bad;
	}

	rc = hashtab_insert(h, key, levdatum);
	if (rc)
		goto bad;
out:
	return rc;
bad:
	sens_destroy(key, levdatum, NULL);
	goto out;
}


int cat_read(struct policydb *p, struct hashtab *h, void *fp)
{
	char *key = NULL;
	struct cat_datum *catdatum;
	int rc;
	u32 buf[3], len;

	catdatum = kmalloc(sizeof(*catdatum), GFP_ATOMIC);
	if (!catdatum) {
		rc = -ENOMEM;
		goto out;
	}
	memset(catdatum, 0, sizeof(*catdatum));

	rc = next_entry(buf, fp, sizeof buf);
	if (rc < 0)
		goto bad;

	len = le32_to_cpu(buf[0]);
	catdatum->value = le32_to_cpu(buf[1]);
	catdatum->isalias = le32_to_cpu(buf[2]);

	key = kmalloc(len + 1,GFP_ATOMIC);
	if (!key) {
		rc = -ENOMEM;
		goto bad;
	}
	rc = next_entry(key, fp, len);
	if (rc < 0)
		goto bad;
	key[len] = 0;

	rc = hashtab_insert(h, key, catdatum);
	if (rc)
		goto bad;
out:
	return rc;

bad:
	cat_destroy(key, catdatum, NULL);
	goto out;
}
