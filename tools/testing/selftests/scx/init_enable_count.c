/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2023 David Vernet <dvernet@meta.com>
 * Copyright (c) 2023 Tejun Heo <tj@kernel.org>
 */
#include <stdio.h>
#include <unistd.h>
#include <sched.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include <sys/wait.h>
#include "scx_test.h"
#include "init_enable_count.bpf.skel.h"

#define SCHED_EXT 7

static struct init_enable_count *
open_load_prog(bool global)
{
	struct init_enable_count *skel;

	skel = init_enable_count__open();
	SCX_BUG_ON(!skel, "Failed to open skel");

	if (global)
		skel->rodata->switch_all = global;

	SCX_BUG_ON(init_enable_count__load(skel), "Failed to load skel");

	return skel;
}

static void run_test(bool global)
{
	struct init_enable_count *skel;
	struct bpf_link *link;
	const u32 num_children = 5;
	int ret, i, status;
	struct sched_param param = {};
	pid_t pids[num_children];

	skel = open_load_prog(global);
	link = bpf_map__attach_struct_ops(skel->maps.init_enable_count_ops);
	SCX_BUG_ON(!link, "Failed to attach struct_ops");

	/* SCHED_EXT children */
	for (i = 0; i < num_children; i++) {
		pids[i] = fork();
		SCX_BUG_ON(pids[i] < 0, "Failed to fork child");

		if (pids[i] == 0) {
			ret = sched_setscheduler(0, SCHED_EXT, &param);
			SCX_BUG_ON(ret, "Failed to set sched to sched_ext");

			/*
			 * Reset to SCHED_OTHER for half of them. Counts for
			 * everything should still be the same regardless, as
			 * ops.disable() is invoked even if a task is still on
			 * SCHED_EXT before it exits.
			 */
			if (i % 2 == 0) {
				ret = sched_setscheduler(0, SCHED_OTHER, &param);
				SCX_BUG_ON(ret, "Failed to reset sched to normal");
			}
			exit(0);
		}
	}
	for (i = 0; i < num_children; i++) {
		SCX_BUG_ON(waitpid(pids[i], &status, 0) != pids[i],
			   "Failed to wait for SCX child");
		SCX_BUG_ON(status != 0, "SCX child %d exited with status %d",
			   i, status);
	}

	/* SCHED_OTHER children */
	for (i = 0; i < num_children; i++) {
		pids[i] = fork();
		if (pids[i] == 0)
			exit(0);
	}
	for (i = 0; i < num_children; i++) {
		SCX_BUG_ON(waitpid(pids[i], &status, 0) != pids[i],
			   "Failed to wait for normal child");
		SCX_BUG_ON(status != 0,
			   "Normal child %d exited with status %d", i, status);
	}

	sleep(1);

	SCX_GE(skel->bss->init_task_cnt, 2 * num_children);
	SCX_GE(skel->bss->exit_task_cnt, 2 * num_children);

	if (global) {
		SCX_GE(skel->bss->enable_cnt, 2 * num_children);
		SCX_GE(skel->bss->disable_cnt, 2 * num_children);
	} else {
		SCX_EQ(skel->bss->enable_cnt, num_children);
		SCX_EQ(skel->bss->disable_cnt, num_children);
	}

	bpf_link__destroy(link);
	init_enable_count__destroy(skel);
}

int main(int argc, char **argv)
{
	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	run_test(true);
	run_test(false);

	return 0;
}
