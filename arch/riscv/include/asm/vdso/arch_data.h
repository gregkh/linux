/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __RISCV_ASM_VDSO_ARCH_DATA_H
#define __RISCV_ASM_VDSO_ARCH_DATA_H

#include <linux/types.h>
#include <vdso/datapage.h>
#include <asm/hwprobe.h>

struct vdso_arch_data {
	/* Stash static answers to the hwprobe queries when all CPUs are selected. */
	__u64 all_cpu_hwprobe_values[RISCV_HWPROBE_MAX_KEY + 1];

	/* Boolean indicating all CPUs have the same static hwprobe values. */
	__u8 homogeneous_cpus;

	/*
	 * A gate to check and see if the hwprobe data is actually ready, as
	 * probing is deferred to avoid boot slowdowns.
	 */
	__u8 ready;
};

#endif /* __RISCV_ASM_VDSO_ARCH_DATA_H */
