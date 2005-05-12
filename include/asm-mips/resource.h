/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1995, 96, 98, 99, 2000 by Ralf Baechle
 * Copyright (C) 1999 Silicon Graphics, Inc.
 */
#ifndef _ASM_RESOURCE_H
#define _ASM_RESOURCE_H

/*
 * Resource limits
 */
#define RLIMIT_CPU 0			/* CPU time in ms */
#define RLIMIT_FSIZE 1			/* Maximum filesize */
#define RLIMIT_DATA 2			/* max data size */
#define RLIMIT_STACK 3			/* max stack size */
#define RLIMIT_CORE 4			/* max core file size */
#define RLIMIT_NOFILE 5			/* max number of open files */
#define RLIMIT_AS 6			/* mapped memory */
#define RLIMIT_RSS 7			/* max resident set size */
#define RLIMIT_NPROC 8			/* max number of processes */
#define RLIMIT_MEMLOCK 9		/* max locked-in-memory address space */
#define RLIMIT_LOCKS 10			/* maximum file locks held */
#define RLIMIT_SIGPENDING 11		/* max number of pending signals */
#define RLIMIT_MSGQUEUE 12		/* maximum bytes in POSIX mqueues */

#define RLIM_NLIMITS 13			/* Number of limit flavors.  */
#define __ARCH_RLIMIT_ORDER

/*
 * SuS says limits have to be unsigned.
 * Which makes a ton more sense anyway.
 */
#include <linux/config.h>
#ifdef CONFIG_MIPS32
#define RLIM_INFINITY	0x7fffffffUL
#endif
#ifdef CONFIG_MIPS64
#define RLIM_INFINITY	(~0UL)
#endif

#include <asm-generic/resource.h>

#endif /* _ASM_RESOURCE_H */
