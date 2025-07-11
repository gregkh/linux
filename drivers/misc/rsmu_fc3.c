// SPDX-License-Identifier: GPL-2.0+
/*
 * This driver is developed for RC38xxx (FemtoClock3) series of
 * timing and synchronization devices.
 *
 * Copyright (C) 2023 Integrated Device Technology, Inc., a Renesas Company.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/mfd/idtRC38xxx_reg.h>
#include <linux/mfd/rsmu.h>
#include <linux/unaligned.h>

#include "rsmu_cdev.h"

#define FW_FILENAME	"rsmufc3.bin"
#define DEVID(rsmu)	(((struct rsmufc3 *)rsmu->ddata)->devid)
#define HW_PARAM(rsmu)	(&((struct rsmufc3 *)rsmu->ddata)->hw_param)
#define MEAS_MODE(rsmu)	(((struct rsmufc3 *)rsmu->ddata)->meas_mode)
#define TDC_APLL(rsmu)	(((struct rsmufc3 *)rsmu->ddata)->tdc_apll_freq)
#define TIME_REF(rsmu)	(((struct rsmufc3 *)rsmu->ddata)->time_ref_freq)

struct rsmufc3 {
	u8 devid;
	u8 meas_mode;
	struct idtfc3_hw_param hw_param;
	u32 tdc_apll_freq;
	u32 time_ref_freq;
};

static int get_apll_reinit_reg_offset(u8 devid, u16 *apll_reinit_reg_offset)
{
	switch (devid) {
	case V_DEFAULT:
	case VFC3W:
		*apll_reinit_reg_offset = SOFT_RESET_CTRL;
		break;
	case VFC3A:
		*apll_reinit_reg_offset = MISC_CTRL;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int read_device_id(struct rsmu_cdev *rsmu)
{
	int err;
	u8 buf[2] = {0};
	u16 device_id;

	err = regmap_bulk_read(rsmu->regmap, DEVICE_ID, buf, sizeof(buf));
	if (err)
		return err;

	device_id = get_unaligned_le16(buf);

	if (device_id & DEVICE_ID_MASK)
		DEVID(rsmu) = VFC3W;
	else
		DEVID(rsmu) = VFC3A;

	dev_info(rsmu->dev, "identified %s device\n",
		 DEVID(rsmu) == VFC3W ? "FC3W" : "FC3A");

	return 0;
}

static int rsmu_get_tdc_apll_freq(struct rsmu_cdev *rsmu)
{
	int err;
	u8 tdc_fb_div_int;
	u8 tdc_ref_div;

	err = regmap_bulk_read(rsmu->regmap, TDC_REF_DIV_CNFG,
				&tdc_ref_div, sizeof(tdc_ref_div));
	if (err)
		return err;

	err = regmap_bulk_read(rsmu->regmap, TDC_FB_DIV_INT_CNFG,
				&tdc_fb_div_int, sizeof(tdc_fb_div_int));
	if (err)
		return err;

	tdc_fb_div_int &= TDC_FB_DIV_INT_MASK;
	tdc_ref_div &= TDC_REF_DIV_CONFIG_MASK;

	TDC_APLL(rsmu) = div_u64(HW_PARAM(rsmu)->tdc_ref_freq *
				 (u64)tdc_fb_div_int, 1 << tdc_ref_div);

	return 0;
}

static int rsmu_get_time_ref_freq(struct rsmu_cdev *rsmu)
{
	int err;
	u8 buf[4];
	u8 time_ref_div;
	u8 time_clk_div;

	err = regmap_bulk_read(rsmu->regmap, TIME_CLOCK_MEAS_DIV_CNFG, buf, sizeof(buf));
	if (err)
		return err;
	time_ref_div = FIELD_GET(TIME_REF_DIV_MASK, get_unaligned_le32(buf)) + 1;

	err = regmap_bulk_read(rsmu->regmap, TIME_CLOCK_COUNT, buf, 1);
	if (err)
		return err;
	time_clk_div = (buf[0] & TIME_CLOCK_COUNT_MASK) + 1;
	TIME_REF(rsmu) = HW_PARAM(rsmu)->time_clk_freq * time_clk_div / time_ref_div;

	return 0;
}

static int set_tdc_meas_mode(struct rsmu_cdev *rsmu, u8 meas_mode)
{
	int err;
	u8 val = 0;

	if (meas_mode >= MEAS_MODE_INVALID)
		return -EINVAL;

	if (MEAS_MODE(rsmu) == meas_mode)
		return 0;

	/* Disable TDC first */
	err = regmap_bulk_write(rsmu->regmap, TIME_CLOCK_MEAS_CTRL, &val, sizeof(val));
	if (err)
		return err;

	/* Change TDC meas mode */
	err = regmap_bulk_write(rsmu->regmap, TIME_CLOCK_MEAS_CNFG,
				&meas_mode, sizeof(meas_mode));
	if (err)
		return err;

	MEAS_MODE(rsmu) = meas_mode;

	if (meas_mode == ONE_SHOT)
		return 0;

	/* Enable TDC and start measurement */
	val = TDC_MEAS_START | TDC_MEAS_EN;
	err = regmap_bulk_write(rsmu->regmap, TIME_CLOCK_MEAS_CTRL, &val, sizeof(val));
	if (err)
		return err;

	return 0;
}

static int hw_init(struct rsmu_cdev *rsmu)
{
	int err;

	if (DEVID(rsmu) == VFC3A)
		return 0;

	MEAS_MODE(rsmu) = MEAS_MODE_INVALID;
	err = set_tdc_meas_mode(rsmu, ONE_SHOT);
	if (err)
		return err;

	err = rsmu_get_time_ref_freq(rsmu);
	if (err)
		return err;

	return rsmu_get_tdc_apll_freq(rsmu);
}

static int hw_calibrate(struct rsmu_cdev *rsmu)
{
	int err = 0;
	u8 val;
	u16 apll_reinit_reg_addr;
	u8 apll_reinit_mask;
	u8 devid = DEVID(rsmu);

	err = get_apll_reinit_reg_offset(devid, &apll_reinit_reg_addr);
	if (err)
		return err;
	apll_reinit_mask = IDTFC3_FW_FIELD(devid, VFC3A, APLL_REINIT);

	/*
	 * Toggle TDC_DAC_RECAL_REQ:
	 * (1) set tdc_en to 1
	 * (2) set tdc_dac_recal_req to 0
	 * (3) set tdc_dac_recal_req to 1
	 */
	if (devid == VFC3A) {
		val = TDC_EN;
		err = regmap_bulk_write(rsmu->regmap, TDC_ENABLE_CTRL,
					&val, sizeof(val));
		if (err)
			return err;
		val = 0;
		err = regmap_bulk_write(rsmu->regmap, TDC_DAC_CAL_CTRL,
					&val, sizeof(val));
		if (err)
			return err;
		val = TDC_DAC_RECAL_REQ_VFC3A;
		err = regmap_bulk_write(rsmu->regmap, TDC_DAC_CAL_CTRL,
					&val, sizeof(val));
		if (err)
			return err;
	} else {
		val = TDC_EN;
		err = regmap_bulk_write(rsmu->regmap, TDC_CTRL,
					&val, sizeof(val));
		if (err)
			return err;
		val = TDC_EN | TDC_DAC_RECAL_REQ;
		err = regmap_bulk_write(rsmu->regmap, TDC_CTRL,
					&val, sizeof(val));
		if (err)
			return err;
	}
	mdelay(10);

	/*
	 * Toggle APLL_REINIT:
	 * (1) set apll_reinit to 0
	 * (2) set apll_reinit to 1
	 */
	err = regmap_bulk_read(rsmu->regmap, apll_reinit_reg_addr,
			       &val, sizeof(val));
	val &= ~apll_reinit_mask;
	err = regmap_bulk_write(rsmu->regmap, apll_reinit_reg_addr,
							&val, sizeof(val));
	if (err)
		return err;
	val |= apll_reinit_mask;
	err = regmap_bulk_write(rsmu->regmap, apll_reinit_reg_addr,
							&val, sizeof(val));
	if (err)
		return err;
	mdelay(10);

	return hw_init(rsmu);
}

static int load_firmware(struct rsmu_cdev *rsmu, char fwname[FW_NAME_LEN_MAX])
{
	char fname[128] = FW_FILENAME;
	const struct firmware *fw;
	struct idtfc3_fwrc *rec;
	u16 addr;
	u8 val;
	int err;
	s32 len;

	if (fwname) /* module parameter */
		snprintf(fname, sizeof(fname), "%s", fwname);

	dev_info(rsmu->dev, "requesting firmware '%s'\n", fname);

	err = request_firmware(&fw, fname, rsmu->dev);

	if (err) {
		dev_err(rsmu->dev,
			"requesting firmware failed with err %d!\n", err);
		return err;
	}

	dev_dbg(rsmu->dev, "firmware size %zu bytes\n", fw->size);

	rec = (struct idtfc3_fwrc *) fw->data;

	for (len = fw->size; len > 0; len -= sizeof(*rec)) {
		if (rec->reserved) {
			dev_err(rsmu->dev,
				"bad firmware, reserved field non-zero\n");
			err = -EINVAL;
		} else {
			val = rec->value;
			addr = rec->hiaddr << 8 | rec->loaddr;

			rec++;

			err = idtfc3_set_hw_param(HW_PARAM(rsmu), addr,
						  get_unaligned_be32((void *)rec));
			if (err == 0)
				rec++;
		}

		if (err != -EINVAL) {
			err = 0;

			/* Max register */
			if (addr > 0xE88)
				continue;

			err = regmap_bulk_write(rsmu->regmap, addr,
						&val, sizeof(val));
		}

		if (err)
			goto out;
	}

	err = hw_calibrate(rsmu);
out:
	release_firmware(fw);
	return err;
}

static u8 clock_index_to_ref_index(struct rsmu_cdev *rsmu, u8 clock_index)
{
	u16 reg_addr;
	u32 ref_sel_cnfg_reg;
	u8 ref_index;
	int err;
	u8 devid = DEVID(rsmu);

	reg_addr = IDTFC3_FW_REG(devid, VFC3A, REF_SEL_CNFG);
	err = regmap_bulk_read(rsmu->regmap, reg_addr, &ref_sel_cnfg_reg, sizeof(ref_sel_cnfg_reg));
	if (err)
		return err;

	for (ref_index = 0; ref_index <= MAX_REF_INDEX; ref_index++) {
		if (clock_index == ((ref_sel_cnfg_reg >> (REF_MUX_SEL_SHIFT * ref_index)) &
				     REF_MUX_SEL_MASK))
			return ref_index;
	}

	return ref_index;
}

static int get_losmon_sts_reg_offset(u8 devid, u8 ref_index, u16 *losmon_sts_reg_offset)
{
	switch (ref_index) {
	case 0:
		*losmon_sts_reg_offset = IDTFC3_FW_REG(devid, VFC3A, LOSMON_STS_0);
		break;
	case 1:
		*losmon_sts_reg_offset = IDTFC3_FW_REG(devid, VFC3A, LOSMON_STS_1);
		break;
	case 2:
		*losmon_sts_reg_offset = IDTFC3_FW_REG(devid, VFC3A, LOSMON_STS_2);
		break;
	case 3:
		*losmon_sts_reg_offset = IDTFC3_FW_REG(devid, VFC3A, LOSMON_STS_3);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int get_freqmon_sts_reg_offset(u8 devid, u8 ref_index, u16 *freqmon_sts_reg_offset)
{
	switch (ref_index) {
	case 0:
		*freqmon_sts_reg_offset = IDTFC3_FW_REG(devid, VFC3A, FREQMON_STS_0);
		break;
	case 1:
		*freqmon_sts_reg_offset = IDTFC3_FW_REG(devid, VFC3A, FREQMON_STS_1);
		break;
	case 2:
		*freqmon_sts_reg_offset = IDTFC3_FW_REG(devid, VFC3A, FREQMON_STS_2);
		break;
	case 3:
		*freqmon_sts_reg_offset = IDTFC3_FW_REG(devid, VFC3A, FREQMON_STS_3);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static inline s64 tdc_meas2offset(struct rsmu_cdev *rsmu, u64 meas_read)
{
	s64 coarse, fine;

	fine = sign_extend64(FIELD_GET(FINE_MEAS_MASK, meas_read), 12);
	coarse = sign_extend64(FIELD_GET(COARSE_MEAS_MASK, meas_read), (39 - 13));

	return div64_s64(coarse * NSEC_PER_SEC, TIME_REF(rsmu)) + div64_s64(
			 fine * NSEC_PER_SEC, TDC_APLL(rsmu) * 62LL);
}

static inline int get_tdc_meas(struct rsmu_cdev *rsmu, s64 *offset_ns)
{
	u8 buf[9];
	u8 val;
	int err;

	/* Waiting for measurement to be done */
	err = read_poll_timeout_atomic(regmap_bulk_read, err, !(val & FIFO_EMPTY),
				       0, 5 * USEC_PER_SEC, false, rsmu->regmap,
				       TDC_FIFO_STS, &val, sizeof(val));
	if (err) {
		dev_err(rsmu->dev, "TDC measurement timeout !!!");
		return err;
	}

	err = regmap_bulk_read(rsmu->regmap, TDC_FIFO_READ_REQ,
			       &buf, sizeof(buf));
	if (err)
		return err;

	*offset_ns = tdc_meas2offset(rsmu, get_unaligned_le64(&buf[1]));

	return 0;
}

static inline int check_tdc_fifo_overrun(struct rsmu_cdev *rsmu)
{
	u8 val;
	int err;

	/* Check if FIFO is overrun */
	err = regmap_bulk_read(rsmu->regmap, TDC_FIFO_STS, &val, sizeof(val));
	if (err)
		return err;

	if (!(val & FIFO_FULL))
		return 0;

	dev_warn(rsmu->dev, "TDC FIFO overrun !!!");

	MEAS_MODE(rsmu) = MEAS_MODE_INVALID;
	err = set_tdc_meas_mode(rsmu, CONTINUOUS);
	if (err)
		return err;

	return 0;
}

static int get_tdc_meas_one_shot(struct rsmu_cdev *rsmu, s64 *offset_ns)
{
	u8 val = TDC_MEAS_EN | TDC_MEAS_START;
	int err;

	err = regmap_bulk_write(rsmu->regmap, TIME_CLOCK_MEAS_CTRL, &val, sizeof(val));
	if (err)
		return err;

	return get_tdc_meas(rsmu, offset_ns);
}

static int get_tdc_meas_continuous(struct rsmu_cdev *rsmu, s64 *offset_ns)
{
	int err;

	err = check_tdc_fifo_overrun(rsmu);
	if (err)
		return err;

	return get_tdc_meas(rsmu, offset_ns);
}

static int rsmu_fc3_get_dpll_state(struct rsmu_cdev *rsmu,
				   u8 dpll,
				   u8 *state)
{
	u16 reg_addr;
	u8 reg;
	int err;
	u8 devid = DEVID(rsmu);

	if (dpll > IDTFC3_FW_MACRO(devid, VFC3A, MAX_DPLL_INDEX))
		return -EINVAL;

	reg_addr = IDTFC3_FW_REG(devid, VFC3A, DPLL_STS);
	if (devid == VFC3A)
		(void)dpll;
	else
		reg_addr += dpll * 0x100;

	err = regmap_bulk_read(rsmu->regmap, reg_addr, &reg, sizeof(reg));
	if (err)
		return err;

	reg = (reg & DPLL_STATE_STS_MASK) >> DPLL_STATE_STS_SHIFT;

	switch (reg) {
	case DPLL_STATE_FREERUN:
	case DPLL_STATE_WRITE_FREQUENCY:
		*state = E_SRVLOUNQUALIFIEDSTATE;
		break;
	case DPLL_STATE_ACQUIRE:
	case DPLL_STATE_HITLESS_SWITCH:
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

static int rsmu_fc3_get_clock_index(struct rsmu_cdev *rsmu,
				    u8 dpll,
				    s8 *clock_index)
{
	u16 reg_addr;
	u8 dpll_sts_reg;
	u32 ref_sel_cnfg_reg;
	enum dpll_state dpll_state_sts;
	u8 ref_index;
	int err;
	u8 devid = DEVID(rsmu);

	*clock_index = -1;

	if (dpll > IDTFC3_FW_MACRO(devid, VFC3A, MAX_DPLL_INDEX))
		return -EINVAL;

	reg_addr = IDTFC3_FW_REG(devid, VFC3A, DPLL_STS);
	if (devid == VFC3A)
		(void)dpll;
	else
		reg_addr += dpll * 0x100;

	err = regmap_bulk_read(rsmu->regmap, reg_addr, &dpll_sts_reg, sizeof(dpll_sts_reg));
	if (err)
		return err;

	dpll_state_sts = (enum dpll_state)((dpll_sts_reg & DPLL_STATE_STS_MASK) >>
					   DPLL_STATE_STS_SHIFT);
	if ((dpll_state_sts == DPLL_STATE_LOCKED) || (dpll_state_sts == DPLL_STATE_ACQUIRE) ||
	    (dpll_state_sts == DPLL_STATE_HITLESS_SWITCH)) {
		ref_index = (dpll_sts_reg & DPLL_REF_SEL_STS_MASK) >> DPLL_REF_SEL_STS_SHIFT;

		reg_addr = IDTFC3_FW_REG(devid, VFC3A, REF_SEL_CNFG);
		err = regmap_bulk_read(rsmu->regmap, reg_addr, &ref_sel_cnfg_reg,
				       sizeof(ref_sel_cnfg_reg));
		if (err)
			return err;

		*clock_index = (ref_sel_cnfg_reg >> (REF_MUX_SEL_SHIFT * ref_index)) &
						    REF_MUX_SEL_MASK;
	}

	return err;
}

static int rsmu_fc3_set_clock_priorities(struct rsmu_cdev *rsmu, u8 dpll, u8 number_entries,
					 struct rsmu_priority_entry *priority_entry)
{
	int priority_index;
	u16 reg = 0;
	u8 clock_index;
	u8 ref_index;
	u8 priority;
	u8 buf[2] = {0};
	int err;
	u16 reg_addr;
	u8 devid = DEVID(rsmu);

	if (dpll > IDTFC3_FW_MACRO(devid, VFC3A, MAX_DPLL_INDEX))
		return -EINVAL;

	reg_addr = DPLL_REF_PRIORITY_CNFG;
	if (devid == VFC3A)
		(void)dpll;
	else
		reg_addr += dpll * 0x100;

	/* MAX_NUM_REF_PRIORITY is maximum number of priorities */
	if (number_entries > MAX_NUM_REF_PRIORITY)
		return -EINVAL;

	/*
	 * Disable clock priorities initially and then enable as needed in loop
	 * (dpll_refx_priority_disable[3:0])
	 */
	reg |= DPLL_REFX_PRIORITY_DISABLE_MASK;

	for (priority_index = 0; priority_index < number_entries; priority_index++) {
		clock_index = priority_entry->clock_index;
		priority = priority_entry->priority;

		if ((clock_index > MAX_INPUT_CLOCK_INDEX) || (priority >= MAX_NUM_REF_PRIORITY))
			return -EINVAL;

		ref_index = clock_index_to_ref_index(rsmu, clock_index);

		/* Set clock priority disable bit to zero to enable it */
		switch (ref_index) {
		case 0:
			reg = ((reg & (~DPLL_REF0_PRIORITY_ENABLE_AND_SET_MASK)) |
			       (priority << DPLL_REF0_PRIORITY_SHIFT));
			break;
		case 1:
			reg = ((reg & (~DPLL_REF1_PRIORITY_ENABLE_AND_SET_MASK)) |
			       (priority << DPLL_REF1_PRIORITY_SHIFT));
			break;
		case 2:
			reg = ((reg & (~DPLL_REF2_PRIORITY_ENABLE_AND_SET_MASK)) |
			       (priority << DPLL_REF2_PRIORITY_SHIFT));
			break;
		case 3:
			reg = ((reg & (~DPLL_REF3_PRIORITY_ENABLE_AND_SET_MASK)) |
			       (priority << DPLL_REF3_PRIORITY_SHIFT));
			break;
		default:
			return -EINVAL;
		}

		priority_entry++;
	}

	put_unaligned_le16(reg, buf);

	err = regmap_bulk_write(rsmu->regmap, reg_addr, &buf, sizeof(buf));

	if (err)
		dev_err(rsmu->dev, "err\n");

	return err;
}

static int rsmu_fc3_get_reference_monitor_status(struct rsmu_cdev *rsmu, u8 clock_index,
					struct rsmu_reference_monitor_status_alarms *alarms)
{
	u8 ref_index;
	u16 losmon_sts_reg_addr;
	u16 freqmon_sts_reg_addr;
	u8 los_reg;
	u8 buf[4] = {0};
	u32 freq_reg;
	int err;
	u8 devid = DEVID(rsmu);

	if (clock_index > MAX_INPUT_CLOCK_INDEX)
		return -EINVAL;

	ref_index = clock_index_to_ref_index(rsmu, clock_index);
	if (ref_index > MAX_REF_INDEX)
		return -EINVAL;

	err = get_losmon_sts_reg_offset(devid, ref_index, &losmon_sts_reg_addr);
	if (err)
		return err;

	err = get_freqmon_sts_reg_offset(devid, ref_index, &freqmon_sts_reg_addr);
	if (err)
		return err;

	err = regmap_bulk_read(rsmu->regmap, losmon_sts_reg_addr,
			       &los_reg, sizeof(los_reg));
	if (err)
		return err;

	alarms->los = los_reg & LOS_STS_MASK;

	alarms->no_activity = 0;

	err = regmap_bulk_read(rsmu->regmap, freqmon_sts_reg_addr,
			       &buf, sizeof(buf));
	if (err)
		return err;

	freq_reg = get_unaligned_le32(buf);
	alarms->frequency_offset_limit = (freq_reg >> FREQ_FAIL_STS_SHIFT) & 1;

	return err;
}

static int rsmu_fc3_get_tdc_meas(struct rsmu_cdev *rsmu, bool continuous, s64 *offset_ns)
{
	int err;
	u8 mode = ONE_SHOT;

	if (DEVID(rsmu) == VFC3A)
		return -EOPNOTSUPP;

	if (continuous)
		mode = CONTINUOUS;

	err = set_tdc_meas_mode(rsmu, mode);
	if (err)
		return err;

	if (continuous)
		err = get_tdc_meas_continuous(rsmu, offset_ns);
	else
		err = get_tdc_meas_one_shot(rsmu, offset_ns);

	return err;
}

static int rsmu_fc3_init(struct rsmu_cdev *rsmu, char fwname[FW_NAME_LEN_MAX])
{
	struct rsmufc3 *ddata;
	int err;

	ddata = devm_kzalloc(rsmu->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;
	rsmu->ddata = ddata;

	err = read_device_id(rsmu);
	if (err) {
		dev_err(rsmu->dev, "reading device id failed with %d", err);
		return err;
	}

	err = load_firmware(rsmu, fwname);
	if (err)
		dev_warn(rsmu->dev, "loading firmware failed with %d", err);

	return 0;
}

struct rsmu_ops fc3_ops = {
	.type = RSMU_FC3,
	.device_init = rsmu_fc3_init,
	.set_combomode = NULL,
	.get_dpll_state = rsmu_fc3_get_dpll_state,
	.get_dpll_ffo = NULL,
	.set_holdover_mode = NULL,
	.set_output_tdc_go = NULL,
	.get_clock_index = rsmu_fc3_get_clock_index,
	.set_clock_priorities = rsmu_fc3_set_clock_priorities,
	.get_reference_monitor_status = rsmu_fc3_get_reference_monitor_status,
	.get_tdc_meas = rsmu_fc3_get_tdc_meas
};

