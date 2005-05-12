#ifndef _ASM_GENERIC_RESOURCE_H
#define _ASM_GENERIC_RESOURCE_H

/*
 * Resource limits
 */

/* Allow arch to control resource order */
#ifndef __ARCH_RLIMIT_ORDER
#define RLIMIT_CPU		0	/* CPU time in ms */
#define RLIMIT_FSIZE		1	/* Maximum filesize */
#define RLIMIT_DATA		2	/* max data size */
#define RLIMIT_STACK		3	/* max stack size */
#define RLIMIT_CORE		4	/* max core file size */
#define RLIMIT_RSS		5	/* max resident set size */
#define RLIMIT_NPROC		6	/* max number of processes */
#define RLIMIT_NOFILE		7	/* max number of open files */
#define RLIMIT_MEMLOCK		8	/* max locked-in-memory address space */
#define RLIMIT_AS		9	/* address space limit */
#define RLIMIT_LOCKS		10	/* maximum file locks held */
#define RLIMIT_SIGPENDING	11	/* max number of pending signals */
#define RLIMIT_MSGQUEUE		12	/* maximum bytes in POSIX mqueues */

#define RLIM_NLIMITS		13
#endif

/*
 * SuS says limits have to be unsigned.
 * Which makes a ton more sense anyway.
 */
#ifndef RLIM_INFINITY
#define RLIM_INFINITY	(~0UL)
#endif

#ifndef _STK_LIM_MAX
#define _STK_LIM_MAX	RLIM_INFINITY
#endif

#ifdef __KERNEL__

#define INIT_RLIMITS							\
{									\
	[RLIMIT_CPU]		= { RLIM_INFINITY, RLIM_INFINITY },	\
	[RLIMIT_FSIZE]		= { RLIM_INFINITY, RLIM_INFINITY },	\
	[RLIMIT_DATA]		= { RLIM_INFINITY, RLIM_INFINITY },	\
	[RLIMIT_STACK]		= {      _STK_LIM, _STK_LIM_MAX  },	\
	[RLIMIT_CORE]		= {             0, RLIM_INFINITY },	\
	[RLIMIT_RSS]		= { RLIM_INFINITY, RLIM_INFINITY },	\
	[RLIMIT_NPROC]		= {             0,             0 },	\
	[RLIMIT_NOFILE]		= {      INR_OPEN,     INR_OPEN  },	\
	[RLIMIT_MEMLOCK]	= {   MLOCK_LIMIT,   MLOCK_LIMIT },	\
	[RLIMIT_AS]		= { RLIM_INFINITY, RLIM_INFINITY },	\
	[RLIMIT_LOCKS]		= { RLIM_INFINITY, RLIM_INFINITY },	\
	[RLIMIT_SIGPENDING]	= { MAX_SIGPENDING, MAX_SIGPENDING },	\
	[RLIMIT_MSGQUEUE]	= { MQ_BYTES_MAX, MQ_BYTES_MAX },	\
}

#endif	/* __KERNEL__ */

#endif
