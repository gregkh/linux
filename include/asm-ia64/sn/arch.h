/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * SGI specific setup.
 *
 * Copyright (C) 1995-1997,1999,2001-2004 Silicon Graphics, Inc.  All rights reserved.
 * Copyright (C) 1999 Ralf Baechle (ralf@gnu.org)
 */
#ifndef _ASM_IA64_SN_ARCH_H
#define _ASM_IA64_SN_ARCH_H

#include <asm/types.h>
#include <asm/sn/types.h>
#include <asm/sn/sn_cpuid.h>

/*
 * This is the maximum number of nodes that can be part of a kernel.
 * Effectively, it's the maximum number of compact node ids (cnodeid_t).
 * This is not necessarily the same as MAX_NASIDS.
 */
#define MAX_COMPACT_NODES	2048
#define CPUS_PER_NODE		4

extern void sn_flush_all_caches(long addr, long bytes);

#endif /* _ASM_IA64_SN_ARCH_H */
