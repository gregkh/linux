/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
 * Driver for the IDT ClockMatrix(TM) and 82p33xxx families of
 * timing and synchronization devices.
 *
 * Copyright (C) 2019 Integrated Device Technology, Inc., a Renesas Company.
 */

#ifndef __UAPI_LINUX_RSMU_CDEV_H
#define __UAPI_LINUX_RSMU_CDEV_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* Set dpll combomode */
struct rsmu_combomode {
	__u8 dpll;
	__u8 mode;
};

/* Get dpll state */
struct rsmu_get_state {
	__u8 dpll;
	__u8 state;
};

/* Get dpll ffo (fractional frequency offset) in ppqt*/
struct rsmu_get_ffo {
	__u8 dpll;
	__s64 ffo;
};

/* Set holdover mode */
struct rsmu_holdover_mode {
  __u8 dpll;
  __u8 enable;
  __u8 mode;
};

/* Set output TDC go bit */
struct rsmu_set_output_tdc_go
{
  __u8 tdc;
  __u8 enable;
};

/* Get register */
struct rsmu_reg_rw {
	__u32 offset;
	__u8 byte_count;
	__u8 bytes[256];
};

/*
 * RSMU IOCTL List
 */
#define RSMU_MAGIC '?'

/**
 * @Description
 * ioctl to set SMU combo mode.Combo mode provides physical layer frequency
 * support from the Ethernet Equipment Clock to the PTP clock
 *
 * @Parameters
 * pointer to struct rsmu_combomode that contains dpll combomode setting
 */
#define RSMU_SET_COMBOMODE  _IOW(RSMU_MAGIC, 1, struct rsmu_combomode)

/**
 * @Description
 * ioctl to get SMU dpll state. Application can call this API to tell if
 * SMU is locked to the GNSS signal
 *
 * @Parameters
 * pointer to struct rsmu_get_state that contains dpll state
 */
#define RSMU_GET_STATE  _IOR(RSMU_MAGIC, 2, struct rsmu_get_state)

/**
 * @Description
 * ioctl to get SMU dpll ffo (fractional frequency offset).
 *
 * @Parameters
 * pointer to struct rsmu_get_ffo that contains dpll ffo in ppqt
 */
#define RSMU_GET_FFO  _IOR(RSMU_MAGIC, 3, struct rsmu_get_ffo)

/**
 * @Description
 * ioctl to enable/disable SMU HW holdover mode.
 *
 * @Parameters
 * pointer to struct rsmu_set_holdover_mode that contains enable flag
 */
#define RSMU_SET_HOLDOVER_MODE  _IOW(RSMU_MAGIC, 4, struct rsmu_holdover_mode)

/**
 * @Description
 * ioctl to set the output TDC 'go' bit.
 *
 * @Parameters
 * pointer to struct rsmu_set_output_tdc_go that contains enable flag
 */
#define RSMU_SET_OUTPUT_TDC_GO  _IOW( RSMU_MAGIC, 5, struct rsmu_set_output_tdc_go )

#define RSMU_REG_READ  _IOR(RSMU_MAGIC, 100, struct rsmu_reg_rw)
#define RSMU_REG_WRITE _IOR(RSMU_MAGIC, 101, struct rsmu_reg_rw)
#endif /* __UAPI_LINUX_RSMU_CDEV_H */
