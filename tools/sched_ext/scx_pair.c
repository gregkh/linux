/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <libgen.h>
#include <bpf/bpf.h>
#include "user_exit_info.h"
#include "scx_pair.h"
#include "scx_pair.skel.h"

const char help_fmt[] =
"A demo sched_ext core-scheduler which always makes every sibling CPU pair\n"
"execute from the same CPU cgroup.\n"
"\n"
"See the top-level comment in .bpf.c for more details.\n"
"\n"
"Usage: %s [-S STRIDE] [-p]\n"
"\n"
"  -S STRIDE     Override CPU pair stride (default: nr_cpus_ids / 2)\n"
"  -p            Switch only tasks on SCHED_EXT policy intead of all\n"
"  -h            Display this help and exit\n";

static volatile int exit_req;

static void sigint_handler(int dummy)
{
	exit_req = 1;
}

int main(int argc, char **argv)
{
	struct scx_pair *skel;
	struct bpf_link *link;
	u64 seq = 0;
	s32 stride, i, opt, outer_fd;

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	skel = scx_pair__open();
	assert(skel);

	skel->rodata->nr_cpu_ids = libbpf_num_possible_cpus();

	/* pair up the earlier half to the latter by default, override with -s */
	stride = skel->rodata->nr_cpu_ids / 2;

	while ((opt = getopt(argc, argv, "S:ph")) != -1) {
		switch (opt) {
		case 'S':
			stride = strtoul(optarg, NULL, 0);
			break;
		case 'p':
			skel->rodata->switch_partial = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	printf("Pairs: ");
	for (i = 0; i < skel->rodata->nr_cpu_ids; i++) {
		int j = (i + stride) % skel->rodata->nr_cpu_ids;

		if (skel->rodata->pair_cpu[i] >= 0)
			continue;

		if (i == j) {
			printf("\n");
			fprintf(stderr, "Invalid stride %d - CPU%d wants to be its own pair\n",
				stride, i);
			return 1;
		}

		if (skel->rodata->pair_cpu[j] >= 0) {
			printf("\n");
			fprintf(stderr, "Invalid stride %d - three CPUs (%d, %d, %d) want to be a pair\n",
				stride, i, j, skel->rodata->pair_cpu[j]);
			return 1;
		}

		skel->rodata->pair_cpu[i] = j;
		skel->rodata->pair_cpu[j] = i;
		skel->rodata->pair_id[i] = i;
		skel->rodata->pair_id[j] = i;
		skel->rodata->in_pair_idx[i] = 0;
		skel->rodata->in_pair_idx[j] = 1;

		printf("[%d, %d] ", i, j);
	}
	printf("\n");

	assert(!scx_pair__load(skel));

	/*
	 * Populate the cgrp_q_arr map which is an array containing per-cgroup
	 * queues. It'd probably be better to do this from BPF but there are too
	 * many to initialize statically and there's no way to dynamically
	 * populate from BPF.
	 */
	outer_fd = bpf_map__fd(skel->maps.cgrp_q_arr);
	assert(outer_fd >= 0);

	printf("Initializing");
        for (i = 0; i < MAX_CGRPS; i++) {
		s32 inner_fd;

		if (exit_req)
			break;

		inner_fd = bpf_map_create(BPF_MAP_TYPE_QUEUE, NULL, 0,
					  sizeof(u32), MAX_QUEUED, NULL);
		assert(inner_fd >= 0);
		assert(!bpf_map_update_elem(outer_fd, &i, &inner_fd, BPF_ANY));
		close(inner_fd);

		if (!(i % 10))
			printf(".");
		fflush(stdout);
        }
	printf("\n");

	/*
	 * Fully initialized, attach and run.
	 */
	link = bpf_map__attach_struct_ops(skel->maps.pair_ops);
	assert(link);

	while (!exit_req && !uei_exited(&skel->bss->uei)) {
		printf("[SEQ %lu]\n", seq++);
		printf(" total:%10lu dispatch:%10lu   missing:%10lu\n",
		       skel->bss->nr_total,
		       skel->bss->nr_dispatched,
		       skel->bss->nr_missing);
		printf(" kicks:%10lu preemptions:%7lu\n",
		       skel->bss->nr_kicks,
		       skel->bss->nr_preemptions);
		printf("   exp:%10lu exp_wait:%10lu exp_empty:%10lu\n",
		       skel->bss->nr_exps,
		       skel->bss->nr_exp_waits,
		       skel->bss->nr_exp_empty);
		printf("cgnext:%10lu   cgcoll:%10lu   cgempty:%10lu\n",
		       skel->bss->nr_cgrp_next,
		       skel->bss->nr_cgrp_coll,
		       skel->bss->nr_cgrp_empty);
		fflush(stdout);
		sleep(1);
	}

	bpf_link__destroy(link);
	uei_print(&skel->bss->uei);
	scx_pair__destroy(skel);
	return 0;
}
