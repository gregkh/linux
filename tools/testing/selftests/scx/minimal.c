/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <libgen.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "minimal.bpf.skel.h"

static volatile int exit_req;

static void sigint_handler(int simple)
{
	exit_req = 1;
}

int main(int argc, char **argv)
{
	struct minimal *skel;
	struct bpf_link *link;

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	skel = minimal__open_and_load();
	SCX_BUG_ON(!skel, "Failed to open and load skel");

	link = bpf_map__attach_struct_ops(skel->maps.minimal_ops);
	SCX_BUG_ON(!link, "Failed to attach struct_ops");
	sleep(1);
	bpf_link__destroy(link);
	minimal__destroy(skel);

	return 0;
}
