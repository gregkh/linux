/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */
#ifndef _LINUX_SCHED_EXT_H
#define _LINUX_SCHED_EXT_H

#ifdef CONFIG_SCHED_CLASS_EXT

#include <linux/rhashtable.h>
#include <linux/llist.h>

struct cgroup;

enum scx_consts {
	SCX_OPS_NAME_LEN	= 128,
	SCX_EXIT_REASON_LEN	= 128,
	SCX_EXIT_BT_LEN		= 64,
	SCX_EXIT_MSG_LEN	= 1024,

	SCX_SLICE_DFL		= 20 * NSEC_PER_MSEC,
	SCX_SLICE_INF		= U64_MAX,	/* infinite, implies nohz */
};

/*
 * DSQ (dispatch queue) IDs are 64bit of the format:
 *
 *   Bits: [63] [62 ..  0]
 *         [ B] [   ID   ]
 *
 *    B: 1 for IDs for built-in DSQs, 0 for ops-created user DSQs
 *   ID: 63 bit ID
 *
 * Built-in IDs:
 *
 *   Bits: [63] [62] [61..32] [31 ..  0]
 *         [ 1] [ L] [   R  ] [    V   ]
 *
 *    1: 1 for built-in DSQs.
 *    L: 1 for LOCAL_ON DSQ IDs, 0 for others
 *    V: For LOCAL_ON DSQ IDs, a CPU number. For others, a pre-defined value.
 */
enum scx_dsq_id_flags {
	SCX_DSQ_FLAG_BUILTIN	= 1LLU << 63,
	SCX_DSQ_FLAG_LOCAL_ON	= 1LLU << 62,

	SCX_DSQ_INVALID		= SCX_DSQ_FLAG_BUILTIN | 0,
	SCX_DSQ_GLOBAL		= SCX_DSQ_FLAG_BUILTIN | 1,
	SCX_DSQ_LOCAL		= SCX_DSQ_FLAG_BUILTIN | 2,
	SCX_DSQ_LOCAL_ON	= SCX_DSQ_FLAG_BUILTIN | SCX_DSQ_FLAG_LOCAL_ON,
	SCX_DSQ_LOCAL_CPU_MASK	= 0xffffffffLLU,
};

enum scx_exit_kind {
	SCX_EXIT_NONE,
	SCX_EXIT_DONE,

	SCX_EXIT_UNREG = 64,	/* BPF unregistration */
	SCX_EXIT_SYSRQ,		/* requested by 'S' sysrq */

	SCX_EXIT_ERROR = 1024,	/* runtime error, error msg contains details */
	SCX_EXIT_ERROR_BPF,	/* ERROR but triggered through scx_bpf_error() */
	SCX_EXIT_ERROR_STALL,	/* watchdog detected stalled runnable tasks */
};

/*
 * scx_exit_info is passed to ops.exit() to describe why the BPF scheduler is
 * being disabled.
 */
struct scx_exit_info {
	/* %SCX_EXIT_* - broad category of the exit reason */
	enum scx_exit_kind	kind;
	/* textual representation of the above */
	char			reason[SCX_EXIT_REASON_LEN];
	/* number of entries in the backtrace */
	u32			bt_len;
	/* backtrace if exiting due to an error */
	unsigned long		bt[SCX_EXIT_BT_LEN];
	/* extra message */
	char			msg[SCX_EXIT_MSG_LEN];
};

/* sched_ext_ops.flags */
enum scx_ops_flags {
	/*
	 * Keep built-in idle tracking even if ops.update_idle() is implemented.
	 */
	SCX_OPS_KEEP_BUILTIN_IDLE = 1LLU << 0,

	/*
	 * By default, if there are no other task to run on the CPU, ext core
	 * keeps running the current task even after its slice expires. If this
	 * flag is specified, such tasks are passed to ops.enqueue() with
	 * %SCX_ENQ_LAST. See the comment above %SCX_ENQ_LAST for more info.
	 */
	SCX_OPS_ENQ_LAST	= 1LLU << 1,

	/*
	 * An exiting task may schedule after PF_EXITING is set. In such cases,
	 * bpf_task_from_pid() may not be able to find the task and if the BPF
	 * scheduler depends on pid lookup for dispatching, the task will be
	 * lost leading to various issues including RCU grace period stalls.
	 *
	 * To mask this problem, by default, unhashed tasks are automatically
	 * dispatched to the local DSQ on enqueue. If the BPF scheduler doesn't
	 * depend on pid lookups and wants to handle these tasks directly, the
	 * following flag can be used.
	 */
	SCX_OPS_ENQ_EXITING	= 1LLU << 2,

	/*
	 * CPU cgroup knob enable flags
	 */
	SCX_OPS_CGROUP_KNOB_WEIGHT = 1LLU << 16,	/* cpu.weight */

	SCX_OPS_ALL_FLAGS	= SCX_OPS_KEEP_BUILTIN_IDLE |
				  SCX_OPS_ENQ_LAST |
				  SCX_OPS_ENQ_EXITING |
				  SCX_OPS_CGROUP_KNOB_WEIGHT,
};

/* argument container for ops.enable() and friends */
struct scx_enable_args {
#ifdef CONFIG_EXT_GROUP_SCHED
	/* the cgroup the task is joining */
	struct cgroup		*cgroup;
#endif
};

/* argument container for ops->cgroup_init() */
struct scx_cgroup_init_args {
	/* the weight of the cgroup [1..10000] */
	u32			weight;
};

enum scx_cpu_preempt_reason {
	/* next task is being scheduled by &sched_class_rt */
        SCX_CPU_PREEMPT_RT,
	/* next task is being scheduled by &sched_class_dl */
        SCX_CPU_PREEMPT_DL,
	/* next task is being scheduled by &sched_class_stop */
        SCX_CPU_PREEMPT_STOP,
	/* unknown reason for SCX being preempted */
        SCX_CPU_PREEMPT_UNKNOWN,
};

/*
 * Argument container for ops->cpu_acquire(). Currently empty, but may be
 * expanded in the future.
 */
struct scx_cpu_acquire_args {};

/* argument container for ops->cpu_release() */
struct scx_cpu_release_args {
	/* the reason the CPU was preempted */
	enum scx_cpu_preempt_reason reason;

	/* the task that's going to be scheduled on the CPU */
	struct task_struct *task;
};

/**
 * struct sched_ext_ops - Operation table for BPF scheduler implementation
 *
 * Userland can implement an arbitrary scheduling policy by implementing and
 * loading operations in this table.
 */
struct sched_ext_ops {
	/**
	 * select_cpu - Pick the target CPU for a task which is being woken up
	 * @p: task being woken up
	 * @prev_cpu: the cpu @p was on before sleeping
	 * @wake_flags: SCX_WAKE_*
	 *
	 * Decision made here isn't final. @p may be moved to any CPU while it
	 * is getting dispatched for execution later. However, as @p is not on
	 * the rq at this point, getting the eventual execution CPU right here
	 * saves a small bit of overhead down the line.
	 *
	 * If an idle CPU is returned, the CPU is kicked and will try to
	 * dispatch. While an explicit custom mechanism can be added,
	 * select_cpu() serves as the default way to wake up idle CPUs.
	 */
	s32 (*select_cpu)(struct task_struct *p, s32 prev_cpu, u64 wake_flags);

	/**
	 * enqueue - Enqueue a task on the BPF scheduler
	 * @p: task being enqueued
	 * @enq_flags: %SCX_ENQ_*
	 *
	 * @p is ready to run. Dispatch directly by calling scx_bpf_dispatch()
	 * or enqueue on the BPF scheduler. If not directly dispatched, the bpf
	 * scheduler owns @p and if it fails to dispatch @p, the task will
	 * stall.
	 */
	void (*enqueue)(struct task_struct *p, u64 enq_flags);

	/**
	 * dequeue - Remove a task from the BPF scheduler
	 * @p: task being dequeued
	 * @deq_flags: %SCX_DEQ_*
	 *
	 * Remove @p from the BPF scheduler. This is usually called to isolate
	 * the task while updating its scheduling properties (e.g. priority).
	 *
	 * The ext core keeps track of whether the BPF side owns a given task or
	 * not and can gracefully ignore spurious dispatches from BPF side,
	 * which makes it safe to not implement this method. However, depending
	 * on the scheduling logic, this can lead to confusing behaviors - e.g.
	 * scheduling position not being updated across a priority change.
	 */
	void (*dequeue)(struct task_struct *p, u64 deq_flags);

	/**
	 * dispatch - Dispatch tasks from the BPF scheduler and/or consume DSQs
	 * @cpu: CPU to dispatch tasks for
	 * @prev: previous task being switched out
	 *
	 * Called when a CPU's local dsq is empty. The operation should dispatch
	 * one or more tasks from the BPF scheduler into the DSQs using
	 * scx_bpf_dispatch() and/or consume user DSQs into the local DSQ using
	 * scx_bpf_consume().
	 *
	 * The maximum number of times scx_bpf_dispatch() can be called without
	 * an intervening scx_bpf_consume() is specified by
	 * ops.dispatch_max_batch. See the comments on top of the two functions
	 * for more details.
	 *
	 * When not %NULL, @prev is an SCX task with its slice depleted. If
	 * @prev is still runnable as indicated by set %SCX_TASK_QUEUED in
	 * @prev->scx.flags, it is not enqueued yet and will be enqueued after
	 * ops.dispatch() returns. To keep executing @prev, return without
	 * dispatching or consuming any tasks. Also see %SCX_OPS_ENQ_LAST.
	 */
	void (*dispatch)(s32 cpu, struct task_struct *prev);

	/**
	 * runnable - A task is becoming runnable on its associated CPU
	 * @p: task becoming runnable
	 * @enq_flags: %SCX_ENQ_*
	 *
	 * This and the following three functions can be used to track a task's
	 * execution state transitions. A task becomes ->runnable() on a CPU,
	 * and then goes through one or more ->running() and ->stopping() pairs
	 * as it runs on the CPU, and eventually becomes ->quiescent() when it's
	 * done running on the CPU.
	 *
	 * @p is becoming runnable on the CPU because it's
	 *
	 * - waking up (%SCX_ENQ_WAKEUP)
	 * - being moved from another CPU
	 * - being restored after temporarily taken off the queue for an
	 *   attribute change.
	 *
	 * This and ->enqueue() are related but not coupled. This operation
	 * notifies @p's state transition and may not be followed by ->enqueue()
	 * e.g. when @p is being dispatched to a remote CPU. Likewise, a task
	 * may be ->enqueue()'d without being preceded by this operation e.g.
	 * after exhausting its slice.
	 */
	void (*runnable)(struct task_struct *p, u64 enq_flags);

	/**
	 * running - A task is starting to run on its associated CPU
	 * @p: task starting to run
	 *
	 * See ->runnable() for explanation on the task state notifiers.
	 */
	void (*running)(struct task_struct *p);

	/**
	 * stopping - A task is stopping execution
	 * @p: task stopping to run
	 * @runnable: is task @p still runnable?
	 *
	 * See ->runnable() for explanation on the task state notifiers. If
	 * !@runnable, ->quiescent() will be invoked after this operation
	 * returns.
	 */
	void (*stopping)(struct task_struct *p, bool runnable);

	/**
	 * quiescent - A task is becoming not runnable on its associated CPU
	 * @p: task becoming not runnable
	 * @deq_flags: %SCX_DEQ_*
	 *
	 * See ->runnable() for explanation on the task state notifiers.
	 *
	 * @p is becoming quiescent on the CPU because it's
	 *
	 * - sleeping (%SCX_DEQ_SLEEP)
	 * - being moved to another CPU
	 * - being temporarily taken off the queue for an attribute change
	 *   (%SCX_DEQ_SAVE)
	 *
	 * This and ->dequeue() are related but not coupled. This operation
	 * notifies @p's state transition and may not be preceded by ->dequeue()
	 * e.g. when @p is being dispatched to a remote CPU.
	 */
	void (*quiescent)(struct task_struct *p, u64 deq_flags);

	/**
	 * yield - Yield CPU
	 * @from: yielding task
	 * @to: optional yield target task
	 *
	 * If @to is NULL, @from is yielding the CPU to other runnable tasks.
	 * The BPF scheduler should ensure that other available tasks are
	 * dispatched before the yielding task. Return value is ignored in this
	 * case.
	 *
	 * If @to is not-NULL, @from wants to yield the CPU to @to. If the bpf
	 * scheduler can implement the request, return %true; otherwise, %false.
	 */
	bool (*yield)(struct task_struct *from, struct task_struct *to);

	/**
	 * core_sched_before - Task ordering for core-sched
	 * @a: task A
	 * @b: task B
	 *
	 * Used by core-sched to determine the ordering between two tasks. See
	 * Documentation/admin-guide/hw-vuln/core-scheduling.rst for details on
	 * core-sched.
	 *
	 * Both @a and @b are runnable and may or may not currently be queued on
	 * the BPF scheduler. Should return %true if @a should run before @b.
	 * %false if there's no required ordering or @b should run before @a.
	 *
	 * If not specified, the default is ordering them according to when they
	 * became runnable.
	 */
	bool (*core_sched_before)(struct task_struct *a,struct task_struct *b);

	/**
	 * set_weight - Set task weight
	 * @p: task to set weight for
	 * @weight: new eight [1..10000]
	 *
	 * Update @p's weight to @weight.
	 */
	void (*set_weight)(struct task_struct *p, u32 weight);

	/**
	 * set_cpumask - Set CPU affinity
	 * @p: task to set CPU affinity for
	 * @cpumask: cpumask of cpus that @p can run on
	 *
	 * Update @p's CPU affinity to @cpumask.
	 */
	void (*set_cpumask)(struct task_struct *p,
			    const struct cpumask *cpumask);

	/**
	 * update_idle - Update the idle state of a CPU
	 * @cpu: CPU to udpate the idle state for
	 * @idle: whether entering or exiting the idle state
	 *
	 * This operation is called when @rq's CPU goes or leaves the idle
	 * state. By default, implementing this operation disables the built-in
	 * idle CPU tracking and the following helpers become unavailable:
	 *
	 * - scx_bpf_select_cpu_dfl()
	 * - scx_bpf_test_and_clear_cpu_idle()
	 * - scx_bpf_pick_idle_cpu()
	 *
	 * The user also must implement ops.select_cpu() as the default
	 * implementation relies on scx_bpf_select_cpu_dfl().
	 *
	 * Specify the %SCX_OPS_KEEP_BUILTIN_IDLE flag to keep the built-in idle
	 * tracking.
	 */
	void (*update_idle)(s32 cpu, bool idle);

	/**
	 * cpu_acquire - A CPU is becoming available to the BPF scheduler
	 * @cpu: The CPU being acquired by the BPF scheduler.
	 * @args: Acquire arguments, see the struct definition.
	 *
	 * A CPU that was previously released from the BPF scheduler is now once
	 * again under its control.
	 */
	void (*cpu_acquire)(s32 cpu, struct scx_cpu_acquire_args *args);

	/**
	 * cpu_release - A CPU is taken away from the BPF scheduler
	 * @cpu: The CPU being released by the BPF scheduler.
	 * @args: Release arguments, see the struct definition.
	 *
	 * The specified CPU is no longer under the control of the BPF
	 * scheduler. This could be because it was preempted by a higher
	 * priority sched_class, though there may be other reasons as well. The
	 * caller should consult @args->reason to determine the cause.
	 */
	void (*cpu_release)(s32 cpu, struct scx_cpu_release_args *args);

	/**
	 * cpu_online - A CPU became online
	 * @cpu: CPU which just came up
	 *
	 * @cpu just came online. @cpu doesn't call ops.enqueue() or run tasks
	 * associated with other CPUs beforehand.
	 */
	void (*cpu_online)(s32 cpu);

	/**
	 * cpu_offline - A CPU is going offline
	 * @cpu: CPU which is going offline
	 *
	 * @cpu is going offline. @cpu doesn't call ops.enqueue() or run tasks
	 * associated with other CPUs afterwards.
	 */
	void (*cpu_offline)(s32 cpu);

	/**
	 * prep_enable - Prepare to enable BPF scheduling for a task
	 * @p: task to prepare BPF scheduling for
	 * @args: enable arguments, see the struct definition
	 *
	 * Either we're loading a BPF scheduler or a new task is being forked.
	 * Prepare BPF scheduling for @p. This operation may block and can be
	 * used for allocations.
	 *
	 * Return 0 for success, -errno for failure. An error return while
	 * loading will abort loading of the BPF scheduler. During a fork, will
	 * abort the specific fork.
	 */
	s32 (*prep_enable)(struct task_struct *p, struct scx_enable_args *args);

	/**
	 * enable - Enable BPF scheduling for a task
	 * @p: task to enable BPF scheduling for
	 * @args: enable arguments, see the struct definition
	 *
	 * Enable @p for BPF scheduling. @p is now in the cgroup specified for
	 * the preceding prep_enable() and will start running soon.
	 */
	void (*enable)(struct task_struct *p, struct scx_enable_args *args);

	/**
	 * cancel_enable - Cancel prep_enable()
	 * @p: task being canceled
	 * @args: enable arguments, see the struct definition
	 *
	 * @p was prep_enable()'d but failed before reaching enable(). Undo the
	 * preparation.
	 */
	void (*cancel_enable)(struct task_struct *p,
			      struct scx_enable_args *args);

	/**
	 * disable - Disable BPF scheduling for a task
	 * @p: task to disable BPF scheduling for
	 *
	 * @p is exiting, leaving SCX or the BPF scheduler is being unloaded.
	 * Disable BPF scheduling for @p.
	 */
	void (*disable)(struct task_struct *p);

#ifdef CONFIG_EXT_GROUP_SCHED
	/**
	 * cgroup_init - Initialize a cgroup
	 * @cgrp: cgroup being initialized
	 * @args: init arguments, see the struct definition
	 *
	 * Either the BPF scheduler is being loaded or @cgrp created, initialize
	 * @cgrp for sched_ext. This operation may block.
	 *
	 * Return 0 for success, -errno for failure. An error return while
	 * loading will abort loading of the BPF scheduler. During cgroup
	 * creation, it will abort the specific cgroup creation.
	 */
	s32 (*cgroup_init)(struct cgroup *cgrp,
			   struct scx_cgroup_init_args *args);

	/**
	 * cgroup_exit - Exit a cgroup
	 * @cgrp: cgroup being exited
	 *
	 * Either the BPF scheduler is being unloaded or @cgrp destroyed, exit
	 * @cgrp for sched_ext. This operation my block.
	 */
	void (*cgroup_exit)(struct cgroup *cgrp);

	/**
	 * cgroup_prep_move - Prepare a task to be moved to a different cgroup
	 * @p: task being moved
	 * @from: cgroup @p is being moved from
	 * @to: cgroup @p is being moved to
	 *
	 * Prepare @p for move from cgroup @from to @to. This operation may
	 * block and can be used for allocations.
	 *
	 * Return 0 for success, -errno for failure. An error return aborts the
	 * migration.
	 */
	s32 (*cgroup_prep_move)(struct task_struct *p,
				struct cgroup *from, struct cgroup *to);

	/**
	 * cgroup_move - Commit cgroup move
	 * @p: task being moved
	 * @from: cgroup @p is being moved from
	 * @to: cgroup @p is being moved to
	 *
	 * Commit the move. @p is dequeued during this operation.
	 */
	void (*cgroup_move)(struct task_struct *p,
			    struct cgroup *from, struct cgroup *to);

	/**
	 * cgroup_cancel_move - Cancel cgroup move
	 * @p: task whose cgroup move is being canceled
	 * @from: cgroup @p was being moved from
	 * @to: cgroup @p was being moved to
	 *
	 * @p was cgroup_prep_move()'d but failed before reaching cgroup_move().
	 * Undo the preparation.
	 */
	void (*cgroup_cancel_move)(struct task_struct *p,
				   struct cgroup *from, struct cgroup *to);

	/**
	 * cgroup_set_weight - A cgroup's weight is being changed
	 * @cgrp: cgroup whose weight is being updated
	 * @weight: new weight [1..10000]
	 *
	 * Update @tg's weight to @weight.
	 */
	void (*cgroup_set_weight)(struct cgroup *cgrp, u32 weight);
#endif	/* CONFIG_CGROUPS */

	/*
	 * All online ops must come before ops.init().
	 */

	/**
	 * init - Initialize the BPF scheduler
	 */
	s32 (*init)(void);

	/**
	 * exit - Clean up after the BPF scheduler
	 * @info: Exit info
	 */
	void (*exit)(struct scx_exit_info *info);

	/**
	 * dispatch_max_batch - Max nr of tasks that dispatch() can dispatch
	 */
	u32 dispatch_max_batch;

	/**
	 * flags - %SCX_OPS_* flags
	 */
	u64 flags;

	/**
	 * timeout_ms - The maximum amount of time, in milliseconds, that a
	 * runnable task should be able to wait before being scheduled. The
	 * maximum timeout may not exceed the default timeout of 30 seconds.
	 *
	 * Defaults to the maximum allowed timeout value of 30 seconds.
	 */
	u32 timeout_ms;

	/**
	 * name - BPF scheduler's name
	 *
	 * Must be a non-zero valid BPF object name including only isalnum(),
	 * '_' and '.' chars. Shows up in kernel.sched_ext_ops sysctl while the
	 * BPF scheduler is enabled.
	 */
	char name[SCX_OPS_NAME_LEN];
};

/*
 * Dispatch queue (dsq) is a simple FIFO which is used to buffer between the
 * scheduler core and the BPF scheduler. See the documentation for more details.
 */
struct scx_dispatch_q {
	raw_spinlock_t		lock;
	struct list_head	fifo;	/* processed in dispatching order */
	struct rb_root_cached	priq;	/* processed in p->scx.dsq_vtime order */
	u32			nr;
	u64			id;
	struct rhash_head	hash_node;
	struct llist_node	free_node;
	struct rcu_head		rcu;
};

/* scx_entity.flags */
enum scx_ent_flags {
	SCX_TASK_QUEUED		= 1 << 0, /* on ext runqueue */
	SCX_TASK_BAL_KEEP	= 1 << 1, /* balance decided to keep current */
	SCX_TASK_ENQ_LOCAL	= 1 << 2, /* used by scx_select_cpu_dfl() to set SCX_ENQ_LOCAL */

	SCX_TASK_OPS_PREPPED	= 1 << 8, /* prepared for BPF scheduler enable */
	SCX_TASK_OPS_ENABLED	= 1 << 9, /* task has BPF scheduler enabled */

	SCX_TASK_WATCHDOG_RESET = 1 << 16, /* task watchdog counter should be reset */
	SCX_TASK_DEQD_FOR_SLEEP	= 1 << 17, /* last dequeue was for SLEEP */

	SCX_TASK_CURSOR		= 1 << 31, /* iteration cursor, not a task */
};

/* scx_entity.dsq_flags */
enum scx_ent_dsq_flags {
	SCX_TASK_DSQ_ON_PRIQ	= 1 << 0, /* task is queued on the priority queue of a dsq */
};

/*
 * Mask bits for scx_entity.kf_mask. Not all kfuncs can be called from
 * everywhere and the following bits track which kfunc sets are currently
 * allowed for %current. This simple per-task tracking works because SCX ops
 * nest in a limited way. BPF will likely implement a way to allow and disallow
 * kfuncs depending on the calling context which will replace this manual
 * mechanism. See scx_kf_allow().
 */
enum scx_kf_mask {
	SCX_KF_UNLOCKED		= 0,	  /* not sleepable, not rq locked */
	/* all non-sleepables may be nested inside INIT and SLEEPABLE */
	SCX_KF_INIT		= 1 << 0, /* running ops.init() */
	SCX_KF_SLEEPABLE	= 1 << 1, /* other sleepable init operations */
	/* ENQUEUE and DISPATCH may be nested inside CPU_RELEASE */
	SCX_KF_CPU_RELEASE	= 1 << 2, /* ops.cpu_release() */
	/* ops.dequeue (in REST) may be nested inside DISPATCH */
	SCX_KF_DISPATCH		= 1 << 3, /* ops.dispatch() */
	SCX_KF_ENQUEUE		= 1 << 4, /* ops.enqueue() */
	SCX_KF_REST		= 1 << 5, /* other rq-locked operations */

	__SCX_KF_RQ_LOCKED	= SCX_KF_CPU_RELEASE | SCX_KF_DISPATCH |
				  SCX_KF_ENQUEUE | SCX_KF_REST,
	__SCX_KF_TERMINAL	= SCX_KF_ENQUEUE | SCX_KF_REST,
};

/*
 * The following is embedded in task_struct and contains all fields necessary
 * for a task to be scheduled by SCX.
 */
struct sched_ext_entity {
	struct scx_dispatch_q	*dsq;
	struct {
		struct list_head	fifo;	/* dispatch order */
		struct rb_node		priq;	/* p->scx.dsq_vtime order */
	} dsq_node;
	struct list_head	watchdog_node;
	u32			flags;		/* protected by rq lock */
	u32			dsq_flags;	/* protected by dsq lock */
	u32			weight;
	s32			sticky_cpu;
	s32			holding_cpu;
	u32			kf_mask;	/* see scx_kf_mask above */
	struct task_struct	*kf_tasks[2];	/* see SCX_CALL_OP_TASK() */
	atomic_long_t		ops_state;
	unsigned long		runnable_at;
#ifdef CONFIG_SCHED_CORE
	u64			core_sched_at;	/* see scx_prio_less() */
#endif

	/* BPF scheduler modifiable fields */

	/*
	 * Runtime budget in nsecs. This is usually set through
	 * scx_bpf_dispatch() but can also be modified directly by the BPF
	 * scheduler. Automatically decreased by SCX as the task executes. On
	 * depletion, a scheduling event is triggered.
	 *
	 * This value is cleared to zero if the task is preempted by
	 * %SCX_KICK_PREEMPT and shouldn't be used to determine how long the
	 * task ran. Use p->se.sum_exec_runtime instead.
	 */
	u64			slice;

	/*
	 * Used to order tasks when dispatching to the vtime-ordered priority
	 * queue of a dsq. This is usually set through scx_bpf_dispatch_vtime()
	 * but can also be modified directly by the BPF scheduler. Modifying it
	 * while a task is queued on a dsq may mangle the ordering and is not
	 * recommended.
	 */
	u64			dsq_vtime;

	/*
	 * If set, reject future sched_setscheduler(2) calls updating the policy
	 * to %SCHED_EXT with -%EACCES.
	 *
	 * If set from ops.prep_enable() and the task's policy is already
	 * %SCHED_EXT, which can happen while the BPF scheduler is being loaded
	 * or by inhering the parent's policy during fork, the task's policy is
	 * rejected and forcefully reverted to %SCHED_NORMAL. The number of such
	 * events are reported through /sys/kernel/debug/sched_ext::nr_rejected.
	 */
	bool			disallow;	/* reject switching into SCX */

	/* cold fields */
	struct list_head	tasks_node;
#ifdef CONFIG_EXT_GROUP_SCHED
	struct cgroup		*cgrp_moving_from;
#endif
};

void sched_ext_free(struct task_struct *p);
void print_scx_info(const char *log_lvl, struct task_struct *p);

#else	/* !CONFIG_SCHED_CLASS_EXT */

static inline void sched_ext_free(struct task_struct *p) {}
static inline void print_scx_info(const char *log_lvl, struct task_struct *p) {}

#endif	/* CONFIG_SCHED_CLASS_EXT */
#endif	/* _LINUX_SCHED_EXT_H */
