/* 
 * Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#ifndef __ARCH_UM_MMU_H
#define __ARCH_UM_MMU_H

#include "linux/config.h"
#include "choose-mode.h"

#ifdef CONFIG_MODE_TT
#include "mmu-tt.h"
#endif

#ifdef CONFIG_MODE_SKAS
#include "mmu-skas.h"
#endif

typedef union {
#ifdef CONFIG_MODE_TT
	struct mmu_context_tt tt;
#endif
#ifdef CONFIG_MODE_SKAS
	struct mmu_context_skas skas;
#endif
} mm_context_t;

#endif

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
