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
#include "scx_central.skel.h"

const char help_fmt[] =
"A central FIFO sched_ext scheduler.\n"
"\n"
"See the top-level comment in .bpf.c for more details.\n"
"\n"
"Usage: %s [-c CPU] [-p]\n"
"\n"
"  -c CPU        Override the central CPU (default: 0)\n"
"  -p            Switch only tasks on SCHED_EXT policy intead of all\n"
"  -h            Display this help and exit\n";

static volatile int exit_req;

static void sigint_handler(int dummy)
{
	exit_req = 1;
}

int main(int argc, char **argv)
{
	struct scx_central *skel;
	struct bpf_link *link;
	u64 seq = 0;
	s32 opt;

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	skel = scx_central__open();
	assert(skel);

	skel->rodata->central_cpu = 0;
	skel->rodata->nr_cpu_ids = libbpf_num_possible_cpus();

	while ((opt = getopt(argc, argv, "c:ph")) != -1) {
		switch (opt) {
		case 'c':
			skel->rodata->central_cpu = strtoul(optarg, NULL, 0);
			break;
		case 'p':
			skel->rodata->switch_partial = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	assert(!scx_central__load(skel));

	link = bpf_map__attach_struct_ops(skel->maps.central_ops);
	assert(link);

	while (!exit_req && !uei_exited(&skel->bss->uei)) {
		printf("[SEQ %lu]\n", seq++);
		printf("total   :%10lu    local:%10lu   queued:%10lu  lost:%10lu\n",
		       skel->bss->nr_total,
		       skel->bss->nr_locals,
		       skel->bss->nr_queued,
		       skel->bss->nr_lost_pids);
		printf("timer   :%10lu dispatch:%10lu mismatch:%10lu retry:%10lu\n",
		       skel->bss->nr_timers,
		       skel->bss->nr_dispatches,
		       skel->bss->nr_mismatches,
		       skel->bss->nr_retries);
		printf("overflow:%10lu\n",
		       skel->bss->nr_overflows);
		fflush(stdout);
		sleep(1);
	}

	bpf_link__destroy(link);
	uei_print(&skel->bss->uei);
	scx_central__destroy(skel);
	return 0;
}
