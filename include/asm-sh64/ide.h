/*
 *  linux/include/asm-sh64/ide.h
 *
 *  Copyright (C) 1994-1996  Linus Torvalds & authors
 *
 *  sh64 version by Richard Curnow & Paul Mundt
 */

/*
 *  This file contains the sh64 architecture specific IDE code.
 */

#ifndef __ASM_SH64_IDE_H
#define __ASM_SH64_IDE_H

#ifdef __KERNEL__

#include <linux/config.h>

#ifndef MAX_HWIFS
#define MAX_HWIFS	CONFIG_IDE_MAX_HWIFS
#endif

#define ide_default_io_ctl(base)	(0)

#include <asm-generic/ide_iops.h>

#endif /* __KERNEL__ */

#endif /* __ASM_SH64_IDE_H */
