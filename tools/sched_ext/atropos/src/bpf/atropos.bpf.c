/* Copyright (c) Meta Platforms, Inc. and affiliates. */
/*
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 *
 * Atropos is a multi-domain BPF / userspace hybrid scheduler where the BPF
 * part does simple round robin in each domain and the userspace part
 * calculates the load factor of each domain and tells the BPF part how to load
 * balance the domains.
 *
 * Every task has an entry in the task_data map which lists which domain the
 * task belongs to. When a task first enters the system (atropos_prep_enable),
 * they are round-robined to a domain.
 *
 * atropos_select_cpu is the primary scheduling logic, invoked when a task
 * becomes runnable. The lb_data map is populated by userspace to inform the BPF
 * scheduler that a task should be migrated to a new domain. Otherwise, the task
 * is scheduled in priority order as follows:
 * * The current core if the task was woken up synchronously and there are idle
 *   cpus in the system
 * * The previous core, if idle
 * * The pinned-to core if the task is pinned to a specific core
 * * Any idle cpu in the domain
 *
 * If none of the above conditions are met, then the task is enqueued to a
 * dispatch queue corresponding to the domain (atropos_enqueue).
 *
 * atropos_dispatch will attempt to consume a task from its domain's
 * corresponding dispatch queue (this occurs after scheduling any tasks directly
 * assigned to it due to the logic in atropos_select_cpu). If no task is found,
 * then greedy load stealing will attempt to find a task on another dispatch
 * queue to run.
 *
 * Load balancing is almost entirely handled by userspace. BPF populates the
 * task weight, dom mask and current dom in the task_data map and executes the
 * load balance based on userspace populating the lb_data map.
 */
#include "../../../scx_common.bpf.h"
#include "atropos.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char _license[] SEC("license") = "GPL";

/*
 * const volatiles are set during initialization and treated as consts by the
 * jit compiler.
 */

/*
 * Domains and cpus
 */
const volatile __u32 nr_doms = 32;	/* !0 for veristat, set during init */
const volatile __u32 nr_cpus = 64;	/* !0 for veristat, set during init */
const volatile __u32 cpu_dom_id_map[MAX_CPUS];
const volatile __u64 dom_cpumasks[MAX_DOMS][MAX_CPUS / 64];

const volatile bool kthreads_local;
const volatile bool fifo_sched;
const volatile bool switch_partial;
const volatile __u32 greedy_threshold;

/* base slice duration */
const volatile __u64 slice_us = 20000;

/*
 * Exit info
 */
int exit_type = SCX_EXIT_NONE;
char exit_msg[SCX_EXIT_MSG_LEN];

struct pcpu_ctx {
	__u32 dom_rr_cur; /* used when scanning other doms */

	/* libbpf-rs does not respect the alignment, so pad out the struct explicitly */
	__u8 _padding[CACHELINE_SIZE - sizeof(u64)];
} __attribute__((aligned(CACHELINE_SIZE)));

struct pcpu_ctx pcpu_ctx[MAX_CPUS];

/*
 * Domain context
 */
struct dom_ctx {
	struct bpf_cpumask __kptr *cpumask;
	u64 vtime_now;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__type(key, u32);
	__type(value, struct dom_ctx);
	__uint(max_entries, MAX_DOMS);
	__uint(map_flags, 0);
} dom_ctx SEC(".maps");

/*
 * Statistics
 */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, ATROPOS_NR_STATS);
} stats SEC(".maps");

static inline void stat_add(enum stat_idx idx, u64 addend)
{
	u32 idx_v = idx;

	u64 *cnt_p = bpf_map_lookup_elem(&stats, &idx_v);
	if (cnt_p)
		(*cnt_p) += addend;
}

/* Map pid -> task_ctx */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, pid_t);
	__type(value, struct task_ctx);
	__uint(max_entries, 1000000);
	__uint(map_flags, 0);
} task_data SEC(".maps");

/*
 * This is populated from userspace to indicate which pids should be reassigned
 * to new doms.
 */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, pid_t);
	__type(value, u32);
	__uint(max_entries, 1000);
	__uint(map_flags, 0);
} lb_data SEC(".maps");

static inline bool vtime_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

static bool task_set_dsq(struct task_ctx *task_ctx, struct task_struct *p,
			 u32 new_dom_id)
{
	struct dom_ctx *old_domc, *new_domc;
	struct bpf_cpumask *d_cpumask, *t_cpumask;
	u32 old_dom_id = task_ctx->dom_id;
	s64 vtime_delta;

	old_domc = bpf_map_lookup_elem(&dom_ctx, &old_dom_id);
	if (!old_domc) {
		scx_bpf_error("No dom%u", old_dom_id);
		return false;
	}

	vtime_delta = p->scx.dsq_vtime - old_domc->vtime_now;

	new_domc = bpf_map_lookup_elem(&dom_ctx, &new_dom_id);
	if (!new_domc) {
		scx_bpf_error("No dom%u", new_dom_id);
		return false;
	}

	d_cpumask = new_domc->cpumask;
	if (!d_cpumask) {
		scx_bpf_error("Failed to get domain %u cpumask kptr",
			      new_dom_id);
		return false;
	}

	t_cpumask = task_ctx->cpumask;
	if (!t_cpumask) {
		scx_bpf_error("Failed to look up task cpumask");
		return false;
	}

	/*
	 * set_cpumask might have happened between userspace requesting LB and
	 * here and @p might not be able to run in @dom_id anymore. Verify.
	 */
	if (bpf_cpumask_intersects((const struct cpumask *)d_cpumask,
				   p->cpus_ptr)) {
		p->scx.dsq_vtime = new_domc->vtime_now + vtime_delta;
		task_ctx->dom_id = new_dom_id;
		bpf_cpumask_and(t_cpumask, (const struct cpumask *)d_cpumask,
				p->cpus_ptr);
	}

	return task_ctx->dom_id == new_dom_id;
}

s32 BPF_STRUCT_OPS(atropos_select_cpu, struct task_struct *p, int prev_cpu,
		   u32 wake_flags)
{
	s32 cpu;
	pid_t pid = p->pid;
	struct task_ctx *task_ctx = bpf_map_lookup_elem(&task_data, &pid);
	struct bpf_cpumask *p_cpumask;

	if (!task_ctx)
		return -ENOENT;

	if (kthreads_local &&
	    (p->flags & PF_KTHREAD) && p->nr_cpus_allowed == 1) {
		cpu = prev_cpu;
		stat_add(ATROPOS_STAT_DIRECT_DISPATCH, 1);
		goto local;
	}

	/*
	 * If WAKE_SYNC and the machine isn't fully saturated, wake up @p to the
	 * local dsq of the waker.
	 */
	if (p->nr_cpus_allowed > 1 && (wake_flags & SCX_WAKE_SYNC)) {
		struct task_struct *current = (void *)bpf_get_current_task();

		if (!(BPF_CORE_READ(current, flags) & PF_EXITING) &&
		    task_ctx->dom_id < MAX_DOMS) {
			struct dom_ctx *domc;
			struct bpf_cpumask *d_cpumask;
			const struct cpumask *idle_cpumask;
			bool has_idle;

			domc = bpf_map_lookup_elem(&dom_ctx, &task_ctx->dom_id);
			if (!domc) {
				scx_bpf_error("Failed to find dom%u",
					      task_ctx->dom_id);
				return prev_cpu;
			}
			d_cpumask = domc->cpumask;
			if (!d_cpumask) {
				scx_bpf_error("Failed to acquire domain %u cpumask kptr",
					      task_ctx->dom_id);
				return prev_cpu;
			}

			idle_cpumask = scx_bpf_get_idle_cpumask();

			has_idle = bpf_cpumask_intersects((const struct cpumask *)d_cpumask,
							  idle_cpumask);

			scx_bpf_put_idle_cpumask(idle_cpumask);

			if (has_idle) {
				cpu = bpf_get_smp_processor_id();
				if (bpf_cpumask_test_cpu(cpu, p->cpus_ptr)) {
					stat_add(ATROPOS_STAT_WAKE_SYNC, 1);
					goto local;
				}
			}
		}
	}

	/* if the previous CPU is idle, dispatch directly to it */
	if (scx_bpf_test_and_clear_cpu_idle(prev_cpu)) {
		stat_add(ATROPOS_STAT_PREV_IDLE, 1);
		cpu = prev_cpu;
		goto local;
	}

	/* If only one core is allowed, dispatch */
	if (p->nr_cpus_allowed == 1) {
		stat_add(ATROPOS_STAT_PINNED, 1);
		cpu = prev_cpu;
		goto local;
	}

	p_cpumask = task_ctx->cpumask;
	if (!p_cpumask)
		return -ENOENT;

	/* If there is an eligible idle CPU, dispatch directly */
	cpu = scx_bpf_pick_idle_cpu((const struct cpumask *)p_cpumask);
	if (cpu >= 0) {
		stat_add(ATROPOS_STAT_DIRECT_DISPATCH, 1);
		goto local;
	}

	/*
	 * @prev_cpu may be in a different domain. Returning an out-of-domain
	 * CPU can lead to stalls as all in-domain CPUs may be idle by the time
	 * @p gets enqueued.
	 */
	if (bpf_cpumask_test_cpu(prev_cpu, (const struct cpumask *)p_cpumask))
		cpu = prev_cpu;
	else
		cpu = bpf_cpumask_any((const struct cpumask *)p_cpumask);

	return cpu;

local:
	task_ctx->dispatch_local = true;
	return cpu;
}

void BPF_STRUCT_OPS(atropos_enqueue, struct task_struct *p, u32 enq_flags)
{
	pid_t pid = p->pid;
	struct task_ctx *task_ctx = bpf_map_lookup_elem(&task_data, &pid);
	u32 *new_dom;

	if (!task_ctx) {
		scx_bpf_error("No task_ctx[%d]", pid);
		return;
	}

	new_dom = bpf_map_lookup_elem(&lb_data, &pid);
	if (new_dom && *new_dom != task_ctx->dom_id &&
	    task_set_dsq(task_ctx, p, *new_dom)) {
		struct bpf_cpumask *p_cpumask;
		s32 cpu;

		stat_add(ATROPOS_STAT_LOAD_BALANCE, 1);

		/*
		 * If dispatch_local is set, We own @p's idle state but we are
		 * not gonna put the task in the associated local dsq which can
		 * cause the CPU to stall. Kick it.
		 */
		if (task_ctx->dispatch_local) {
			task_ctx->dispatch_local = false;
			scx_bpf_kick_cpu(scx_bpf_task_cpu(p), 0);
		}

		p_cpumask = task_ctx->cpumask;
		if (!p_cpumask) {
			scx_bpf_error("Failed to get task_ctx->cpumask");
			return;
		}
		cpu = scx_bpf_pick_idle_cpu((const struct cpumask *)p_cpumask);

		if (cpu >= 0)
			scx_bpf_kick_cpu(cpu, 0);
	}

	if (task_ctx->dispatch_local) {
		task_ctx->dispatch_local = false;
		scx_bpf_dispatch(p, SCX_DSQ_LOCAL, slice_us * 1000, enq_flags);
		return;
	}

	if (fifo_sched) {
		scx_bpf_dispatch(p, task_ctx->dom_id, slice_us * 1000,
				 enq_flags);
	} else {
		u64 vtime = p->scx.dsq_vtime;
		u32 dom_id = task_ctx->dom_id;
		struct dom_ctx *domc;

		domc = bpf_map_lookup_elem(&dom_ctx, &dom_id);
		if (!domc) {
			scx_bpf_error("No dom[%u]", dom_id);
			return;
		}

		/*
		 * Limit the amount of budget that an idling task can accumulate
		 * to one slice.
		 */
		if (vtime_before(vtime, domc->vtime_now - slice_us * 1000))
			vtime = domc->vtime_now - slice_us * 1000;

		scx_bpf_dispatch_vtime(p, task_ctx->dom_id, SCX_SLICE_DFL, vtime,
				       enq_flags);
	}
}

static u32 cpu_to_dom_id(s32 cpu)
{
	const volatile u32 *dom_idp;

	if (nr_doms <= 1)
		return 0;

	dom_idp = MEMBER_VPTR(cpu_dom_id_map, [cpu]);
	if (!dom_idp)
		return MAX_DOMS;

	return *dom_idp;
}

static bool cpumask_intersects_domain(const struct cpumask *cpumask, u32 dom_id)
{
	s32 cpu;

	if (dom_id >= MAX_DOMS)
		return false;

	bpf_for(cpu, 0, nr_cpus) {
		if (bpf_cpumask_test_cpu(cpu, cpumask) &&
		    (dom_cpumasks[dom_id][cpu / 64] & (1LLU << (cpu % 64))))
			return true;
	}
	return false;
}

static u32 dom_rr_next(s32 cpu)
{
	struct pcpu_ctx *pcpuc;
	u32 dom_id;

	pcpuc = MEMBER_VPTR(pcpu_ctx, [cpu]);
	if (!pcpuc)
		return 0;

	dom_id = (pcpuc->dom_rr_cur + 1) % nr_doms;

	if (dom_id == cpu_to_dom_id(cpu))
		dom_id = (dom_id + 1) % nr_doms;

	pcpuc->dom_rr_cur = dom_id;
	return dom_id;
}

void BPF_STRUCT_OPS(atropos_dispatch, s32 cpu, struct task_struct *prev)
{
	u32 dom = cpu_to_dom_id(cpu);

	if (scx_bpf_consume(dom)) {
		stat_add(ATROPOS_STAT_DSQ_DISPATCH, 1);
		return;
	}

	if (!greedy_threshold)
		return;

	bpf_repeat(nr_doms - 1) {
		u32 dom_id = dom_rr_next(cpu);

		if (scx_bpf_dsq_nr_queued(dom_id) >= greedy_threshold &&
		    scx_bpf_consume(dom_id)) {
			stat_add(ATROPOS_STAT_GREEDY, 1);
			break;
		}
	}
}

void BPF_STRUCT_OPS(atropos_runnable, struct task_struct *p, u64 enq_flags)
{
	pid_t pid = p->pid;
	struct task_ctx *task_ctx = bpf_map_lookup_elem(&task_data, &pid);

	if (!task_ctx) {
		scx_bpf_error("No task_ctx[%d]", pid);
		return;
	}

	task_ctx->runnable_at = bpf_ktime_get_ns();
}

void BPF_STRUCT_OPS(atropos_running, struct task_struct *p)
{
	struct task_ctx *taskc;
	struct dom_ctx *domc;
	pid_t pid = p->pid;
	u32 dom_id;

	if (fifo_sched)
		return;

	taskc = bpf_map_lookup_elem(&task_data, &pid);
	if (!taskc) {
		scx_bpf_error("No task_ctx[%d]", pid);
		return;
	}
	dom_id = taskc->dom_id;

	domc = bpf_map_lookup_elem(&dom_ctx, &dom_id);
	if (!domc) {
		scx_bpf_error("No dom[%u]", dom_id);
		return;
	}

	/*
	 * Global vtime always progresses forward as tasks start executing. The
	 * test and update can be performed concurrently from multiple CPUs and
	 * thus racy. Any error should be contained and temporary. Let's just
	 * live with it.
	 */
	if (vtime_before(domc->vtime_now, p->scx.dsq_vtime))
		domc->vtime_now = p->scx.dsq_vtime;
}

void BPF_STRUCT_OPS(atropos_stopping, struct task_struct *p, bool runnable)
{
	if (fifo_sched)
		return;

	/* scale the execution time by the inverse of the weight and charge */
	p->scx.dsq_vtime += (SCX_SLICE_DFL - p->scx.slice) * 100 / p->scx.weight;
}

void BPF_STRUCT_OPS(atropos_quiescent, struct task_struct *p, u64 deq_flags)
{
	pid_t pid = p->pid;
	struct task_ctx *task_ctx = bpf_map_lookup_elem(&task_data, &pid);

	if (!task_ctx) {
		scx_bpf_error("No task_ctx[%d]", pid);
		return;
	}

	task_ctx->runnable_for += bpf_ktime_get_ns() - task_ctx->runnable_at;
	task_ctx->runnable_at = 0;
}

void BPF_STRUCT_OPS(atropos_set_weight, struct task_struct *p, u32 weight)
{
	pid_t pid = p->pid;
	struct task_ctx *task_ctx = bpf_map_lookup_elem(&task_data, &pid);

	if (!task_ctx) {
		scx_bpf_error("No task_ctx[%d]", pid);
		return;
	}

	task_ctx->weight = weight;
}

struct pick_task_domain_loop_ctx {
	struct task_struct *p;
	const struct cpumask *cpumask;
	u64 dom_mask;
	u32 dom_rr_base;
	u32 dom_id;
};

static int pick_task_domain_loopfn(u32 idx, void *data)
{
	struct pick_task_domain_loop_ctx *lctx = data;
	u32 dom_id = (lctx->dom_rr_base + idx) % nr_doms;

	if (dom_id >= MAX_DOMS)
		return 1;

	if (cpumask_intersects_domain(lctx->cpumask, dom_id)) {
		lctx->dom_mask |= 1LLU << dom_id;
		if (lctx->dom_id == MAX_DOMS)
			lctx->dom_id = dom_id;
	}
	return 0;
}

static u32 pick_task_domain(struct task_ctx *task_ctx, struct task_struct *p,
			    const struct cpumask *cpumask)
{
	struct pick_task_domain_loop_ctx lctx = {
		.p = p,
		.cpumask = cpumask,
		.dom_id = MAX_DOMS,
	};
	s32 cpu = bpf_get_smp_processor_id();

	if (cpu < 0 || cpu >= MAX_CPUS)
		return MAX_DOMS;

	lctx.dom_rr_base = ++(pcpu_ctx[cpu].dom_rr_cur);

	bpf_loop(nr_doms, pick_task_domain_loopfn, &lctx, 0);
	task_ctx->dom_mask = lctx.dom_mask;

	return lctx.dom_id;
}

static void task_set_domain(struct task_ctx *task_ctx, struct task_struct *p,
			    const struct cpumask *cpumask)
{
	u32 dom_id = 0;

	if (nr_doms > 1)
		dom_id = pick_task_domain(task_ctx, p, cpumask);

	if (!task_set_dsq(task_ctx, p, dom_id))
		scx_bpf_error("Failed to set domain %d for %s[%d]",
			      dom_id, p->comm, p->pid);
}

void BPF_STRUCT_OPS(atropos_set_cpumask, struct task_struct *p,
		    const struct cpumask *cpumask)
{
	pid_t pid = p->pid;
	struct task_ctx *task_ctx = bpf_map_lookup_elem(&task_data, &pid);
	if (!task_ctx) {
		scx_bpf_error("No task_ctx[%d]", pid);
		return;
	}

	task_set_domain(task_ctx, p, cpumask);
}

s32 BPF_STRUCT_OPS(atropos_prep_enable, struct task_struct *p,
		   struct scx_enable_args *args)
{
	struct bpf_cpumask *cpumask;
	struct task_ctx task_ctx, *map_value;
	long ret;
	pid_t pid;

	memset(&task_ctx, 0, sizeof(task_ctx));

	pid = p->pid;
	ret = bpf_map_update_elem(&task_data, &pid, &task_ctx, BPF_NOEXIST);
	if (ret) {
		stat_add(ATROPOS_STAT_TASK_GET_ERR, 1);
		return ret;
	}

	/*
	 * Read the entry from the map immediately so we can add the cpumask
	 * with bpf_kptr_xchg().
	 */
	map_value = bpf_map_lookup_elem(&task_data, &pid);
	if (!map_value)
		/* Should never happen -- it was just inserted above. */
		return -EINVAL;

	cpumask = bpf_cpumask_create();
	if (!cpumask) {
		bpf_map_delete_elem(&task_data, &pid);
		return -ENOMEM;
	}

	cpumask = bpf_kptr_xchg(&map_value->cpumask, cpumask);
	if (cpumask) {
		/* Should never happen as we just inserted it above. */
		bpf_cpumask_release(cpumask);
		bpf_map_delete_elem(&task_data, &pid);
		return -EINVAL;
	}

	task_set_domain(map_value, p, p->cpus_ptr);

	return 0;
}

void BPF_STRUCT_OPS(atropos_disable, struct task_struct *p)
{
	pid_t pid = p->pid;
	long ret = bpf_map_delete_elem(&task_data, &pid);
	if (ret) {
		stat_add(ATROPOS_STAT_TASK_GET_ERR, 1);
		return;
	}
}

static int create_dom_dsq(u32 idx, void *data)
{
	struct dom_ctx domc_init = {}, *domc;
	struct bpf_cpumask *cpumask;
	u32 cpu, dom_id = idx;
	s32 ret;

	ret = scx_bpf_create_dsq(dom_id, -1);
	if (ret < 0) {
		scx_bpf_error("Failed to create dsq %u (%d)", dom_id, ret);
		return 1;
	}

	ret = bpf_map_update_elem(&dom_ctx, &dom_id, &domc_init, 0);
	if (ret) {
		scx_bpf_error("Failed to add dom_ctx entry %u (%d)", dom_id, ret);
		return 1;
	}

	domc = bpf_map_lookup_elem(&dom_ctx, &dom_id);
	if (!domc) {
		/* Should never happen, we just inserted it above. */
		scx_bpf_error("No dom%u", dom_id);
		return 1;
	}

	cpumask = bpf_cpumask_create();
	if (!cpumask) {
		scx_bpf_error("Failed to create BPF cpumask for domain %u", dom_id);
		return 1;
	}

	for (cpu = 0; cpu < MAX_CPUS; cpu++) {
		const volatile __u64 *dmask;

		dmask = MEMBER_VPTR(dom_cpumasks, [dom_id][cpu / 64]);
		if (!dmask) {
			scx_bpf_error("array index error");
			bpf_cpumask_release(cpumask);
			return 1;
		}

		if (*dmask & (1LLU << (cpu % 64)))
			bpf_cpumask_set_cpu(cpu, cpumask);
	}

	cpumask = bpf_kptr_xchg(&domc->cpumask, cpumask);
	if (cpumask) {
		scx_bpf_error("Domain %u was already present", dom_id);
		bpf_cpumask_release(cpumask);
		return 1;
	}

	return 0;
}

int BPF_STRUCT_OPS_SLEEPABLE(atropos_init)
{
	if (!switch_partial)
		scx_bpf_switch_all();

	bpf_loop(nr_doms, create_dom_dsq, NULL, 0);

	for (u32 i = 0; i < nr_cpus; i++)
		pcpu_ctx[i].dom_rr_cur = i;

	return 0;
}

void BPF_STRUCT_OPS(atropos_exit, struct scx_exit_info *ei)
{
	bpf_probe_read_kernel_str(exit_msg, sizeof(exit_msg), ei->msg);
	exit_type = ei->type;
}

SEC(".struct_ops")
struct sched_ext_ops atropos = {
	.select_cpu = (void *)atropos_select_cpu,
	.enqueue = (void *)atropos_enqueue,
	.dispatch = (void *)atropos_dispatch,
	.runnable = (void *)atropos_runnable,
	.running = (void *)atropos_running,
	.stopping = (void *)atropos_stopping,
	.quiescent = (void *)atropos_quiescent,
	.set_weight = (void *)atropos_set_weight,
	.set_cpumask = (void *)atropos_set_cpumask,
	.prep_enable = (void *)atropos_prep_enable,
	.disable = (void *)atropos_disable,
	.init = (void *)atropos_init,
	.exit = (void *)atropos_exit,
	.flags = 0,
	.name = "atropos",
};
