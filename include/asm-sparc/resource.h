/* $Id: resource.h,v 1.12 2000/09/23 02:09:21 davem Exp $
 * resource.h: Resource definitions.
 *
 * Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 */

#ifndef _SPARC_RESOURCE_H
#define _SPARC_RESOURCE_H

/*
 * Resource limits
 */

#define RLIMIT_CPU	0		/* CPU time in ms */
#define RLIMIT_FSIZE	1		/* Maximum filesize */
#define RLIMIT_DATA	2		/* max data size */
#define RLIMIT_STACK	3		/* max stack size */
#define RLIMIT_CORE	4		/* max core file size */
#define RLIMIT_RSS	5		/* max resident set size */
#define RLIMIT_NOFILE	6		/* max number of open files */
#define RLIMIT_NPROC	7		/* max number of processes */
#define RLIMIT_MEMLOCK	8		/* max locked-in-memory address space */
#define RLIMIT_AS	9		/* address space limit */
#define RLIMIT_LOCKS	10		/* maximum file locks held */
#define RLIMIT_SIGPENDING 11		/* max number of pending signals */
#define RLIMIT_MSGQUEUE 12		/* maximum bytes in POSIX mqueues */

#define RLIM_NLIMITS	13
#define __ARCH_RLIMIT_ORDER

/*
 * SuS says limits have to be unsigned.
 * We make this unsigned, but keep the
 * old value.
 */
#define RLIM_INFINITY	0x7fffffff

#include <asm-generic/resource.h>

#endif /* !(_SPARC_RESOURCE_H) */
