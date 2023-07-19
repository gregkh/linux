/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2023 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2023 David Vernet <dvernet@meta.com>
 */
#ifndef __SCHED_EXT_USER_COMMON_H
#define __SCHED_EXT_USER_COMMON_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __KERNEL__
#error "Should not be included by BPF programs"
#endif

#define SCX_BUG(__fmt, ...)							\
	do {									\
		fprintf(stderr, "%s:%d [scx panic]: %s\n", __FILE__, __LINE__,	\
			strerror(errno));					\
		fprintf(stderr, __fmt __VA_OPT__(,) __VA_ARGS__);		\
		fprintf(stderr, "\n");						\
										\
		exit(EXIT_FAILURE);						\
	} while (0)

#define SCX_BUG_ON(__cond, __fmt, ...)					\
	do {								\
		if (__cond)						\
			SCX_BUG((__fmt) __VA_OPT__(,) __VA_ARGS__);	\
	} while (0)

#endif	/* __SCHED_EXT_USER_COMMON_H */
