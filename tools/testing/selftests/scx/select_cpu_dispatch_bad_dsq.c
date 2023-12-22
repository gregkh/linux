/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2023 David Vernet <dvernet@meta.com>
 * Copyright (c) 2023 Tejun Heo <tj@kernel.org>
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <libgen.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include <sys/wait.h>
#include "select_cpu_dispatch_bad_dsq.bpf.skel.h"
#include "scx_test.h"

#define NUM_CHILDREN 1028
#define SCX_EXIT_ERROR 1024

int main(int argc, char **argv)
{
	struct select_cpu_dispatch_bad_dsq *skel;
	struct bpf_link *link;
	pid_t pids[NUM_CHILDREN];
	int i, status;

	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	skel = select_cpu_dispatch_bad_dsq__open_and_load();
	SCX_BUG_ON(!skel, "Failed to open and load skel");

	/*
	 * The scheduler is expected to gracefully exit after bad_dsqoneously
	 * double-dispatching from ops.selec_cpu().
	 */
	link = bpf_map__attach_struct_ops(skel->maps.select_cpu_dispatch_bad_dsq_ops);
	SCX_BUG_ON(!link, "Failed to attach struct_ops");

	for (i = 0; i < NUM_CHILDREN; i++) {
		pids[i] = fork();
		if (pids[i] == 0) {
			sleep(1);
			exit(0);
		}
	}

	for (i = 0; i < NUM_CHILDREN; i++) {
		SCX_EQ(waitpid(pids[i], &status, 0), pids[i]);
		SCX_EQ(status, 0);
	}

	SCX_EQ(skel->bss->uei.kind, SCX_EXIT_ERROR);
	bpf_link__destroy(link);
	select_cpu_dispatch_bad_dsq__destroy(skel);

	return 0;
}
