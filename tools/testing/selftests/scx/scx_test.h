/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2023 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2023 David Vernet <dvernet@meta.com>
 */

#ifndef __SCX_TEST_H__
#define __SCX_TEST_H__

#include <scx/common.h>

#define SCX_GT(_x, _y) SCX_BUG_ON((_x) <= (_y), "Expected %s > %s (%lu > %lu)",		\
				  #_x, #_y, (u64)(_x), (u64)(_y))
#define SCX_GE(_x, _y) SCX_BUG_ON((_x) < (_y), "Expected %s >= %s (%lu >= %lu)",	\
				  #_x, #_y, (u64)(_x), (u64)(_y))
#define SCX_LT(_x, _y) SCX_BUG_ON((_x) >= (_y), "Expected %s < %s (%lu < %lu)",		\
				  #_x, #_y, (u64)(_x), (u64)(_y))
#define SCX_LE(_x, _y) SCX_BUG_ON((_x) > (_y), "Expected %s <= %s (%lu <= %lu)",	\
				  #_x, #_y, (u64)(_x), (u64)(_y))
#define SCX_EQ(_x, _y) SCX_BUG_ON((_x) != (_y), "Expected %s == %s (%lu == %lu)",	\
				  #_x, #_y, (u64)(_x), (u64)(_y))
#define SCX_ASSERT(_x) SCX_BUG_ON(!(_x), "Expected %s to be true (%lu)",		\
				  #_x, (u64)(_x))

#endif  // # __SCX_TEST_H__
