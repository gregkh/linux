/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2024 David Vernet <dvernet@meta.com>
 * Copyright (c) 2024 Tejun Heo <tj@kernel.org>
 */
#include <stdio.h>
#include <unistd.h>
#include <libgen.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include <sys/wait.h>
#include "select_cpu_vtime.bpf.skel.h"
#include "scx_test.h"

int main(int argc, char **argv)
{
	struct select_cpu_vtime *skel;
	struct bpf_link *link;

	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	skel = select_cpu_vtime__open_and_load();
	SCX_BUG_ON(!skel, "Failed to open and load skel");

	SCX_ASSERT(!skel->bss->consumed);

	link = bpf_map__attach_struct_ops(skel->maps.select_cpu_vtime_ops);
	SCX_BUG_ON(!link, "Failed to attach struct_ops");

	sleep(1);

	SCX_ASSERT(skel->bss->consumed);

	bpf_link__destroy(link);
	select_cpu_vtime__destroy(skel);

	return 0;
}
