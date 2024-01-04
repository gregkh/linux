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
#include "enqueue_select_cpu_fails.bpf.skel.h"
#include "scx_test.h"

int main(int argc, char **argv)
{
	struct enqueue_select_cpu_fails *skel;
	struct bpf_link *link;

	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	skel = enqueue_select_cpu_fails__open_and_load();
	SCX_BUG_ON(!skel, "Failed to open and load skel");

	link = bpf_map__attach_struct_ops(skel->maps.enqueue_select_cpu_fails_ops);
	SCX_BUG_ON(!link, "Failed to attach struct_ops");

	sleep(1);

	bpf_link__destroy(link);
	enqueue_select_cpu_fails__destroy(skel);

	return 0;
}
