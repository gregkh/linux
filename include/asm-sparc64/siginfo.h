#ifndef _SPARC64_SIGINFO_H
#define _SPARC64_SIGINFO_H

#define SI_PAD_SIZE32	((SI_MAX_SIZE/sizeof(int)) - 3)

#define SIGEV_PAD_SIZE	((SIGEV_MAX_SIZE/sizeof(int)) - 4)
#define SIGEV_PAD_SIZE32 ((SIGEV_MAX_SIZE/sizeof(int)) - 3)

#define __ARCH_SI_PREAMBLE_SIZE	(4 * sizeof(int))
#define __ARCH_SI_TRAPNO
#define __ARCH_SI_BAND_T int

#include <asm-generic/siginfo.h>

#ifdef __KERNEL__

#include <linux/config.h>
#include <linux/compat.h>

#ifdef CONFIG_COMPAT

typedef union sigval32 {
	int sival_int;
	u32 sival_ptr;
} sigval_t32;

struct compat_siginfo;

#endif /* CONFIG_COMPAT */

#endif /* __KERNEL__ */

#define SI_NOINFO	32767		/* no information in siginfo_t */

/*
 * SIGEMT si_codes
 */
#define EMT_TAGOVF	(__SI_FAULT|1)	/* tag overflow */
#define NSIGEMT		1

#ifdef __KERNEL__

#ifdef CONFIG_COMPAT

typedef struct sigevent32 {
	sigval_t32 sigev_value;
	int sigev_signo;
	int sigev_notify;
	union {
		int _pad[SIGEV_PAD_SIZE32];

		struct {
			u32 _function;
			u32 _attribute;	/* really pthread_attr_t */
		} _sigev_thread;
	} _sigev_un;
} sigevent_t32;

#endif /* CONFIG_COMPAT */

#endif /* __KERNEL__ */

#endif
