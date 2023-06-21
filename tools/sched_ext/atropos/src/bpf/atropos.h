// Copyright (c) Meta Platforms, Inc. and affiliates.

// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.
#ifndef __ATROPOS_H
#define __ATROPOS_H

#include <stdbool.h>
#ifndef __kptr
#ifdef __KERNEL__
#error "__kptr_ref not defined in the kernel"
#endif
#define __kptr
#endif

#define	MAX_CPUS 512
#define	MAX_DOMS 64 /* limited to avoid complex bitmask ops */
#define	CACHELINE_SIZE 64

/* Statistics */
enum stat_idx {
	/* The following fields add up to all dispatched tasks */
	ATROPOS_STAT_WAKE_SYNC,
	ATROPOS_STAT_PREV_IDLE,
	ATROPOS_STAT_GREEDY_IDLE,
	ATROPOS_STAT_PINNED,
	ATROPOS_STAT_DIRECT_DISPATCH,
	ATROPOS_STAT_DIRECT_GREEDY,
	ATROPOS_STAT_DIRECT_GREEDY_FAR,
	ATROPOS_STAT_DSQ_DISPATCH,
	ATROPOS_STAT_GREEDY,

	/* Extra stats that don't contribute to total */
	ATROPOS_STAT_REPATRIATE,
	ATROPOS_STAT_KICK_GREEDY,
	ATROPOS_STAT_LOAD_BALANCE,

	/* Errors */
	ATROPOS_STAT_TASK_GET_ERR,

	ATROPOS_NR_STATS,
};

struct task_ctx {
	/* The domains this task can run on */
	unsigned long long dom_mask;

	struct bpf_cpumask __kptr *cpumask;
	unsigned int dom_id;
	unsigned int weight;
	unsigned long long runnable_at;
	unsigned long long runnable_for;

	/* The task is a workqueue worker thread */
	bool is_kworker;

	/* Allowed on all CPUs and eligible for DIRECT_GREEDY optimization */
	bool all_cpus;

	/* select_cpu() telling enqueue() to queue directly on the DSQ */
	bool dispatch_local;
};

#endif /* __ATROPOS_H */
