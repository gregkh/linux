/* SPDX-License-Identifier: GPL-2.0 */
/*
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
};

enum scx_deq_flags {
	/* expose select DEQUEUE_* flags as enums */
	SCX_DEQ_SLEEP		= DEQUEUE_SLEEP,
};

#ifdef CONFIG_SCHED_CLASS_EXT

extern const struct sched_class ext_sched_class;
extern const struct bpf_verifier_ops bpf_sched_ext_verifier_ops;
extern const struct file_operations sched_ext_fops;
extern unsigned long scx_watchdog_timeout;
extern unsigned long scx_watchdog_timestamp;

DECLARE_STATIC_KEY_FALSE(__scx_ops_enabled);
#define scx_enabled()		static_branch_unlikely(&__scx_ops_enabled)

bool task_on_scx(struct task_struct *p);
void scx_pre_fork(struct task_struct *p);
int scx_fork(struct task_struct *p);
void scx_post_fork(struct task_struct *p);
void scx_cancel_fork(struct task_struct *p);
int scx_check_setscheduler(struct task_struct *p, int policy);
void init_sched_ext_class(void);

__printf(2, 3) void scx_ops_error_type(enum scx_exit_type type,
				       const char *fmt, ...);
#define scx_ops_error(fmt, args...)						\
	scx_ops_error_type(SCX_EXIT_ERROR, fmt, ##args)

static inline void scx_notify_sched_tick(void)
{
	unsigned long last_check;

	if (!scx_enabled())
		return;

	last_check = scx_watchdog_timestamp;
	if (unlikely(time_after(jiffies, last_check + scx_watchdog_timeout))) {
		u32 dur_ms = jiffies_to_msecs(jiffies - last_check);

		scx_ops_error_type(SCX_EXIT_ERROR_STALL,
				   "watchdog failed to check in for %u.%03us",
				   dur_ms / 1000, dur_ms % 1000);
	}
}

static inline const struct sched_class *next_active_class(const struct sched_class *class)
{
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

#else	/* CONFIG_SCHED_CLASS_EXT */

#define scx_enabled()		false

static inline void scx_pre_fork(struct task_struct *p) {}
static inline int scx_fork(struct task_struct *p) { return 0; }
static inline void scx_post_fork(struct task_struct *p) {}
static inline void scx_cancel_fork(struct task_struct *p) {}
static inline int scx_check_setscheduler(struct task_struct *p,
					 int policy) { return 0; }
static inline void init_sched_ext_class(void) {}
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
