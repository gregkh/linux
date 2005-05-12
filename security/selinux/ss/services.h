/*
 * Implementation of the security services.
 *
 * Author : Stephen Smalley, <sds@epoch.ncsc.mil>
 */
#ifndef _SS_SERVICES_H_
#define _SS_SERVICES_H_

#include "policydb.h"
#include "sidtab.h"

/*
 * The security server uses two global data structures
 * when providing its services:  the SID table (sidtab)
 * and the policy database (policydb).
 */
extern struct sidtab sidtab;
extern struct policydb policydb;

#endif	/* _SS_SERVICES_H_ */

