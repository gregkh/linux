/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */

#include <linux/btf.h>

#define SCX_OP_IDX(op)		(offsetof(struct sched_ext_ops, op) / sizeof(void (*)(void)))

enum scx_internal_consts {
	SCX_OPI_BEGIN			= 0,
	SCX_OPI_NORMAL_BEGIN		= 0,
	SCX_OPI_NORMAL_END		= SCX_OP_IDX(cpu_online),
	SCX_OPI_CPU_HOTPLUG_BEGIN	= SCX_OP_IDX(cpu_online),
	SCX_OPI_CPU_HOTPLUG_END		= SCX_OP_IDX(init),
	SCX_OPI_END			= SCX_OP_IDX(init),
	SCX_DSP_DFL_MAX_BATCH		= 32,
	SCX_DSP_MAX_LOOPS		= 32,
	SCX_WATCHDOG_MAX_TIMEOUT	= 30 * HZ,

	SCX_EXIT_BT_LEN			= 64,
	SCX_EXIT_MSG_LEN		= 1024,
	SCX_EXIT_DUMP_LEN		= 32768,
};

enum scx_ops_enable_state {
	SCX_OPS_PREPPING,
	SCX_OPS_ENABLING,
	SCX_OPS_ENABLED,
	SCX_OPS_DISABLING,
	SCX_OPS_DISABLED,
};

static const char *scx_ops_enable_state_str[] = {
	[SCX_OPS_PREPPING]	= "prepping",
	[SCX_OPS_ENABLING]	= "enabling",
	[SCX_OPS_ENABLED]	= "enabled",
	[SCX_OPS_DISABLING]	= "disabling",
	[SCX_OPS_DISABLED]	= "disabled",
};

/*
 * sched_ext_entity->ops_state
 *
 * Used to track the task ownership between the SCX core and the BPF scheduler.
 * State transitions look as follows:
 *
 * NONE -> QUEUEING -> QUEUED -> DISPATCHING
 *   ^              |                 |
 *   |              v                 v
 *   \-------------------------------/
 *
 * QUEUEING and DISPATCHING states can be waited upon. See wait_ops_state() call
 * sites for explanations on the conditions being waited upon and why they are
 * safe. Transitions out of them into NONE or QUEUED must store_release and the
 * waiters should load_acquire.
 *
 * Tracking scx_ops_state enables sched_ext core to reliably determine whether
 * any given task can be dispatched by the BPF scheduler at all times and thus
 * relaxes the requirements on the BPF scheduler. This allows the BPF scheduler
 * to try to dispatch any task anytime regardless of its state as the SCX core
 * can safely reject invalid dispatches.
 */
enum scx_ops_state {
	SCX_OPSS_NONE,		/* owned by the SCX core */
	SCX_OPSS_QUEUEING,	/* in transit to the BPF scheduler */
	SCX_OPSS_QUEUED,	/* owned by the BPF scheduler */
	SCX_OPSS_DISPATCHING,	/* in transit back to the SCX core */

	/*
	 * QSEQ brands each QUEUED instance so that, when dispatch races
	 * dequeue/requeue, the dispatcher can tell whether it still has a claim
	 * on the task being dispatched.
	 *
	 * As some 32bit archs can't do 64bit store_release/load_acquire,
	 * p->scx.ops_state is atomic_long_t which leaves 30 bits for QSEQ on
	 * 32bit machines. The dispatch race window QSEQ protects is very narrow
	 * and runs with IRQ disabled. 30 bits should be sufficient.
	 */
	SCX_OPSS_QSEQ_SHIFT	= 2,
};

/* Use macros to ensure that the type is unsigned long for the masks */
#define SCX_OPSS_STATE_MASK	((1LU << SCX_OPSS_QSEQ_SHIFT) - 1)
#define SCX_OPSS_QSEQ_MASK	(~SCX_OPSS_STATE_MASK)

/*
 * During exit, a task may schedule after losing its PIDs. When disabling the
 * BPF scheduler, we need to be able to iterate tasks in every state to
 * guarantee system safety. Maintain a dedicated task list which contains every
 * task between its fork and eventual free.
 */
static DEFINE_SPINLOCK(scx_tasks_lock);
static LIST_HEAD(scx_tasks);

/* ops enable/disable */
static struct kthread_worker *scx_ops_helper;
static DEFINE_MUTEX(scx_ops_enable_mutex);
DEFINE_STATIC_KEY_FALSE(__scx_ops_enabled);
DEFINE_STATIC_PERCPU_RWSEM(scx_fork_rwsem);
static atomic_t scx_ops_enable_state_var = ATOMIC_INIT(SCX_OPS_DISABLED);
static atomic_t scx_ops_bypass_depth = ATOMIC_INIT(0);
static bool scx_switch_all_req;
static bool scx_switching_all;
DEFINE_STATIC_KEY_FALSE(__scx_switched_all);

static struct sched_ext_ops scx_ops;
static bool scx_warned_zero_slice;

static DEFINE_STATIC_KEY_FALSE(scx_ops_enq_last);
static DEFINE_STATIC_KEY_FALSE(scx_ops_enq_exiting);
DEFINE_STATIC_KEY_FALSE(scx_ops_cpu_preempt);
static DEFINE_STATIC_KEY_FALSE(scx_builtin_idle_enabled);

struct static_key_false scx_has_op[SCX_OPI_END] =
	{ [0 ... SCX_OPI_END-1] = STATIC_KEY_FALSE_INIT };

static atomic_t scx_exit_kind = ATOMIC_INIT(SCX_EXIT_DONE);
static struct scx_exit_info *scx_exit_info;

static atomic_long_t scx_nr_rejected = ATOMIC_LONG_INIT(0);

/*
 * The maximum amount of time in jiffies that a task may be runnable without
 * being scheduled on a CPU. If this timeout is exceeded, it will trigger
 * scx_ops_error().
 */
unsigned long scx_watchdog_timeout;

/*
 * The last time the delayed work was run. This delayed work relies on
 * ksoftirqd being able to run to service timer interrupts, so it's possible
 * that this work itself could get wedged. To account for this, we check that
 * it's not stalled in the timer tick, and trigger an error if it is.
 */
unsigned long scx_watchdog_timestamp = INITIAL_JIFFIES;

static struct delayed_work scx_watchdog_work;

/* idle tracking */
#ifdef CONFIG_SMP
#ifdef CONFIG_CPUMASK_OFFSTACK
#define CL_ALIGNED_IF_ONSTACK
#else
#define CL_ALIGNED_IF_ONSTACK __cacheline_aligned_in_smp
#endif

static struct {
	cpumask_var_t cpu;
	cpumask_var_t smt;
} idle_masks CL_ALIGNED_IF_ONSTACK;

#endif	/* CONFIG_SMP */

/* for %SCX_KICK_WAIT */
static unsigned long __percpu *scx_kick_cpus_pnt_seqs;

/*
 * Direct dispatch marker.
 *
 * Non-NULL values are used for direct dispatch from enqueue path. A valid
 * pointer points to the task currently being enqueued. An ERR_PTR value is used
 * to indicate that direct dispatch has already happened.
 */
static DEFINE_PER_CPU(struct task_struct *, direct_dispatch_task);

/* dispatch queues */
static struct scx_dispatch_q __cacheline_aligned_in_smp scx_dsq_global;

static const struct rhashtable_params dsq_hash_params = {
	.key_len		= 8,
	.key_offset		= offsetof(struct scx_dispatch_q, id),
	.head_offset		= offsetof(struct scx_dispatch_q, hash_node),
};

static struct rhashtable dsq_hash;
static LLIST_HEAD(dsqs_to_free);

/* dispatch buf */
struct scx_dsp_buf_ent {
	struct task_struct	*task;
	unsigned long		qseq;
	u64			dsq_id;
	u64			enq_flags;
};

static u32 scx_dsp_max_batch;
static struct scx_dsp_buf_ent __percpu *scx_dsp_buf;

struct scx_dsp_ctx {
	struct rq		*rq;
	struct rq_flags		*rf;
	u32			buf_cursor;
	u32			nr_tasks;
};

static DEFINE_PER_CPU(struct scx_dsp_ctx, scx_dsp_ctx);

/* /sys/kernel/sched_ext interface */
static struct kset *scx_kset;
static struct kobject *scx_root_kobj;

static void scx_bpf_dispatch(struct task_struct *p, u64 dsq_id, u64 slice,
			     u64 enq_flags);
static void scx_bpf_kick_cpu(s32 cpu, u64 flags);

struct scx_task_iter {
	struct sched_ext_entity		cursor;
	struct task_struct		*locked;
	struct rq			*rq;
	struct rq_flags			rf;
};

#define SCX_HAS_OP(op)	static_branch_likely(&scx_has_op[SCX_OP_IDX(op)])

static long jiffies_delta_msecs(unsigned long at, unsigned long now)
{
	if (time_after(at, now))
		return jiffies_to_msecs(at - now);
	else
		return -(long)jiffies_to_msecs(now - at);
}

/* if the highest set bit is N, return a mask with bits [N+1, 31] set */
static u32 higher_bits(u32 flags)
{
	return ~((1 << fls(flags)) - 1);
}

/* return the mask with only the highest bit set */
static u32 highest_bit(u32 flags)
{
	int bit = fls(flags);
	return bit ? 1 << (bit - 1) : 0;
}

/*
 * scx_kf_mask enforcement. Some kfuncs can only be called from specific SCX
 * ops. When invoking SCX ops, SCX_CALL_OP[_RET]() should be used to indicate
 * the allowed kfuncs and those kfuncs should use scx_kf_allowed() to check
 * whether it's running from an allowed context.
 *
 * @mask is constant, always inline to cull the mask calculations.
 */
static __always_inline void scx_kf_allow(u32 mask)
{
	/* nesting is allowed only in increasing scx_kf_mask order */
	WARN_ONCE((mask | higher_bits(mask)) & current->scx.kf_mask,
		  "invalid nesting current->scx.kf_mask=0x%x mask=0x%x\n",
		  current->scx.kf_mask, mask);
	current->scx.kf_mask |= mask;
}

static void scx_kf_disallow(u32 mask)
{
	current->scx.kf_mask &= ~mask;
}

#define SCX_CALL_OP(mask, op, args...)						\
do {										\
	if (mask) {								\
		scx_kf_allow(mask);						\
		scx_ops.op(args);						\
		scx_kf_disallow(mask);						\
	} else {								\
		scx_ops.op(args);						\
	}									\
} while (0)

#define SCX_CALL_OP_RET(mask, op, args...)					\
({										\
	__typeof__(scx_ops.op(args)) __ret;					\
	if (mask) {								\
		scx_kf_allow(mask);						\
		__ret = scx_ops.op(args);					\
		scx_kf_disallow(mask);						\
	} else {								\
		__ret = scx_ops.op(args);					\
	}									\
	__ret;									\
})

/*
 * Some kfuncs are allowed only on the tasks that are subjects of the
 * in-progress scx_ops operation for, e.g., locking guarantees. To enforce such
 * restrictions, the following SCX_CALL_OP_*() variants should be used when
 * invoking scx_ops operations that take task arguments. These can only be used
 * for non-nesting operations due to the way the tasks are tracked.
 *
 * kfuncs which can only operate on such tasks can in turn use
 * scx_kf_allowed_on_arg_tasks() to test whether the invocation is allowed on
 * the specific task.
 */
#define SCX_CALL_OP_TASK(mask, op, task, args...)				\
do {										\
	BUILD_BUG_ON((mask) & ~__SCX_KF_TERMINAL);				\
	current->scx.kf_tasks[0] = task;					\
	SCX_CALL_OP(mask, op, task, ##args);					\
	current->scx.kf_tasks[0] = NULL;					\
} while (0)

#define SCX_CALL_OP_TASK_RET(mask, op, task, args...)				\
({										\
	__typeof__(scx_ops.op(task, ##args)) __ret;				\
	BUILD_BUG_ON((mask) & ~__SCX_KF_TERMINAL);				\
	current->scx.kf_tasks[0] = task;					\
	__ret = SCX_CALL_OP_RET(mask, op, task, ##args);			\
	current->scx.kf_tasks[0] = NULL;					\
	__ret;									\
})

#define SCX_CALL_OP_2TASKS_RET(mask, op, task0, task1, args...)			\
({										\
	__typeof__(scx_ops.op(task0, task1, ##args)) __ret;			\
	BUILD_BUG_ON((mask) & ~__SCX_KF_TERMINAL);				\
	current->scx.kf_tasks[0] = task0;					\
	current->scx.kf_tasks[1] = task1;					\
	__ret = SCX_CALL_OP_RET(mask, op, task0, task1, ##args);		\
	current->scx.kf_tasks[0] = NULL;					\
	current->scx.kf_tasks[1] = NULL;					\
	__ret;									\
})

/* @mask is constant, always inline to cull unnecessary branches */
static __always_inline bool scx_kf_allowed(u32 mask)
{
	if (unlikely(!(current->scx.kf_mask & mask))) {
		scx_ops_error("kfunc with mask 0x%x called from an operation only allowing 0x%x",
			      mask, current->scx.kf_mask);
		return false;
	}

	if (unlikely((mask & (SCX_KF_INIT | SCX_KF_SLEEPABLE)) &&
		     in_interrupt())) {
		scx_ops_error("sleepable kfunc called from non-sleepable context");
		return false;
	}

	/*
	 * Enforce nesting boundaries. e.g. A kfunc which can be called from
	 * DISPATCH must not be called if we're running DEQUEUE which is nested
	 * inside ops.dispatch(). We don't need to check the SCX_KF_SLEEPABLE
	 * boundary thanks to the above in_interrupt() check.
	 */
	if (unlikely(highest_bit(mask) == SCX_KF_CPU_RELEASE &&
		     (current->scx.kf_mask & higher_bits(SCX_KF_CPU_RELEASE)))) {
		scx_ops_error("cpu_release kfunc called from a nested operation");
		return false;
	}

	if (unlikely(highest_bit(mask) == SCX_KF_DISPATCH &&
		     (current->scx.kf_mask & higher_bits(SCX_KF_DISPATCH)))) {
		scx_ops_error("dispatch kfunc called from a nested operation");
		return false;
	}

	return true;
}

/* see SCX_CALL_OP_TASK() */
static __always_inline bool scx_kf_allowed_on_arg_tasks(u32 mask,
							struct task_struct *p)
{
	if (!scx_kf_allowed(__SCX_KF_RQ_LOCKED))
		return false;

	if (unlikely((p != current->scx.kf_tasks[0] &&
		      p != current->scx.kf_tasks[1]))) {
		scx_ops_error("called on a task not being operated on");
		return false;
	}

	return true;
}

/**
 * scx_task_iter_init - Initialize a task iterator
 * @iter: iterator to init
 *
 * Initialize @iter. Must be called with scx_tasks_lock held. Once initialized,
 * @iter must eventually be exited with scx_task_iter_exit().
 *
 * scx_tasks_lock may be released between this and the first next() call or
 * between any two next() calls. If scx_tasks_lock is released between two
 * next() calls, the caller is responsible for ensuring that the task being
 * iterated remains accessible either through RCU read lock or obtaining a
 * reference count.
 *
 * All tasks which existed when the iteration started are guaranteed to be
 * visited as long as they still exist.
 */
static void scx_task_iter_init(struct scx_task_iter *iter)
{
	lockdep_assert_held(&scx_tasks_lock);

	iter->cursor = (struct sched_ext_entity){ .flags = SCX_TASK_CURSOR };
	list_add(&iter->cursor.tasks_node, &scx_tasks);
	iter->locked = NULL;
}

/**
 * scx_task_iter_exit - Exit a task iterator
 * @iter: iterator to exit
 *
 * Exit a previously initialized @iter. Must be called with scx_tasks_lock held.
 * If the iterator holds a task's rq lock, that rq lock is released. See
 * scx_task_iter_init() for details.
 */
static void scx_task_iter_exit(struct scx_task_iter *iter)
{
	struct list_head *cursor = &iter->cursor.tasks_node;

	lockdep_assert_held(&scx_tasks_lock);

	if (iter->locked) {
		task_rq_unlock(iter->rq, iter->locked, &iter->rf);
		iter->locked = NULL;
	}

	if (list_empty(cursor))
		return;

	list_del_init(cursor);
}

/**
 * scx_task_iter_next - Next task
 * @iter: iterator to walk
 *
 * Visit the next task. See scx_task_iter_init() for details.
 */
static struct task_struct *scx_task_iter_next(struct scx_task_iter *iter)
{
	struct list_head *cursor = &iter->cursor.tasks_node;
	struct sched_ext_entity *pos;

	lockdep_assert_held(&scx_tasks_lock);

	list_for_each_entry(pos, cursor, tasks_node) {
		if (&pos->tasks_node == &scx_tasks)
			return NULL;
		if (!(pos->flags & SCX_TASK_CURSOR)) {
			list_move(cursor, &pos->tasks_node);
			return container_of(pos, struct task_struct, scx);
		}
	}

	/* can't happen, should always terminate at scx_tasks above */
	BUG();
}

/**
 * scx_task_iter_next_filtered - Next non-idle task
 * @iter: iterator to walk
 *
 * Visit the next non-idle task. See scx_task_iter_init() for details.
 */
static struct task_struct *
scx_task_iter_next_filtered(struct scx_task_iter *iter)
{
	struct task_struct *p;

	while ((p = scx_task_iter_next(iter))) {
		/*
		 * is_idle_task() tests %PF_IDLE which may not be set for CPUs
		 * which haven't yet been onlined. Test sched_class directly.
		 */
		if (p->sched_class != &idle_sched_class)
			return p;
	}
	return NULL;
}

/**
 * scx_task_iter_next_filtered_locked - Next non-idle task with its rq locked
 * @iter: iterator to walk
 *
 * Visit the next non-idle task with its rq lock held. See scx_task_iter_init()
 * for details.
 */
static struct task_struct *
scx_task_iter_next_filtered_locked(struct scx_task_iter *iter)
{
	struct task_struct *p;

	if (iter->locked) {
		task_rq_unlock(iter->rq, iter->locked, &iter->rf);
		iter->locked = NULL;
	}

	p = scx_task_iter_next_filtered(iter);
	if (!p)
		return NULL;

	iter->rq = task_rq_lock(p, &iter->rf);
	iter->locked = p;
	return p;
}

static enum scx_ops_enable_state scx_ops_enable_state(void)
{
	return atomic_read(&scx_ops_enable_state_var);
}

static enum scx_ops_enable_state
scx_ops_set_enable_state(enum scx_ops_enable_state to)
{
	return atomic_xchg(&scx_ops_enable_state_var, to);
}

static bool scx_ops_tryset_enable_state(enum scx_ops_enable_state to,
					enum scx_ops_enable_state from)
{
	int from_v = from;

	return atomic_try_cmpxchg(&scx_ops_enable_state_var, &from_v, to);
}

static bool scx_ops_bypassing(void)
{
	return unlikely(atomic_read(&scx_ops_bypass_depth));
}

/**
 * wait_ops_state - Busy-wait the specified ops state to end
 * @p: target task
 * @opss: state to wait the end of
 *
 * Busy-wait for @p to transition out of @opss. This can only be used when the
 * state part of @opss is %SCX_QUEUEING or %SCX_DISPATCHING. This function also
 * has load_acquire semantics to ensure that the caller can see the updates made
 * in the enqueueing and dispatching paths.
 */
static void wait_ops_state(struct task_struct *p, unsigned long opss)
{
	do {
		cpu_relax();
	} while (atomic_long_read_acquire(&p->scx.ops_state) == opss);
}

/**
 * ops_cpu_valid - Verify a cpu number
 * @cpu: cpu number which came from a BPF ops
 *
 * @cpu is a cpu number which came from the BPF scheduler and can be any value.
 * Verify that it is in range and one of the possible cpus.
 */
static bool ops_cpu_valid(s32 cpu)
{
	return likely(cpu >= 0 && cpu < nr_cpu_ids && cpu_possible(cpu));
}

/**
 * ops_sanitize_err - Sanitize a -errno value
 * @ops_name: operation to blame on failure
 * @err: -errno value to sanitize
 *
 * Verify @err is a valid -errno. If not, trigger scx_ops_error() and return
 * -%EPROTO. This is necessary because returning a rogue -errno up the chain
 * can cause misbehaviors. For an example, a large negative return from
 * ops.init_task() triggers an oops when passed up the call chain because the
 * value fails IS_ERR() test after being encoded with ERR_PTR() and then is
 * handled as a pointer.
 */
static int ops_sanitize_err(const char *ops_name, s32 err)
{
	if (err < 0 && err >= -MAX_ERRNO)
		return err;

	scx_ops_error("ops.%s() returned an invalid errno %d", ops_name, err);
	return -EPROTO;
}

/**
 * touch_core_sched - Update timestamp used for core-sched task ordering
 * @rq: rq to read clock from, must be locked
 * @p: task to update the timestamp for
 *
 * Update @p->scx.core_sched_at timestamp. This is used by scx_prio_less() to
 * implement global or local-DSQ FIFO ordering for core-sched. Should be called
 * when a task becomes runnable and its turn on the CPU ends (e.g. slice
 * exhaustion).
 */
static void touch_core_sched(struct rq *rq, struct task_struct *p)
{
#ifdef CONFIG_SCHED_CORE
	/*
	 * It's okay to update the timestamp spuriously. Use
	 * sched_core_disabled() which is cheaper than enabled().
	 */
	if (!sched_core_disabled())
		p->scx.core_sched_at = rq_clock_task(rq);
#endif
}

/**
 * touch_core_sched_dispatch - Update core-sched timestamp on dispatch
 * @rq: rq to read clock from, must be locked
 * @p: task being dispatched
 *
 * If the BPF scheduler implements custom core-sched ordering via
 * ops.core_sched_before(), @p->scx.core_sched_at is used to implement FIFO
 * ordering within each local DSQ. This function is called from dispatch paths
 * and updates @p->scx.core_sched_at if custom core-sched ordering is in effect.
 */
static void touch_core_sched_dispatch(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_rq_held(rq);
	assert_clock_updated(rq);

#ifdef CONFIG_SCHED_CORE
	if (SCX_HAS_OP(core_sched_before))
		touch_core_sched(rq, p);
#endif
}

static void update_curr_scx(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 now = rq_clock_task(rq);
	u64 delta_exec;

	if (time_before_eq64(now, curr->se.exec_start))
		return;

	delta_exec = now - curr->se.exec_start;
	curr->se.exec_start = now;
	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);
	cgroup_account_cputime(curr, delta_exec);

	if (curr->scx.slice != SCX_SLICE_INF) {
		curr->scx.slice -= min(curr->scx.slice, delta_exec);
		if (!curr->scx.slice)
			touch_core_sched(rq, curr);
	}
}

static bool scx_dsq_priq_less(struct rb_node *node_a,
			      const struct rb_node *node_b)
{
	const struct task_struct *a =
		container_of(node_a, struct task_struct, scx.dsq_node.priq);
	const struct task_struct *b =
		container_of(node_b, struct task_struct, scx.dsq_node.priq);

	return time_before64(a->scx.dsq_vtime, b->scx.dsq_vtime);
}

static void dispatch_enqueue(struct scx_dispatch_q *dsq, struct task_struct *p,
			     u64 enq_flags)
{
	bool is_local = dsq->id == SCX_DSQ_LOCAL;

	WARN_ON_ONCE(p->scx.dsq || !list_empty(&p->scx.dsq_node.fifo));
	WARN_ON_ONCE((p->scx.dsq_flags & SCX_TASK_DSQ_ON_PRIQ) ||
		     !RB_EMPTY_NODE(&p->scx.dsq_node.priq));

	if (!is_local) {
		raw_spin_lock(&dsq->lock);
		if (unlikely(dsq->id == SCX_DSQ_INVALID)) {
			scx_ops_error("attempting to dispatch to a destroyed dsq");
			/* fall back to the global dsq */
			raw_spin_unlock(&dsq->lock);
			dsq = &scx_dsq_global;
			raw_spin_lock(&dsq->lock);
		}
	}

	if (unlikely((dsq->id & SCX_DSQ_FLAG_BUILTIN) &&
		     (enq_flags & SCX_ENQ_DSQ_PRIQ))) {
		/*
		 * SCX_DSQ_LOCAL and SCX_DSQ_GLOBAL DSQs always consume from
		 * their FIFO queues. To avoid confusion and accidentally
		 * starving vtime-dispatched tasks by FIFO-dispatched tasks, we
		 * disallow any internal DSQ from doing vtime ordering of
		 * tasks.
		 */
		scx_ops_error("Cannot use vtime ordering for built-in DSQs");
		enq_flags &= ~SCX_ENQ_DSQ_PRIQ;
	}

	if (enq_flags & SCX_ENQ_DSQ_PRIQ) {
		p->scx.dsq_flags |= SCX_TASK_DSQ_ON_PRIQ;
		rb_add_cached(&p->scx.dsq_node.priq, &dsq->priq,
			      scx_dsq_priq_less);
		/* A DSQ should only be using either FIFO or PRIQ enqueuing. */
		if (unlikely(!list_empty(&dsq->fifo)))
			scx_ops_error("DSQ ID 0x%016llx already had FIFO-enqueued tasks",
				      dsq->id);
	} else {
		if (enq_flags & (SCX_ENQ_HEAD | SCX_ENQ_PREEMPT))
			list_add(&p->scx.dsq_node.fifo, &dsq->fifo);
		else
			list_add_tail(&p->scx.dsq_node.fifo, &dsq->fifo);
		/* A DSQ should only be using either FIFO or PRIQ enqueuing. */
		if (unlikely(rb_first_cached(&dsq->priq)))
			scx_ops_error("DSQ ID 0x%016llx already had PRIQ-enqueued tasks",
				      dsq->id);
	}
	dsq->nr++;
	p->scx.dsq = dsq;

	/*
	 * scx.ddsp_dsq_id and scx.ddsp_enq_flags are only relevant on the
	 * direct dispatch path, but we clear them here because the direct
	 * dispatch verdict may be overridden on the enqueue path during e.g.
	 * bypass.
	 */
	p->scx.ddsp_dsq_id = SCX_DSQ_INVALID;
	p->scx.ddsp_enq_flags = 0;

	/*
	 * We're transitioning out of QUEUEING or DISPATCHING. store_release to
	 * match waiters' load_acquire.
	 */
	if (enq_flags & SCX_ENQ_CLEAR_OPSS)
		atomic_long_set_release(&p->scx.ops_state, SCX_OPSS_NONE);

	if (is_local) {
		struct rq *rq = container_of(dsq, struct rq, scx.local_dsq);
		bool preempt = false;

		if ((enq_flags & SCX_ENQ_PREEMPT) && p != rq->curr &&
		    rq->curr->sched_class == &ext_sched_class) {
			rq->curr->scx.slice = 0;
			preempt = true;
		}

		if (preempt || sched_class_above(&ext_sched_class,
						 rq->curr->sched_class))
			resched_curr(rq);
	} else {
		raw_spin_unlock(&dsq->lock);
	}
}

static void task_unlink_from_dsq(struct task_struct *p,
				 struct scx_dispatch_q *dsq)
{
	if (p->scx.dsq_flags & SCX_TASK_DSQ_ON_PRIQ) {
		rb_erase_cached(&p->scx.dsq_node.priq, &dsq->priq);
		RB_CLEAR_NODE(&p->scx.dsq_node.priq);
		p->scx.dsq_flags &= ~SCX_TASK_DSQ_ON_PRIQ;
	} else {
		list_del_init(&p->scx.dsq_node.fifo);
	}
}

static bool task_linked_on_dsq(struct task_struct *p)
{
	return !list_empty(&p->scx.dsq_node.fifo) ||
		!RB_EMPTY_NODE(&p->scx.dsq_node.priq);
}

static void dispatch_dequeue(struct scx_rq *scx_rq, struct task_struct *p)
{
	struct scx_dispatch_q *dsq = p->scx.dsq;
	bool is_local = dsq == &scx_rq->local_dsq;

	if (!dsq) {
		WARN_ON_ONCE(task_linked_on_dsq(p));
		/*
		 * When dispatching directly from the BPF scheduler to a local
		 * DSQ, the task isn't associated with any DSQ but
		 * @p->scx.holding_cpu may be set under the protection of
		 * %SCX_OPSS_DISPATCHING.
		 */
		if (p->scx.holding_cpu >= 0)
			p->scx.holding_cpu = -1;
		return;
	}

	if (!is_local)
		raw_spin_lock(&dsq->lock);

	/*
	 * Now that we hold @dsq->lock, @p->holding_cpu and @p->scx.dsq_node
	 * can't change underneath us.
	*/
	if (p->scx.holding_cpu < 0) {
		/* @p must still be on @dsq, dequeue */
		WARN_ON_ONCE(!task_linked_on_dsq(p));
		task_unlink_from_dsq(p, dsq);
		dsq->nr--;
	} else {
		/*
		 * We're racing against dispatch_to_local_dsq() which already
		 * removed @p from @dsq and set @p->scx.holding_cpu. Clear the
		 * holding_cpu which tells dispatch_to_local_dsq() that it lost
		 * the race.
		 */
		WARN_ON_ONCE(task_linked_on_dsq(p));
		p->scx.holding_cpu = -1;
	}
	p->scx.dsq = NULL;

	if (!is_local)
		raw_spin_unlock(&dsq->lock);
}

static struct scx_dispatch_q *find_non_local_dsq(u64 dsq_id)
{
	lockdep_assert(rcu_read_lock_any_held());

	if (dsq_id == SCX_DSQ_GLOBAL)
		return &scx_dsq_global;
	else
		return rhashtable_lookup_fast(&dsq_hash, &dsq_id,
					      dsq_hash_params);
}

static struct scx_dispatch_q *find_dsq_for_dispatch(struct rq *rq, u64 dsq_id,
						    struct task_struct *p)
{
	struct scx_dispatch_q *dsq;

	if (dsq_id == SCX_DSQ_LOCAL)
		return &rq->scx.local_dsq;

	dsq = find_non_local_dsq(dsq_id);
	if (unlikely(!dsq)) {
		scx_ops_error("non-existent DSQ 0x%llx for %s[%d]",
			      dsq_id, p->comm, p->pid);
		return &scx_dsq_global;
	}

	return dsq;
}

static void mark_direct_dispatch(struct task_struct *ddsp_task,
				 struct task_struct *p, u64 dsq_id,
				 u64 enq_flags)
{
	/*
	 * Mark that dispatch already happened from ops.select_cpu() or
	 * ops.enqueue() by spoiling direct_dispatch_task with a non-NULL value
	 * which can never match a valid task pointer.
	 */
	__this_cpu_write(direct_dispatch_task, ERR_PTR(-ESRCH));

	/* @p must match the task on the enqueue path */
	if (unlikely(p != ddsp_task)) {
		if (IS_ERR(ddsp_task))
			scx_ops_error("%s[%d] already direct-dispatched",
				      p->comm, p->pid);
		else
			scx_ops_error("scheduling for %s[%d] but trying to direct-dispatch %s[%d]",
				      ddsp_task->comm, ddsp_task->pid,
				      p->comm, p->pid);
		return;
	}

	/*
	 * %SCX_DSQ_LOCAL_ON is not supported during direct dispatch because
	 * dispatching to the local DSQ of a different CPU requires unlocking
	 * the current rq which isn't allowed in the enqueue path. Use
	 * ops.select_cpu() to be on the target CPU and then %SCX_DSQ_LOCAL.
	 */
	if (unlikely((dsq_id & SCX_DSQ_LOCAL_ON) == SCX_DSQ_LOCAL_ON)) {
		scx_ops_error("SCX_DSQ_LOCAL_ON can't be used for direct-dispatch");
		return;
	}

	WARN_ON_ONCE(p->scx.ddsp_dsq_id != SCX_DSQ_INVALID);
	WARN_ON_ONCE(p->scx.ddsp_enq_flags);

	p->scx.ddsp_dsq_id = dsq_id;
	p->scx.ddsp_enq_flags = enq_flags;
}

static void direct_dispatch(struct task_struct *p, u64 enq_flags)
{
	struct scx_dispatch_q *dsq;

	touch_core_sched_dispatch(task_rq(p), p);

	enq_flags |= (p->scx.ddsp_enq_flags | SCX_ENQ_CLEAR_OPSS);
	dsq = find_dsq_for_dispatch(task_rq(p), p->scx.ddsp_dsq_id, p);
	dispatch_enqueue(dsq, p, enq_flags);
}

static bool test_rq_online(struct rq *rq)
{
#ifdef CONFIG_SMP
	return rq->online;
#else
	return true;
#endif
}

static void do_enqueue_task(struct rq *rq, struct task_struct *p, u64 enq_flags,
			    int sticky_cpu)
{
	struct task_struct **ddsp_taskp;
	unsigned long qseq;

	WARN_ON_ONCE(!(p->scx.flags & SCX_TASK_QUEUED));

	/* rq migration */
	if (sticky_cpu == cpu_of(rq))
		goto local_norefill;

	/*
	 * If !rq->online, we already told the BPF scheduler that the CPU is
	 * offline. We're just trying to on/offline the CPU. Don't bother the
	 * BPF scheduler.
	 */
	if (unlikely(!test_rq_online(rq)))
		goto local;

	if (scx_ops_bypassing()) {
		if (enq_flags & SCX_ENQ_LAST)
			goto local;
		else
			goto global;
	}

	if (p->scx.ddsp_dsq_id != SCX_DSQ_INVALID)
		goto direct;

	/* see %SCX_OPS_ENQ_EXITING */
	if (!static_branch_unlikely(&scx_ops_enq_exiting) &&
	    unlikely(p->flags & PF_EXITING))
		goto local;

	/* see %SCX_OPS_ENQ_LAST */
	if (!static_branch_unlikely(&scx_ops_enq_last) &&
	    (enq_flags & SCX_ENQ_LAST))
		goto local;

	if (!SCX_HAS_OP(enqueue))
		goto global;

	/* DSQ bypass didn't trigger, enqueue on the BPF scheduler */
	qseq = rq->scx.ops_qseq++ << SCX_OPSS_QSEQ_SHIFT;

	WARN_ON_ONCE(atomic_long_read(&p->scx.ops_state) != SCX_OPSS_NONE);
	atomic_long_set(&p->scx.ops_state, SCX_OPSS_QUEUEING | qseq);

	ddsp_taskp = this_cpu_ptr(&direct_dispatch_task);
	WARN_ON_ONCE(*ddsp_taskp);
	*ddsp_taskp = p;

	SCX_CALL_OP_TASK(SCX_KF_ENQUEUE, enqueue, p, enq_flags);

	*ddsp_taskp = NULL;
	if (p->scx.ddsp_dsq_id != SCX_DSQ_INVALID)
		goto direct;

	/*
	 * If not directly dispatched, QUEUEING isn't clear yet and dispatch or
	 * dequeue may be waiting. The store_release matches their load_acquire.
	 */
	atomic_long_set_release(&p->scx.ops_state, SCX_OPSS_QUEUED | qseq);
	return;

direct:
	direct_dispatch(p, enq_flags);
	return;

local:
	/*
	 * For task-ordering, slice refill must be treated as implying the end
	 * of the current slice. Otherwise, the longer @p stays on the CPU, the
	 * higher priority it becomes from scx_prio_less()'s POV.
	 */
	touch_core_sched(rq, p);
	p->scx.slice = SCX_SLICE_DFL;
local_norefill:
	dispatch_enqueue(&rq->scx.local_dsq, p, enq_flags);
	return;

global:
	touch_core_sched(rq, p);	/* see the comment in local: */
	p->scx.slice = SCX_SLICE_DFL;
	dispatch_enqueue(&scx_dsq_global, p, enq_flags);
}

static bool task_runnable(const struct task_struct *p)
{
	return !list_empty(&p->scx.runnable_node);
}

static void set_task_runnable(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_rq_held(rq);

	if (p->scx.flags & SCX_TASK_RESET_RUNNABLE_AT) {
		p->scx.runnable_at = jiffies;
		p->scx.flags &= ~SCX_TASK_RESET_RUNNABLE_AT;
	}

	/*
	 * list_add_tail() must be used. scx_ops_bypass() depends on tasks being
	 * appened to the runnable_list.
	 */
	list_add_tail(&p->scx.runnable_node, &rq->scx.runnable_list);
}

static void clr_task_runnable(struct task_struct *p, bool reset_runnable_at)
{
	list_del_init(&p->scx.runnable_node);
	if (reset_runnable_at)
		p->scx.flags |= SCX_TASK_RESET_RUNNABLE_AT;
}

static void enqueue_task_scx(struct rq *rq, struct task_struct *p, int enq_flags)
{
	int sticky_cpu = p->scx.sticky_cpu;

	enq_flags |= rq->scx.extra_enq_flags;

	if (sticky_cpu >= 0)
		p->scx.sticky_cpu = -1;

	/*
	 * Restoring a running task will be immediately followed by
	 * set_next_task_scx() which expects the task to not be on the BPF
	 * scheduler as tasks can only start running through local DSQs. Force
	 * direct-dispatch into the local DSQ by setting the sticky_cpu.
	 */
	if (unlikely(enq_flags & ENQUEUE_RESTORE) && task_current(rq, p))
		sticky_cpu = cpu_of(rq);

	if (p->scx.flags & SCX_TASK_QUEUED) {
		WARN_ON_ONCE(!task_runnable(p));
		return;
	}

	set_task_runnable(rq, p);
	p->scx.flags |= SCX_TASK_QUEUED;
	rq->scx.nr_running++;
	add_nr_running(rq, 1);

	if (SCX_HAS_OP(runnable))
		SCX_CALL_OP_TASK(SCX_KF_REST, runnable, p, enq_flags);

	if (enq_flags & SCX_ENQ_WAKEUP)
		touch_core_sched(rq, p);

	do_enqueue_task(rq, p, enq_flags, sticky_cpu);
}

static void ops_dequeue(struct task_struct *p, u64 deq_flags)
{
	unsigned long opss;

	/* dequeue is always temporary, don't reset runnable_at */
	clr_task_runnable(p, false);

	/* acquire ensures that we see the preceding updates on QUEUED */
	opss = atomic_long_read_acquire(&p->scx.ops_state);

	switch (opss & SCX_OPSS_STATE_MASK) {
	case SCX_OPSS_NONE:
		break;
	case SCX_OPSS_QUEUEING:
		/*
		 * QUEUEING is started and finished while holding @p's rq lock.
		 * As we're holding the rq lock now, we shouldn't see QUEUEING.
		 */
		BUG();
	case SCX_OPSS_QUEUED:
		if (SCX_HAS_OP(dequeue))
			SCX_CALL_OP_TASK(SCX_KF_REST, dequeue, p, deq_flags);

		if (atomic_long_try_cmpxchg(&p->scx.ops_state, &opss,
					    SCX_OPSS_NONE))
			break;
		fallthrough;
	case SCX_OPSS_DISPATCHING:
		/*
		 * If @p is being dispatched from the BPF scheduler to a DSQ,
		 * wait for the transfer to complete so that @p doesn't get
		 * added to its DSQ after dequeueing is complete.
		 *
		 * As we're waiting on DISPATCHING with the rq locked, the
		 * dispatching side shouldn't try to lock the rq while
		 * DISPATCHING is set. See dispatch_to_local_dsq().
		 *
		 * DISPATCHING shouldn't have qseq set and control can reach
		 * here with NONE @opss from the above QUEUED case block.
		 * Explicitly wait on %SCX_OPSS_DISPATCHING instead of @opss.
		 */
		wait_ops_state(p, SCX_OPSS_DISPATCHING);
		BUG_ON(atomic_long_read(&p->scx.ops_state) != SCX_OPSS_NONE);
		break;
	}
}

static void dequeue_task_scx(struct rq *rq, struct task_struct *p, int deq_flags)
{
	struct scx_rq *scx_rq = &rq->scx;

	if (!(p->scx.flags & SCX_TASK_QUEUED)) {
		WARN_ON_ONCE(task_runnable(p));
		return;
	}

	ops_dequeue(p, deq_flags);

	/*
	 * A currently running task which is going off @rq first gets dequeued
	 * and then stops running. As we want running <-> stopping transitions
	 * to be contained within runnable <-> quiescent transitions, trigger
	 * ->stopping() early here instead of in put_prev_task_scx().
	 *
	 * @p may go through multiple stopping <-> running transitions between
	 * here and put_prev_task_scx() if task attribute changes occur while
	 * balance_scx() leaves @rq unlocked. However, they don't contain any
	 * information meaningful to the BPF scheduler and can be suppressed by
	 * skipping the callbacks if the task is !QUEUED.
	 */
	if (SCX_HAS_OP(stopping) && task_current(rq, p)) {
		update_curr_scx(rq);
		SCX_CALL_OP_TASK(SCX_KF_REST, stopping, p, false);
	}

	if (SCX_HAS_OP(quiescent))
		SCX_CALL_OP_TASK(SCX_KF_REST, quiescent, p, deq_flags);

	if (deq_flags & SCX_DEQ_SLEEP)
		p->scx.flags |= SCX_TASK_DEQD_FOR_SLEEP;
	else
		p->scx.flags &= ~SCX_TASK_DEQD_FOR_SLEEP;

	p->scx.flags &= ~SCX_TASK_QUEUED;
	scx_rq->nr_running--;
	sub_nr_running(rq, 1);

	dispatch_dequeue(scx_rq, p);
}

static void yield_task_scx(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	if (SCX_HAS_OP(yield))
		SCX_CALL_OP_2TASKS_RET(SCX_KF_REST, yield, p, NULL);
	else
		p->scx.slice = 0;
}

static bool yield_to_task_scx(struct rq *rq, struct task_struct *to)
{
	struct task_struct *from = rq->curr;

	if (SCX_HAS_OP(yield))
		return SCX_CALL_OP_2TASKS_RET(SCX_KF_REST, yield, from, to);
	else
		return false;
}

#ifdef CONFIG_SMP
/**
 * move_task_to_local_dsq - Move a task from a different rq to a local DSQ
 * @rq: rq to move the task into, currently locked
 * @p: task to move
 * @enq_flags: %SCX_ENQ_*
 *
 * Move @p which is currently on a different rq to @rq's local DSQ. The caller
 * must:
 *
 * 1. Start with exclusive access to @p either through its DSQ lock or
 *    %SCX_OPSS_DISPATCHING flag.
 *
 * 2. Set @p->scx.holding_cpu to raw_smp_processor_id().
 *
 * 3. Remember task_rq(@p). Release the exclusive access so that we don't
 *    deadlock with dequeue.
 *
 * 4. Lock @rq and the task_rq from #3.
 *
 * 5. Call this function.
 *
 * Returns %true if @p was successfully moved. %false after racing dequeue and
 * losing.
 */
static bool move_task_to_local_dsq(struct rq *rq, struct task_struct *p,
				   u64 enq_flags)
{
	struct rq *task_rq;

	lockdep_assert_rq_held(rq);

	/*
	 * If dequeue got to @p while we were trying to lock both rq's, it'd
	 * have cleared @p->scx.holding_cpu to -1. While other cpus may have
	 * updated it to different values afterwards, as this operation can't be
	 * preempted or recurse, @p->scx.holding_cpu can never become
	 * raw_smp_processor_id() again before we're done. Thus, we can tell
	 * whether we lost to dequeue by testing whether @p->scx.holding_cpu is
	 * still raw_smp_processor_id().
	 *
	 * See dispatch_dequeue() for the counterpart.
	 */
	if (unlikely(p->scx.holding_cpu != raw_smp_processor_id()))
		return false;

	/* @p->rq couldn't have changed if we're still the holding cpu */
	task_rq = task_rq(p);
	lockdep_assert_rq_held(task_rq);

	WARN_ON_ONCE(!cpumask_test_cpu(cpu_of(rq), p->cpus_ptr));
	deactivate_task(task_rq, p, 0);
	set_task_cpu(p, cpu_of(rq));
	p->scx.sticky_cpu = cpu_of(rq);

	/*
	 * We want to pass scx-specific enq_flags but activate_task() will
	 * truncate the upper 32 bit. As we own @rq, we can pass them through
	 * @rq->scx.extra_enq_flags instead.
	 */
	WARN_ON_ONCE(rq->scx.extra_enq_flags);
	rq->scx.extra_enq_flags = enq_flags;
	activate_task(rq, p, 0);
	rq->scx.extra_enq_flags = 0;

	return true;
}

/**
 * dispatch_to_local_dsq_lock - Ensure source and desitnation rq's are locked
 * @rq: current rq which is locked
 * @rf: rq_flags to use when unlocking @rq
 * @src_rq: rq to move task from
 * @dst_rq: rq to move task to
 *
 * We're holding @rq lock and trying to dispatch a task from @src_rq to
 * @dst_rq's local DSQ and thus need to lock both @src_rq and @dst_rq. Whether
 * @rq stays locked isn't important as long as the state is restored after
 * dispatch_to_local_dsq_unlock().
 */
static void dispatch_to_local_dsq_lock(struct rq *rq, struct rq_flags *rf,
				       struct rq *src_rq, struct rq *dst_rq)
{
	rq_unpin_lock(rq, rf);

	if (src_rq == dst_rq) {
		raw_spin_rq_unlock(rq);
		raw_spin_rq_lock(dst_rq);
	} else if (rq == src_rq) {
		double_lock_balance(rq, dst_rq);
		rq_repin_lock(rq, rf);
	} else if (rq == dst_rq) {
		double_lock_balance(rq, src_rq);
		rq_repin_lock(rq, rf);
	} else {
		raw_spin_rq_unlock(rq);
		double_rq_lock(src_rq, dst_rq);
	}
}

/**
 * dispatch_to_local_dsq_unlock - Undo dispatch_to_local_dsq_lock()
 * @rq: current rq which is locked
 * @rf: rq_flags to use when unlocking @rq
 * @src_rq: rq to move task from
 * @dst_rq: rq to move task to
 *
 * Unlock @src_rq and @dst_rq and ensure that @rq is locked on return.
 */
static void dispatch_to_local_dsq_unlock(struct rq *rq, struct rq_flags *rf,
					 struct rq *src_rq, struct rq *dst_rq)
{
	if (src_rq == dst_rq) {
		raw_spin_rq_unlock(dst_rq);
		raw_spin_rq_lock(rq);
		rq_repin_lock(rq, rf);
	} else if (rq == src_rq) {
		double_unlock_balance(rq, dst_rq);
	} else if (rq == dst_rq) {
		double_unlock_balance(rq, src_rq);
	} else {
		double_rq_unlock(src_rq, dst_rq);
		raw_spin_rq_lock(rq);
		rq_repin_lock(rq, rf);
	}
}
#endif	/* CONFIG_SMP */


static bool task_can_run_on_rq(struct task_struct *p, struct rq *rq)
{
	return likely(test_rq_online(rq)) && !is_migration_disabled(p) &&
		cpumask_test_cpu(cpu_of(rq), p->cpus_ptr);
}

static bool consume_dispatch_q(struct rq *rq, struct rq_flags *rf,
			       struct scx_dispatch_q *dsq)
{
	struct scx_rq *scx_rq = &rq->scx;
	struct task_struct *p;
	struct rb_node *rb_node;
	struct rq *task_rq;
	bool moved = false;
retry:
	if (list_empty(&dsq->fifo) && !rb_first_cached(&dsq->priq))
		return false;

	raw_spin_lock(&dsq->lock);

	list_for_each_entry(p, &dsq->fifo, scx.dsq_node.fifo) {
		task_rq = task_rq(p);
		if (rq == task_rq)
			goto this_rq;
		if (task_can_run_on_rq(p, rq))
			goto remote_rq;
	}

	for (rb_node = rb_first_cached(&dsq->priq); rb_node;
	     rb_node = rb_next(rb_node)) {
		p = container_of(rb_node, struct task_struct, scx.dsq_node.priq);
		task_rq = task_rq(p);
		if (rq == task_rq)
			goto this_rq;
		if (task_can_run_on_rq(p, rq))
			goto remote_rq;
	}

	raw_spin_unlock(&dsq->lock);
	return false;

this_rq:
	/* @dsq is locked and @p is on this rq */
	WARN_ON_ONCE(p->scx.holding_cpu >= 0);
	task_unlink_from_dsq(p, dsq);
	list_add_tail(&p->scx.dsq_node.fifo, &scx_rq->local_dsq.fifo);
	dsq->nr--;
	scx_rq->local_dsq.nr++;
	p->scx.dsq = &scx_rq->local_dsq;
	raw_spin_unlock(&dsq->lock);
	return true;

remote_rq:
#ifdef CONFIG_SMP
	/*
	 * @dsq is locked and @p is on a remote rq. @p is currently protected by
	 * @dsq->lock. We want to pull @p to @rq but may deadlock if we grab
	 * @task_rq while holding @dsq and @rq locks. As dequeue can't drop the
	 * rq lock or fail, do a little dancing from our side. See
	 * move_task_to_local_dsq().
	 */
	WARN_ON_ONCE(p->scx.holding_cpu >= 0);
	task_unlink_from_dsq(p, dsq);
	dsq->nr--;
	p->scx.holding_cpu = raw_smp_processor_id();
	raw_spin_unlock(&dsq->lock);

	rq_unpin_lock(rq, rf);
	double_lock_balance(rq, task_rq);
	rq_repin_lock(rq, rf);

	moved = move_task_to_local_dsq(rq, p, 0);

	double_unlock_balance(rq, task_rq);
#endif /* CONFIG_SMP */
	if (likely(moved))
		return true;
	goto retry;
}

enum dispatch_to_local_dsq_ret {
	DTL_DISPATCHED,		/* successfully dispatched */
	DTL_LOST,		/* lost race to dequeue */
	DTL_NOT_LOCAL,		/* destination is not a local DSQ */
	DTL_INVALID,		/* invalid local dsq_id */
};

/**
 * dispatch_to_local_dsq - Dispatch a task to a local dsq
 * @rq: current rq which is locked
 * @rf: rq_flags to use when unlocking @rq
 * @dsq_id: destination dsq ID
 * @p: task to dispatch
 * @enq_flags: %SCX_ENQ_*
 *
 * We're holding @rq lock and want to dispatch @p to the local DSQ identified by
 * @dsq_id. This function performs all the synchronization dancing needed
 * because local DSQs are protected with rq locks.
 *
 * The caller must have exclusive ownership of @p (e.g. through
 * %SCX_OPSS_DISPATCHING).
 */
static enum dispatch_to_local_dsq_ret
dispatch_to_local_dsq(struct rq *rq, struct rq_flags *rf, u64 dsq_id,
		      struct task_struct *p, u64 enq_flags)
{
	struct rq *src_rq = task_rq(p);
	struct rq *dst_rq;

	/*
	 * We're synchronized against dequeue through DISPATCHING. As @p can't
	 * be dequeued, its task_rq and cpus_allowed are stable too.
	 */
	if (dsq_id == SCX_DSQ_LOCAL) {
		dst_rq = rq;
	} else if ((dsq_id & SCX_DSQ_LOCAL_ON) == SCX_DSQ_LOCAL_ON) {
		s32 cpu = dsq_id & SCX_DSQ_LOCAL_CPU_MASK;

		if (!ops_cpu_valid(cpu)) {
			scx_ops_error("invalid cpu %d in SCX_DSQ_LOCAL_ON verdict for %s[%d]",
				      cpu, p->comm, p->pid);
			return DTL_INVALID;
		}
		dst_rq = cpu_rq(cpu);
	} else {
		return DTL_NOT_LOCAL;
	}

	/* if dispatching to @rq that @p is already on, no lock dancing needed */
	if (rq == src_rq && rq == dst_rq) {
		dispatch_enqueue(&dst_rq->scx.local_dsq, p,
				 enq_flags | SCX_ENQ_CLEAR_OPSS);
		return DTL_DISPATCHED;
	}

#ifdef CONFIG_SMP
	if (cpumask_test_cpu(cpu_of(dst_rq), p->cpus_ptr)) {
		struct rq *locked_dst_rq = dst_rq;
		bool dsp;

		/*
		 * @p is on a possibly remote @src_rq which we need to lock to
		 * move the task. If dequeue is in progress, it'd be locking
		 * @src_rq and waiting on DISPATCHING, so we can't grab @src_rq
		 * lock while holding DISPATCHING.
		 *
		 * As DISPATCHING guarantees that @p is wholly ours, we can
		 * pretend that we're moving from a DSQ and use the same
		 * mechanism - mark the task under transfer with holding_cpu,
		 * release DISPATCHING and then follow the same protocol.
		 */
		p->scx.holding_cpu = raw_smp_processor_id();

		/* store_release ensures that dequeue sees the above */
		atomic_long_set_release(&p->scx.ops_state, SCX_OPSS_NONE);

		dispatch_to_local_dsq_lock(rq, rf, src_rq, locked_dst_rq);

		/*
		 * We don't require the BPF scheduler to avoid dispatching to
		 * offline CPUs mostly for convenience but also because CPUs can
		 * go offline between scx_bpf_dispatch() calls and here. If @p
		 * is destined to an offline CPU, queue it on its current CPU
		 * instead, which should always be safe. As this is an allowed
		 * behavior, don't trigger an ops error.
		 */
		if (unlikely(!test_rq_online(dst_rq)))
			dst_rq = src_rq;

		if (src_rq == dst_rq) {
			/*
			 * As @p is staying on the same rq, there's no need to
			 * go through the full deactivate/activate cycle.
			 * Optimize by abbreviating the operations in
			 * move_task_to_local_dsq().
			 */
			dsp = p->scx.holding_cpu == raw_smp_processor_id();
			if (likely(dsp)) {
				p->scx.holding_cpu = -1;
				dispatch_enqueue(&dst_rq->scx.local_dsq, p,
						 enq_flags);
			}
		} else {
			dsp = move_task_to_local_dsq(dst_rq, p, enq_flags);
		}

		/* if the destination CPU is idle, wake it up */
		if (dsp && p->sched_class > dst_rq->curr->sched_class)
			resched_curr(dst_rq);

		dispatch_to_local_dsq_unlock(rq, rf, src_rq, locked_dst_rq);

		return dsp ? DTL_DISPATCHED : DTL_LOST;
	}
#endif /* CONFIG_SMP */

	scx_ops_error("SCX_DSQ_LOCAL[_ON] verdict target cpu %d not allowed for %s[%d]",
		      cpu_of(dst_rq), p->comm, p->pid);
	return DTL_INVALID;
}

/**
 * finish_dispatch - Asynchronously finish dispatching a task
 * @rq: current rq which is locked
 * @rf: rq_flags to use when unlocking @rq
 * @p: task to finish dispatching
 * @qseq_at_dispatch: qseq when @p started getting dispatched
 * @dsq_id: destination DSQ ID
 * @enq_flags: %SCX_ENQ_*
 *
 * Dispatching to local DSQs may need to wait for queueing to complete or
 * require rq lock dancing. As we don't wanna do either while inside
 * ops.dispatch() to avoid locking order inversion, we split dispatching into
 * two parts. scx_bpf_dispatch() which is called by ops.dispatch() records the
 * task and its qseq. Once ops.dispatch() returns, this function is called to
 * finish up.
 *
 * There is no guarantee that @p is still valid for dispatching or even that it
 * was valid in the first place. Make sure that the task is still owned by the
 * BPF scheduler and claim the ownership before dispatching.
 */
static void finish_dispatch(struct rq *rq, struct rq_flags *rf,
			    struct task_struct *p,
			    unsigned long qseq_at_dispatch,
			    u64 dsq_id, u64 enq_flags)
{
	struct scx_dispatch_q *dsq;
	unsigned long opss;

	touch_core_sched_dispatch(rq, p);
retry:
	/*
	 * No need for _acquire here. @p is accessed only after a successful
	 * try_cmpxchg to DISPATCHING.
	 */
	opss = atomic_long_read(&p->scx.ops_state);

	switch (opss & SCX_OPSS_STATE_MASK) {
	case SCX_OPSS_DISPATCHING:
	case SCX_OPSS_NONE:
		/* someone else already got to it */
		return;
	case SCX_OPSS_QUEUED:
		/*
		 * If qseq doesn't match, @p has gone through at least one
		 * dispatch/dequeue and re-enqueue cycle between
		 * scx_bpf_dispatch() and here and we have no claim on it.
		 */
		if ((opss & SCX_OPSS_QSEQ_MASK) != qseq_at_dispatch)
			return;

		/*
		 * While we know @p is accessible, we don't yet have a claim on
		 * it - the BPF scheduler is allowed to dispatch tasks
		 * spuriously and there can be a racing dequeue attempt. Let's
		 * claim @p by atomically transitioning it from QUEUED to
		 * DISPATCHING.
		 */
		if (likely(atomic_long_try_cmpxchg(&p->scx.ops_state, &opss,
						   SCX_OPSS_DISPATCHING)))
			break;
		goto retry;
	case SCX_OPSS_QUEUEING:
		/*
		 * do_enqueue_task() is in the process of transferring the task
		 * to the BPF scheduler while holding @p's rq lock. As we aren't
		 * holding any kernel or BPF resource that the enqueue path may
		 * depend upon, it's safe to wait.
		 */
		wait_ops_state(p, opss);
		goto retry;
	}

	BUG_ON(!(p->scx.flags & SCX_TASK_QUEUED));

	switch (dispatch_to_local_dsq(rq, rf, dsq_id, p, enq_flags)) {
	case DTL_DISPATCHED:
		break;
	case DTL_LOST:
		break;
	case DTL_INVALID:
		dsq_id = SCX_DSQ_GLOBAL;
		fallthrough;
	case DTL_NOT_LOCAL:
		dsq = find_dsq_for_dispatch(cpu_rq(raw_smp_processor_id()),
					    dsq_id, p);
		dispatch_enqueue(dsq, p, enq_flags | SCX_ENQ_CLEAR_OPSS);
		break;
	}
}

static void flush_dispatch_buf(struct rq *rq, struct rq_flags *rf)
{
	struct scx_dsp_ctx *dspc = this_cpu_ptr(&scx_dsp_ctx);
	u32 u;

	for (u = 0; u < dspc->buf_cursor; u++) {
		struct scx_dsp_buf_ent *ent = &this_cpu_ptr(scx_dsp_buf)[u];

		finish_dispatch(rq, rf, ent->task, ent->qseq, ent->dsq_id,
				ent->enq_flags);
	}

	dspc->nr_tasks += dspc->buf_cursor;
	dspc->buf_cursor = 0;
}

static int balance_one(struct rq *rq, struct task_struct *prev,
		       struct rq_flags *rf, bool local)
{
	struct scx_rq *scx_rq = &rq->scx;
	struct scx_dsp_ctx *dspc = this_cpu_ptr(&scx_dsp_ctx);
	bool prev_on_scx = prev->sched_class == &ext_sched_class;
	int nr_loops = SCX_DSP_MAX_LOOPS;
	bool has_tasks = false;

	lockdep_assert_rq_held(rq);
	scx_rq->flags |= SCX_RQ_BALANCING;

	if (static_branch_unlikely(&scx_ops_cpu_preempt) &&
	    unlikely(rq->scx.cpu_released)) {
		/*
		 * If the previous sched_class for the current CPU was not SCX,
		 * notify the BPF scheduler that it again has control of the
		 * core. This callback complements ->cpu_release(), which is
		 * emitted in scx_notify_pick_next_task().
		 */
		if (SCX_HAS_OP(cpu_acquire))
			SCX_CALL_OP(SCX_KF_UNLOCKED, cpu_acquire, cpu_of(rq),
				    NULL);
		rq->scx.cpu_released = false;
	}

	if (prev_on_scx) {
		WARN_ON_ONCE(local && (prev->scx.flags & SCX_TASK_BAL_KEEP));
		update_curr_scx(rq);

		/*
		 * If @prev is runnable & has slice left, it has priority and
		 * fetching more just increases latency for the fetched tasks.
		 * Tell put_prev_task_scx() to put @prev on local_dsq. If the
		 * BPF scheduler wants to handle this explicitly, it should
		 * implement ->cpu_released().
		 *
		 * See scx_ops_disable_workfn() for the explanation on the
		 * disabling() test.
		 *
		 * When balancing a remote CPU for core-sched, there won't be a
		 * following put_prev_task_scx() call and we don't own
		 * %SCX_TASK_BAL_KEEP. Instead, pick_task_scx() will test the
		 * same conditions later and pick @rq->curr accordingly.
		 */
		if ((prev->scx.flags & SCX_TASK_QUEUED) &&
		    prev->scx.slice && !scx_ops_bypassing()) {
			if (local)
				prev->scx.flags |= SCX_TASK_BAL_KEEP;
			goto has_tasks;
		}
	}

	/* if there already are tasks to run, nothing to do */
	if (scx_rq->local_dsq.nr)
		goto has_tasks;

	if (consume_dispatch_q(rq, rf, &scx_dsq_global))
		goto has_tasks;

	if (!SCX_HAS_OP(dispatch) || scx_ops_bypassing())
		goto out;

	dspc->rq = rq;
	dspc->rf = rf;

	/*
	 * The dispatch loop. Because flush_dispatch_buf() may drop the rq lock,
	 * the local DSQ might still end up empty after a successful
	 * ops.dispatch(). If the local DSQ is empty even after ops.dispatch()
	 * produced some tasks, retry. The BPF scheduler may depend on this
	 * looping behavior to simplify its implementation.
	 */
	do {
		dspc->nr_tasks = 0;

		SCX_CALL_OP(SCX_KF_DISPATCH, dispatch, cpu_of(rq),
			    prev_on_scx ? prev : NULL);

		flush_dispatch_buf(rq, rf);

		if (scx_rq->local_dsq.nr)
			goto has_tasks;
		if (consume_dispatch_q(rq, rf, &scx_dsq_global))
			goto has_tasks;

		/*
		 * ops.dispatch() can trap us in this loop by repeatedly
		 * dispatching ineligible tasks. Break out once in a while to
		 * allow the watchdog to run. As IRQ can't be enabled in
		 * balance(), we want to complete this scheduling cycle and then
		 * start a new one. IOW, we want to call resched_curr() on the
		 * next, most likely idle, task, not the current one. Use
		 * scx_bpf_kick_cpu() for deferred kicking.
		 */
		if (unlikely(!--nr_loops)) {
			scx_bpf_kick_cpu(cpu_of(rq), 0);
			break;
		}
	} while (dspc->nr_tasks);

	goto out;

has_tasks:
	has_tasks = true;
out:
	scx_rq->flags &= ~SCX_RQ_BALANCING;
	return has_tasks;
}

static int balance_scx(struct rq *rq, struct task_struct *prev,
		       struct rq_flags *rf)
{
	int ret;

	ret = balance_one(rq, prev, rf, true);

#ifdef CONFIG_SCHED_SMT
	/*
	 * When core-sched is enabled, this ops.balance() call will be followed
	 * by put_prev_scx() and pick_task_scx() on this CPU and pick_task_scx()
	 * on the SMT siblings. Balance the siblings too.
	 */
	if (sched_core_enabled(rq)) {
		const struct cpumask *smt_mask = cpu_smt_mask(cpu_of(rq));
		int scpu;

		for_each_cpu_andnot(scpu, smt_mask, cpumask_of(cpu_of(rq))) {
			struct rq *srq = cpu_rq(scpu);
			struct rq_flags srf;
			struct task_struct *sprev = srq->curr;

			/*
			 * While core-scheduling, rq lock is shared among
			 * siblings but the debug annotations and rq clock
			 * aren't. Do pinning dance to transfer the ownership.
			 */
			WARN_ON_ONCE(__rq_lockp(rq) != __rq_lockp(srq));
			rq_unpin_lock(rq, rf);
			rq_pin_lock(srq, &srf);

			update_rq_clock(srq);
			balance_one(srq, sprev, &srf, false);

			rq_unpin_lock(srq, &srf);
			rq_repin_lock(rq, rf);
		}
	}
#endif
	return ret;
}

static void set_next_task_scx(struct rq *rq, struct task_struct *p, bool first)
{
	if (p->scx.flags & SCX_TASK_QUEUED) {
		/*
		 * Core-sched might decide to execute @p before it is
		 * dispatched. Call ops_dequeue() to notify the BPF scheduler.
		 */
		ops_dequeue(p, SCX_DEQ_CORE_SCHED_EXEC);
		dispatch_dequeue(&rq->scx, p);
	}

	p->se.exec_start = rq_clock_task(rq);

	/* see dequeue_task_scx() on why we skip when !QUEUED */
	if (SCX_HAS_OP(running) && (p->scx.flags & SCX_TASK_QUEUED))
		SCX_CALL_OP_TASK(SCX_KF_REST, running, p);

	clr_task_runnable(p, true);

	/*
	 * @p is getting newly scheduled or got kicked after someone updated its
	 * slice. Refresh whether tick can be stopped. See scx_can_stop_tick().
	 */
	if ((p->scx.slice == SCX_SLICE_INF) !=
	    (bool)(rq->scx.flags & SCX_RQ_CAN_STOP_TICK)) {
		if (p->scx.slice == SCX_SLICE_INF)
			rq->scx.flags |= SCX_RQ_CAN_STOP_TICK;
		else
			rq->scx.flags &= ~SCX_RQ_CAN_STOP_TICK;

		sched_update_tick_dependency(rq);
	}
}

static void put_prev_task_scx(struct rq *rq, struct task_struct *p)
{
#ifndef CONFIG_SMP
	/*
	 * UP workaround.
	 *
	 * Because SCX may transfer tasks across CPUs during dispatch, dispatch
	 * is performed from its balance operation which isn't called in UP.
	 * Let's work around by calling it from the operations which come right
	 * after.
	 *
	 * 1. If the prev task is on SCX, pick_next_task() calls
	 *    .put_prev_task() right after. As .put_prev_task() is also called
	 *    from other places, we need to distinguish the calls which can be
	 *    done by looking at the previous task's state - if still queued or
	 *    dequeued with %SCX_DEQ_SLEEP, the caller must be pick_next_task().
	 *    This case is handled here.
	 *
	 * 2. If the prev task is not on SCX, the first following call into SCX
	 *    will be .pick_next_task(), which is covered by calling
	 *    balance_scx() from pick_next_task_scx().
	 *
	 * Note that we can't merge the first case into the second as
	 * balance_scx() must be called before the previous SCX task goes
	 * through put_prev_task_scx().
	 *
	 * As UP doesn't transfer tasks around, balance_scx() doesn't need @rf.
	 * Pass in %NULL.
	 */
	if (p->scx.flags & (SCX_TASK_QUEUED | SCX_TASK_DEQD_FOR_SLEEP))
		balance_scx(rq, p, NULL);
#endif

	update_curr_scx(rq);

	/* see dequeue_task_scx() on why we skip when !QUEUED */
	if (SCX_HAS_OP(stopping) && (p->scx.flags & SCX_TASK_QUEUED))
		SCX_CALL_OP_TASK(SCX_KF_REST, stopping, p, true);

	/*
	 * If we're being called from put_prev_task_balance(), balance_scx() may
	 * have decided that @p should keep running.
	 */
	if (p->scx.flags & SCX_TASK_BAL_KEEP) {
		p->scx.flags &= ~SCX_TASK_BAL_KEEP;
		set_task_runnable(rq, p);
		dispatch_enqueue(&rq->scx.local_dsq, p, SCX_ENQ_HEAD);
		return;
	}

	if (p->scx.flags & SCX_TASK_QUEUED) {
		set_task_runnable(rq, p);

		/*
		 * If @p has slice left and balance_scx() didn't tag it for
		 * keeping, @p is getting preempted by a higher priority
		 * scheduler class or core-sched forcing a different task. Leave
		 * it at the head of the local DSQ.
		 */
		if (p->scx.slice && !scx_ops_bypassing()) {
			dispatch_enqueue(&rq->scx.local_dsq, p, SCX_ENQ_HEAD);
			return;
		}

		/*
		 * If we're in the pick_next_task path, balance_scx() should
		 * have already populated the local DSQ if there are any other
		 * available tasks. If empty, tell ops.enqueue() that @p is the
		 * only one available for this cpu. ops.enqueue() should put it
		 * on the local DSQ so that the subsequent pick_next_task_scx()
		 * can find the task unless it wants to trigger a separate
		 * follow-up scheduling event.
		 */
		if (list_empty(&rq->scx.local_dsq.fifo))
			do_enqueue_task(rq, p, SCX_ENQ_LAST, -1);
		else
			do_enqueue_task(rq, p, 0, -1);
	}
}

static struct task_struct *first_local_task(struct rq *rq)
{
	WARN_ON_ONCE(rb_first_cached(&rq->scx.local_dsq.priq));
	return list_first_entry_or_null(&rq->scx.local_dsq.fifo,
					struct task_struct, scx.dsq_node.fifo);
}

static struct task_struct *pick_next_task_scx(struct rq *rq)
{
	struct task_struct *p;

#ifndef CONFIG_SMP
	/* UP workaround - see the comment at the head of put_prev_task_scx() */
	if (unlikely(rq->curr->sched_class != &ext_sched_class))
		balance_scx(rq, rq->curr, NULL);
#endif

	p = first_local_task(rq);
	if (!p)
		return NULL;

	if (unlikely(!p->scx.slice)) {
		if (!scx_ops_bypassing() && !scx_warned_zero_slice) {
			printk_deferred(KERN_WARNING "sched_ext: %s[%d] has zero slice in pick_next_task_scx()\n",
					p->comm, p->pid);
			scx_warned_zero_slice = true;
		}
		p->scx.slice = SCX_SLICE_DFL;
	}

	set_next_task_scx(rq, p, true);

	return p;
}

#ifdef CONFIG_SCHED_CORE
/**
 * scx_prio_less - Task ordering for core-sched
 * @a: task A
 * @b: task B
 *
 * Core-sched is implemented as an additional scheduling layer on top of the
 * usual sched_class'es and needs to find out the expected task ordering. For
 * SCX, core-sched calls this function to interrogate the task ordering.
 *
 * Unless overridden by ops.core_sched_before(), @p->scx.core_sched_at is used
 * to implement the default task ordering. The older the timestamp, the higher
 * prority the task - the global FIFO ordering matching the default scheduling
 * behavior.
 *
 * When ops.core_sched_before() is enabled, @p->scx.core_sched_at is used to
 * implement FIFO ordering within each local DSQ. See pick_task_scx().
 */
bool scx_prio_less(const struct task_struct *a, const struct task_struct *b,
		   bool in_fi)
{
	/*
	 * The const qualifiers are dropped from task_struct pointers when
	 * calling ops.core_sched_before(). Accesses are controlled by the
	 * verifier.
	 */
	if (SCX_HAS_OP(core_sched_before) && !scx_ops_bypassing())
		return SCX_CALL_OP_2TASKS_RET(SCX_KF_REST, core_sched_before,
					      (struct task_struct *)a,
					      (struct task_struct *)b);
	else
		return time_after64(a->scx.core_sched_at, b->scx.core_sched_at);
}

/**
 * pick_task_scx - Pick a candidate task for core-sched
 * @rq: rq to pick the candidate task from
 *
 * Core-sched calls this function on each SMT sibling to determine the next
 * tasks to run on the SMT siblings. balance_one() has been called on all
 * siblings and put_prev_task_scx() has been called only for the current CPU.
 *
 * As put_prev_task_scx() hasn't been called on remote CPUs, we can't just look
 * at the first task in the local dsq. @rq->curr has to be considered explicitly
 * to mimic %SCX_TASK_BAL_KEEP.
 */
static struct task_struct *pick_task_scx(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct task_struct *first = first_local_task(rq);

	if (curr->scx.flags & SCX_TASK_QUEUED) {
		/* is curr the only runnable task? */
		if (!first)
			return curr;

		/*
		 * Does curr trump first? We can always go by core_sched_at for
		 * this comparison as it represents global FIFO ordering when
		 * the default core-sched ordering is used and local-DSQ FIFO
		 * ordering otherwise.
		 *
		 * We can have a task with an earlier timestamp on the DSQ. For
		 * example, when a current task is preempted by a sibling
		 * picking a different cookie, the task would be requeued at the
		 * head of the local DSQ with an earlier timestamp than the
		 * core-sched picked next task. Besides, the BPF scheduler may
		 * dispatch any tasks to the local DSQ anytime.
		 */
		if (curr->scx.slice && time_before64(curr->scx.core_sched_at,
						     first->scx.core_sched_at))
			return curr;
	}

	return first;	/* this may be %NULL */
}
#endif	/* CONFIG_SCHED_CORE */

static enum scx_cpu_preempt_reason
preempt_reason_from_class(const struct sched_class *class)
{
#ifdef CONFIG_SMP
	if (class == &stop_sched_class)
		return SCX_CPU_PREEMPT_STOP;
#endif
	if (class == &dl_sched_class)
		return SCX_CPU_PREEMPT_DL;
	if (class == &rt_sched_class)
		return SCX_CPU_PREEMPT_RT;
	return SCX_CPU_PREEMPT_UNKNOWN;
}

void __scx_notify_pick_next_task(struct rq *rq, struct task_struct *task,
				 const struct sched_class *active)
{
	lockdep_assert_rq_held(rq);

	/*
	 * The callback is conceptually meant to convey that the CPU is no
	 * longer under the control of SCX. Therefore, don't invoke the
	 * callback if the CPU is is staying on SCX, or going idle (in which
	 * case the SCX scheduler has actively decided not to schedule any
	 * tasks on the CPU).
	 */
	if (likely(active >= &ext_sched_class))
		return;

	/*
	 * At this point we know that SCX was preempted by a higher priority
	 * sched_class, so invoke the ->cpu_release() callback if we have not
	 * done so already. We only send the callback once between SCX being
	 * preempted, and it regaining control of the CPU.
	 *
	 * ->cpu_release() complements ->cpu_acquire(), which is emitted the
	 *  next time that balance_scx() is invoked.
	 */
	if (!rq->scx.cpu_released) {
		if (SCX_HAS_OP(cpu_release)) {
			struct scx_cpu_release_args args = {
				.reason = preempt_reason_from_class(active),
				.task = task,
			};

			SCX_CALL_OP(SCX_KF_CPU_RELEASE,
				    cpu_release, cpu_of(rq), &args);
		}
		rq->scx.cpu_released = true;
	}
}

#ifdef CONFIG_SMP

static bool test_and_clear_cpu_idle(int cpu)
{
#ifdef CONFIG_SCHED_SMT
	/*
	 * SMT mask should be cleared whether we can claim @cpu or not. The SMT
	 * cluster is not wholly idle either way. This also prevents
	 * scx_pick_idle_cpu() from getting caught in an infinite loop.
	 */
	if (sched_smt_active()) {
		const struct cpumask *smt = cpu_smt_mask(cpu);

		/*
		 * If offline, @cpu is not its own sibling and
		 * scx_pick_idle_cpu() can get caught in an infinite loop as
		 * @cpu is never cleared from idle_masks.smt. Ensure that @cpu
		 * is eventually cleared.
		 */
		if (cpumask_intersects(smt, idle_masks.smt))
			cpumask_andnot(idle_masks.smt, idle_masks.smt, smt);
		else if (cpumask_test_cpu(cpu, idle_masks.smt))
			__cpumask_clear_cpu(cpu, idle_masks.smt);
	}
#endif
	return cpumask_test_and_clear_cpu(cpu, idle_masks.cpu);
}

static s32 scx_pick_idle_cpu(const struct cpumask *cpus_allowed, u64 flags)
{
	int cpu;

retry:
	if (sched_smt_active()) {
		cpu = cpumask_any_and_distribute(idle_masks.smt, cpus_allowed);
		if (cpu < nr_cpu_ids)
			goto found;

		if (flags & SCX_PICK_IDLE_CORE)
			return -EBUSY;
	}

	cpu = cpumask_any_and_distribute(idle_masks.cpu, cpus_allowed);
	if (cpu >= nr_cpu_ids)
		return -EBUSY;

found:
	if (test_and_clear_cpu_idle(cpu))
		return cpu;
	else
		goto retry;
}

static s32 scx_select_cpu_dfl(struct task_struct *p, s32 prev_cpu,
			      u64 wake_flags, bool *found)
{
	s32 cpu;

	*found = false;

	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		return prev_cpu;
	}

	/*
	 * If WAKE_SYNC and the machine isn't fully saturated, wake up @p to the
	 * local DSQ of the waker.
	 */
	if ((wake_flags & SCX_WAKE_SYNC) && p->nr_cpus_allowed > 1 &&
	    !cpumask_empty(idle_masks.cpu) && !(current->flags & PF_EXITING)) {
		cpu = smp_processor_id();
		if (cpumask_test_cpu(cpu, p->cpus_ptr))
			goto cpu_found;
	}

	if (p->nr_cpus_allowed == 1) {
		if (test_and_clear_cpu_idle(prev_cpu)) {
			cpu = prev_cpu;
			goto cpu_found;
		} else {
			return prev_cpu;
		}
	}

	/*
	 * If CPU has SMT, any wholly idle CPU is likely a better pick than
	 * partially idle @prev_cpu.
	 */
	if (sched_smt_active()) {
		if (cpumask_test_cpu(prev_cpu, idle_masks.smt) &&
		    test_and_clear_cpu_idle(prev_cpu)) {
			cpu = prev_cpu;
			goto cpu_found;
		}

		cpu = scx_pick_idle_cpu(p->cpus_ptr, SCX_PICK_IDLE_CORE);
		if (cpu >= 0)
			goto cpu_found;
	}

	if (test_and_clear_cpu_idle(prev_cpu)) {
		cpu = prev_cpu;
		goto cpu_found;
	}

	cpu = scx_pick_idle_cpu(p->cpus_ptr, 0);
	if (cpu >= 0)
		goto cpu_found;

	return prev_cpu;

cpu_found:
	*found = true;
	return cpu;
}

__bpf_kfunc_start_defs();

__bpf_kfunc s32 scx_bpf_select_cpu_dfl(struct task_struct *p, s32 prev_cpu,
				       u64 wake_flags, bool *found)
{
	if (!scx_kf_allowed(SCX_KF_SELECT_CPU)) {
		*found = false;
		return prev_cpu;
	}

	return scx_select_cpu_dfl(p, prev_cpu, wake_flags, found);
}

__bpf_kfunc_end_defs();

static int select_task_rq_scx(struct task_struct *p, int prev_cpu, int wake_flags)
{
	if (SCX_HAS_OP(select_cpu)) {
		s32 cpu;
		struct task_struct **ddsp_taskp;

		ddsp_taskp = this_cpu_ptr(&direct_dispatch_task);
		WARN_ON_ONCE(*ddsp_taskp);
		*ddsp_taskp = p;

		cpu = SCX_CALL_OP_TASK_RET(SCX_KF_ENQUEUE | SCX_KF_SELECT_CPU,
					   select_cpu, p, prev_cpu, wake_flags);
		*ddsp_taskp = NULL;
		if (ops_cpu_valid(cpu)) {
			return cpu;
		} else {
			scx_ops_error("select_cpu returned invalid cpu %d", cpu);
			return prev_cpu;
		}
	} else {
		bool found;
		s32 cpu;

		cpu = scx_select_cpu_dfl(p, prev_cpu, wake_flags, &found);
		if (found) {
			p->scx.slice = SCX_SLICE_DFL;
			p->scx.ddsp_dsq_id = SCX_DSQ_LOCAL;
		}
		return cpu;
	}
}

static void set_cpus_allowed_scx(struct task_struct *p,
				 struct affinity_context *ac)
{
	set_cpus_allowed_common(p, ac);

	/*
	 * The effective cpumask is stored in @p->cpus_ptr which may temporarily
	 * differ from the configured one in @p->cpus_mask. Always tell the bpf
	 * scheduler the effective one.
	 *
	 * Fine-grained memory write control is enforced by BPF making the const
	 * designation pointless. Cast it away when calling the operation.
	 */
	if (SCX_HAS_OP(set_cpumask))
		SCX_CALL_OP_TASK(SCX_KF_REST, set_cpumask, p,
				 (struct cpumask *)p->cpus_ptr);
}

static void reset_idle_masks(void)
{
	/* consider all cpus idle, should converge to the actual state quickly */
	cpumask_setall(idle_masks.cpu);
	cpumask_setall(idle_masks.smt);
}

void __scx_update_idle(struct rq *rq, bool idle)
{
	int cpu = cpu_of(rq);

	if (SCX_HAS_OP(update_idle)) {
		SCX_CALL_OP(SCX_KF_REST, update_idle, cpu_of(rq), idle);
		if (!static_branch_unlikely(&scx_builtin_idle_enabled))
			return;
	}

	if (idle)
		cpumask_set_cpu(cpu, idle_masks.cpu);
	else
		cpumask_clear_cpu(cpu, idle_masks.cpu);

#ifdef CONFIG_SCHED_SMT
	if (sched_smt_active()) {
		const struct cpumask *smt = cpu_smt_mask(cpu);

		if (idle) {
			/*
			 * idle_masks.smt handling is racy but that's fine as
			 * it's only for optimization and self-correcting.
			 */
			for_each_cpu(cpu, smt) {
				if (!cpumask_test_cpu(cpu, idle_masks.cpu))
					return;
			}
			cpumask_or(idle_masks.smt, idle_masks.smt, smt);
		} else {
			cpumask_andnot(idle_masks.smt, idle_masks.smt, smt);
		}
	}
#endif
}

static void rq_online_scx(struct rq *rq, enum rq_onoff_reason reason)
{
	if (SCX_HAS_OP(cpu_online) && reason == RQ_ONOFF_HOTPLUG)
		SCX_CALL_OP(SCX_KF_REST, cpu_online, cpu_of(rq));
}

static void rq_offline_scx(struct rq *rq, enum rq_onoff_reason reason)
{
	if (SCX_HAS_OP(cpu_offline) && reason == RQ_ONOFF_HOTPLUG)
		SCX_CALL_OP(SCX_KF_REST, cpu_offline, cpu_of(rq));
}

#else /* !CONFIG_SMP */

static bool test_and_clear_cpu_idle(int cpu) { return false; }
static s32 scx_pick_idle_cpu(const struct cpumask *cpus_allowed, u64 flags) { return -EBUSY; }
static void reset_idle_masks(void) {}

#endif /* CONFIG_SMP */

static bool check_rq_for_timeouts(struct rq *rq)
{
	struct task_struct *p;
	struct rq_flags rf;
	bool timed_out = false;

	rq_lock_irqsave(rq, &rf);
	list_for_each_entry(p, &rq->scx.runnable_list, scx.runnable_node) {
		unsigned long last_runnable = p->scx.runnable_at;

		if (unlikely(time_after(jiffies,
					last_runnable + scx_watchdog_timeout))) {
			u32 dur_ms = jiffies_to_msecs(jiffies - last_runnable);

			scx_ops_error_kind(SCX_EXIT_ERROR_STALL,
					   "%s[%d] failed to run for %u.%03us",
					   p->comm, p->pid,
					   dur_ms / 1000, dur_ms % 1000);
			timed_out = true;
			break;
		}
	}
	rq_unlock_irqrestore(rq, &rf);

	return timed_out;
}

static void scx_watchdog_workfn(struct work_struct *work)
{
	int cpu;

	WRITE_ONCE(scx_watchdog_timestamp, jiffies);

	for_each_online_cpu(cpu) {
		if (unlikely(check_rq_for_timeouts(cpu_rq(cpu))))
			break;

		cond_resched();
	}
	queue_delayed_work(system_unbound_wq, to_delayed_work(work),
			   scx_watchdog_timeout / 2);
}

static void task_tick_scx(struct rq *rq, struct task_struct *curr, int queued)
{
	update_curr_scx(rq);

	/*
	 * While disabling, always resched and refresh core-sched timestamp as
	 * we can't trust the slice management or ops.core_sched_before().
	 */
	if (scx_ops_bypassing()) {
		curr->scx.slice = 0;
		touch_core_sched(rq, curr);
	}

	if (!curr->scx.slice)
		resched_curr(rq);
}

#ifdef CONFIG_EXT_GROUP_SCHED
static struct cgroup *tg_cgrp(struct task_group *tg)
{
	/*
	 * If CGROUP_SCHED is disabled, @tg is NULL. If @tg is an autogroup,
	 * @tg->css.cgroup is NULL. In both cases, @tg can be treated as the
	 * root cgroup.
	 */
	if (tg && tg->css.cgroup)
		return tg->css.cgroup;
	else
		return &cgrp_dfl_root.cgrp;
}

#define SCX_INIT_TASK_ARGS_CGROUP(tg)		.cgroup = tg_cgrp(tg),

#else	/* CONFIG_EXT_GROUP_SCHED */

#define SCX_INIT_TASK_ARGS_CGROUP(tg)

#endif	/* CONFIG_EXT_GROUP_SCHED */

static enum scx_task_state scx_get_task_state(const struct task_struct *p)
{
	return (p->scx.flags & SCX_TASK_STATE_MASK) >> SCX_TASK_STATE_SHIFT;
}

static void scx_set_task_state(struct task_struct *p, enum scx_task_state state)
{
	enum scx_task_state prev_state = scx_get_task_state(p);
	bool warn = false;

	BUILD_BUG_ON(SCX_TASK_NR_STATES > (1 << SCX_TASK_STATE_BITS));

	switch (state) {
	case SCX_TASK_NONE:
		break;
	case SCX_TASK_INIT:
		warn = prev_state != SCX_TASK_NONE;
		break;
	case SCX_TASK_READY:
		warn = prev_state == SCX_TASK_NONE;
		break;
	case SCX_TASK_ENABLED:
		warn = prev_state != SCX_TASK_READY;
		break;
	default:
		warn = true;
		return;
	}

	WARN_ONCE(warn, "sched_ext: Invalid task state transition %d -> %d for %s[%d]",
		  prev_state, state, p->comm, p->pid);

	p->scx.flags &= ~SCX_TASK_STATE_MASK;
	p->scx.flags |= state << SCX_TASK_STATE_SHIFT;
}

static int scx_ops_init_task(struct task_struct *p, struct task_group *tg, bool fork)
{
	int ret;

	p->scx.disallow = false;

	if (SCX_HAS_OP(init_task)) {
		struct scx_init_task_args args = {
			SCX_INIT_TASK_ARGS_CGROUP(tg)
			.fork = fork,
		};

		ret = SCX_CALL_OP_RET(SCX_KF_SLEEPABLE, init_task, p, &args);
		if (unlikely(ret)) {
			ret = ops_sanitize_err("init_task", ret);
			return ret;
		}
	}

	scx_set_task_state(p, SCX_TASK_INIT);

	if (p->scx.disallow) {
		struct rq *rq;
		struct rq_flags rf;

		rq = task_rq_lock(p, &rf);

		/*
		 * We're either in fork or load path and @p->policy will be
		 * applied right after. Reverting @p->policy here and rejecting
		 * %SCHED_EXT transitions from scx_check_setscheduler()
		 * guarantees that if ops.init_task() sets @p->disallow, @p can
		 * never be in SCX.
		 */
		if (p->policy == SCHED_EXT) {
			p->policy = SCHED_NORMAL;
			atomic_long_inc(&scx_nr_rejected);
		}

		task_rq_unlock(rq, p, &rf);
	}

	p->scx.flags |= SCX_TASK_RESET_RUNNABLE_AT;
	return 0;
}

static void set_task_scx_weight(struct task_struct *p)
{
	u32 weight = sched_prio_to_weight[p->static_prio - MAX_RT_PRIO];

	p->scx.weight = sched_weight_to_cgroup(weight);
}

static void scx_ops_enable_task(struct task_struct *p)
{
	lockdep_assert_rq_held(task_rq(p));

	/*
	 * Set the weight before calling ops.enable() so that the scheduler
	 * doesn't see a stale value if they inspect the task struct.
	 */
	set_task_scx_weight(p);
	if (SCX_HAS_OP(enable))
		SCX_CALL_OP_TASK(SCX_KF_REST, enable, p);
	scx_set_task_state(p, SCX_TASK_ENABLED);

	if (SCX_HAS_OP(set_weight))
		SCX_CALL_OP_TASK(SCX_KF_REST, set_weight, p, p->scx.weight);
}

static void scx_ops_disable_task(struct task_struct *p)
{
	lockdep_assert_rq_held(task_rq(p));
	WARN_ON_ONCE(scx_get_task_state(p) != SCX_TASK_ENABLED);

	if (SCX_HAS_OP(disable))
		SCX_CALL_OP(SCX_KF_REST, disable, p);
	scx_set_task_state(p, SCX_TASK_READY);
}

static void scx_ops_exit_task(struct task_struct *p)
{
	struct scx_exit_task_args args = {
		.cancelled = false,
	};

	lockdep_assert_rq_held(task_rq(p));
	switch (scx_get_task_state(p)) {
	case SCX_TASK_NONE:
		return;
	case SCX_TASK_INIT:
		args.cancelled = true;
		break;
	case SCX_TASK_READY:
		break;
	case SCX_TASK_ENABLED:
		scx_ops_disable_task(p);
		break;
	default:
		WARN_ON_ONCE(true);
		return;
	}

	if (SCX_HAS_OP(exit_task))
		SCX_CALL_OP(SCX_KF_REST, exit_task, p, &args);
	scx_set_task_state(p, SCX_TASK_NONE);
}

void scx_pre_fork(struct task_struct *p)
{
	/*
	 * BPF scheduler enable/disable paths want to be able to iterate and
	 * update all tasks which can become complex when racing forks. As
	 * enable/disable are very cold paths, let's use a percpu_rwsem to
	 * exclude forks.
	 */
	percpu_down_read(&scx_fork_rwsem);
}

int scx_fork(struct task_struct *p)
{
	percpu_rwsem_assert_held(&scx_fork_rwsem);

	if (scx_enabled())
		return scx_ops_init_task(p, task_group(p), true);
	else
		return 0;
}

void scx_post_fork(struct task_struct *p)
{
	if (scx_enabled()) {
		scx_set_task_state(p, SCX_TASK_READY);
		/*
		 * Enable the task immediately if it's running on sched_ext.
		 * Otherwise, it'll be enabled in switching_to_scx() if and
		 * when it's ever configured to run with a SCHED_EXT policy.
		 */
		if (p->sched_class == &ext_sched_class) {
			struct rq_flags rf;
			struct rq *rq;

			rq = task_rq_lock(p, &rf);
			scx_ops_enable_task(p);
			task_rq_unlock(rq, p, &rf);
		}
	}

	spin_lock_irq(&scx_tasks_lock);
	list_add_tail(&p->scx.tasks_node, &scx_tasks);
	spin_unlock_irq(&scx_tasks_lock);

	percpu_up_read(&scx_fork_rwsem);
}

void scx_cancel_fork(struct task_struct *p)
{
	if (scx_enabled()) {
		struct rq *rq;
		struct rq_flags rf;

		rq = task_rq_lock(p, &rf);
		WARN_ON_ONCE(scx_get_task_state(p) >= SCX_TASK_READY);
		scx_ops_exit_task(p);
		task_rq_unlock(rq, p, &rf);
	}
	percpu_up_read(&scx_fork_rwsem);
}

void sched_ext_free(struct task_struct *p)
{
	unsigned long flags;

	spin_lock_irqsave(&scx_tasks_lock, flags);
	list_del_init(&p->scx.tasks_node);
	spin_unlock_irqrestore(&scx_tasks_lock, flags);

	/*
	 * @p is off scx_tasks and wholly ours. scx_ops_enable()'s READY ->
	 * ENABLED transitions can't race us. Disable ops for @p.
	 */
	if (scx_get_task_state(p) != SCX_TASK_NONE) {
		struct rq_flags rf;
		struct rq *rq;

		rq = task_rq_lock(p, &rf);
		scx_ops_exit_task(p);
		task_rq_unlock(rq, p, &rf);
	}
}

static void reweight_task_scx(struct rq *rq, struct task_struct *p, int newprio)
{
	lockdep_assert_rq_held(task_rq(p));

	set_task_scx_weight(p);
	if (SCX_HAS_OP(set_weight))
		SCX_CALL_OP_TASK(SCX_KF_REST, set_weight, p, p->scx.weight);
}

static void prio_changed_scx(struct rq *rq, struct task_struct *p, int oldprio)
{
}

static void switching_to_scx(struct rq *rq, struct task_struct *p)
{
	scx_ops_enable_task(p);

	/*
	 * set_cpus_allowed_scx() is not called while @p is associated with a
	 * different scheduler class. Keep the BPF scheduler up-to-date.
	 */
	if (SCX_HAS_OP(set_cpumask))
		SCX_CALL_OP_TASK(SCX_KF_REST, set_cpumask, p,
				 (struct cpumask *)p->cpus_ptr);
}

static void switched_from_scx(struct rq *rq, struct task_struct *p)
{
	scx_ops_disable_task(p);
}

static void wakeup_preempt_scx(struct rq *rq, struct task_struct *p,int wake_flags) {}
static void switched_to_scx(struct rq *rq, struct task_struct *p) {}

int scx_check_setscheduler(struct task_struct *p, int policy)
{
	lockdep_assert_rq_held(task_rq(p));

	/* if disallow, reject transitioning into SCX */
	if (scx_enabled() && READ_ONCE(p->scx.disallow) &&
	    p->policy != policy && policy == SCHED_EXT)
		return -EACCES;

	return 0;
}

#ifdef CONFIG_NO_HZ_FULL
bool scx_can_stop_tick(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	if (scx_ops_bypassing())
		return false;

	if (p->sched_class != &ext_sched_class)
		return true;

	/*
	 * @rq can dispatch from different DSQs, so we can't tell whether it
	 * needs the tick or not by looking at nr_running. Allow stopping ticks
	 * iff the BPF scheduler indicated so. See set_next_task_scx().
	 */
	return rq->scx.flags & SCX_RQ_CAN_STOP_TICK;
}
#endif

#ifdef CONFIG_EXT_GROUP_SCHED

DEFINE_STATIC_PERCPU_RWSEM(scx_cgroup_rwsem);

int scx_tg_online(struct task_group *tg)
{
	int ret = 0;

	WARN_ON_ONCE(tg->scx_flags & (SCX_TG_ONLINE | SCX_TG_INITED));

	percpu_down_read(&scx_cgroup_rwsem);

	if (SCX_HAS_OP(cgroup_init)) {
		struct scx_cgroup_init_args args = { .weight = tg->scx_weight };

		ret = SCX_CALL_OP_RET(SCX_KF_SLEEPABLE, cgroup_init,
				      tg->css.cgroup, &args);
		if (!ret)
			tg->scx_flags |= SCX_TG_ONLINE | SCX_TG_INITED;
		else
			ret = ops_sanitize_err("cgroup_init", ret);
	} else {
		tg->scx_flags |= SCX_TG_ONLINE;
	}

	percpu_up_read(&scx_cgroup_rwsem);
	return ret;
}

void scx_tg_offline(struct task_group *tg)
{
	WARN_ON_ONCE(!(tg->scx_flags & SCX_TG_ONLINE));

	percpu_down_read(&scx_cgroup_rwsem);

	if (SCX_HAS_OP(cgroup_exit) && (tg->scx_flags & SCX_TG_INITED))
		SCX_CALL_OP(SCX_KF_SLEEPABLE, cgroup_exit, tg->css.cgroup);
	tg->scx_flags &= ~(SCX_TG_ONLINE | SCX_TG_INITED);

	percpu_up_read(&scx_cgroup_rwsem);
}

int scx_cgroup_can_attach(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct task_struct *p;
	int ret;

	/* released in scx_finish/cancel_attach() */
	percpu_down_read(&scx_cgroup_rwsem);

	if (!scx_enabled())
		return 0;

	cgroup_taskset_for_each(p, css, tset) {
		struct cgroup *from = tg_cgrp(task_group(p));

		if (SCX_HAS_OP(cgroup_prep_move)) {
			ret = SCX_CALL_OP_RET(SCX_KF_SLEEPABLE, cgroup_prep_move,
					      p, from, css->cgroup);
			if (ret)
				goto err;
		}

		WARN_ON_ONCE(p->scx.cgrp_moving_from);
		p->scx.cgrp_moving_from = from;
	}

	return 0;

err:
	cgroup_taskset_for_each(p, css, tset) {
		if (!p->scx.cgrp_moving_from)
			break;
		if (SCX_HAS_OP(cgroup_cancel_move))
			SCX_CALL_OP(SCX_KF_SLEEPABLE, cgroup_cancel_move, p,
				    p->scx.cgrp_moving_from, css->cgroup);
		p->scx.cgrp_moving_from = NULL;
	}

	percpu_up_read(&scx_cgroup_rwsem);
	return ops_sanitize_err("cgroup_prep_move", ret);
}

void scx_move_task(struct task_struct *p)
{
	/*
	 * We're called from sched_move_task() which handles both cgroup and
	 * autogroup moves. Ignore the latter.
	 *
	 * Also ignore exiting tasks, because in the exit path tasks transition
	 * from the autogroup to the root group, so task_group_is_autogroup()
	 * alone isn't able to catch exiting autogroup tasks. This is safe for
	 * cgroup_move(), because cgroup migrations never happen for PF_EXITING
	 * tasks.
	 */
	if (p->flags & PF_EXITING || task_group_is_autogroup(task_group(p)))
		return;

	if (!scx_enabled())
		return;

	if (SCX_HAS_OP(cgroup_move)) {
		if (WARN_ON_ONCE(!p->scx.cgrp_moving_from))
			return;
		SCX_CALL_OP_TASK(SCX_KF_UNLOCKED, cgroup_move, p,
			p->scx.cgrp_moving_from, tg_cgrp(task_group(p)));
	}
	p->scx.cgrp_moving_from = NULL;
}

void scx_cgroup_finish_attach(void)
{
	percpu_up_read(&scx_cgroup_rwsem);
}

void scx_cgroup_cancel_attach(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct task_struct *p;

	if (!scx_enabled())
		goto out_unlock;

	cgroup_taskset_for_each(p, css, tset) {
		if (SCX_HAS_OP(cgroup_cancel_move)) {
			WARN_ON_ONCE(!p->scx.cgrp_moving_from);
			SCX_CALL_OP(SCX_KF_SLEEPABLE, cgroup_cancel_move, p,
				    p->scx.cgrp_moving_from, css->cgroup);
		}
		p->scx.cgrp_moving_from = NULL;
	}
out_unlock:
	percpu_up_read(&scx_cgroup_rwsem);
}

void scx_group_set_weight(struct task_group *tg, unsigned long weight)
{
	percpu_down_read(&scx_cgroup_rwsem);

	if (tg->scx_weight != weight) {
		if (SCX_HAS_OP(cgroup_set_weight))
			SCX_CALL_OP(SCX_KF_SLEEPABLE, cgroup_set_weight,
				    tg_cgrp(tg), weight);
		tg->scx_weight = weight;
	}

	percpu_up_read(&scx_cgroup_rwsem);
}

static void scx_cgroup_lock(void)
{
	percpu_down_write(&scx_cgroup_rwsem);
}

static void scx_cgroup_unlock(void)
{
	percpu_up_write(&scx_cgroup_rwsem);
}

#else	/* CONFIG_EXT_GROUP_SCHED */

static inline void scx_cgroup_lock(void) {}
static inline void scx_cgroup_unlock(void) {}

#endif	/* CONFIG_EXT_GROUP_SCHED */

/*
 * Omitted operations:
 *
 * - wakeup_preempt: NOOP as it isn't useful in the wakeup path because the task
 *   isn't tied to the CPU at that point. Preemption is implemented by resetting
 *   the victim task's slice to 0 and triggering reschedule on the target CPU.
 *
 * - migrate_task_rq: Unncessary as task to cpu mapping is transient.
 *
 * - task_fork/dead: We need fork/dead notifications for all tasks regardless of
 *   their current sched_class. Call them directly from sched core instead.
 *
 * - task_woken: Unnecessary.
 */
DEFINE_SCHED_CLASS(ext) = {
	.enqueue_task		= enqueue_task_scx,
	.dequeue_task		= dequeue_task_scx,
	.yield_task		= yield_task_scx,
	.yield_to_task		= yield_to_task_scx,

	.wakeup_preempt		= wakeup_preempt_scx,

	.pick_next_task		= pick_next_task_scx,

	.put_prev_task		= put_prev_task_scx,
	.set_next_task          = set_next_task_scx,

#ifdef CONFIG_SMP
	.balance		= balance_scx,
	.select_task_rq		= select_task_rq_scx,
	.set_cpus_allowed	= set_cpus_allowed_scx,

	.rq_online		= rq_online_scx,
	.rq_offline		= rq_offline_scx,
#endif

#ifdef CONFIG_SCHED_CORE
	.pick_task		= pick_task_scx,
#endif

	.task_tick		= task_tick_scx,

	.switching_to		= switching_to_scx,
	.switched_from		= switched_from_scx,
	.switched_to		= switched_to_scx,
	.reweight_task		= reweight_task_scx,
	.prio_changed		= prio_changed_scx,

	.update_curr		= update_curr_scx,

#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 0,
#endif
};

static void init_dsq(struct scx_dispatch_q *dsq, u64 dsq_id)
{
	memset(dsq, 0, sizeof(*dsq));

	raw_spin_lock_init(&dsq->lock);
	INIT_LIST_HEAD(&dsq->fifo);
	dsq->id = dsq_id;
}

static struct scx_dispatch_q *create_dsq(u64 dsq_id, int node)
{
	struct scx_dispatch_q *dsq;
	int ret;

	if (dsq_id & SCX_DSQ_FLAG_BUILTIN)
		return ERR_PTR(-EINVAL);

	dsq = kmalloc_node(sizeof(*dsq), GFP_KERNEL, node);
	if (!dsq)
		return ERR_PTR(-ENOMEM);

	init_dsq(dsq, dsq_id);

	ret = rhashtable_insert_fast(&dsq_hash, &dsq->hash_node,
				     dsq_hash_params);
	if (ret) {
		kfree(dsq);
		return ERR_PTR(ret);
	}
	return dsq;
}

static void free_dsq_irq_workfn(struct irq_work *irq_work)
{
	struct llist_node *to_free = llist_del_all(&dsqs_to_free);
	struct scx_dispatch_q *dsq, *tmp_dsq;

	llist_for_each_entry_safe(dsq, tmp_dsq, to_free, free_node)
		kfree_rcu(dsq, rcu);
}

static DEFINE_IRQ_WORK(free_dsq_irq_work, free_dsq_irq_workfn);

static void destroy_dsq(u64 dsq_id)
{
	struct scx_dispatch_q *dsq;
	unsigned long flags;

	rcu_read_lock();

	dsq = rhashtable_lookup_fast(&dsq_hash, &dsq_id, dsq_hash_params);
	if (!dsq)
		goto out_unlock_rcu;

	raw_spin_lock_irqsave(&dsq->lock, flags);

	if (dsq->nr) {
		scx_ops_error("attempting to destroy in-use dsq 0x%016llx (nr=%u)",
			      dsq->id, dsq->nr);
		goto out_unlock_dsq;
	}

	if (rhashtable_remove_fast(&dsq_hash, &dsq->hash_node, dsq_hash_params))
		goto out_unlock_dsq;

	/*
	 * Mark dead by invalidating ->id to prevent dispatch_enqueue() from
	 * queueing more tasks. As this function can be called from anywhere,
	 * freeing is bounced through an irq work to avoid nesting RCU
	 * operations inside scheduler locks.
	 */
	dsq->id = SCX_DSQ_INVALID;
	llist_add(&dsq->free_node, &dsqs_to_free);
	irq_work_queue(&free_dsq_irq_work);

out_unlock_dsq:
	raw_spin_unlock_irqrestore(&dsq->lock, flags);
out_unlock_rcu:
	rcu_read_unlock();
}

#ifdef CONFIG_EXT_GROUP_SCHED
static void scx_cgroup_exit(void)
{
	struct cgroup_subsys_state *css;

	percpu_rwsem_assert_held(&scx_cgroup_rwsem);

	/*
	 * scx_tg_on/offline() are excluded through scx_cgroup_rwsem. If we walk
	 * cgroups and exit all the inited ones, all online cgroups are exited.
	 */
	rcu_read_lock();
	css_for_each_descendant_post(css, &root_task_group.css) {
		struct task_group *tg = css_tg(css);

		if (!(tg->scx_flags & SCX_TG_INITED))
			continue;
		tg->scx_flags &= ~SCX_TG_INITED;

		if (!scx_ops.cgroup_exit)
			continue;

		if (WARN_ON_ONCE(!css_tryget(css)))
			continue;
		rcu_read_unlock();

		SCX_CALL_OP(SCX_KF_UNLOCKED, cgroup_exit, css->cgroup);

		rcu_read_lock();
		css_put(css);
	}
	rcu_read_unlock();
}

static int scx_cgroup_init(void)
{
	struct cgroup_subsys_state *css;
	int ret;

	percpu_rwsem_assert_held(&scx_cgroup_rwsem);

	/*
	 * scx_tg_on/offline() are excluded thorugh scx_cgroup_rwsem. If we walk
	 * cgroups and init, all online cgroups are initialized.
	 */
	rcu_read_lock();
	css_for_each_descendant_pre(css, &root_task_group.css) {
		struct task_group *tg = css_tg(css);
		struct scx_cgroup_init_args args = { .weight = tg->scx_weight };

		if ((tg->scx_flags &
		     (SCX_TG_ONLINE | SCX_TG_INITED)) != SCX_TG_ONLINE)
			continue;

		if (!scx_ops.cgroup_init) {
			tg->scx_flags |= SCX_TG_INITED;
			continue;
		}

		if (WARN_ON_ONCE(!css_tryget(css)))
			continue;
		rcu_read_unlock();

		ret = SCX_CALL_OP_RET(SCX_KF_SLEEPABLE, cgroup_init,
				      css->cgroup, &args);
		if (ret) {
			css_put(css);
			return ret;
		}
		tg->scx_flags |= SCX_TG_INITED;

		rcu_read_lock();
		css_put(css);
	}
	rcu_read_unlock();

	return 0;
}

static void scx_cgroup_config_knobs(void)
{
	static DEFINE_MUTEX(cgintf_mutex);
	DECLARE_BITMAP(mask, CPU_CFTYPE_CNT) = { };
	u64 knob_flags;
	int i;

	/*
	 * Called from both class switch and ops enable/disable paths,
	 * synchronize internally.
	 */
	mutex_lock(&cgintf_mutex);

	/* if fair is in use, all knobs should be shown */
	if (!scx_switched_all()) {
		bitmap_fill(mask, CPU_CFTYPE_CNT);
		goto apply;
	}

	/*
	 * On ext, only show the supported knobs. Otherwise, show all possible
	 * knobs so that configuration attempts succeed and the states are
	 * remembered while ops is not loaded.
	 */
	if (scx_enabled())
		knob_flags = scx_ops.flags;
	else
		knob_flags = SCX_OPS_ALL_FLAGS;

	if (knob_flags & SCX_OPS_CGROUP_KNOB_WEIGHT) {
		__set_bit(CPU_CFTYPE_WEIGHT, mask);
		__set_bit(CPU_CFTYPE_WEIGHT_NICE, mask);
	}
apply:
	for (i = 0; i < CPU_CFTYPE_CNT; i++)
		cgroup_show_cftype(&cpu_cftypes[i], test_bit(i, mask));

	mutex_unlock(&cgintf_mutex);
}

#else
static void scx_cgroup_exit(void) {}
static int scx_cgroup_init(void) { return 0; }
static void scx_cgroup_config_knobs(void) {}
#endif


/********************************************************************************
 * Sysfs interface and ops enable/disable.
 */

#define SCX_ATTR(_name)								\
	static struct kobj_attribute scx_attr_##_name = {			\
		.attr = { .name = __stringify(_name), .mode = 0444 },		\
		.show = scx_attr_##_name##_show,				\
	}

static ssize_t scx_attr_state_show(struct kobject *kobj,
				   struct kobj_attribute *ka, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  scx_ops_enable_state_str[scx_ops_enable_state()]);
}
SCX_ATTR(state);

static ssize_t scx_attr_switch_all_show(struct kobject *kobj,
					struct kobj_attribute *ka, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(scx_switching_all));
}
SCX_ATTR(switch_all);

static ssize_t scx_attr_nr_rejected_show(struct kobject *kobj,
					 struct kobj_attribute *ka, char *buf)
{
	return sysfs_emit(buf, "%ld\n", atomic_long_read(&scx_nr_rejected));
}
SCX_ATTR(nr_rejected);

static struct attribute *scx_global_attrs[] = {
	&scx_attr_state.attr,
	&scx_attr_switch_all.attr,
	&scx_attr_nr_rejected.attr,
	NULL,
};

static const struct attribute_group scx_global_attr_group = {
	.attrs = scx_global_attrs,
};

static void scx_kobj_release(struct kobject *kobj)
{
	kfree(kobj);
}

static ssize_t scx_attr_ops_show(struct kobject *kobj,
				 struct kobj_attribute *ka, char *buf)
{
	return sysfs_emit(buf, "%s\n", scx_ops.name);
}
SCX_ATTR(ops);

static struct attribute *scx_sched_attrs[] = {
	&scx_attr_ops.attr,
	NULL,
};
ATTRIBUTE_GROUPS(scx_sched);

static const struct kobj_type scx_ktype = {
	.release = scx_kobj_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = scx_sched_groups,
};

static int scx_uevent(const struct kobject *kobj, struct kobj_uevent_env *env)
{
	return add_uevent_var(env, "SCXOPS=%s", scx_ops.name);
}

static const struct kset_uevent_ops scx_uevent_ops = {
	.uevent = scx_uevent,
};

/*
 * Used by sched_fork() and __setscheduler_prio() to pick the matching
 * sched_class. dl/rt are already handled.
 */
bool task_should_scx(struct task_struct *p)
{
	if (!scx_enabled() ||
	    unlikely(scx_ops_enable_state() == SCX_OPS_DISABLING))
		return false;
	if (READ_ONCE(scx_switching_all))
		return true;
	return p->policy == SCHED_EXT;
}

/**
 * scx_ops_bypass - [Un]bypass scx_ops and guarantee forward progress
 *
 * Bypassing guarantees that all runnable tasks make forward progress without
 * trusting the BPF scheduler. We can't grab any mutexes or rwsems as they might
 * be held by tasks that the BPF scheduler is forgetting to run, which
 * unfortunately also excludes toggling the static branches.
 *
 * Let's work around by overriding a couple ops and modifying behaviors based on
 * the DISABLING state and then cycling the queued tasks through dequeue/enqueue
 * to force global FIFO scheduling.
 *
 * a. ops.enqueue() is ignored and tasks are queued in simple global FIFO order.
 *
 * b. ops.dispatch() is ignored.
 *
 * c. balance_scx() never sets %SCX_TASK_BAL_KEEP as the slice value can't be
 *    trusted. Whenever a tick triggers, the running task is rotated to the tail
 *    of the queue with core_sched_at touched.
 *
 * d. pick_next_task() suppresses zero slice warning.
 *
 * e. scx_prio_less() reverts to the default core_sched_at order.
 *
 * f. scx_bpf_kick_cpu() is disabled to avoid irq_work malfunction during PM
 *    operations.
 */
static void scx_ops_bypass(bool bypass)
{
	int depth, cpu;

	if (bypass) {
		depth = atomic_inc_return(&scx_ops_bypass_depth);
		WARN_ON_ONCE(depth <= 0);
		if (depth != 1)
			return;
	} else {
		depth = atomic_dec_return(&scx_ops_bypass_depth);
		WARN_ON_ONCE(depth < 0);
		if (depth != 0)
			return;
	}

	/*
	 * No task property is changing. We just need to make sure all currently
	 * queued tasks are re-queued according to the new scx_ops_bypassing()
	 * state. As an optimization, walk each rq's runnable_list instead of
	 * the scx_tasks list.
	 *
	 * This function can't trust the scheduler and thus can't use
	 * cpus_read_lock(). Walk all possible CPUs instead of online.
	 */
	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);
		struct rq_flags rf;
		struct task_struct *p, *n;

		rq_lock_irqsave(rq, &rf);

		/*
		 * The use of list_for_each_entry_safe_reverse() is required
		 * because each task is going to be removed from and added back
		 * to the runnable_list during iteration. Because they're added
		 * to the tail of the list, safe reverse iteration can still
		 * visit all nodes.
		 */
		list_for_each_entry_safe_reverse(p, n, &rq->scx.runnable_list,
						 scx.runnable_node) {
			struct sched_enq_and_set_ctx ctx;

			/* cycling deq/enq is enough, see the function comment */
			sched_deq_and_put_task(p, DEQUEUE_SAVE | DEQUEUE_MOVE, &ctx);
			sched_enq_and_set_task(&ctx);
		}

		rq_unlock_irqrestore(rq, &rf);

		/* kick to restore ticks */
		resched_cpu(cpu);
	}
}

static void free_exit_info(struct scx_exit_info *ei)
{
	kfree(ei->dump);
	kfree(ei->msg);
	kfree(ei->bt);
	kfree(ei);
}

static struct scx_exit_info *alloc_exit_info(void)
{
	struct scx_exit_info *ei;

	ei = kzalloc(sizeof(*ei), GFP_KERNEL);
	if (!ei)
		return NULL;

	ei->bt = kcalloc(sizeof(ei->bt[0]), SCX_EXIT_BT_LEN, GFP_KERNEL);
	ei->msg = kzalloc(SCX_EXIT_MSG_LEN, GFP_KERNEL);
	ei->dump = kzalloc(SCX_EXIT_DUMP_LEN, GFP_KERNEL);

	if (!ei->bt || !ei->msg || !ei->dump) {
		free_exit_info(ei);
		return NULL;
	}

	return ei;
}

static const char *scx_exit_reason(enum scx_exit_kind kind)
{
	switch (kind) {
	case SCX_EXIT_UNREG:
		return "BPF scheduler unregistered";
	case SCX_EXIT_SYSRQ:
		return "disabled by sysrq-S";
	case SCX_EXIT_ERROR:
		return "runtime error";
	case SCX_EXIT_ERROR_BPF:
		return "scx_bpf_error";
	case SCX_EXIT_ERROR_STALL:
		return "runnable task stall";
	default:
		return "<UNKNOWN>";
	}
}

static void scx_ops_disable_workfn(struct kthread_work *work)
{
	struct scx_exit_info *ei = scx_exit_info;
	struct scx_task_iter sti;
	struct task_struct *p;
	struct rhashtable_iter rht_iter;
	struct scx_dispatch_q *dsq;
	int i, kind;

	kind = atomic_read(&scx_exit_kind);
	while (true) {
		/*
		 * NONE indicates that a new scx_ops has been registered since
		 * disable was scheduled - don't kill the new ops. DONE
		 * indicates that the ops has already been disabled.
		 */
		if (kind == SCX_EXIT_NONE || kind == SCX_EXIT_DONE)
			return;
		if (atomic_try_cmpxchg(&scx_exit_kind, &kind, SCX_EXIT_DONE))
			break;
	}
	ei->kind = kind;
	ei->reason = scx_exit_reason(ei->kind);

	/* guarantee forward progress by bypassing scx_ops */
	scx_ops_bypass(true);

	switch (scx_ops_set_enable_state(SCX_OPS_DISABLING)) {
	case SCX_OPS_DISABLING:
		WARN_ONCE(true, "sched_ext: duplicate disabling instance?");
		break;
	case SCX_OPS_DISABLED:
		pr_warn("sched_ext: ops error detected without ops (%s)\n",
			scx_exit_info->msg);
		WARN_ON_ONCE(scx_ops_set_enable_state(SCX_OPS_DISABLED) !=
			     SCX_OPS_DISABLING);
		goto done;
	default:
		break;
	}

	/*
	 * Here, every runnable task is guaranteed to make forward progress and
	 * we can safely use blocking synchronization constructs. Actually
	 * disable ops.
	 */
	mutex_lock(&scx_ops_enable_mutex);

	static_branch_disable(&__scx_switched_all);
	WRITE_ONCE(scx_switching_all, false);

	/*
	 * Avoid racing against fork and cgroup changes. See scx_ops_enable()
	 * for explanation on the locking order.
	 */
	percpu_down_write(&scx_fork_rwsem);
	cpus_read_lock();
	scx_cgroup_lock();

	spin_lock_irq(&scx_tasks_lock);
	scx_task_iter_init(&sti);
	while ((p = scx_task_iter_next_filtered_locked(&sti))) {
		const struct sched_class *old_class = p->sched_class;
		struct sched_enq_and_set_ctx ctx;

		sched_deq_and_put_task(p, DEQUEUE_SAVE | DEQUEUE_MOVE, &ctx);

		p->scx.slice = min_t(u64, p->scx.slice, SCX_SLICE_DFL);
		__setscheduler_prio(p, p->prio);
		check_class_changing(task_rq(p), p, old_class);

		sched_enq_and_set_task(&ctx);

		check_class_changed(task_rq(p), p, old_class, p->prio);
		scx_ops_exit_task(p);
	}
	scx_task_iter_exit(&sti);
	spin_unlock_irq(&scx_tasks_lock);

	/* no task is on scx, turn off all the switches and flush in-progress calls */
	static_branch_disable_cpuslocked(&__scx_ops_enabled);
	for (i = SCX_OPI_BEGIN; i < SCX_OPI_END; i++)
		static_branch_disable_cpuslocked(&scx_has_op[i]);
	static_branch_disable_cpuslocked(&scx_ops_enq_last);
	static_branch_disable_cpuslocked(&scx_ops_enq_exiting);
	static_branch_disable_cpuslocked(&scx_ops_cpu_preempt);
	static_branch_disable_cpuslocked(&scx_builtin_idle_enabled);
	synchronize_rcu();

	scx_cgroup_exit();

	scx_cgroup_unlock();
	cpus_read_unlock();
	percpu_up_write(&scx_fork_rwsem);

	if (ei->kind >= SCX_EXIT_ERROR) {
		printk(KERN_ERR "sched_ext: BPF scheduler \"%s\" errored, disabling\n", scx_ops.name);

		if (ei->msg[0] == '\0')
			printk(KERN_ERR "sched_ext: %s\n", ei->reason);
		else
			printk(KERN_ERR "sched_ext: %s (%s)\n", ei->reason, ei->msg);

		stack_trace_print(ei->bt, ei->bt_len, 2);
	}

	if (scx_ops.exit)
		SCX_CALL_OP(SCX_KF_UNLOCKED, exit, ei);

	cancel_delayed_work_sync(&scx_watchdog_work);
	/*
	 * Delete the kobject from the hierarchy eagerly in addition to just
	 * dropping a reference. Otherwise, if the object is deleted
	 * asynchronously, sysfs could observe an object of the same name still
	 * in the hierarchy when another scheduler is loaded.
	 */
	kobject_del(scx_root_kobj);
	kobject_put(scx_root_kobj);
	scx_root_kobj = NULL;

	memset(&scx_ops, 0, sizeof(scx_ops));

	rhashtable_walk_enter(&dsq_hash, &rht_iter);
	do {
		rhashtable_walk_start(&rht_iter);

		while ((dsq = rhashtable_walk_next(&rht_iter)) && !IS_ERR(dsq))
			destroy_dsq(dsq->id);

		rhashtable_walk_stop(&rht_iter);
	} while (dsq == ERR_PTR(-EAGAIN));
	rhashtable_walk_exit(&rht_iter);

	free_percpu(scx_dsp_buf);
	scx_dsp_buf = NULL;
	scx_dsp_max_batch = 0;

	free_exit_info(scx_exit_info);
	scx_exit_info = NULL;

	mutex_unlock(&scx_ops_enable_mutex);

	WARN_ON_ONCE(scx_ops_set_enable_state(SCX_OPS_DISABLED) !=
		     SCX_OPS_DISABLING);

	scx_cgroup_config_knobs();
done:
	scx_ops_bypass(false);
}

static DEFINE_KTHREAD_WORK(scx_ops_disable_work, scx_ops_disable_workfn);

static void schedule_scx_ops_disable_work(void)
{
	struct kthread_worker *helper = READ_ONCE(scx_ops_helper);

	/*
	 * We may be called spuriously before the first bpf_sched_ext_reg(). If
	 * scx_ops_helper isn't set up yet, there's nothing to do.
	 */
	if (helper)
		kthread_queue_work(helper, &scx_ops_disable_work);
}

static void scx_ops_disable(enum scx_exit_kind kind)
{
	int none = SCX_EXIT_NONE;

	if (WARN_ON_ONCE(kind == SCX_EXIT_NONE || kind == SCX_EXIT_DONE))
		kind = SCX_EXIT_ERROR;

	atomic_try_cmpxchg(&scx_exit_kind, &none, kind);

	schedule_scx_ops_disable_work();
}

static void scx_dump_task(struct seq_buf *s, struct task_struct *p, char marker,
			  unsigned long now)
{
	static unsigned long bt[SCX_EXIT_BT_LEN];
	char dsq_id_buf[19] = "(n/a)";
	unsigned long ops_state = atomic_long_read(&p->scx.ops_state);
	unsigned int bt_len;
	size_t avail, used;
	char *buf;

	if (p->scx.dsq)
		scnprintf(dsq_id_buf, sizeof(dsq_id_buf), "0x%llx",
			  (unsigned long long)p->scx.dsq->id);

	seq_buf_printf(s, "\n %c%c %s[%d] %+ldms\n",
		       marker, task_state_to_char(p), p->comm, p->pid,
		       jiffies_delta_msecs(p->scx.runnable_at, now));
	seq_buf_printf(s, "      scx_state/flags=%u/0x%x dsq_flags=0x%x ops_state/qseq=%lu/%lu\n",
		       scx_get_task_state(p),
		       p->scx.flags & ~SCX_TASK_STATE_MASK,
		       p->scx.dsq_flags,
		       ops_state & SCX_OPSS_STATE_MASK,
		       ops_state >> SCX_OPSS_QSEQ_SHIFT);
	seq_buf_printf(s, "      sticky/holding_cpu=%d/%d dsq_id=%s\n",
		       p->scx.sticky_cpu, p->scx.holding_cpu, dsq_id_buf);
	seq_buf_printf(s, "      cpus=%*pb\n\n", cpumask_pr_args(p->cpus_ptr));

	bt_len = stack_trace_save_tsk(p, bt, SCX_EXIT_BT_LEN, 1);

	avail = seq_buf_get_buf(s, &buf);
	used = stack_trace_snprint(buf, avail, bt, bt_len, 3);
	seq_buf_commit(s, used < avail ? used : -1);
}

static void scx_dump_state(struct scx_exit_info *ei)
{
	const char trunc_marker[] = "\n\n~~~~ TRUNCATED ~~~~\n";
	unsigned long now = jiffies;
	struct seq_buf s;
	size_t avail, used;
	char *buf;
	int cpu;

	seq_buf_init(&s, ei->dump, SCX_EXIT_DUMP_LEN - sizeof(trunc_marker));

	seq_buf_printf(&s, "%s[%d] triggered exit kind %d:\n  %s (%s)\n\n",
		       current->comm, current->pid, ei->kind, ei->reason, ei->msg);
	seq_buf_printf(&s, "Backtrace:\n");
	avail = seq_buf_get_buf(&s, &buf);
	used = stack_trace_snprint(buf, avail, ei->bt, ei->bt_len, 1);
	seq_buf_commit(&s, used < avail ? used : -1);

	seq_buf_printf(&s, "\nRunqueue states\n");
	seq_buf_printf(&s, "---------------\n");

	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);
		struct rq_flags rf;
		struct task_struct *p;

		rq_lock(rq, &rf);

		if (list_empty(&rq->scx.runnable_list) &&
		    rq->curr->sched_class == &idle_sched_class)
			goto next;

		seq_buf_printf(&s, "\nCPU %-4d: nr_run=%u flags=0x%x cpu_rel=%d ops_qseq=%lu pnt_seq=%lu\n",
			       cpu, rq->scx.nr_running, rq->scx.flags,
			       rq->scx.cpu_released, rq->scx.ops_qseq,
			       rq->scx.pnt_seq);
		seq_buf_printf(&s, "          curr=%s[%d] class=%ps\n",
			       rq->curr->comm, rq->curr->pid,
			       rq->curr->sched_class);
		if (!cpumask_empty(rq->scx.cpus_to_kick))
			seq_buf_printf(&s, "  cpus_to_kick   : %*pb\n",
				       cpumask_pr_args(rq->scx.cpus_to_kick));
		if (!cpumask_empty(rq->scx.cpus_to_preempt))
			seq_buf_printf(&s, "  cpus_to_preempt: %*pb\n",
				       cpumask_pr_args(rq->scx.cpus_to_preempt));
		if (!cpumask_empty(rq->scx.cpus_to_wait))
			seq_buf_printf(&s, "  cpus_to_wait   : %*pb\n",
				       cpumask_pr_args(rq->scx.cpus_to_wait));

		if (rq->curr->sched_class == &ext_sched_class)
			scx_dump_task(&s, rq->curr, '*', now);

		list_for_each_entry(p, &rq->scx.runnable_list, scx.runnable_node)
			scx_dump_task(&s, p, ' ', now);
	next:
		rq_unlock(rq, &rf);
	}

	if (seq_buf_has_overflowed(&s)) {
		used = strlen(seq_buf_str(&s));
		memcpy(ei->dump + used, trunc_marker, sizeof(trunc_marker));
	}
}

static void scx_ops_error_irq_workfn(struct irq_work *irq_work)
{
	scx_dump_state(scx_exit_info);
	schedule_scx_ops_disable_work();
}

static DEFINE_IRQ_WORK(scx_ops_error_irq_work, scx_ops_error_irq_workfn);

__printf(2, 3) void scx_ops_error_kind(enum scx_exit_kind kind,
				       const char *fmt, ...)
{
	struct scx_exit_info *ei = scx_exit_info;
	int none = SCX_EXIT_NONE;
	va_list args;

	if (!atomic_try_cmpxchg(&scx_exit_kind, &none, kind))
		return;

	ei->bt_len = stack_trace_save(ei->bt, SCX_EXIT_BT_LEN, 1);

	va_start(args, fmt);
	vscnprintf(ei->msg, SCX_EXIT_MSG_LEN, fmt, args);
	va_end(args);

	/*
	 * Set ei->kind and ->reason for scx_dump_state(). They'll be set again
	 * in scx_ops_disable_workfn().
	 */
	ei->kind = kind;
	ei->reason = scx_exit_reason(ei->kind);

	irq_work_queue(&scx_ops_error_irq_work);
}

static struct kthread_worker *scx_create_rt_helper(const char *name)
{
	struct kthread_worker *helper;

	helper = kthread_create_worker(0, name);
	if (helper)
		sched_set_fifo(helper->task);
	return helper;
}

static int validate_ops(const struct sched_ext_ops *ops)
{
	/*
	 * It doesn't make sense to specify the SCX_OPS_ENQ_LAST flag if the
	 * ops.enqueue() callback isn't implemented.
	 */
	if ((ops->flags & SCX_OPS_ENQ_LAST) && !ops->enqueue) {
		scx_ops_error("SCX_OPS_ENQ_LAST requires ops.enqueue() to be implemented");
		return -EINVAL;
	}

	return 0;
}

static int scx_ops_enable(struct sched_ext_ops *ops)
{
	struct scx_task_iter sti;
	struct task_struct *p;
	unsigned long timeout;
	int i, ret;

	mutex_lock(&scx_ops_enable_mutex);

	if (!scx_ops_helper) {
		WRITE_ONCE(scx_ops_helper,
			   scx_create_rt_helper("sched_ext_ops_helper"));
		if (!scx_ops_helper) {
			ret = -ENOMEM;
			goto err_unlock;
		}
	}

	if (scx_ops_enable_state() != SCX_OPS_DISABLED) {
		ret = -EBUSY;
		goto err_unlock;
	}

	scx_root_kobj = kzalloc(sizeof(*scx_root_kobj), GFP_KERNEL);
	if (!scx_root_kobj) {
		ret = -ENOMEM;
		goto err_unlock;
	}

	scx_root_kobj->kset = scx_kset;
	ret = kobject_init_and_add(scx_root_kobj, &scx_ktype, NULL, "root");
	if (ret < 0)
		goto err;

	scx_exit_info = alloc_exit_info();
	if (!scx_exit_info) {
		ret = -ENOMEM;
		goto err_del;
	}

	/*
	 * Set scx_ops, transition to PREPPING and clear exit info to arm the
	 * disable path. Failure triggers full disabling from here on.
	 */
	scx_ops = *ops;

	WARN_ON_ONCE(scx_ops_set_enable_state(SCX_OPS_PREPPING) !=
		     SCX_OPS_DISABLED);

	atomic_set(&scx_exit_kind, SCX_EXIT_NONE);
	scx_warned_zero_slice = false;

	atomic_long_set(&scx_nr_rejected, 0);

	/*
	 * Keep CPUs stable during enable so that the BPF scheduler can track
	 * online CPUs by watching ->on/offline_cpu() after ->init().
	 */
	cpus_read_lock();

	scx_switch_all_req = false;
	if (scx_ops.init) {
		ret = SCX_CALL_OP_RET(SCX_KF_INIT, init);
		if (ret) {
			ret = ops_sanitize_err("init", ret);
			goto err_disable_unlock_cpus;
		}

		/*
		 * Exit early if ops.init() triggered scx_bpf_error(). Not
		 * strictly necessary as we'll fail transitioning into ENABLING
		 * later but that'd be after calling ops.init_task() on all
		 * tasks and with -EBUSY which isn't very intuitive. Let's exit
		 * early with success so that the condition is notified through
		 * ops.exit() like other scx_bpf_error() invocations.
		 */
		if (atomic_read(&scx_exit_kind) != SCX_EXIT_NONE)
			goto err_disable_unlock_cpus;
	}

	for (i = SCX_OPI_CPU_HOTPLUG_BEGIN; i < SCX_OPI_CPU_HOTPLUG_END; i++)
		if (((void (**)(void))ops)[i])
			static_branch_enable_cpuslocked(&scx_has_op[i]);

	cpus_read_unlock();

	ret = validate_ops(ops);
	if (ret)
		goto err_disable;

	WARN_ON_ONCE(scx_dsp_buf);
	scx_dsp_max_batch = ops->dispatch_max_batch ?: SCX_DSP_DFL_MAX_BATCH;
	scx_dsp_buf = __alloc_percpu(sizeof(scx_dsp_buf[0]) * scx_dsp_max_batch,
				     __alignof__(scx_dsp_buf[0]));
	if (!scx_dsp_buf) {
		ret = -ENOMEM;
		goto err_disable;
	}

	if (ops->timeout_ms)
		timeout = msecs_to_jiffies(ops->timeout_ms);
	else
		timeout = SCX_WATCHDOG_MAX_TIMEOUT;

	WRITE_ONCE(scx_watchdog_timeout, timeout);
	WRITE_ONCE(scx_watchdog_timestamp, jiffies);
	queue_delayed_work(system_unbound_wq, &scx_watchdog_work,
			   scx_watchdog_timeout / 2);

	/*
	 * Lock out forks, cgroup on/offlining and moves before opening the
	 * floodgate so that they don't wander into the operations prematurely.
	 *
	 * We don't need to keep the CPUs stable but static_branch_*() requires
	 * cpus_read_lock() and scx_cgroup_rwsem must nest inside
	 * cpu_hotplug_lock because of the following dependency chain:
	 *
	 *   cpu_hotplug_lock --> cgroup_threadgroup_rwsem --> scx_cgroup_rwsem
	 *
	 * So, we need to do cpus_read_lock() before scx_cgroup_lock() and use
	 * static_branch_*_cpuslocked().
	 *
	 * Note that cpu_hotplug_lock must nest inside scx_fork_rwsem due to the
	 * following dependency chain:
	 *
	 *   scx_fork_rwsem --> pernet_ops_rwsem --> cpu_hotplug_lock
	 */
	percpu_down_write(&scx_fork_rwsem);
	cpus_read_lock();
	scx_cgroup_lock();

	for (i = SCX_OPI_NORMAL_BEGIN; i < SCX_OPI_NORMAL_END; i++)
		if (((void (**)(void))ops)[i])
			static_branch_enable_cpuslocked(&scx_has_op[i]);

	if (ops->flags & SCX_OPS_ENQ_LAST)
		static_branch_enable_cpuslocked(&scx_ops_enq_last);

	if (ops->flags & SCX_OPS_ENQ_EXITING)
		static_branch_enable_cpuslocked(&scx_ops_enq_exiting);
	if (scx_ops.cpu_acquire || scx_ops.cpu_release)
		static_branch_enable_cpuslocked(&scx_ops_cpu_preempt);

	if (!ops->update_idle || (ops->flags & SCX_OPS_KEEP_BUILTIN_IDLE)) {
		reset_idle_masks();
		static_branch_enable_cpuslocked(&scx_builtin_idle_enabled);
	} else {
		static_branch_disable_cpuslocked(&scx_builtin_idle_enabled);
	}

	/*
	 * All cgroups should be initialized before letting in tasks. cgroup
	 * on/offlining and task migrations are already locked out.
	 */
	ret = scx_cgroup_init();
	if (ret)
		goto err_disable_unlock_all;

	static_branch_enable_cpuslocked(&__scx_ops_enabled);

	/*
	 * Enable ops for every task. Fork is excluded by scx_fork_rwsem
	 * preventing new tasks from being added. No need to exclude tasks
	 * leaving as sched_ext_free() can handle both prepped and enabled
	 * tasks. Prep all tasks first and then enable them with preemption
	 * disabled.
	 */
	spin_lock_irq(&scx_tasks_lock);

	scx_task_iter_init(&sti);
	while ((p = scx_task_iter_next_filtered(&sti))) {
		get_task_struct(p);
		spin_unlock_irq(&scx_tasks_lock);

		ret = scx_ops_init_task(p, task_group(p), false);
		if (ret) {
			put_task_struct(p);
			spin_lock_irq(&scx_tasks_lock);
			scx_task_iter_exit(&sti);
			spin_unlock_irq(&scx_tasks_lock);
			pr_err("sched_ext: ops.init_task() failed (%d) for %s[%d] while loading\n",
			       ret, p->comm, p->pid);
			goto err_disable_unlock_all;
		}

		put_task_struct(p);
		spin_lock_irq(&scx_tasks_lock);
	}
	scx_task_iter_exit(&sti);

	/*
	 * All tasks are prepped but are still ops-disabled. Ensure that
	 * %current can't be scheduled out and switch everyone.
	 * preempt_disable() is necessary because we can't guarantee that
	 * %current won't be starved if scheduled out while switching.
	 */
	preempt_disable();

	/*
	 * From here on, the disable path must assume that tasks have ops
	 * enabled and need to be recovered.
	 */
	if (!scx_ops_tryset_enable_state(SCX_OPS_ENABLING, SCX_OPS_PREPPING)) {
		preempt_enable();
		spin_unlock_irq(&scx_tasks_lock);
		ret = -EBUSY;
		goto err_disable_unlock_all;
	}

	/*
	 * We're fully committed and can't fail. The PREPPED -> ENABLED
	 * transitions here are synchronized against sched_ext_free() through
	 * scx_tasks_lock.
	 */
	WRITE_ONCE(scx_switching_all, scx_switch_all_req);

	scx_task_iter_init(&sti);
	while ((p = scx_task_iter_next_filtered_locked(&sti))) {
		const struct sched_class *old_class = p->sched_class;
		struct sched_enq_and_set_ctx ctx;

		sched_deq_and_put_task(p, DEQUEUE_SAVE | DEQUEUE_MOVE, &ctx);

		scx_set_task_state(p, SCX_TASK_READY);
		__setscheduler_prio(p, p->prio);
		check_class_changing(task_rq(p), p, old_class);

		sched_enq_and_set_task(&ctx);

		check_class_changed(task_rq(p), p, old_class, p->prio);
	}
	scx_task_iter_exit(&sti);

	spin_unlock_irq(&scx_tasks_lock);
	preempt_enable();
	scx_cgroup_unlock();
	cpus_read_unlock();
	percpu_up_write(&scx_fork_rwsem);

	if (!scx_ops_tryset_enable_state(SCX_OPS_ENABLED, SCX_OPS_ENABLING)) {
		ret = -EBUSY;
		goto err_disable;
	}

	if (scx_switch_all_req)
		static_branch_enable(&__scx_switched_all);

	kobject_uevent(scx_root_kobj, KOBJ_ADD);
	mutex_unlock(&scx_ops_enable_mutex);

	scx_cgroup_config_knobs();

	return 0;

err_del:
	kobject_del(scx_root_kobj);
err:
	kobject_put(scx_root_kobj);
	scx_root_kobj = NULL;
	if (scx_exit_info) {
		free_exit_info(scx_exit_info);
		scx_exit_info = NULL;
	}
err_unlock:
	mutex_unlock(&scx_ops_enable_mutex);
	return ret;

err_disable_unlock_all:
	scx_cgroup_unlock();
	percpu_up_write(&scx_fork_rwsem);
err_disable_unlock_cpus:
	cpus_read_unlock();
err_disable:
	mutex_unlock(&scx_ops_enable_mutex);
	/* must be fully disabled before returning */
	scx_ops_disable(SCX_EXIT_ERROR);
	kthread_flush_work(&scx_ops_disable_work);
	return ret;
}


/********************************************************************************
 * bpf_struct_ops plumbing.
 */
#include <linux/bpf_verifier.h>
#include <linux/bpf.h>

extern struct btf *btf_vmlinux;
static const struct btf_type *task_struct_type;
static u32 task_struct_type_id;

/* Make the 2nd argument of .dispatch a pointer that can be NULL. */
static bool promote_dispatch_2nd_arg(int off, int size,
                                     enum bpf_access_type type,
                                     const struct bpf_prog *prog,
                                     struct bpf_insn_access_aux *info)
{
	const struct bpf_struct_ops *st_ops;
	const struct btf_member *member;
        const struct btf_type *t;
        u32 btf_id, member_idx;
	const char *mname;

        /* btf_id should be the type id of struct sched_ext_ops */
	btf_id = prog->aux->attach_btf_id;
	st_ops = bpf_struct_ops_find(btf_id);
	if (!st_ops)
                return false;

        /* BTF type of struct sched_ext_ops */
        t = st_ops->type;

	member_idx = prog->expected_attach_type;
	if (member_idx >= btf_type_vlen(t))
                return false;

        /*
	 * Get the member name of this struct_ops program, which corresponds to
	 * a field in struct sched_ext_ops. For example, the member name of the
	 * dispatch struct_ops program (callback) is "dispatch".
         */
	member = &btf_type_member(t)[member_idx];
	mname = btf_name_by_offset(btf_vmlinux, member->name_off);

        /*
	 * Check if it is the second argument of the function pointer at
	 * "dispatch" in struct sched_ext_ops. The arguments of struct_ops
	 * operators are sequential and 64-bit, so the second argument is at
	 * offset sizeof(__u64).
         */
        if (strcmp(mname, "dispatch") == 0 &&
            off == sizeof(__u64)) {
                /*
		 * The value is a pointer to a type (struct task_struct) given
		 * by a BTF ID (PTR_TO_BTF_ID). It is trusted (PTR_TRUSTED),
		 * however, can be a NULL (PTR_MAYBE_NULL). The BPF program
		 * should check the pointer to make sure it is not NULL before
		 * using it, or the verifier will reject the program.
		 *
		 * Longer term, this is something that should be addressed by
		 * BTF, and be fully contained within the verifier.
                 */
                info->reg_type = PTR_MAYBE_NULL | PTR_TO_BTF_ID |
                  PTR_TRUSTED;
                info->btf = btf_vmlinux;
                info->btf_id = task_struct_type_id;

                return true;
        }

        return false;
}

static bool bpf_scx_is_valid_access(int off, int size,
				    enum bpf_access_type type,
				    const struct bpf_prog *prog,
				    struct bpf_insn_access_aux *info)
{
	if (type != BPF_READ)
		return false;
        if (promote_dispatch_2nd_arg(off, size, type, prog, info))
                return true;
	if (off < 0 || off >= sizeof(__u64) * MAX_BPF_FUNC_ARGS)
		return false;
	if (off % size != 0)
		return false;

	return btf_ctx_access(off, size, type, prog, info);
}

static int bpf_scx_btf_struct_access(struct bpf_verifier_log *log,
				     const struct bpf_reg_state *reg, int off,
				     int size)
{
	const struct btf_type *t;

	t = btf_type_by_id(reg->btf, reg->btf_id);
	if (t == task_struct_type) {
		if (off >= offsetof(struct task_struct, scx.slice) &&
		    off + size <= offsetofend(struct task_struct, scx.slice))
			return SCALAR_VALUE;
		if (off >= offsetof(struct task_struct, scx.dsq_vtime) &&
		    off + size <= offsetofend(struct task_struct, scx.dsq_vtime))
			return SCALAR_VALUE;
		if (off >= offsetof(struct task_struct, scx.disallow) &&
		    off + size <= offsetofend(struct task_struct, scx.disallow))
			return SCALAR_VALUE;
	}

	return -EACCES;
}

static const struct bpf_func_proto *
bpf_scx_get_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	switch (func_id) {
	case BPF_FUNC_task_storage_get:
		return &bpf_task_storage_get_proto;
	case BPF_FUNC_task_storage_delete:
		return &bpf_task_storage_delete_proto;
	default:
		return bpf_base_func_proto(func_id);
	}
}

const struct bpf_verifier_ops bpf_scx_verifier_ops = {
	.get_func_proto = bpf_scx_get_func_proto,
	.is_valid_access = bpf_scx_is_valid_access,
	.btf_struct_access = bpf_scx_btf_struct_access,
};

static int bpf_scx_init_member(const struct btf_type *t,
			       const struct btf_member *member,
			       void *kdata, const void *udata)
{
	const struct sched_ext_ops *uops = udata;
	struct sched_ext_ops *ops = kdata;
	u32 moff = __btf_member_bit_offset(t, member) / 8;
	int ret;

	switch (moff) {
	case offsetof(struct sched_ext_ops, dispatch_max_batch):
		if (*(u32 *)(udata + moff) > INT_MAX)
			return -E2BIG;
		ops->dispatch_max_batch = *(u32 *)(udata + moff);
		return 1;
	case offsetof(struct sched_ext_ops, flags):
		if (*(u64 *)(udata + moff) & ~SCX_OPS_ALL_FLAGS)
			return -EINVAL;
		ops->flags = *(u64 *)(udata + moff);
		return 1;
	case offsetof(struct sched_ext_ops, name):
		ret = bpf_obj_name_cpy(ops->name, uops->name,
				       sizeof(ops->name));
		if (ret < 0)
			return ret;
		if (ret == 0)
			return -EINVAL;
		return 1;
	case offsetof(struct sched_ext_ops, timeout_ms):
		if (msecs_to_jiffies(*(u32 *)(udata + moff)) >
		    SCX_WATCHDOG_MAX_TIMEOUT)
			return -E2BIG;
		ops->timeout_ms = *(u32 *)(udata + moff);
		return 1;
	}

	return 0;
}

static int bpf_scx_check_member(const struct btf_type *t,
				const struct btf_member *member,
				const struct bpf_prog *prog)
{
	u32 moff = __btf_member_bit_offset(t, member) / 8;

	switch (moff) {
	case offsetof(struct sched_ext_ops, init_task):
#ifdef CONFIG_EXT_GROUP_SCHED
	case offsetof(struct sched_ext_ops, cgroup_init):
	case offsetof(struct sched_ext_ops, cgroup_exit):
	case offsetof(struct sched_ext_ops, cgroup_prep_move):
#endif
	case offsetof(struct sched_ext_ops, init):
	case offsetof(struct sched_ext_ops, exit):
		break;
	default:
		if (prog->aux->sleepable)
			return -EINVAL;
	}

	return 0;
}

static int bpf_scx_reg(void *kdata)
{
	return scx_ops_enable(kdata);
}

static void bpf_scx_unreg(void *kdata)
{
	scx_ops_disable(SCX_EXIT_UNREG);
	kthread_flush_work(&scx_ops_disable_work);
}

static int bpf_scx_init(struct btf *btf)
{
	u32 type_id;

	type_id = btf_find_by_name_kind(btf, "task_struct", BTF_KIND_STRUCT);
	if (type_id < 0)
		return -EINVAL;
	task_struct_type = btf_type_by_id(btf, type_id);
        task_struct_type_id = type_id;

	return 0;
}

static int bpf_scx_update(void *kdata, void *old_kdata)
{
	/*
	 * sched_ext does not support updating the actively-loaded BPF
	 * scheduler, as registering a BPF scheduler can always fail if the
	 * scheduler returns an error code for e.g. ops.init(),
	 * ops.init_task(), etc. Similarly, we can always race with
	 * unregistration happening elsewhere, such as with sysrq.
	 */
	return -EOPNOTSUPP;
}

static int bpf_scx_validate(void *kdata)
{
	return 0;
}

/* "extern" to avoid sparse warning, only used in this file */
extern struct bpf_struct_ops bpf_sched_ext_ops;

static s32 select_cpu_stub(struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	return -EINVAL;
}

static void enqueue_stub(struct task_struct *p, u64 enq_flags)
{}

static void dequeue_stub(struct task_struct *p, u64 enq_flags)
{}

static void dispatch_stub(s32 prev_cpu, struct task_struct *p)
{}

static void runnable_stub(struct task_struct *p, u64 enq_flags)
{}

static void running_stub(struct task_struct *p)
{}

static void stopping_stub(struct task_struct *p, bool runnable)
{}

static void quiescent_stub(struct task_struct *p, u64 deq_flags)
{}

static bool yield_stub(struct task_struct *from, struct task_struct *to)
{
	return false;
}

static bool core_sched_before_stub(struct task_struct *a, struct task_struct *b)
{
	return false;
}

static void set_weight_stub(struct task_struct *p, u32 weight)
{}

static void set_cpumask_stub(struct task_struct *p, const struct cpumask *mask)
{}

static void update_idle_stub(s32 cpu, bool idle)
{}

static void cpu_acquire_stub(s32 cpu, struct scx_cpu_acquire_args *args)
{}

static void cpu_release_stub(s32 cpu, struct scx_cpu_release_args *args)
{}

static s32 init_task_stub(struct task_struct *p,
			  struct scx_init_task_args *args)
{
	return -EINVAL;
}


static void exit_task_stub(struct task_struct *p,
			   struct scx_exit_task_args *args)
{}

static void enable_stub(struct task_struct *p)
{}

static void disable_stub(struct task_struct *p)
{}

#ifdef CONFIG_EXT_GROUP_SCHED
static s32 cgroup_init_stub(struct cgroup *cgrp,
			    struct scx_cgroup_init_args *args)
{
	return -EINVAL;
}

static void cgroup_exit_stub(struct cgroup *cgrp)
{}

static s32 cgroup_prep_move_stub(struct task_struct *p,
				 struct cgroup *from, struct cgroup *to)
{
	return -EINVAL;
}

static void cgroup_move_stub(struct task_struct *p,
			     struct cgroup *from, struct cgroup *to)
{}

static void cgroup_cancel_move_stub(struct task_struct *p,
				    struct cgroup *from, struct cgroup *to)
{}

static void cgroup_set_weight_stub(struct cgroup *cgrp, u32 weight)
{}
#endif

static void cpu_online_stub(s32 cpu)
{}

static void cpu_offline_stub(s32 cpu)
{}

static s32 init_stub(void)
{
	return -EINVAL;
}

static void exit_stub(struct scx_exit_info *info)
{}

static struct sched_ext_ops __bpf_ops_sched_ext_ops = {
	.select_cpu = select_cpu_stub,
	.enqueue = enqueue_stub,
	.dequeue = dequeue_stub,
	.dispatch = dispatch_stub,
	.runnable = runnable_stub,
	.running = running_stub,
	.stopping = stopping_stub,
	.quiescent = quiescent_stub,
	.yield = yield_stub,
	.core_sched_before = core_sched_before_stub,
	.set_weight = set_weight_stub,
	.set_cpumask = set_cpumask_stub,
	.update_idle = update_idle_stub,
	.cpu_acquire = cpu_acquire_stub,
	.cpu_release = cpu_release_stub,
	.init_task = init_task_stub,
	.exit_task = exit_task_stub,
	.enable = enable_stub,
	.disable = disable_stub,
#ifdef CONFIG_EXT_GROUP_SCHED
	.cgroup_init = cgroup_init_stub,
	.cgroup_exit = cgroup_exit_stub,
	.cgroup_prep_move = cgroup_prep_move_stub,
	.cgroup_move = cgroup_move_stub,
	.cgroup_cancel_move = cgroup_cancel_move_stub,
	.cgroup_set_weight = cgroup_set_weight_stub,
#endif
	.cpu_online = cpu_online_stub,
	.cpu_offline = cpu_offline_stub,
	.init = init_stub,
	.exit = exit_stub,
};

struct bpf_struct_ops bpf_sched_ext_ops = {
	.verifier_ops = &bpf_scx_verifier_ops,
	.reg = bpf_scx_reg,
	.unreg = bpf_scx_unreg,
	.check_member = bpf_scx_check_member,
	.init_member = bpf_scx_init_member,
	.init = bpf_scx_init,
	.update = bpf_scx_update,
	.validate = bpf_scx_validate,
	.name = "sched_ext_ops",
	.cfi_stubs = &__bpf_ops_sched_ext_ops
};


/********************************************************************************
 * System integration and init.
 */

static void sysrq_handle_sched_ext_reset(u8 key)
{
	if (scx_ops_helper)
		scx_ops_disable(SCX_EXIT_SYSRQ);
	else
		pr_info("sched_ext: BPF scheduler not yet used\n");
}

static const struct sysrq_key_op sysrq_sched_ext_reset_op = {
	.handler	= sysrq_handle_sched_ext_reset,
	.help_msg	= "reset-sched-ext(S)",
	.action_msg	= "Disable sched_ext and revert all tasks to CFS",
	.enable_mask	= SYSRQ_ENABLE_RTNICE,
};

static bool can_skip_idle_kick(struct rq *rq)
{
	lockdep_assert_rq_held(rq);

	/*
	 * We can skip idle kicking if @rq is going to go through at least one
	 * full SCX scheduling cycle before going idle. Just checking whether
	 * curr is not idle is insufficient because we could be racing
	 * balance_one() trying to pull the next task from a remote rq, which
	 * may fail, and @rq may become idle afterwards.
	 *
	 * The race window is small and we don't and can't guarantee that @rq is
	 * only kicked while idle anyway. Skip only when sure.
	 */
	return !is_idle_task(rq->curr) && !(rq->scx.flags & SCX_RQ_BALANCING);
}

static bool kick_one_cpu(s32 cpu, struct rq *this_rq, unsigned long *pseqs)
{
	struct rq *rq = cpu_rq(cpu);
	struct scx_rq *this_scx = &this_rq->scx;
	bool should_wait = false;
	unsigned long flags;

	raw_spin_rq_lock_irqsave(rq, flags);

	/*
	 * During CPU hotplug, a CPU may depend on kicking itself to make
	 * forward progress. Allow kicking self regardless of online state.
	 */
	if (cpu_online(cpu) || cpu == cpu_of(this_rq)) {
		if (cpumask_test_cpu(cpu, this_scx->cpus_to_preempt)) {
			if (rq->curr->sched_class == &ext_sched_class)
				rq->curr->scx.slice = 0;
			cpumask_clear_cpu(cpu, this_scx->cpus_to_preempt);
		}

		if (cpumask_test_cpu(cpu, this_scx->cpus_to_wait)) {
			pseqs[cpu] = rq->scx.pnt_seq;
			should_wait = true;
		}

		resched_curr(rq);
	} else {
		cpumask_clear_cpu(cpu, this_scx->cpus_to_preempt);
		cpumask_clear_cpu(cpu, this_scx->cpus_to_wait);
	}

	raw_spin_rq_unlock_irqrestore(rq, flags);

	return should_wait;
}

static void kick_one_cpu_if_idle(s32 cpu, struct rq *this_rq)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	raw_spin_rq_lock_irqsave(rq, flags);

	if (!can_skip_idle_kick(rq) &&
	    (cpu_online(cpu) || cpu == cpu_of(this_rq)))
		resched_curr(rq);

	raw_spin_rq_unlock_irqrestore(rq, flags);
}

static void kick_cpus_irq_workfn(struct irq_work *irq_work)
{
	struct rq *this_rq = this_rq();
	struct scx_rq *this_scx = &this_rq->scx;
	unsigned long *pseqs = this_cpu_ptr(scx_kick_cpus_pnt_seqs);
	bool should_wait = false;
	s32 cpu;

	for_each_cpu(cpu, this_scx->cpus_to_kick) {
		should_wait |= kick_one_cpu(cpu, this_rq, pseqs);
		cpumask_clear_cpu(cpu, this_scx->cpus_to_kick);
		cpumask_clear_cpu(cpu, this_scx->cpus_to_kick_if_idle);
	}

	for_each_cpu(cpu, this_scx->cpus_to_kick_if_idle) {
		kick_one_cpu_if_idle(cpu, this_rq);
		cpumask_clear_cpu(cpu, this_scx->cpus_to_kick_if_idle);
	}

	if (!should_wait)
		return;

	for_each_cpu(cpu, this_scx->cpus_to_wait) {
		unsigned long *wait_pnt_seq = &cpu_rq(cpu)->scx.pnt_seq;

		if (cpu != cpu_of(this_rq)) {
			/*
			 * Pairs with smp_store_release() issued by this CPU in
			 * scx_notify_pick_next_task() on the resched path.
			 *
			 * We busy-wait here to guarantee that no other task can
			 * be scheduled on our core before the target CPU has
			 * entered the resched path.
			 */
			while (smp_load_acquire(wait_pnt_seq) == pseqs[cpu])
				cpu_relax();
		}

		cpumask_clear_cpu(cpu, this_scx->cpus_to_wait);
	}
}

/**
 * print_scx_info - print out sched_ext scheduler state
 * @log_lvl: the log level to use when printing
 * @p: target task
 *
 * If a sched_ext scheduler is enabled, print the name and state of the
 * scheduler. If @p is on sched_ext, print further information about the task.
 *
 * This function can be safely called on any task as long as the task_struct
 * itself is accessible. While safe, this function isn't synchronized and may
 * print out mixups or garbages of limited length.
 */
void print_scx_info(const char *log_lvl, struct task_struct *p)
{
	enum scx_ops_enable_state state = scx_ops_enable_state();
	const char *all = READ_ONCE(scx_switching_all) ? "+all" : "";
	char runnable_at_buf[22] = "?";
	struct sched_class *class;
	unsigned long runnable_at;

	if (state == SCX_OPS_DISABLED)
		return;

	/*
	 * Carefully check if the task was running on sched_ext, and then
	 * carefully copy the time it's been runnable, and its state.
	 */
	if (copy_from_kernel_nofault(&class, &p->sched_class, sizeof(class)) ||
	    class != &ext_sched_class) {
		printk("%sSched_ext: %s (%s%s)", log_lvl, scx_ops.name,
		       scx_ops_enable_state_str[state], all);
		return;
	}

	if (!copy_from_kernel_nofault(&runnable_at, &p->scx.runnable_at,
				      sizeof(runnable_at)))
		scnprintf(runnable_at_buf, sizeof(runnable_at_buf), "%+ldms",
			  jiffies_delta_msecs(runnable_at, jiffies));

	/* print everything onto one line to conserve console space */
	printk("%sSched_ext: %s (%s%s), task: runnable_at=%s",
	       log_lvl, scx_ops.name, scx_ops_enable_state_str[state], all,
	       runnable_at_buf);
}

static int scx_pm_handler(struct notifier_block *nb, unsigned long event, void *ptr)
{
	if (!scx_enabled())
		return NOTIFY_OK;

	/*
	 * SCX schedulers often have userspace components which are sometimes
	 * involved in critial scheduling paths. PM operations involve freezing
	 * userspace which can lead to scheduling misbehaviors including stalls.
	 * Let's bypass while PM operations are in progress.
	 */
	switch (event) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
	case PM_RESTORE_PREPARE:
		scx_ops_bypass(true);
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
	case PM_POST_RESTORE:
		scx_ops_bypass(false);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block scx_pm_notifier = {
	.notifier_call = scx_pm_handler,
};

void __init init_sched_ext_class(void)
{
	s32 cpu, v;

	/*
	 * The following is to prevent the compiler from optimizing out the enum
	 * definitions so that BPF scheduler implementations can use them
	 * through the generated vmlinux.h.
	 */
	WRITE_ONCE(v, SCX_WAKE_EXEC | SCX_ENQ_WAKEUP | SCX_DEQ_SLEEP |
		   SCX_TG_ONLINE | SCX_KICK_PREEMPT);

	BUG_ON(rhashtable_init(&dsq_hash, &dsq_hash_params));
	init_dsq(&scx_dsq_global, SCX_DSQ_GLOBAL);
#ifdef CONFIG_SMP
	BUG_ON(!alloc_cpumask_var(&idle_masks.cpu, GFP_KERNEL));
	BUG_ON(!alloc_cpumask_var(&idle_masks.smt, GFP_KERNEL));
#endif
	scx_kick_cpus_pnt_seqs =
		__alloc_percpu(sizeof(scx_kick_cpus_pnt_seqs[0]) *
			       num_possible_cpus(),
			       __alignof__(scx_kick_cpus_pnt_seqs[0]));
	BUG_ON(!scx_kick_cpus_pnt_seqs);

	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		init_dsq(&rq->scx.local_dsq, SCX_DSQ_LOCAL);
		INIT_LIST_HEAD(&rq->scx.runnable_list);

		BUG_ON(!zalloc_cpumask_var(&rq->scx.cpus_to_kick, GFP_KERNEL));
		BUG_ON(!zalloc_cpumask_var(&rq->scx.cpus_to_kick_if_idle, GFP_KERNEL));
		BUG_ON(!zalloc_cpumask_var(&rq->scx.cpus_to_preempt, GFP_KERNEL));
		BUG_ON(!zalloc_cpumask_var(&rq->scx.cpus_to_wait, GFP_KERNEL));
		init_irq_work(&rq->scx.kick_cpus_irq_work, kick_cpus_irq_workfn);
	}

	register_sysrq_key('S', &sysrq_sched_ext_reset_op);
	INIT_DELAYED_WORK(&scx_watchdog_work, scx_watchdog_workfn);
	scx_cgroup_config_knobs();
}


/********************************************************************************
 * Helpers that can be called from the BPF scheduler.
 */
#include <linux/btf_ids.h>

__bpf_kfunc_start_defs();

/**
 * scx_bpf_switch_all - Switch all tasks into SCX
 *
 * Switch all existing and future non-dl/rt tasks to SCX. This can only be
 * called from ops.init(), and actual switching is performed asynchronously.
 */
__bpf_kfunc void scx_bpf_switch_all(void)
{
	if (!scx_kf_allowed(SCX_KF_INIT))
		return;

	scx_switch_all_req = true;
}

BTF_SET8_START(scx_kfunc_ids_init)
BTF_ID_FLAGS(func, scx_bpf_switch_all)
BTF_SET8_END(scx_kfunc_ids_init)

static const struct btf_kfunc_id_set scx_kfunc_set_init = {
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_init,
};

/**
 * scx_bpf_create_dsq - Create a custom DSQ
 * @dsq_id: DSQ to create
 * @node: NUMA node to allocate from
 *
 * Create a custom DSQ identified by @dsq_id. Can be called from ops.init(),
 * ops.init_task(), ops.cgroup_init() and ops.cgroup_prep_move().
 */
__bpf_kfunc s32 scx_bpf_create_dsq(u64 dsq_id, s32 node)
{
	if (!scx_kf_allowed(SCX_KF_INIT | SCX_KF_SLEEPABLE))
		return -EINVAL;

	if (unlikely(node >= (int)nr_node_ids ||
		     (node < 0 && node != NUMA_NO_NODE)))
		return -EINVAL;
	return PTR_ERR_OR_ZERO(create_dsq(dsq_id, node));
}

BTF_SET8_START(scx_kfunc_ids_sleepable)
BTF_ID_FLAGS(func, scx_bpf_create_dsq, KF_SLEEPABLE)
BTF_SET8_END(scx_kfunc_ids_sleepable)

static const struct btf_kfunc_id_set scx_kfunc_set_sleepable = {
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_sleepable,
};

__bpf_kfunc_end_defs();

static bool scx_dispatch_preamble(struct task_struct *p, u64 enq_flags)
{
	if (!scx_kf_allowed(SCX_KF_ENQUEUE | SCX_KF_DISPATCH))
		return false;

	lockdep_assert_irqs_disabled();

	if (unlikely(!p)) {
		scx_ops_error("called with NULL task");
		return false;
	}

	if (unlikely(enq_flags & __SCX_ENQ_INTERNAL_MASK)) {
		scx_ops_error("invalid enq_flags 0x%llx", enq_flags);
		return false;
	}

	return true;
}

static void scx_dispatch_commit(struct task_struct *p, u64 dsq_id, u64 enq_flags)
{
	struct task_struct *ddsp_task;
	int idx;

	ddsp_task = __this_cpu_read(direct_dispatch_task);
	if (ddsp_task) {
		mark_direct_dispatch(ddsp_task, p, dsq_id, enq_flags);
		return;
	}

	idx = __this_cpu_read(scx_dsp_ctx.buf_cursor);
	if (unlikely(idx >= scx_dsp_max_batch)) {
		scx_ops_error("dispatch buffer overflow");
		return;
	}

	this_cpu_ptr(scx_dsp_buf)[idx] = (struct scx_dsp_buf_ent){
		.task = p,
		.qseq = atomic_long_read(&p->scx.ops_state) & SCX_OPSS_QSEQ_MASK,
		.dsq_id = dsq_id,
		.enq_flags = enq_flags,
	};
	__this_cpu_inc(scx_dsp_ctx.buf_cursor);
}

__bpf_kfunc_start_defs();

/**
 * scx_bpf_dispatch - Dispatch a task into the FIFO queue of a DSQ
 * @p: task_struct to dispatch
 * @dsq_id: DSQ to dispatch to
 * @slice: duration @p can run for in nsecs
 * @enq_flags: SCX_ENQ_*
 *
 * Dispatch @p into the FIFO queue of the DSQ identified by @dsq_id. It is safe
 * to call this function spuriously. Can be called from ops.enqueue(),
 * ops.select_cpu(), and ops.dispatch().
 *
 * When called from ops.select_cpu() or ops.enqueue(), it's for direct dispatch
 * and @p must match the task being enqueued. Also, %SCX_DSQ_LOCAL_ON can't be
 * used to target the local DSQ of a CPU other than the enqueueing one. Use
 * ops.select_cpu() to be on the target CPU in the first place.
 *
 * When called from ops.select_cpu(), @enq_flags and @dsp_id are stored, and @p
 * will be directly dispatched to the corresponding dispatch queue after
 * ops.select_cpu() returns. If @p is dispatched to SCX_DSQ_LOCAL, it will be
 * dispatched to the local DSQ of the CPU returned by ops.select_cpu().
 * @enq_flags are OR'd with the enqueue flags on the enqueue path before the
 * task is dispatched.
 *
 * When called from ops.dispatch(), there are no restrictions on @p or @dsq_id
 * and this function can be called upto ops.dispatch_max_batch times to dispatch
 * multiple tasks. scx_bpf_dispatch_nr_slots() returns the number of the
 * remaining slots. scx_bpf_consume() flushes the batch and resets the counter.
 *
 * This function doesn't have any locking restrictions and may be called under
 * BPF locks (in the future when BPF introduces more flexible locking).
 *
 * @p is allowed to run for @slice. The scheduling path is triggered on slice
 * exhaustion. If zero, the current residual slice is maintained. If
 * %SCX_SLICE_INF, @p never expires and the BPF scheduler must kick the CPU with
 * scx_bpf_kick_cpu() to trigger scheduling.
 */
__bpf_kfunc void scx_bpf_dispatch(struct task_struct *p, u64 dsq_id, u64 slice,
		      u64 enq_flags)
{
	if (!scx_dispatch_preamble(p, enq_flags))
		return;

	if (slice)
		p->scx.slice = slice;
	else
		p->scx.slice = p->scx.slice ?: 1;

	scx_dispatch_commit(p, dsq_id, enq_flags);
}

/**
 * scx_bpf_dispatch_vtime - Dispatch a task into the vtime priority queue of a DSQ
 * @p: task_struct to dispatch
 * @dsq_id: DSQ to dispatch to
 * @slice: duration @p can run for in nsecs
 * @vtime: @p's ordering inside the vtime-sorted queue of the target DSQ
 * @enq_flags: SCX_ENQ_*
 *
 * Dispatch @p into the vtime priority queue of the DSQ identified by @dsq_id.
 * Tasks queued into the priority queue are ordered by @vtime and always
 * consumed after the tasks in the FIFO queue. All other aspects are identical
 * to scx_bpf_dispatch().
 *
 * @vtime ordering is according to time_before64() which considers wrapping. A
 * numerically larger vtime may indicate an earlier position in the ordering and
 * vice-versa.
 */
__bpf_kfunc void scx_bpf_dispatch_vtime(struct task_struct *p, u64 dsq_id, u64 slice,
			    u64 vtime, u64 enq_flags)
{
	if (!scx_dispatch_preamble(p, enq_flags))
		return;

	if (slice)
		p->scx.slice = slice;
	else
		p->scx.slice = p->scx.slice ?: 1;

	p->scx.dsq_vtime = vtime;

	scx_dispatch_commit(p, dsq_id, enq_flags | SCX_ENQ_DSQ_PRIQ);
}

BTF_SET8_START(scx_kfunc_ids_enqueue_dispatch)
BTF_ID_FLAGS(func, scx_bpf_dispatch, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_dispatch_vtime, KF_RCU)
BTF_SET8_END(scx_kfunc_ids_enqueue_dispatch)

static const struct btf_kfunc_id_set scx_kfunc_set_enqueue_dispatch = {
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_enqueue_dispatch,
};

/**
 * scx_bpf_dispatch_nr_slots - Return the number of remaining dispatch slots
 *
 * Can only be called from ops.dispatch().
 */
__bpf_kfunc u32 scx_bpf_dispatch_nr_slots(void)
{
	if (!scx_kf_allowed(SCX_KF_DISPATCH))
		return 0;

	return scx_dsp_max_batch - __this_cpu_read(scx_dsp_ctx.buf_cursor);
}

/**
 * scx_bpf_dispatch_cancel - Cancel the latest dispatch
 *
 * Cancel the latest dispatch. Can be called multiple times to cancel further
 * dispatches. Can only be called from ops.dispatch().
 */
__bpf_kfunc void scx_bpf_dispatch_cancel(void)
{
	struct scx_dsp_ctx *dspc = this_cpu_ptr(&scx_dsp_ctx);

	if (!scx_kf_allowed(SCX_KF_DISPATCH))
		return;

	if (dspc->buf_cursor > 0)
		dspc->buf_cursor--;
	else
		scx_ops_error("dispatch buffer underflow");
}

/**
 * scx_bpf_consume - Transfer a task from a DSQ to the current CPU's local DSQ
 * @dsq_id: DSQ to consume
 *
 * Consume a task from the non-local DSQ identified by @dsq_id and transfer it
 * to the current CPU's local DSQ for execution. Can only be called from
 * ops.dispatch().
 *
 * This function flushes the in-flight dispatches from scx_bpf_dispatch() before
 * trying to consume the specified DSQ. It may also grab rq locks and thus can't
 * be called under any BPF locks.
 *
 * Returns %true if a task has been consumed, %false if there isn't any task to
 * consume.
 */
__bpf_kfunc bool scx_bpf_consume(u64 dsq_id)
{
	struct scx_dsp_ctx *dspc = this_cpu_ptr(&scx_dsp_ctx);
	struct scx_dispatch_q *dsq;

	if (!scx_kf_allowed(SCX_KF_DISPATCH))
		return false;

	flush_dispatch_buf(dspc->rq, dspc->rf);

	dsq = find_non_local_dsq(dsq_id);
	if (unlikely(!dsq)) {
		scx_ops_error("invalid DSQ ID 0x%016llx", dsq_id);
		return false;
	}

	if (consume_dispatch_q(dspc->rq, dspc->rf, dsq)) {
		/*
		 * A successfully consumed task can be dequeued before it starts
		 * running while the CPU is trying to migrate other dispatched
		 * tasks. Bump nr_tasks to tell balance_scx() to retry on empty
		 * local DSQ.
		 */
		dspc->nr_tasks++;
		return true;
	} else {
		return false;
	}
}

BTF_SET8_START(scx_kfunc_ids_dispatch)
BTF_ID_FLAGS(func, scx_bpf_dispatch_nr_slots)
BTF_ID_FLAGS(func, scx_bpf_dispatch_cancel)
BTF_ID_FLAGS(func, scx_bpf_consume)
BTF_SET8_END(scx_kfunc_ids_dispatch)

static const struct btf_kfunc_id_set scx_kfunc_set_dispatch = {
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_dispatch,
};

/**
 * scx_bpf_reenqueue_local - Re-enqueue tasks on a local DSQ
 *
 * Iterate over all of the tasks currently enqueued on the local DSQ of the
 * caller's CPU, and re-enqueue them in the BPF scheduler. Returns the number of
 * processed tasks. Can only be called from ops.cpu_release().
 */
__bpf_kfunc u32 scx_bpf_reenqueue_local(void)
{
	u32 nr_enqueued, i;
	struct rq *rq;
	struct scx_rq *scx_rq;

	if (!scx_kf_allowed(SCX_KF_CPU_RELEASE))
		return 0;

	rq = cpu_rq(smp_processor_id());
	lockdep_assert_rq_held(rq);
	scx_rq = &rq->scx;

	/*
	 * Get the number of tasks on the local DSQ before iterating over it to
	 * pull off tasks. The enqueue callback below can signal that it wants
	 * the task to stay on the local DSQ, and we want to prevent the BPF
	 * scheduler from causing us to loop indefinitely.
	 */
	nr_enqueued = scx_rq->local_dsq.nr;
	for (i = 0; i < nr_enqueued; i++) {
		struct task_struct *p;

		p = first_local_task(rq);
		WARN_ON_ONCE(atomic_long_read(&p->scx.ops_state) !=
			     SCX_OPSS_NONE);
		WARN_ON_ONCE(!(p->scx.flags & SCX_TASK_QUEUED));
		WARN_ON_ONCE(p->scx.holding_cpu != -1);
		dispatch_dequeue(scx_rq, p);
		do_enqueue_task(rq, p, SCX_ENQ_REENQ, -1);
	}

	return nr_enqueued;
}

BTF_SET8_START(scx_kfunc_ids_cpu_release)
BTF_ID_FLAGS(func, scx_bpf_reenqueue_local)
BTF_SET8_END(scx_kfunc_ids_cpu_release)

static const struct btf_kfunc_id_set scx_kfunc_set_cpu_release = {
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_cpu_release,
};

/**
 * scx_bpf_kick_cpu - Trigger reschedule on a CPU
 * @cpu: cpu to kick
 * @flags: %SCX_KICK_* flags
 *
 * Kick @cpu into rescheduling. This can be used to wake up an idle CPU or
 * trigger rescheduling on a busy CPU. This can be called from any online
 * scx_ops operation and the actual kicking is performed asynchronously through
 * an irq work.
 */
__bpf_kfunc void scx_bpf_kick_cpu(s32 cpu, u64 flags)
{
	struct rq *this_rq;
	unsigned long irq_flags;

	if (!ops_cpu_valid(cpu)) {
		scx_ops_error("invalid cpu %d", cpu);
		return;
	}

	/*
	 * While bypassing for PM ops, IRQ handling may not be online which can
	 * lead to irq_work_queue() malfunction such as infinite busy wait for
	 * IRQ status update. Suppress kicking.
	 */
	if (scx_ops_bypassing())
		return;

	local_irq_save(irq_flags);

	this_rq = this_rq();

	/*
	 * Actual kicking is bounced to kick_cpus_irq_workfn() to avoid nesting
	 * rq locks. We can probably be smarter and avoid bouncing if called
	 * from ops which don't hold a rq lock.
	 */
	if (flags & SCX_KICK_IDLE) {
		struct rq *target_rq = cpu_rq(cpu);

		if (unlikely(flags & (SCX_KICK_PREEMPT | SCX_KICK_WAIT)))
			scx_ops_error("PREEMPT/WAIT cannot be used with SCX_KICK_IDLE");

		if (raw_spin_rq_trylock(target_rq)) {
			if (can_skip_idle_kick(target_rq)) {
				raw_spin_rq_unlock(target_rq);
				goto out;
			}
			raw_spin_rq_unlock(target_rq);
		}
		cpumask_set_cpu(cpu, this_rq->scx.cpus_to_kick_if_idle);
	} else {
		cpumask_set_cpu(cpu, this_rq->scx.cpus_to_kick);

		if (flags & SCX_KICK_PREEMPT)
			cpumask_set_cpu(cpu, this_rq->scx.cpus_to_preempt);
		if (flags & SCX_KICK_WAIT)
			cpumask_set_cpu(cpu, this_rq->scx.cpus_to_wait);
	}

	irq_work_queue(&this_rq->scx.kick_cpus_irq_work);
out:
	local_irq_restore(irq_flags);
}

/**
 * scx_bpf_dsq_nr_queued - Return the number of queued tasks
 * @dsq_id: id of the DSQ
 *
 * Return the number of tasks in the DSQ matching @dsq_id. If not found,
 * -%ENOENT is returned. Can be called from any non-sleepable online scx_ops
 * operations.
 */
__bpf_kfunc s32 scx_bpf_dsq_nr_queued(u64 dsq_id)
{
	struct scx_dispatch_q *dsq;

	lockdep_assert(rcu_read_lock_any_held());

	if (dsq_id == SCX_DSQ_LOCAL) {
		return this_rq()->scx.local_dsq.nr;
	} else if ((dsq_id & SCX_DSQ_LOCAL_ON) == SCX_DSQ_LOCAL_ON) {
		s32 cpu = dsq_id & SCX_DSQ_LOCAL_CPU_MASK;

		if (ops_cpu_valid(cpu))
			return cpu_rq(cpu)->scx.local_dsq.nr;
	} else {
		dsq = find_non_local_dsq(dsq_id);
		if (dsq)
			return dsq->nr;
	}
	return -ENOENT;
}

/**
 * scx_bpf_test_and_clear_cpu_idle - Test and clear @cpu's idle state
 * @cpu: cpu to test and clear idle for
 *
 * Returns %true if @cpu was idle and its idle state was successfully cleared.
 * %false otherwise.
 *
 * Unavailable if ops.update_idle() is implemented and
 * %SCX_OPS_KEEP_BUILTIN_IDLE is not set.
 */
__bpf_kfunc bool scx_bpf_test_and_clear_cpu_idle(s32 cpu)
{
	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		return false;
	}

	if (ops_cpu_valid(cpu))
		return test_and_clear_cpu_idle(cpu);
	else
		return false;
}

/**
 * scx_bpf_pick_idle_cpu - Pick and claim an idle cpu
 * @cpus_allowed: Allowed cpumask
 * @flags: %SCX_PICK_IDLE_CPU_* flags
 *
 * Pick and claim an idle cpu in @cpus_allowed. Returns the picked idle cpu
 * number on success. -%EBUSY if no matching cpu was found.
 *
 * Idle CPU tracking may race against CPU scheduling state transitions. For
 * example, this function may return -%EBUSY as CPUs are transitioning into the
 * idle state. If the caller then assumes that there will be dispatch events on
 * the CPUs as they were all busy, the scheduler may end up stalling with CPUs
 * idling while there are pending tasks. Use scx_bpf_pick_any_cpu() and
 * scx_bpf_kick_cpu() to guarantee that there will be at least one dispatch
 * event in the near future.
 *
 * Unavailable if ops.update_idle() is implemented and
 * %SCX_OPS_KEEP_BUILTIN_IDLE is not set.
 */
__bpf_kfunc s32 scx_bpf_pick_idle_cpu(const struct cpumask *cpus_allowed, u64 flags)
{
	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		return -EBUSY;
	}

	return scx_pick_idle_cpu(cpus_allowed, flags);
}

/**
 * scx_bpf_pick_any_cpu - Pick and claim an idle cpu if available or pick any CPU
 * @cpus_allowed: Allowed cpumask
 * @flags: %SCX_PICK_IDLE_CPU_* flags
 *
 * Pick and claim an idle cpu in @cpus_allowed. If none is available, pick any
 * CPU in @cpus_allowed. Guaranteed to succeed and returns the picked idle cpu
 * number if @cpus_allowed is not empty. -%EBUSY is returned if @cpus_allowed is
 * empty.
 *
 * If ops.update_idle() is implemented and %SCX_OPS_KEEP_BUILTIN_IDLE is not
 * set, this function can't tell which CPUs are idle and will always pick any
 * CPU.
 */
__bpf_kfunc s32 scx_bpf_pick_any_cpu(const struct cpumask *cpus_allowed, u64 flags)
{
	s32 cpu;

	if (static_branch_likely(&scx_builtin_idle_enabled)) {
		cpu = scx_pick_idle_cpu(cpus_allowed, flags);
		if (cpu >= 0)
			return cpu;
	}

	cpu = cpumask_any_distribute(cpus_allowed);
	if (cpu < nr_cpu_ids)
		return cpu;
	else
		return -EBUSY;
}

/**
 * scx_bpf_get_idle_cpumask - Get a referenced kptr to the idle-tracking
 * per-CPU cpumask.
 *
 * Returns NULL if idle tracking is not enabled, or running on a UP kernel.
 */
__bpf_kfunc const struct cpumask *scx_bpf_get_idle_cpumask(void)
{
	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		return cpu_none_mask;
	}

#ifdef CONFIG_SMP
	return idle_masks.cpu;
#else
	return cpu_none_mask;
#endif
}

/**
 * scx_bpf_get_idle_smtmask - Get a referenced kptr to the idle-tracking,
 * per-physical-core cpumask. Can be used to determine if an entire physical
 * core is free.
 *
 * Returns NULL if idle tracking is not enabled, or running on a UP kernel.
 */
__bpf_kfunc const struct cpumask *scx_bpf_get_idle_smtmask(void)
{
	if (!static_branch_likely(&scx_builtin_idle_enabled)) {
		scx_ops_error("built-in idle tracking is disabled");
		return cpu_none_mask;
	}

#ifdef CONFIG_SMP
	if (sched_smt_active())
		return idle_masks.smt;
	else
		return idle_masks.cpu;
#else
	return cpu_none_mask;
#endif
}

/**
 * scx_bpf_put_idle_cpumask - Release a previously acquired referenced kptr to
 * either the percpu, or SMT idle-tracking cpumask.
 */
__bpf_kfunc void scx_bpf_put_idle_cpumask(const struct cpumask *idle_mask)
{
	/*
	 * Empty function body because we aren't actually acquiring or
	 * releasing a reference to a global idle cpumask, which is read-only
	 * in the caller and is never released. The acquire / release semantics
	 * here are just used to make the cpumask is a trusted pointer in the
	 * caller.
	 */
}

struct scx_bpf_error_bstr_bufs {
	u64			data[MAX_BPRINTF_VARARGS];
	char			msg[SCX_EXIT_MSG_LEN];
};

static DEFINE_PER_CPU(struct scx_bpf_error_bstr_bufs, scx_bpf_error_bstr_bufs);

/**
 * scx_bpf_error_bstr - Indicate fatal error
 * @fmt: error message format string
 * @data: format string parameters packaged using ___bpf_fill() macro
 * @data__sz: @data len, must end in '__sz' for the verifier
 *
 * Indicate that the BPF scheduler encountered a fatal error and initiate ops
 * disabling.
 */
__bpf_kfunc void scx_bpf_error_bstr(char *fmt, unsigned long long *data,
				    u32 data__sz)
{
	struct bpf_bprintf_data bprintf_data = { .get_bin_args = true };
	struct scx_bpf_error_bstr_bufs *bufs;
	unsigned long flags;
	int ret;

	local_irq_save(flags);
	bufs = this_cpu_ptr(&scx_bpf_error_bstr_bufs);

	if (data__sz % 8 || data__sz > MAX_BPRINTF_VARARGS * 8 ||
	    (data__sz && !data)) {
		scx_ops_error("invalid data=%p and data__sz=%u",
			      (void *)data, data__sz);
		goto out_restore;
	}

	ret = copy_from_kernel_nofault(bufs->data, data, data__sz);
	if (ret) {
		scx_ops_error("failed to read data fields (%d)", ret);
		goto out_restore;
	}

	ret = bpf_bprintf_prepare(fmt, UINT_MAX, bufs->data, data__sz / 8,
				  &bprintf_data);
	if (ret < 0) {
		scx_ops_error("failed to format prepration (%d)", ret);
		goto out_restore;
	}

	ret = bstr_printf(bufs->msg, sizeof(bufs->msg), fmt,
			  bprintf_data.bin_args);
	bpf_bprintf_cleanup(&bprintf_data);
	if (ret < 0) {
		scx_ops_error("scx_ops_error(\"%s\", %p, %u) failed to format",
			      fmt, data, data__sz);
		goto out_restore;
	}

	scx_ops_error_kind(SCX_EXIT_ERROR_BPF, "%s", bufs->msg);
out_restore:
	local_irq_restore(flags);
}

/**
 * scx_bpf_destroy_dsq - Destroy a custom DSQ
 * @dsq_id: DSQ to destroy
 *
 * Destroy the custom DSQ identified by @dsq_id. Only DSQs created with
 * scx_bpf_create_dsq() can be destroyed. The caller must ensure that the DSQ is
 * empty and no further tasks are dispatched to it. Ignored if called on a DSQ
 * which doesn't exist. Can be called from any online scx_ops operations.
 */
__bpf_kfunc void scx_bpf_destroy_dsq(u64 dsq_id)
{
	destroy_dsq(dsq_id);
}

/**
 * scx_bpf_task_running - Is task currently running?
 * @p: task of interest
 */
__bpf_kfunc bool scx_bpf_task_running(const struct task_struct *p)
{
	return task_rq(p)->curr == p;
}

/**
 * scx_bpf_task_cpu - CPU a task is currently associated with
 * @p: task of interest
 */
__bpf_kfunc s32 scx_bpf_task_cpu(const struct task_struct *p)
{
	return task_cpu(p);
}

/**
 * scx_bpf_task_cgroup - Return the sched cgroup of a task
 * @p: task of interest
 *
 * @p->sched_task_group->css.cgroup represents the cgroup @p is associated with
 * from the scheduler's POV. SCX operations should use this function to
 * determine @p's current cgroup as, unlike following @p->cgroups,
 * @p->sched_task_group is protected by @p's rq lock and thus atomic w.r.t. all
 * rq-locked operations. Can be called on the parameter tasks of rq-locked
 * operations. The restriction guarantees that @p's rq is locked by the caller.
 */
#ifdef CONFIG_CGROUP_SCHED
__bpf_kfunc struct cgroup *scx_bpf_task_cgroup(struct task_struct *p)
{
	struct task_group *tg = p->sched_task_group;
	struct cgroup *cgrp = &cgrp_dfl_root.cgrp;

	if (!scx_kf_allowed_on_arg_tasks(__SCX_KF_RQ_LOCKED, p))
		goto out;

	/*
	 * A task_group may either be a cgroup or an autogroup. In the latter
	 * case, @tg->css.cgroup is %NULL. A task_group can't become the other
	 * kind once created.
	 */
	if (tg && tg->css.cgroup)
		cgrp = tg->css.cgroup;
	else
		cgrp = &cgrp_dfl_root.cgrp;
out:
	cgroup_get(cgrp);
	return cgrp;
}
#endif

BTF_SET8_START(scx_kfunc_ids_ops_only)
BTF_ID_FLAGS(func, scx_bpf_kick_cpu)
BTF_ID_FLAGS(func, scx_bpf_dsq_nr_queued)
BTF_ID_FLAGS(func, scx_bpf_test_and_clear_cpu_idle)
BTF_ID_FLAGS(func, scx_bpf_pick_idle_cpu, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_pick_any_cpu, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_destroy_dsq)
BTF_ID_FLAGS(func, scx_bpf_select_cpu_dfl, KF_RCU)
BTF_SET8_END(scx_kfunc_ids_ops_only)

static const struct btf_kfunc_id_set scx_kfunc_set_ops_only = {
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_ops_only,
};

BTF_SET8_START(scx_kfunc_ids_any)
BTF_ID_FLAGS(func, scx_bpf_get_idle_cpumask, KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_get_idle_smtmask, KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_put_idle_cpumask, KF_RELEASE)
BTF_ID_FLAGS(func, scx_bpf_error_bstr, KF_TRUSTED_ARGS)
BTF_ID_FLAGS(func, scx_bpf_task_running, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_task_cpu, KF_RCU)
#ifdef CONFIG_CGROUP_SCHED
BTF_ID_FLAGS(func, scx_bpf_task_cgroup, KF_RCU | KF_ACQUIRE)
#endif
BTF_SET8_END(scx_kfunc_ids_any)

static const struct btf_kfunc_id_set scx_kfunc_set_any = {
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_any,
};

__bpf_kfunc_end_defs();

static int __init scx_init(void)
{
	int ret;

	/*
	 * kfunc registration can't be done from init_sched_ext_class() as
	 * register_btf_kfunc_id_set() needs most of the system to be up.
	 *
	 * Some kfuncs are context-sensitive and can only be called from
	 * specific SCX ops. They are grouped into BTF sets accordingly.
	 * Unfortunately, BPF currently doesn't have a way of enforcing such
	 * restrictions. Eventually, the verifier should be able to enforce
	 * them. For now, register them the same and make each kfunc explicitly
	 * check using scx_kf_allowed().
	 */
	if ((ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					     &scx_kfunc_set_init)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					     &scx_kfunc_set_sleepable)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					     &scx_kfunc_set_enqueue_dispatch)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					     &scx_kfunc_set_dispatch)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					     &scx_kfunc_set_cpu_release)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					     &scx_kfunc_set_ops_only)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					     &scx_kfunc_set_any)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_TRACING,
					     &scx_kfunc_set_any))) {
		pr_err("sched_ext: Failed to register kfunc sets (%d)\n", ret);
		return ret;
	}

	ret = register_pm_notifier(&scx_pm_notifier);
	if (ret) {
		pr_err("sched_ext: Failed to register PM notifier (%d)\n", ret);
		return ret;
	}

	scx_kset = kset_create_and_add("sched_ext", &scx_uevent_ops, kernel_kobj);
	if (!scx_kset) {
		pr_err("sched_ext: Failed to create /sys/sched_ext\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(&scx_kset->kobj, &scx_global_attr_group);
	if (ret < 0) {
		pr_err("sched_ext: Failed to add global attributes\n");
		return ret;
	}

	return 0;
}
__initcall(scx_init);
