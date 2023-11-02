// Copyright (c) Meta Platforms, Inc. and affiliates.

// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.
#ifndef __RUSTY_H
#define __RUSTY_H

#include <stdbool.h>
#ifndef __kptr
#ifdef __KERNEL__
#error "__kptr_ref not defined in the kernel"
#endif
#define __kptr
#endif

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#include "../../../scx_ravg.bpf.h"

#define	MAX_CPUS	512
#define	MAX_DOMS	64		/* limited to avoid complex bitmask ops */
#define	CACHELINE_SIZE	64
#define USAGE_HALF_LIFE	1000000000	/* 1s */

/* Statistics */
enum stat_idx {
	/* The following fields add up to all dispatched tasks */
	RUSTY_STAT_WAKE_SYNC,
	RUSTY_STAT_PREV_IDLE,
	RUSTY_STAT_GREEDY_IDLE,
	RUSTY_STAT_PINNED,
	RUSTY_STAT_DIRECT_DISPATCH,
	RUSTY_STAT_DIRECT_GREEDY,
	RUSTY_STAT_DIRECT_GREEDY_FAR,
	RUSTY_STAT_DSQ_DISPATCH,
	RUSTY_STAT_GREEDY,

	/* Extra stats that don't contribute to total */
	RUSTY_STAT_REPATRIATE,
	RUSTY_STAT_KICK_GREEDY,
	RUSTY_STAT_LOAD_BALANCE,

	/* Errors */
	RUSTY_STAT_TASK_GET_ERR,

	RUSTY_NR_STATS,
};

struct task_ctx {
	/* The domains this task can run on */
	u64 dom_mask;

	struct bpf_cpumask __kptr *cpumask;
	u32 dom_id;
	u32 weight;
	bool runnable;
	u64 runnable_at;
	u64 running_at;
	u64 runnable_for;

	/* The task is a workqueue worker thread */
	bool is_kworker;

	/* Allowed on all CPUs and eligible for DIRECT_GREEDY optimization */
	bool all_cpus;

	/* select_cpu() telling enqueue() to queue directly on the DSQ */
	bool dispatch_local;
};

struct dom_ctx {
	struct bpf_cpumask __kptr *cpumask;
	struct bpf_cpumask __kptr *direct_greedy_cpumask;
	u64 vtime_now;
};

#endif /* __RUSTY_H */
