/*
 * Copyright 2003 PathScale, Inc.
 *
 * Licensed under the GPL
 */

#include <stddef.h>
#include <errno.h>
#define __FRAME_OFFSETS
#include <sys/ptrace.h>
#include <asm/ptrace.h>
#include "user.h"
#include "kern_constants.h"

int ptrace_getregs(long pid, unsigned long *regs_out)
{
	if(ptrace(PTRACE_GETREGS, pid, 0, regs_out) < 0)
		return(-errno);
	return(0);
}

int ptrace_setregs(long pid, unsigned long *regs)
{
	if(ptrace(PTRACE_SETREGS, pid, 0, regs) < 0)
		return(-errno);
	return(0);
}

void ptrace_pokeuser(unsigned long addr, unsigned long data)
{
	panic("ptrace_pokeuser");
}

#define DS 184
#define ES 192
#define __USER_DS     0x2b

void arch_enter_kernel(void *task, int pid)
{
}

void arch_leave_kernel(void *task, int pid)
{
#ifdef UM_USER_CS
	if(ptrace(PTRACE_POKEUSER, pid, CS, UM_USER_CS) < 0)
		tracer_panic("POKEUSER CS failed");
#endif

	if(ptrace(PTRACE_POKEUSER, pid, DS, __USER_DS) < 0)
		tracer_panic("POKEUSER DS failed");
	if(ptrace(PTRACE_POKEUSER, pid, ES, __USER_DS) < 0)
		tracer_panic("POKEUSER ES failed");
}

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
