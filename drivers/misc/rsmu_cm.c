// SPDX-License-Identifier: GPL-2.0+
/*
 * This driver is developed for the IDT ClockMatrix(TM) of
 * timing and synchronization devices.
 *
 * Copyright (C) 2019 Integrated Device Technology, Inc., a Renesas Company.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/mfd/idt8a340_reg.h>
#include <linux/mfd/rsmu.h>
#include <uapi/linux/rsmu.h>
#include <asm/unaligned.h>

#include "rsmu_cdev.h"

static int rsmu_cm_set_combomode(struct rsmu_cdev *rsmu, u8 dpll, u8 mode)
{
	u32 dpll_ctrl_n;
	u8 cfg;
	int err;

	switch (dpll) {
	case 0:
		dpll_ctrl_n = DPLL_CTRL_0;
		break;
	case 1:
		dpll_ctrl_n = DPLL_CTRL_1;
		break;
	case 2:
		dpll_ctrl_n = DPLL_CTRL_2;
		break;
	case 3:
		dpll_ctrl_n = DPLL_CTRL_3;
		break;
	case 4:
		dpll_ctrl_n = DPLL_CTRL_4;
		break;
	case 5:
		dpll_ctrl_n = DPLL_CTRL_5;
		break;
	case 6:
		dpll_ctrl_n = DPLL_CTRL_6;
		break;
	case 7:
		dpll_ctrl_n = DPLL_CTRL_7;
		break;
	default:
		return -EINVAL;
	}

	if (mode >= E_COMBOMODE_MAX)
		return -EINVAL;

	err = regmap_bulk_read(rsmu->regmap, dpll_ctrl_n + DPLL_CTRL_COMBO_MASTER_CFG,
			       &cfg, sizeof(cfg));
	if (err)
		return err;

	/* Only need to enable/disable COMBO_MODE_HOLD. */
	if (mode)
		cfg |= COMBO_MASTER_HOLD;
	else
		cfg &= ~COMBO_MASTER_HOLD;

	return regmap_bulk_write(rsmu->regmap, dpll_ctrl_n + DPLL_CTRL_COMBO_MASTER_CFG,
				 &cfg, sizeof(cfg));
}

static int rsmu_cm_set_holdover_mode(struct rsmu_cdev *rsmu, u8 dpll, u8 enable, u8 mode)
{
	/* This function enable or disable holdover. The mode is ignored. */
	u8 dpll_mode_offset;
	u8 state_mode;
	u32 dpll_n;
	u8 reg;
	int err;

	(void)mode;

	dpll_mode_offset = IDTCM_FW_REG(rsmu->fw_version, V520, DPLL_MODE);

	switch (dpll) {
	case 0:
		dpll_n = DPLL_0;
		break;
	case 1:
		dpll_n = DPLL_1;
		break;
	case 2:
		dpll_n = IDTCM_FW_REG(rsmu->fw_version, V520, DPLL_2);
		break;
	case 3:
		dpll_n = DPLL_3;
		break;
	case 4:
		dpll_n = IDTCM_FW_REG(rsmu->fw_version, V520, DPLL_4);
		break;
	case 5:
		dpll_n = DPLL_5;
		break;
	case 6:
		dpll_n = IDTCM_FW_REG(rsmu->fw_version, V520, DPLL_6);
		break;
	case 7:
		dpll_n = DPLL_7;
		break;
	default:
		return -EINVAL;
	}

	err = regmap_bulk_read(rsmu->regmap, dpll_n + dpll_mode_offset,
			       &reg, sizeof(reg));
	if (err)
		return err;

	/* To enable holdover, set state_mode (bits [0,2]) to force_holdover (3).
	   To disable holdover, set  state_mode (bits [0,2]) to automatic (0). */
	state_mode = reg & 0x7;
	if (enable) {
		if (state_mode == 3)
			return 0;
	} else {
		if (state_mode == 0)
			return 0;
	}

	/* Set state_mode = 0 */
	reg &= 0xF8;
	if (enable)
		reg |= 3;

	return regmap_bulk_write(rsmu->regmap, dpll_n + dpll_mode_offset,
				 &reg, sizeof(reg));
}

static int rsmu_cm_set_output_tdc_go(struct rsmu_cdev *rsmu, u8 tdc, u8 enable)
{
	/* This function enable or disable the output TDC alignment. */
	u8 tdc_ctrl4_offset;
	u32 tdc_n;
	u8 reg;
	int err;

	tdc_ctrl4_offset = IDTCM_FW_REG(rsmu->fw_version, V520, OUTPUT_TDC_CTRL_4);

	switch (tdc) {
	case 0:
		tdc_n = OUTPUT_TDC_0;
		break;
	case 1:
		tdc_n = OUTPUT_TDC_1;
		break;
	case 2:
		tdc_n = OUTPUT_TDC_2;
		break;
	case 3:
		tdc_n = OUTPUT_TDC_3;
		break;
	default:
		return -EINVAL;
	}

	err = regmap_bulk_read(rsmu->regmap, tdc_n + tdc_ctrl4_offset,
			       &reg, sizeof(reg));

	if (enable)
		reg |= 0x01;
	else
		reg &= ~0x01;

	return regmap_bulk_write(rsmu->regmap, tdc_n + tdc_ctrl4_offset,
				 &reg, sizeof(reg));
}

static int rsmu_cm_get_dpll_state(struct rsmu_cdev *rsmu, u8 dpll, u8 *state)
{
	u8 cfg;
	int err;

	/* 8 is sys dpll */
	if (dpll > 8)
		return -EINVAL;

	err = regmap_bulk_read(rsmu->regmap, STATUS + DPLL0_STATUS + dpll, &cfg, sizeof(cfg));
	if (err)
		return err;

	switch (cfg & DPLL_STATE_MASK) {
	case DPLL_STATE_FREERUN:
		*state = E_SRVLOUNQUALIFIEDSTATE;
		break;
	case DPLL_STATE_LOCKACQ:
	case DPLL_STATE_LOCKREC:
		*state = E_SRVLOLOCKACQSTATE;
		break;
	case DPLL_STATE_LOCKED:
		*state = E_SRVLOTIMELOCKEDSTATE;
		break;
	case DPLL_STATE_HOLDOVER:
		*state = E_SRVLOHOLDOVERINSPECSTATE;
		break;
	default:
		*state = E_SRVLOSTATEINVALID;
		break;
	}

	return 0;
}

static int rsmu_cm_get_dpll_ffo(struct rsmu_cdev *rsmu, u8 dpll,
				struct rsmu_get_ffo *ffo)
{
	u8 buf[8] = {0};
	s64 fcw = 0;
	u16 dpll_filter_status;
	int err;

	switch (dpll) {
	case 0:
		dpll_filter_status = DPLL0_FILTER_STATUS;
		break;
	case 1:
		dpll_filter_status = DPLL1_FILTER_STATUS;
		break;
	case 2:
		dpll_filter_status = DPLL2_FILTER_STATUS;
		break;
	case 3:
		dpll_filter_status = DPLL3_FILTER_STATUS;
		break;
	case 4:
		dpll_filter_status = DPLL4_FILTER_STATUS;
		break;
	case 5:
		dpll_filter_status = DPLL5_FILTER_STATUS;
		break;
	case 6:
		dpll_filter_status = DPLL6_FILTER_STATUS;
		break;
	case 7:
		dpll_filter_status = DPLL7_FILTER_STATUS;
		break;
	case 8:
		dpll_filter_status = DPLLSYS_FILTER_STATUS;
		break;
	default:
		return -EINVAL;
	}

	err = regmap_bulk_read(rsmu->regmap, STATUS + dpll_filter_status, buf, 6);
	if (err)
		return err;

	/* Convert to frequency control word */
	fcw = sign_extend64(get_unaligned_le64(buf), 47);

	/* FCW unit is 2 ^ -53 = 1.1102230246251565404236316680908e-16 */
	ffo->ffo = fcw * 111;

	return 0;
}

static int rsmu_cm_get_fw_version(struct rsmu_cdev *rsmu)
{
	int err;
	u8 major;
	u8 minor;
	u8 hotfix;

	err = regmap_bulk_read(rsmu->regmap, GENERAL_STATUS + MAJ_REL,
			       &major, sizeof(major));
	if (err)
		return err;
	major >>= 1;

	err = regmap_bulk_read(rsmu->regmap, GENERAL_STATUS + MIN_REL,
			       &minor, sizeof(minor));
	if (err)
		return err;

	err = regmap_bulk_read(rsmu->regmap, GENERAL_STATUS + HOTFIX_REL,
			       &hotfix, sizeof(hotfix));
	if (err)
		return err;

	if (major >= 5 && minor >= 2) {
		rsmu->fw_version = V520;
		return 0;
	}

	if (major == 4 && minor >= 8) {
		rsmu->fw_version = V487;
		return 0;
	}

	rsmu->fw_version = V_DEFAULT;
	return 0;
}

struct rsmu_ops cm_ops = {
	.type = RSMU_CM,
	.set_combomode = rsmu_cm_set_combomode,
	.get_dpll_state = rsmu_cm_get_dpll_state,
	.get_dpll_ffo = rsmu_cm_get_dpll_ffo,
	.set_holdover_mode = rsmu_cm_set_holdover_mode,
	.set_output_tdc_go = rsmu_cm_set_output_tdc_go,
	.get_fw_version = rsmu_cm_get_fw_version,
};
