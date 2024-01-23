/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A scheduler that validates the behavior of direct dispatching with a default
 * select_cpu implementation.
 *
 * Copyright (c) 2023 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2023 David Vernet <dvernet@meta.com>
 * Copyright (c) 2023 Tejun Heo <tj@kernel.org>
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

s32 BPF_STRUCT_OPS(enq_last_no_enq_fails_init)
{
	scx_bpf_switch_all();

	return 0;
}

SEC(".struct_ops.link")
struct sched_ext_ops enq_last_no_enq_fails_ops = {
	.init			= enq_last_no_enq_fails_init,
	.name			= "enq_last_no_enq_fails",
	/* Need to define ops.enqueue() with SCX_OPS_ENQ_LAST */
	.flags			= SCX_OPS_ENQ_LAST,
	.timeout_ms		= 1000U,
};
