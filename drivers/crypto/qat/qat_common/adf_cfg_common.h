/* SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0-only) */
/* Copyright(c) 2014 - 2020 Intel Corporation */
#ifndef ADF_CFG_COMMON_H_
#define ADF_CFG_COMMON_H_

#include <linux/types.h>

#define ADF_CFG_MAX_STR_LEN 64
#define ADF_CFG_MAX_KEY_LEN_IN_BYTES ADF_CFG_MAX_STR_LEN
#define ADF_CFG_MAX_VAL_LEN_IN_BYTES ADF_CFG_MAX_STR_LEN
#define ADF_CFG_MAX_SECTION_LEN_IN_BYTES ADF_CFG_MAX_STR_LEN
#define ADF_MAX_DEVICES (32 * 32)
#define ADF_DEVS_ARRAY_SIZE BITS_TO_LONGS(ADF_MAX_DEVICES)

enum adf_cfg_val_type {
	ADF_DEC,
	ADF_HEX,
	ADF_STR
};

enum adf_device_type {
	DEV_UNKNOWN = 0,
	DEV_DH895XCC,
	DEV_DH895XCCVF,
	DEV_C62X,
	DEV_C62XVF,
	DEV_C3XXX,
	DEV_C3XXXVF,
	DEV_4XXX,
};
#endif
