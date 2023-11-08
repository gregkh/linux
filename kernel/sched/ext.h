/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */
enum scx_wake_flags {
	/* expose select WF_* flags as enums */
	SCX_WAKE_EXEC		= WF_EXEC,
	SCX_WAKE_FORK		= WF_FORK,
	SCX_WAKE_TTWU		= WF_TTWU,
	SCX_WAKE_SYNC		= WF_SYNC,
};

enum scx_enq_flags {
	/* expose select ENQUEUE_* flags as enums */
	SCX_ENQ_WAKEUP		= ENQUEUE_WAKEUP,
	SCX_ENQ_HEAD		= ENQUEUE_HEAD,

	/* high 32bits are SCX specific */

	/*
	 * Set the following to trigger preemption when calling
	 * scx_bpf_dispatch() with a local dsq as the target. The slice of the
	 * current task is cleared to zero and the CPU is kicked into the
	 * scheduling path. Implies %SCX_ENQ_HEAD.
	 */
	SCX_ENQ_PREEMPT		= 1LLU << 32,

	/*
	 * The task being enqueued was previously enqueued on the current CPU's
	 * %SCX_DSQ_LOCAL, but was removed from it in a call to the
	 * bpf_scx_reenqueue_local() kfunc. If bpf_scx_reenqueue_local() was
	 * invoked in a ->cpu_release() callback, and the task is again
	 * dispatched back to %SCX_LOCAL_DSQ by this current ->enqueue(), the
	 * task will not be scheduled on the CPU until at least the next invocation
	 * of the ->cpu_acquire() callback.
	 */
	SCX_ENQ_REENQ		= 1LLU << 40,

	/*
	 * The task being enqueued is the only task available for the cpu. By
	 * default, ext core keeps executing such tasks but when
	 * %SCX_OPS_ENQ_LAST is specified, they're ops.enqueue()'d with
	 * %SCX_ENQ_LAST and %SCX_ENQ_LOCAL flags set.
	 *
	 * If the BPF scheduler wants to continue executing the task,
	 * ops.enqueue() should dispatch the task to %SCX_DSQ_LOCAL immediately.
	 * If the task gets queued on a different dsq or the BPF side, the BPF
	 * scheduler is responsible for triggering a follow-up scheduling event.
	 * Otherwise, Execution may stall.
	 */
	SCX_ENQ_LAST		= 1LLU << 41,

	/*
	 * A hint indicating that it's advisable to enqueue the task on the
	 * local dsq of the currently selected CPU. Currently used by
	 * select_cpu_dfl() and together with %SCX_ENQ_LAST.
	 */
	SCX_ENQ_LOCAL		= 1LLU << 42,

	/* high 8 bits are internal */
	__SCX_ENQ_INTERNAL_MASK	= 0xffLLU << 56,

	SCX_ENQ_CLEAR_OPSS	= 1LLU << 56,
	SCX_ENQ_DSQ_PRIQ	= 1LLU << 57,
};

enum scx_deq_flags {
	/* expose select DEQUEUE_* flags as enums */
	SCX_DEQ_SLEEP		= DEQUEUE_SLEEP,

	/* high 32bits are SCX specific */

	/*
	 * The generic core-sched layer decided to execute the task even though
	 * it hasn't been dispatched yet. Dequeue from the BPF side.
	 */
	SCX_DEQ_CORE_SCHED_EXEC	= 1LLU << 32,
};

enum scx_pick_idle_cpu_flags {
	SCX_PICK_IDLE_CORE	= 1LLU << 0,	/* pick a CPU whose SMT siblings are also idle */
};

enum scx_kick_flags {
	SCX_KICK_PREEMPT	= 1LLU << 0,	/* force scheduling on the CPU */
	SCX_KICK_WAIT		= 1LLU << 1,	/* wait for the CPU to be rescheduled */
};

enum scx_tg_flags {
	SCX_TG_ONLINE		= 1U << 0,
	SCX_TG_INITED		= 1U << 1,
};

#ifdef CONFIG_SCHED_CLASS_EXT

struct sched_enq_and_set_ctx {
	struct task_struct	*p;
	int			queue_flags;
	bool			queued;
	bool			running;
};

void sched_deq_and_put_task(struct task_struct *p, int queue_flags,
			    struct sched_enq_and_set_ctx *ctx);
void sched_enq_and_set_task(struct sched_enq_and_set_ctx *ctx);

extern const struct sched_class ext_sched_class;
extern const struct bpf_verifier_ops bpf_sched_ext_verifier_ops;
extern const struct file_operations sched_ext_fops;
extern unsigned long scx_watchdog_timeout;
extern unsigned long scx_watchdog_timestamp;

DECLARE_STATIC_KEY_FALSE(__scx_ops_enabled);
DECLARE_STATIC_KEY_FALSE(__scx_switched_all);
#define scx_enabled()		static_branch_unlikely(&__scx_ops_enabled)
#define scx_switched_all()	static_branch_unlikely(&__scx_switched_all)

DECLARE_STATIC_KEY_FALSE(scx_ops_cpu_preempt);

static inline bool task_on_scx(const struct task_struct *p)
{
	return scx_enabled() && p->sched_class == &ext_sched_class;
}

bool task_should_scx(struct task_struct *p);
void scx_pre_fork(struct task_struct *p);
int scx_fork(struct task_struct *p);
void scx_post_fork(struct task_struct *p);
void scx_cancel_fork(struct task_struct *p);
int scx_check_setscheduler(struct task_struct *p, int policy);
bool scx_can_stop_tick(struct rq *rq);
void init_sched_ext_class(void);

__printf(2, 3) void scx_ops_error_kind(enum scx_exit_kind kind,
				       const char *fmt, ...);
#define scx_ops_error(fmt, args...)						\
	scx_ops_error_kind(SCX_EXIT_ERROR, fmt, ##args)

void __scx_notify_pick_next_task(struct rq *rq,
				 struct task_struct *p,
				 const struct sched_class *active);

static inline void scx_notify_pick_next_task(struct rq *rq,
					     struct task_struct *p,
					     const struct sched_class *active)
{
	if (!scx_enabled())
		return;
#ifdef CONFIG_SMP
	/*
	 * Pairs with the smp_load_acquire() issued by a CPU in
	 * kick_cpus_irq_workfn() who is waiting for this CPU to perform a
	 * resched.
	 */
	smp_store_release(&rq->scx.pnt_seq, rq->scx.pnt_seq + 1);
#endif
	if (!static_branch_unlikely(&scx_ops_cpu_preempt))
		return;
	__scx_notify_pick_next_task(rq, p, active);
}

static inline void scx_notify_sched_tick(void)
{
	unsigned long last_check;

	if (!scx_enabled())
		return;

	last_check = scx_watchdog_timestamp;
	if (unlikely(time_after(jiffies, last_check + scx_watchdog_timeout))) {
		u32 dur_ms = jiffies_to_msecs(jiffies - last_check);

		scx_ops_error_kind(SCX_EXIT_ERROR_STALL,
				   "watchdog failed to check in for %u.%03us",
				   dur_ms / 1000, dur_ms % 1000);
	}
}

static inline const struct sched_class *next_active_class(const struct sched_class *class)
{
	class++;
	if (scx_switched_all() && class == &fair_sched_class)
		class++;
	if (!scx_enabled() && class == &ext_sched_class)
		class++;
	return class;
}

#define for_active_class_range(class, _from, _to)				\
	for (class = (_from); class != (_to); class = next_active_class(class))

#define for_each_active_class(class)						\
	for_active_class_range(class, __sched_class_highest, __sched_class_lowest)

/*
 * SCX requires a balance() call before every pick_next_task() call including
 * when waking up from idle.
 */
#define for_balance_class_range(class, prev_class, end_class)			\
	for_active_class_range(class, (prev_class) > &ext_sched_class ?		\
			       &ext_sched_class : (prev_class), (end_class))

#ifdef CONFIG_SCHED_CORE
bool scx_prio_less(const struct task_struct *a, const struct task_struct *b,
		   bool in_fi);
#endif

#else	/* CONFIG_SCHED_CLASS_EXT */

#define scx_enabled()		false
#define scx_switched_all()	false

static inline bool task_on_scx(const struct task_struct *p) { return false; }
static inline void scx_pre_fork(struct task_struct *p) {}
static inline int scx_fork(struct task_struct *p) { return 0; }
static inline void scx_post_fork(struct task_struct *p) {}
static inline void scx_cancel_fork(struct task_struct *p) {}
static inline int scx_check_setscheduler(struct task_struct *p,
					 int policy) { return 0; }
static inline bool scx_can_stop_tick(struct rq *rq) { return true; }
static inline void init_sched_ext_class(void) {}
static inline void scx_notify_pick_next_task(struct rq *rq,
					     const struct task_struct *p,
					     const struct sched_class *active) {}
static inline void scx_notify_sched_tick(void) {}

#define for_each_active_class		for_each_class
#define for_balance_class_range		for_class_range

#endif	/* CONFIG_SCHED_CLASS_EXT */

#if defined(CONFIG_SCHED_CLASS_EXT) && defined(CONFIG_SMP)
void __scx_update_idle(struct rq *rq, bool idle);

static inline void scx_update_idle(struct rq *rq, bool idle)
{
	if (scx_enabled())
		__scx_update_idle(rq, idle);
}
#else
static inline void scx_update_idle(struct rq *rq, bool idle) {}
#endif

#ifdef CONFIG_CGROUP_SCHED
#ifdef CONFIG_EXT_GROUP_SCHED
int scx_tg_online(struct task_group *tg);
void scx_tg_offline(struct task_group *tg);
int scx_cgroup_can_attach(struct cgroup_taskset *tset);
void scx_move_task(struct task_struct *p);
void scx_cgroup_finish_attach(void);
void scx_cgroup_cancel_attach(struct cgroup_taskset *tset);
void scx_group_set_weight(struct task_group *tg, unsigned long cgrp_weight);
#else	/* CONFIG_EXT_GROUP_SCHED */
static inline int scx_tg_online(struct task_group *tg) { return 0; }
static inline void scx_tg_offline(struct task_group *tg) {}
static inline int scx_cgroup_can_attach(struct cgroup_taskset *tset) { return 0; }
static inline void scx_move_task(struct task_struct *p) {}
static inline void scx_cgroup_finish_attach(void) {}
static inline void scx_cgroup_cancel_attach(struct cgroup_taskset *tset) {}
static inline void scx_group_set_weight(struct task_group *tg, unsigned long cgrp_weight) {}
#endif	/* CONFIG_EXT_GROUP_SCHED */
#endif	/* CONFIG_CGROUP_SCHED */
