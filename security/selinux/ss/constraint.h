/*
 * A constraint is a condition that must be satisfied in
 * order for one or more permissions to be granted.
 * Constraints are used to impose additional restrictions
 * beyond the type-based rules in `te' or the role-based
 * transition rules in `rbac'.  Constraints are typically
 * used to prevent a process from transitioning to a new user
 * identity or role unless it is in a privileged type.
 * Constraints are likewise typically used to prevent a
 * process from labeling an object with a different user
 * identity.
 *
 * Author : Stephen Smalley, <sds@epoch.ncsc.mil>
 */
#ifndef _SS_CONSTRAINT_H_
#define _SS_CONSTRAINT_H_

#include "ebitmap.h"

#define CEXPR_MAXDEPTH 5

struct constraint_expr {
#define CEXPR_NOT		1 /* not expr */
#define CEXPR_AND		2 /* expr and expr */
#define CEXPR_OR		3 /* expr or expr */
#define CEXPR_ATTR		4 /* attr op attr */
#define CEXPR_NAMES		5 /* attr op names */
	u32 expr_type;		/* expression type */

#define CEXPR_USER 1		/* user */
#define CEXPR_ROLE 2		/* role */
#define CEXPR_TYPE 4		/* type */
#define CEXPR_TARGET 8		/* target if set, source otherwise */
	u32 attr;		/* attribute */

#define CEXPR_EQ     1		/* == or eq */
#define CEXPR_NEQ    2		/* != */
#define CEXPR_DOM    3		/* dom */
#define CEXPR_DOMBY  4		/* domby  */
#define CEXPR_INCOMP 5		/* incomp */
	u32 op;			/* operator */

	struct ebitmap names;	/* names */

	struct constraint_expr *next;   /* next expression */
};

struct constraint_node {
	u32 permissions;	/* constrained permissions */
	struct constraint_expr *expr;	/* constraint on permissions */
	struct constraint_node *next;	/* next constraint */
};

#endif	/* _SS_CONSTRAINT_H_ */
