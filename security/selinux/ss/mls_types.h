/*
 * Type definitions for the multi-level security (MLS) policy.
 *
 * Author : Stephen Smalley, <sds@epoch.ncsc.mil>
 */
#ifndef _SS_MLS_TYPES_H_
#define _SS_MLS_TYPES_H_

struct mls_level {
	u32 sens;		/* sensitivity */
	struct ebitmap cat;	/* category set */
};

struct mls_range {
	struct mls_level level[2]; /* low == level[0], high == level[1] */
};

struct mls_range_list {
	struct mls_range range;
	struct mls_range_list *next;
};

#define MLS_RELATION_DOM	1 /* source dominates */
#define MLS_RELATION_DOMBY	2 /* target dominates */
#define MLS_RELATION_EQ		4 /* source and target are equivalent */
#define MLS_RELATION_INCOMP	8 /* source and target are incomparable */

#define mls_level_eq(l1,l2) \
(((l1).sens == (l2).sens) && ebitmap_cmp(&(l1).cat,&(l2).cat))

#define mls_level_relation(l1,l2) ( \
(((l1).sens == (l2).sens) && ebitmap_cmp(&(l1).cat,&(l2).cat)) ? \
				    MLS_RELATION_EQ : \
(((l1).sens >= (l2).sens) && ebitmap_contains(&(l1).cat, &(l2).cat)) ? \
				    MLS_RELATION_DOM : \
(((l2).sens >= (l1).sens) && ebitmap_contains(&(l2).cat, &(l1).cat)) ? \
				    MLS_RELATION_DOMBY : \
				    MLS_RELATION_INCOMP )

#define mls_range_contains(r1,r2) \
((mls_level_relation((r1).level[0], (r2).level[0]) & \
	  (MLS_RELATION_EQ | MLS_RELATION_DOMBY)) && \
	 (mls_level_relation((r1).level[1], (r2).level[1]) & \
	  (MLS_RELATION_EQ | MLS_RELATION_DOM)))

/*
 * Every access vector permission is mapped to a set of MLS base
 * permissions, based on the flow properties of the corresponding
 * operation.
 */
struct mls_perms {
	u32 read;     /* permissions that map to `read' */
	u32 readby;   /* permissions that map to `readby' */
	u32 write;    /* permissions that map to `write' */
	u32 writeby;  /* permissions that map to `writeby' */
};

#endif	/* _SS_MLS_TYPES_H_ */
