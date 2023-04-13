/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A simple five-level FIFO queue scheduler.
 *
 * There are five FIFOs implemented using BPF_MAP_TYPE_QUEUE. A task gets
 * assigned to one depending on its compound weight. Each CPU round robins
 * through the FIFOs and dispatches more from FIFOs with higher indices - 1 from
 * queue0, 2 from queue1, 4 from queue2 and so on.
 *
 * This scheduler demonstrates:
 *
 * - BPF-side queueing using PIDs.
 * - Sleepable per-task storage allocation using ops.prep_enable().
 *
 * This scheduler is primarily for demonstration and testing of sched_ext
 * features and unlikely to be useful for actual workloads.
 *
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */
#include "scx_common.bpf.h"
#include <linux/sched/prio.h>

char _license[] SEC("license") = "GPL";

const volatile u64 slice_ns = SCX_SLICE_DFL;
const volatile bool switch_partial;
const volatile u32 stall_user_nth;
const volatile u32 stall_kernel_nth;
const volatile u32 dsp_inf_loop_after;
const volatile s32 disallow_tgid;

u32 test_error_cnt;

struct user_exit_info uei;

struct qmap {
	__uint(type, BPF_MAP_TYPE_QUEUE);
	__uint(max_entries, 4096);
	__type(value, u32);
} queue0 SEC(".maps"),
  queue1 SEC(".maps"),
  queue2 SEC(".maps"),
  queue3 SEC(".maps"),
  queue4 SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY_OF_MAPS);
	__uint(max_entries, 5);
	__type(key, int);
	__array(values, struct qmap);
} queue_arr SEC(".maps") = {
	.values = {
		[0] = &queue0,
		[1] = &queue1,
		[2] = &queue2,
		[3] = &queue3,
		[4] = &queue4,
	},
};

/* Per-task scheduling context */
struct task_ctx {
	bool	force_local;	/* Dispatch directly to local_dsq */
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

/* Per-cpu dispatch index and remaining count */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 2);
	__type(key, u32);
	__type(value, u64);
} dispatch_idx_cnt SEC(".maps");

/* Statistics */
unsigned long nr_enqueued, nr_dispatched, nr_dequeued;

s32 BPF_STRUCT_OPS(qmap_select_cpu, struct task_struct *p,
		   s32 prev_cpu, u64 wake_flags)
{
	struct task_ctx *tctx;
	s32 cpu;

	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0, 0);
	if (!tctx) {
		scx_bpf_error("task_ctx lookup failed");
		return -ESRCH;
	}

	if (p->nr_cpus_allowed == 1 ||
	    scx_bpf_test_and_clear_cpu_idle(prev_cpu)) {
		tctx->force_local = true;
		return prev_cpu;
	}

	cpu = scx_bpf_pick_idle_cpu(p->cpus_ptr);
	if (cpu >= 0)
		return cpu;

	return prev_cpu;
}

static int weight_to_idx(u32 weight)
{
	/* Coarsely map the compound weight to a FIFO. */
	if (weight <= 25)
		return 0;
	else if (weight <= 50)
		return 1;
	else if (weight < 200)
		return 2;
	else if (weight < 400)
		return 3;
	else
		return 4;
}

void BPF_STRUCT_OPS(qmap_enqueue, struct task_struct *p, u64 enq_flags)
{
	static u32 user_cnt, kernel_cnt;
	struct task_ctx *tctx;
	u32 pid = p->pid;
	int idx = weight_to_idx(p->scx.weight);
	void *ring;

	if (p->flags & PF_KTHREAD) {
		if (stall_kernel_nth && !(++kernel_cnt % stall_kernel_nth))
			return;
	} else {
		if (stall_user_nth && !(++user_cnt % stall_user_nth))
			return;
	}

	if (test_error_cnt && !--test_error_cnt)
		scx_bpf_error("test triggering error");

	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0, 0);
	if (!tctx) {
		scx_bpf_error("task_ctx lookup failed");
		return;
	}

	/* Is select_cpu() is telling us to enqueue locally? */
	if (tctx->force_local) {
		tctx->force_local = false;
		scx_bpf_dispatch(p, SCX_DSQ_LOCAL, slice_ns, enq_flags);
		return;
	}

	ring = bpf_map_lookup_elem(&queue_arr, &idx);
	if (!ring) {
		scx_bpf_error("failed to find ring %d", idx);
		return;
	}

	/* Queue on the selected FIFO. If the FIFO overflows, punt to global. */
	if (bpf_map_push_elem(ring, &pid, 0)) {
		scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, slice_ns, enq_flags);
		return;
	}

	__sync_fetch_and_add(&nr_enqueued, 1);
}

/*
 * The BPF queue map doesn't support removal and sched_ext can handle spurious
 * dispatches. qmap_dequeue() is only used to collect statistics.
 */
void BPF_STRUCT_OPS(qmap_dequeue, struct task_struct *p, u64 deq_flags)
{
	__sync_fetch_and_add(&nr_dequeued, 1);
}

void BPF_STRUCT_OPS(qmap_dispatch, s32 cpu, struct task_struct *prev)
{
	u32 zero = 0, one = 1;
	u64 *idx = bpf_map_lookup_elem(&dispatch_idx_cnt, &zero);
	u64 *cnt = bpf_map_lookup_elem(&dispatch_idx_cnt, &one);
	void *fifo;
	s32 pid;
	int i;

	if (dsp_inf_loop_after && nr_dispatched > dsp_inf_loop_after) {
		struct task_struct *p;

		/*
		 * PID 2 should be kthreadd which should mostly be idle and off
		 * the scheduler. Let's keep dispatching it to force the kernel
		 * to call this function over and over again.
		 */
		p = bpf_task_from_pid(2);
		if (p) {
			scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, slice_ns, 0);
			bpf_task_release(p);
			return;
		}
	}

	if (!idx || !cnt) {
		scx_bpf_error("failed to lookup idx[%p], cnt[%p]", idx, cnt);
		return;
	}

	for (i = 0; i < 5; i++) {
		/* Advance the dispatch cursor and pick the fifo. */
		if (!*cnt) {
			*idx = (*idx + 1) % 5;
			*cnt = 1 << *idx;
		}
		(*cnt)--;

		fifo = bpf_map_lookup_elem(&queue_arr, idx);
		if (!fifo) {
			scx_bpf_error("failed to find ring %llu", *idx);
			return;
		}

		/* Dispatch or advance. */
		if (!bpf_map_pop_elem(fifo, &pid)) {
			struct task_struct *p;

			p = bpf_task_from_pid(pid);
			if (p) {
				__sync_fetch_and_add(&nr_dispatched, 1);
				scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, slice_ns, 0);
				bpf_task_release(p);
				return;
			}
		}

		*cnt = 0;
	}
}

s32 BPF_STRUCT_OPS(qmap_prep_enable, struct task_struct *p,
		   struct scx_enable_args *args)
{
	if (p->tgid == disallow_tgid)
		p->scx.disallow = true;

	/*
	 * @p is new. Let's ensure that its task_ctx is available. We can sleep
	 * in this function and the following will automatically use GFP_KERNEL.
	 */
	if (bpf_task_storage_get(&task_ctx_stor, p, 0,
				 BPF_LOCAL_STORAGE_GET_F_CREATE))
		return 0;
	else
		return -ENOMEM;
}

s32 BPF_STRUCT_OPS(qmap_init)
{
	if (!switch_partial)
		scx_bpf_switch_all();
	return 0;
}

void BPF_STRUCT_OPS(qmap_exit, struct scx_exit_info *ei)
{
	uei_record(&uei, ei);
}

SEC(".struct_ops")
struct sched_ext_ops qmap_ops = {
	.select_cpu		= (void *)qmap_select_cpu,
	.enqueue		= (void *)qmap_enqueue,
	.dequeue		= (void *)qmap_dequeue,
	.dispatch		= (void *)qmap_dispatch,
	.prep_enable		= (void *)qmap_prep_enable,
	.init			= (void *)qmap_init,
	.exit			= (void *)qmap_exit,
	.timeout_ms		= 5000U,
	.name			= "qmap",
};
