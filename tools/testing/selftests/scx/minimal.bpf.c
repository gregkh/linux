/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A completely minimal scheduler.
 *
 * This scheduler defines the absolute minimal set of struct sched_ext_ops
 * fields: its name (and until a bug is fixed in libbpf, also an ops.running()
 * callback). It should _not_ fail to be loaded, and can be used to exercise
 * the default scheduling paths in ext.c.
 *
 * Copyright (c) 2023 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2023 David Vernet <dvernet@meta.com>
 * Copyright (c) 2023 Tejun Heo <tj@kernel.org>
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

void BPF_STRUCT_OPS(minimal_running, struct task_struct *p)
{}

SEC(".struct_ops.link")
struct sched_ext_ops minimal_ops = {
	/*
	 * It shouldn't be necessary to define this minimal_running op, but
	 * libbpf currently expects that a struct_ops map will always have at
	 * least one struct_ops prog when loading. Until that issue is fixed,
	 * let's also define a minimal prog so that we can load and test.
	 */
	.enable			= minimal_running,
	.name			= "minimal",
};
