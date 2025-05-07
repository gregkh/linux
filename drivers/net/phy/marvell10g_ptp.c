// SPDX-License-Identifier: GPL-2.0+
/*
 * Marvell 10G 88x3310 PHY driver PTP support
 *
 * There are four 32-bit TOD registers (fractional nanoseconds, nanoseconds,
 * seconds low and seconds high). Each 32-bit register write requires two MDIO
 * operations and each read requires four MDIO operations. MDIO access is slow,
 * therefore this implementation protects against concurrent access to the TOD
 * registers by using mutex instead of spinlock to avoid potential RCU stalls
 * when the spinlock would not be available for a long time.
 */
#include <linux/phy.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/mutex.h>

#define MV_EXTTS_PERIOD_MS 95

enum {
	/* PMA/PMD MMD Registers */
	MV_PMA_XG_EXT_STATUS		= 0xc001,
	MV_PMA_XG_EXT_STATUS_PTP_UNSUPP = BIT(12),

	/* Vendor2 MMD registers */
	MV_V2_SLC_CFG_GEN		= 0x8000,
	MV_V2_SLC_CFG_GEN_DEF_VAL	= 0x7e50000f,
	MV_V2_SLC_CFG_GEN_WMC_ANEG_EN	= BIT(23),
	MV_V2_SLC_CFG_GEN_SMC_ANEG_EN	= BIT(24),
	MV_V2_MODE_CFG 			= 0xf000,
	MV_V2_MODE_CFG_M_UNIT_PWRUP 	= BIT(12),

	/* Vendor2 MMD PTP registers */
	MV_V2_INDIRECT_READ_ADDR 	= 0x97fd,
	MV_V2_INDIRECT_READ_DATA_LOW 	= 0x97fe,
	MV_V2_INDIRECT_READ_DATA_HIGH 	= 0x97ff,

	MV_V2_PTP_TOD_LOAD_NSEC_FRAC 	= 0xbc2a,
	MV_V2_PTP_TOD_LOAD_NSEC 	= 0xbc2c,
	MV_V2_PTP_TOD_LOAD_SEC_LOW 	= 0xbc2e,
	MV_V2_PTP_TOD_LOAD_SEC_HIGH 	= 0xbc30,
	MV_V2_PTP_TOD_CAP0_NSEC_FRAC 	= 0xbc32,
	MV_V2_PTP_TOD_CAP0_NSEC 	= 0xbc34,
	MV_V2_PTP_TOD_CAP0_SEC_LOW 	= 0xbc36,
	MV_V2_PTP_TOD_CAP0_SEC_HIGH 	= 0xbc38,

	MV_V2_PTP_TOD_CAP_CFG 		= 0xbc42,
	MV_V2_PTP_TOD_CAP_CFG_VAL0 	= BIT(0),
	MV_V2_PTP_TOD_CAP_CFG_VAL1 	= BIT(1),
	MV_V2_PTP_TOD_FUNC_CFG 		= 0xbc46,
	MV_V2_PTP_TOD_FUNC_CFG_TRIG 	= BIT(28),
	MV_V2_PTP_TOD_FUNC_CFG_UPDATE 	= 0,
	MV_V2_PTP_TOD_FUNC_CFG_INCR 	= BIT(30),
	MV_V2_PTP_TOD_FUNC_CFG_DECR 	= BIT(31),
	MV_V2_PTP_TOD_FUNC_CFG_CAPTURE 	= BIT(31) | BIT(30),
};

struct mv3310_ptp_priv {
	struct phy_device *phydev;
	struct ptp_clock_info caps;
	struct ptp_clock *clock;
	struct mutex lock; /* Protects against concurrent MDIO register access */
	bool extts_enabled;
};

struct mv3310_ptp_priv *mv3310_ptp_probe(struct phy_device *phydev);
int mv3310_ptp_power_up(struct phy_device *phydev);
int mv3310_ptp_power_down(struct phy_device *phydev);

static int mv3310_read_ptp_reg(struct phy_device *phydev, u32 regnum,
			       u32 *regval);
static int mv3310_write_ptp_reg(struct phy_device *phydev, u32 regnum,
				u32 regval);

static int mv3310_adjfine(struct ptp_clock_info *ptp, long scaled_ppm);
static int mv3310_adjphase(struct ptp_clock_info *ptp, s32 phase);
static int mv3310_adjtime(struct ptp_clock_info *ptp, s64 delta);
static int mv3310_gettimex64(struct ptp_clock_info *ptp, struct timespec64 *ts,
			     struct ptp_system_timestamp *sts);
static int mv3310_settime64(struct ptp_clock_info *ptp,
			    const struct timespec64 *ts);
static int mv3310_enable(struct ptp_clock_info *ptp,
			 struct ptp_clock_request *request, int on);
static int mv3310_verify(struct ptp_clock_info *ptp, unsigned int pin,
			 enum ptp_pin_function func, unsigned int chan);
static long mv3310_do_aux_work(struct ptp_clock_info *ptp);

static bool mv3310_is_ptp_supported(struct phy_device *phydev)
{
	int ret;

	ret = phy_read_mmd(phydev, MDIO_MMD_PMAPMD, MV_PMA_XG_EXT_STATUS);
	if (ret < 0)
		return false;

	return !(ret & MV_PMA_XG_EXT_STATUS_PTP_UNSUPP);
}

struct mv3310_ptp_priv *mv3310_ptp_probe(struct phy_device *phydev)
{
	struct mv3310_ptp_priv *priv;

	if (!mv3310_is_ptp_supported(phydev)) {
		dev_info(&phydev->mdio.dev, "PTP is not present in this device\n");
		return NULL;
	}

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return NULL;

	priv->phydev = phydev;
	mutex_init(&priv->lock);
	priv->extts_enabled = false;

	priv->caps.owner = THIS_MODULE;
	strscpy(priv->caps.name, "mv10g-phy-phc", sizeof(priv->caps.name));
	priv->caps.max_adj = 0;
	priv->caps.n_alarm = 0;
	priv->caps.n_ext_ts = 1;
	priv->caps.n_per_out = 0;
	priv->caps.n_pins = 0;
	priv->caps.pps = 0;
	priv->caps.pin_config = NULL;
	priv->caps.adjfine = mv3310_adjfine;
	priv->caps.adjphase = mv3310_adjphase;
	priv->caps.adjtime = mv3310_adjtime;
	priv->caps.gettimex64 = mv3310_gettimex64;
	priv->caps.settime64 = mv3310_settime64;
	priv->caps.enable = mv3310_enable;
	priv->caps.verify = mv3310_verify;
	priv->caps.do_aux_work = mv3310_do_aux_work;
	/* This is set to NULL instead of EOPNOTSUPP, simply defining it will
	   present "has cross timestamping support" in capabilities. */
	priv->caps.getcrosststamp = NULL;

	priv->clock = ptp_clock_register(&priv->caps, &phydev->mdio.dev);
	if (IS_ERR(priv->clock)) {
		dev_err(&phydev->mdio.dev, "failed to register PTP clock\n");
		devm_kfree(&phydev->mdio.dev, priv);
		return NULL;
	}

	return priv;
}

int mv3310_ptp_power_up(struct phy_device *phydev)
{
	int ret;

	if (!mv3310_is_ptp_supported(phydev))
		return 0;

	/* Enable M unit used for PTP */
	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, MV_V2_MODE_CFG,
			       MV_V2_MODE_CFG_M_UNIT_PWRUP);
	if (ret < 0)
		return ret;

	/* PHY Errata section 4.4: after the M unit is powered up
	   auto-negotiation is disabled by default. Enable:
	   * WMC - auto negotiation for wire mac
	   * SMC - auto negotiation for system mac */
	ret = mv3310_write_ptp_reg(phydev, MV_V2_SLC_CFG_GEN,
				   MV_V2_SLC_CFG_GEN_DEF_VAL |
					   MV_V2_SLC_CFG_GEN_WMC_ANEG_EN |
					   MV_V2_SLC_CFG_GEN_SMC_ANEG_EN);
	if (ret < 0)
		return ret;

	return 0;
}

int mv3310_ptp_power_down(struct phy_device *phydev)
{
	if (!mv3310_is_ptp_supported(phydev))
		return 0;

	return phy_clear_bits_mmd(phydev, MDIO_MMD_VEND2, MV_V2_MODE_CFG,
				  MV_V2_MODE_CFG_M_UNIT_PWRUP);
}

static int mv3310_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	return -EOPNOTSUPP;
}

static int mv3310_adjphase(struct ptp_clock_info *ptp, s32 phase)
{
	return -EOPNOTSUPP;
}

static int mv3310_verify(struct ptp_clock_info *ptp, unsigned int pin,
			 enum ptp_pin_function func, unsigned int chan)
{
	return -EOPNOTSUPP;
}

static int mv3310_read_ptp_reg(struct phy_device *phydev, u32 regnum,
			       u32 *regval)
{
	int ret;

	/* Read register address */
	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2, regnum);
	if (ret < 0)
		return ret;

	/* Read that Indirect_read_address gives requested address */
	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2, MV_V2_INDIRECT_READ_ADDR);
	if (ret < 0)
		return ret;
	if (ret != regnum) {
		pr_err("Indirect read address mismatch: %04x != %04x\n", ret,
		       regnum);
		return -EINVAL;
	}

	/* Read Indirect_read_data_low provides lower 16-bits (15:0) of data */
	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2,
			   MV_V2_INDIRECT_READ_DATA_LOW);
	if (ret < 0)
		return ret;
	*regval = ret & 0xffff;

	/* Read Indirect_read_data_high provides upper 16-bits (31:16) of data */
	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2,
			   MV_V2_INDIRECT_READ_DATA_HIGH);
	if (ret < 0)
		return ret;
	*regval += ((ret & 0xffff) << 16);

	return 0;
}

static int mv3310_write_ptp_reg(struct phy_device *phydev, u32 regnum,
				u32 regval)
{
	int ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, regnum, regval);
	if (ret < 0)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, regnum + 1, regval >> 16U);
	if (ret < 0)
		return ret;

	return 0;
}

static int mv3310_trigger_ptp_op(struct phy_device *phydev, int op)
{
	int ret;

	ret = mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_FUNC_CFG,
				   MV_V2_PTP_TOD_FUNC_CFG_TRIG | op);
	if (ret < 0)
		return ret;

	if (op != MV_V2_PTP_TOD_FUNC_CFG_CAPTURE) {
		/* Restore capture mode */
		return mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_FUNC_CFG,
					    MV_V2_PTP_TOD_FUNC_CFG_CAPTURE);
	}

	return 0;
}

static int mv3310_read_tod(struct phy_device *phydev, struct timespec64 *ts,
			   struct ptp_system_timestamp *sts)
{
	int ret = 0;
	u32 nsec_frac = 0, nsec = 0, sec_low = 0, sec_high = 0;

	ptp_read_system_prets(sts);
	ret |= mv3310_read_ptp_reg(phydev, MV_V2_PTP_TOD_CAP0_NSEC_FRAC,
				   &nsec_frac);
	ptp_read_system_postts(sts);
	ret |= mv3310_read_ptp_reg(phydev, MV_V2_PTP_TOD_CAP0_NSEC, &nsec);
	ret |= mv3310_read_ptp_reg(phydev, MV_V2_PTP_TOD_CAP0_SEC_LOW,
				   &sec_low);
	ret |= mv3310_read_ptp_reg(phydev, MV_V2_PTP_TOD_CAP0_SEC_HIGH,
				   &sec_high);

	if (ret < 0)
		return -EIO;

	/* check if nsec should be rounded up */
	if (nsec_frac > (U32_MAX / 2))
		nsec++;
	ts->tv_sec = ((u64)sec_high << 32U) | sec_low;
	ts->tv_nsec = nsec;

	return 0;
}

static int mv3310_write_tod(struct phy_device *phydev,
			    const struct timespec64 *ts)
{
	int ret = 0;
	u32 nsec = lower_32_bits(ts->tv_nsec);
	u32 sec_low = lower_32_bits(ts->tv_sec);
	u32 sec_high = upper_32_bits(ts->tv_sec) & 0xffff;

	ret |= mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_LOAD_NSEC_FRAC, 0);
	ret |= mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_LOAD_NSEC, nsec);
	ret |= mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_LOAD_SEC_LOW,
				    sec_low);
	ret |= mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_LOAD_SEC_HIGH,
				    sec_high);

	if (ret < 0)
		return -EIO;

	return 0;
}

static int mv3310_getppstime(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	int ret;
	u32 cap_cfg = 0;

	struct mv3310_ptp_priv *priv =
		container_of(ptp, struct mv3310_ptp_priv, caps);
	struct phy_device *phydev = priv->phydev;

	mutex_lock(&priv->lock);
	/* Check if TOD@pps is available */
	ret = mv3310_read_ptp_reg(phydev, MV_V2_PTP_TOD_CAP_CFG, &cap_cfg);
	if (ret < 0)
		goto unlock_out;
	if (!(cap_cfg & MV_V2_PTP_TOD_CAP_CFG_VAL0)) {
		ret = -EAGAIN;
		goto unlock_out;
	}

	ret = mv3310_read_tod(phydev, ts, NULL);
	if (ret < 0)
		goto unlock_out;

	/* Finished reading capture, reset */
	ret = mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_CAP_CFG, 0);

unlock_out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mv3310_gettimex64(struct ptp_clock_info *ptp, struct timespec64 *ts,
			     struct ptp_system_timestamp *sts)
{
	int ret;
	struct mv3310_ptp_priv *priv =
		container_of(ptp, struct mv3310_ptp_priv, caps);
	struct phy_device *phydev = priv->phydev;

	mutex_lock(&priv->lock);
	/* Clear existing TOD Capture Values and trigger new capture.
	   In the unlikely event that a pulse-in trigger will capture the TOD
	   to TOD_CAP0 and this CPU trigger will capture it to TOD_CAP1, we are
	   still reading from TOD_CAP0 as they will be almost equal. */
	ret = mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_CAP_CFG, 0);
	if (ret < 0)
		goto unlock_out;

	ret = mv3310_trigger_ptp_op(phydev, MV_V2_PTP_TOD_FUNC_CFG_CAPTURE);
	if (ret < 0)
		goto unlock_out;

	/* Read capture */
	ret = mv3310_read_tod(phydev, ts, sts);
	if (ret < 0)
		goto unlock_out;

	/* Finished reading capture, reset */
	ret = mv3310_write_ptp_reg(phydev, MV_V2_PTP_TOD_CAP_CFG, 0);

unlock_out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mv3310_settime64(struct ptp_clock_info *ptp,
			    const struct timespec64 *ts)
{
	int ret;
	struct mv3310_ptp_priv *priv =
		container_of(ptp, struct mv3310_ptp_priv, caps);
	struct phy_device *phydev = priv->phydev;

	mutex_lock(&priv->lock);
	/* Load the new timestamp */
	ret = mv3310_write_tod(phydev, ts);
	if (ret < 0)
		goto unlock_out;

	/* Trigger update */
	ret = mv3310_trigger_ptp_op(phydev, MV_V2_PTP_TOD_FUNC_CFG_UPDATE);

unlock_out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mv3310_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	int ret;
	const struct timespec64 ts = ns_to_timespec64(abs(delta));
	struct mv3310_ptp_priv *priv =
		container_of(ptp, struct mv3310_ptp_priv, caps);
	struct phy_device *phydev = priv->phydev;

	if (delta == 0)
		return 0;

	mutex_lock(&priv->lock);
	/* Load the new timestamp */
	ret = mv3310_write_tod(phydev, &ts);
	if (ret < 0)
		goto unlock_out;

	/* Trigger update */
	ret = mv3310_trigger_ptp_op(phydev,
				    delta < 0 ? MV_V2_PTP_TOD_FUNC_CFG_DECR :
						MV_V2_PTP_TOD_FUNC_CFG_INCR);
unlock_out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mv3310_enable(struct ptp_clock_info *ptp,
			 struct ptp_clock_request *request, int on)
{
	int ret = 0;
	bool enable = on != 0;
	struct mv3310_ptp_priv *priv =
		container_of(ptp, struct mv3310_ptp_priv, caps);

	switch (request->type) {
	case PTP_CLK_REQ_EXTTS:
		if (enable)
			if (!priv->extts_enabled)
				ptp_schedule_worker(priv->clock, 0);
			else
				ret = -EBUSY;
		else
			if (priv->extts_enabled)
				ptp_cancel_worker_sync(priv->clock);

		priv->extts_enabled = enable;
		break;

	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static long mv3310_do_aux_work(struct ptp_clock_info *ptp)
{
	struct ptp_clock_event event;
	struct timespec64 ts;
	struct mv3310_ptp_priv *priv =
		container_of(ptp, struct mv3310_ptp_priv, caps);

	if (mv3310_getppstime(ptp, &ts) == 0) {
		event.type = PTP_CLOCK_EXTTS;
		event.index = 0; /* We only have one channel */
		event.timestamp = timespec64_to_ns(&ts);
		ptp_clock_event(priv->clock, &event);
	}

	return msecs_to_jiffies(MV_EXTTS_PERIOD_MS);
}
