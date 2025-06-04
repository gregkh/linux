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
#include <linux/sched.h>
#include <linux/firmware.h>
#include <linux/netdevice.h>
#include <linux/net_tstamp.h>

#define MV_EXTTS_PERIOD_MS 95
#define PAM_ADDR(base, offset) ((base) + ((offset) * 2))

enum {
	/* PMA/PMD MMD Registers */
	MV_PMA_XG_EXT_STATUS		= 0xc001,
	MV_PMA_XG_EXT_STATUS_PTP_UNSUPP = BIT(12),

	/* Vendor2 MMD registers */
	MV_V2_SLC_CFG_GEN		= 0x8000,
	MV_V2_SLC_CFG_GEN_EGR_SF_EN	= BIT(2),
	MV_V2_SLC_CFG_GEN_WMC_ADD_CRC	= BIT(8),
	MV_V2_SLC_CFG_GEN_SMC_ADD_CRC	= BIT(9),
	MV_V2_SLC_CFG_GEN_WMC_STRIP_CRC	= BIT(10),
	MV_V2_SLC_CFG_GEN_SMC_STRIP_CRC	= BIT(11),
	MV_V2_SLC_CFG_GEN_WMC_ANEG_EN	= BIT(23),
	MV_V2_SLC_CFG_GEN_SMC_ANEG_EN	= BIT(24),
	MV_V2_MODE_CFG 			= 0xf000,
	MV_V2_MODE_CFG_M_UNIT_PWRUP 	= BIT(12),

	/* Vendor2 MMD PTP registers */
	MV_V2_INDIRECT_READ_ADDR 	= 0x97fd,
	MV_V2_INDIRECT_READ_DATA_LOW 	= 0x97fe,
	MV_V2_INDIRECT_READ_DATA_HIGH 	= 0x97ff,

	MV_V2_PTP_PR_EG_PAM_BASE	= 0xa000,
	MV_V2_PTP_PR_IG_PAM_BASE	= 0xa800,
	MV_V2_PTP_UR_EG_PAM_BASE	= 0xa080,
	MV_V2_PTP_UR_IG_PAM_BASE	= 0xa880,

	MV_V2_PTP_CFG_GEN_EG		= 0xa100,
	MV_V2_PTP_CFG_GEN_IG		= 0xa900,
	MV_V2_PTP_CFG_GEN_H_ENABLE	= BIT(0),
	MV_V2_PTP_CFG_IG_MODE		= 0xa938,
	MV_V2_PTP_CFG_IG_MODE_ENABLE	= BIT(10),

	MV_V2_PTP_LUT_KEY_EG_BASE	= 0xa700,
	MV_V2_PTP_LUT_KEY_IG_BASE	= 0xaf00,
	MV_V2_PTP_LUT_ACTION_EG_BASE	= 0xa600,
	MV_V2_PTP_LUT_ACTION_IG_BASE	= 0xae00,

	MV_V2_PTP_PARSER_EG_UDATA	= 0xa200,
	MV_V2_PTP_UPDATER_EG_UDATA	= 0xa400,
	MV_V2_PTP_PARSER_IG_UDATA	= 0xaa00,
	MV_V2_PTP_UPDATER_IG_UDATA	= 0xac00,
	MV_V2_PTP_UDATA_EMPTY		= 0x30000,

	MV_V2_PTP_EG_STATS_BASE		= 0xa180,
	MV_V2_PTP_IG_STATS_BASE		= 0xa980,

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
	struct mii_timestamper mii_ts;
	bool extts_enabled;
};

struct mv3310_ptp_counter {
	u32 regnum;
	const char string[ETH_GSTRING_LEN];
};

static const struct mv3310_ptp_counter mv3310_ptp_stats[] = {
	{ MV_V2_PTP_EG_STATS_BASE + 0x0c, "tx_ptp_drop" },
	{ MV_V2_PTP_EG_STATS_BASE + 0x0e, "tx_ptp_update_res" },
	{ MV_V2_PTP_EG_STATS_BASE + 0x18, "tx_ptp_v2" },
	{ MV_V2_PTP_EG_STATS_BASE + 0x28, "tx_ptp_v1" },
	{ MV_V2_PTP_EG_STATS_BASE + 0x36, "tx_ptp_parser_err" },
	{ MV_V2_PTP_EG_STATS_BASE + 0x1a, "tx_udp" },
	{ MV_V2_PTP_EG_STATS_BASE + 0x1c, "tx_ipv4" },
	{ MV_V2_PTP_EG_STATS_BASE + 0x1e, "tx_ipv6" },
	{ MV_V2_PTP_EG_STATS_BASE + 0x2a, "tx_dot1q" },
	{ MV_V2_PTP_EG_STATS_BASE + 0x2c, "tx_stag" },

	{ MV_V2_PTP_IG_STATS_BASE + 0x0c, "rx_ptp_drop" },
	{ MV_V2_PTP_IG_STATS_BASE + 0x10, "rx_ptp_ini_piggyback" },
	{ MV_V2_PTP_IG_STATS_BASE + 0x18, "rx_ptp_v2" },
	{ MV_V2_PTP_IG_STATS_BASE + 0x28, "rx_ptp_v1" },
	{ MV_V2_PTP_IG_STATS_BASE + 0x36, "rx_ptp_parser_err" },
	{ MV_V2_PTP_IG_STATS_BASE + 0x1a, "rx_udp" },
	{ MV_V2_PTP_IG_STATS_BASE + 0x1c, "rx_ipv4" },
	{ MV_V2_PTP_IG_STATS_BASE + 0x1e, "rx_ipv6" },
	{ MV_V2_PTP_IG_STATS_BASE + 0x2a, "rx_dot1q" },
	{ MV_V2_PTP_IG_STATS_BASE + 0x2c, "rx_stag" },
};

/* Public functions */
struct mv3310_ptp_priv *mv3310_ptp_probe(struct phy_device *phydev);
int mv3310_ptp_power_up(struct mv3310_ptp_priv *priv);
int mv3310_ptp_power_down(struct mv3310_ptp_priv *priv);
int mv3310_ptp_start(struct mv3310_ptp_priv *priv);
/* Get statistics from the PHY using ethtool */
int mv3310_ptp_get_sset_count(struct phy_device *dev);
void mv3310_ptp_get_strings(struct phy_device *dev, u8 *data);
void mv3310_ptp_get_stats(struct phy_device *dev, struct ethtool_stats *stats,
			  u64 *data, struct mv3310_ptp_priv *priv);

/* Helper functions */
static int mv3310_read_ptp_reg(struct phy_device *phydev, u32 regnum,
			       u32 *regval);
static int mv3310_write_ptp_reg(struct phy_device *phydev, u32 regnum,
				u32 regval);
static int mv3310_write_ptp_lut_reg(struct phy_device *phydev, u32 regnum,
				    u32 regval);
static int mv3310_set_ptp_reg_bits(struct phy_device *phydev, u32 regnum,
				   u32 bits);
static int mv3310_clear_ptp_reg_bits(struct phy_device *phydev, u32 regnum,
				     u32 bits);

/* TOD functions */
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

/* PTP functions */
static int mv3310_ptp_set_pam(struct mv3310_ptp_priv *priv);
static int mv3310_ptp_set_udata(struct mv3310_ptp_priv *priv, const u8 *udata,
				size_t udata_len, u32 baseaddr);
static int mv3310_ptp_load_ucode(struct mv3310_ptp_priv *priv);
static int mv3310_ptp_check_ucode(struct mv3310_ptp_priv *priv);
static int mv3310_ptp_set_lut(struct phy_device *phydev);
static int mv3310_ptp_set_lut_actions(struct phy_device *phydev, bool enable_tx,
				      bool enable_rx);

/* Timestamping callbacks */
static int mv3310_ts_hwtstamp(struct mii_timestamper *mii_ts,
			      struct kernel_hwtstamp_config *cfg,
			      struct netlink_ext_ack *extack);
static int mv3310_ts_info(struct mii_timestamper *mii_ts,
			  struct kernel_ethtool_ts_info *ts_info);

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

	/* Setup timestamping */
	priv->mii_ts.hwtstamp = mv3310_ts_hwtstamp;
	priv->mii_ts.ts_info = mv3310_ts_info;
	priv->mii_ts.device = &phydev->mdio.dev;
	priv->phydev->mii_ts = &priv->mii_ts;

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

int mv3310_ptp_power_up(struct mv3310_ptp_priv *priv)
{
	int ret;
	struct phy_device *phydev = priv->phydev;

	if (!mv3310_is_ptp_supported(phydev))
		return 0;

	mutex_lock(&priv->lock);
	/* Enable M unit used for PTP */
	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, MV_V2_MODE_CFG,
			       MV_V2_MODE_CFG_M_UNIT_PWRUP);
	if (ret < 0)
		goto unlock_out;

	/* PHY Errata section 4.4: after the M unit is powered up
	   auto-negotiation is disabled by default. Enable:
	   * WMC - auto negotiation for wire mac
	   * SMC - auto negotiation for system mac */
	/* LinkCrypt MAC Configuration: enable remove crc at rx and add back to tx */
	ret = mv3310_set_ptp_reg_bits(phydev, MV_V2_SLC_CFG_GEN,
				      MV_V2_SLC_CFG_GEN_WMC_ANEG_EN |
					      MV_V2_SLC_CFG_GEN_SMC_ANEG_EN |
					      MV_V2_SLC_CFG_GEN_WMC_ADD_CRC |
					      MV_V2_SLC_CFG_GEN_SMC_ADD_CRC |
					      MV_V2_SLC_CFG_GEN_WMC_STRIP_CRC |
					      MV_V2_SLC_CFG_GEN_SMC_STRIP_CRC);
	/* Disable store-and-forward mode for egress drop FIFO. Without this
	   setting there are time error spikes of up to 1200ns when performing
	   1588TC accuracy measurements. */
	ret |= mv3310_clear_ptp_reg_bits(phydev, MV_V2_SLC_CFG_GEN,
					 MV_V2_SLC_CFG_GEN_EGR_SF_EN);
unlock_out:
	mutex_unlock(&priv->lock);
	return ret;
}

int mv3310_ptp_power_down(struct mv3310_ptp_priv *priv)
{
	if (!mv3310_is_ptp_supported(priv->phydev))
		return 0;

	return phy_clear_bits_mmd(priv->phydev, MDIO_MMD_VEND2, MV_V2_MODE_CFG,
				  MV_V2_MODE_CFG_M_UNIT_PWRUP);
}

int mv3310_ptp_start(struct mv3310_ptp_priv *priv)
{
	int ret;
	struct phy_device *phydev = priv->phydev;

	if (!mv3310_is_ptp_supported(phydev))
		return 0;

	ret = mv3310_ptp_set_pam(priv);
	if (ret < 0) {
		dev_err(&phydev->mdio.dev, "failed to set PTP PAM: %d\n", ret);
		return ret;
	}

	ret = mv3310_ptp_check_ucode(priv);
	if (ret < 0) {
		dev_err(&phydev->mdio.dev, "failed to load PTP microcode: %d\n",
			ret);
		return ret;
	}

	mutex_lock(&priv->lock);
	ret = 0;
	ret |= mv3310_set_ptp_reg_bits(phydev, MV_V2_PTP_CFG_GEN_EG,
				       MV_V2_PTP_CFG_GEN_H_ENABLE);
	ret |= mv3310_set_ptp_reg_bits(phydev, MV_V2_PTP_CFG_GEN_IG,
				       MV_V2_PTP_CFG_GEN_H_ENABLE);
	ret |= mv3310_set_ptp_reg_bits(phydev, MV_V2_PTP_CFG_IG_MODE,
				       MV_V2_PTP_CFG_IG_MODE_ENABLE);
	if (ret < 0) {
		dev_err(&phydev->mdio.dev, "failed to enable PTP core: %d\n",
			ret);
		goto unlock_out;
	}

	ret = mv3310_ptp_set_lut(phydev);
	if (ret < 0)
		dev_err(&phydev->mdio.dev, "failed to set PTP LUT: %d\n", ret);

unlock_out:
	mutex_unlock(&priv->lock);
	return ret;
}

int mv3310_ptp_get_sset_count(struct phy_device *dev)
{
	if (!mv3310_is_ptp_supported(dev))
		return 0;

	return ARRAY_SIZE(mv3310_ptp_stats);
}

void mv3310_ptp_get_strings(struct phy_device *dev, u8 *data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mv3310_ptp_stats); i++) {
		strscpy(data, mv3310_ptp_stats[i].string, ETH_GSTRING_LEN);
		data += ETH_GSTRING_LEN;
	}
}

void mv3310_ptp_get_stats(struct phy_device *dev, struct ethtool_stats *stats,
			  u64 *data, struct mv3310_ptp_priv *priv)
{
	int i, ret;
	u32 regval;

	mutex_lock(&priv->lock);

	for (i = 0; i < ARRAY_SIZE(mv3310_ptp_stats); i++) {
		ret = mv3310_read_ptp_reg(dev, mv3310_ptp_stats[i].regnum,
					  &regval);
		if (ret < 0) {
			dev_err(&dev->mdio.dev,
				"failed to read PTP stat %s: %d\n",
				mv3310_ptp_stats[i].string, ret);
			data[i] = 0;
		} else {
			data[i] = regval;
		}
	}

	mutex_unlock(&priv->lock);
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
		dev_err(&phydev->mdio.dev,
			"Indirect read address mismatch: %04x != %04x\n", ret,
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

/* The Lookup Action/Match registers need 96-bit write operation */
static int mv3310_write_ptp_lut_reg(struct phy_device *phydev, u32 regnum,
				    u32 regval)
{
	int ret;

	ret = mv3310_write_ptp_reg(phydev, regnum, regval);
	if (ret < 0)
		return ret;

	/* The following writes are mandatory (although registers are already 0) */
	ret = mv3310_write_ptp_reg(phydev, regnum + 2, 0);
	if (ret < 0)
		return ret;

	ret = mv3310_write_ptp_reg(phydev, regnum + 4, 0);
	if (ret < 0)
		return ret;

	return 0;
}

static int mv3310_set_ptp_reg_bits(struct phy_device *phydev, u32 regnum,
				   u32 bits)
{
	int ret;
	u32 regval;

	ret = mv3310_read_ptp_reg(phydev, regnum, &regval);
	if (ret < 0)
		return ret;

	ret = mv3310_write_ptp_reg(phydev, regnum, regval | bits);
	if (ret < 0)
		return ret;

	return 0;
}

static int mv3310_clear_ptp_reg_bits(struct phy_device *phydev, u32 regnum,
				     u32 bits)
{
	int ret;
	u32 regval;

	ret = mv3310_read_ptp_reg(phydev, regnum, &regval);
	if (ret < 0)
		return ret;

	ret = mv3310_write_ptp_reg(phydev, regnum, regval & ~bits);
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

/* Configure Parser/Update PAM Range, except for settings pertaining to TST
 * header, which is not used as this driver configures piggyback. Without this
 * PAM configuration the parser will not identify, e.g., IPv4 packets. */
static int mv3310_ptp_set_pam(struct mv3310_ptp_priv *priv)
{
	/* Mask used to obtain the IPv4 length in words */
	const u32 IPV4_LEN_MASK = 0x0f00;
	/* If Ethertype is <= this value, the packet's type is LLC/SNAP */
	const u32 SAPLEN = 1500;
	/* Bits [3:0] of Ethernet-over-MPLS tunnel label */
	const u32 MPLS_LABEL_3_0 = 0x3000;
	/* Mask used to obtain bits [3:0] of the MPLS label */
	const u32 MPLS_LABEL_MASK = 0xf000;
	/* Bits [23:8] of the LLC<DSAP-SSAP-CTRL> field of an LLC/SNAP packet */
	const u32 DSAP_SSAP_23_8 = 0xaaaa;
	/* Bits [7:0] of the LLC<DSAP-SSAP-CTRL> field of an LLC/SNAP packet */
	const u32 DSAP_SSAP_7_0 = 0x0300;
	/* Mask used to obtain bits [7:0] of the LLC<DSAP-SSAP-CTRL> field on an LLC/SNAP packet */
	const u32 DSAP_SSAP_MASK = 0xff00;
	/* Bits [15:0] of one-second constant */
	const u32 ONESECOND_LO = 0xca00;
	/* Bits [31:16] of one-second constant */
	const u32 ONESECOND_HI = 0x3b9a;
	/* EtherType for Y1731 */
	const u32 UDP_Y131_ETYPE = 0x8902;
	/* UDP Port # for PTP */
	const u32 UDP_PORT_PTP = 320;
	/* Values for hardware internal use */
	const u32 ALL_ONE = 0xffff;
	const u32 ONE = 0x0001;

	int ret = 0;
	struct phy_device *dev = priv->phydev;

	mutex_lock(&priv->lock);

	/* TX Parser */
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_EG_PAM_BASE, 16),
				    IPV4_LEN_MASK);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_EG_PAM_BASE, 18),
				    SAPLEN);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_EG_PAM_BASE, 20),
				    MPLS_LABEL_3_0);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_EG_PAM_BASE, 21),
				    MPLS_LABEL_MASK);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_EG_PAM_BASE, 22),
				    DSAP_SSAP_23_8);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_EG_PAM_BASE, 23),
				    DSAP_SSAP_7_0);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_EG_PAM_BASE, 24),
				    DSAP_SSAP_MASK);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_EG_PAM_BASE, 25),
				    ONESECOND_LO);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_EG_PAM_BASE, 26),
				    ONESECOND_HI);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_EG_PAM_BASE, 30),
				    UDP_Y131_ETYPE);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_EG_PAM_BASE, 31),
				    UDP_PORT_PTP);

	/* RX Parser */
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_IG_PAM_BASE, 16),
				    IPV4_LEN_MASK);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_IG_PAM_BASE, 18),
				    SAPLEN);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_IG_PAM_BASE, 20),
				    MPLS_LABEL_3_0);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_IG_PAM_BASE, 21),
				    MPLS_LABEL_MASK);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_IG_PAM_BASE, 22),
				    DSAP_SSAP_23_8);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_IG_PAM_BASE, 23),
				    DSAP_SSAP_7_0);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_IG_PAM_BASE, 24),
				    DSAP_SSAP_MASK);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_IG_PAM_BASE, 30),
				    UDP_Y131_ETYPE);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_PR_IG_PAM_BASE, 31),
				    UDP_PORT_PTP);

	/* TX Update */
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_UR_EG_PAM_BASE, 25),
				    ALL_ONE);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_UR_EG_PAM_BASE, 26),
				    ONE);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_UR_EG_PAM_BASE, 30),
				    ONESECOND_LO);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_UR_EG_PAM_BASE, 31),
				    ONESECOND_HI);

	/* RX Update */
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_UR_IG_PAM_BASE, 25),
				    ALL_ONE);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_UR_IG_PAM_BASE, 30),
				    ONESECOND_LO);
	ret |= mv3310_write_ptp_reg(dev, PAM_ADDR(MV_V2_PTP_UR_IG_PAM_BASE, 31),
				    ONESECOND_HI);

	mutex_unlock(&priv->lock);
	return ret;
}

static int mv3310_ptp_set_udata(struct mv3310_ptp_priv *priv, const u8 *udata,
				size_t udata_len, u32 baseaddr)
{
	int ret, i;
	u32 regval;
	struct phy_device *phydev = priv->phydev;

	mutex_lock(&priv->lock);

	for (i = 0; i < udata_len / sizeof(u32); i++) {
		memcpy(&regval, udata + (i * sizeof(u32)), sizeof(u32));
		ret = mv3310_write_ptp_reg(phydev, baseaddr + (i * 2), regval);
		if (ret < 0) {
				dev_err(&phydev->mdio.dev,
					"Failed to write PTP microcode address: %x\n",
					baseaddr + (i * 2));
				break;
		}
	}

	mutex_unlock(&priv->lock);
	return ret;
}

static int mv3310_ptp_load_ucode(struct mv3310_ptp_priv *priv)
{
	struct phy_device *phydev = priv->phydev;
	const struct firmware *pr_entry;
	const struct firmware *ur_entry;
	const char *parser_ucode = "mrvl/x3310uc_pr.hdr";
	const char *updater_ucode = "mrvl/x3310uc_ur.hdr";
	int ret = 0;

	ret = request_firmware(&pr_entry, parser_ucode, &phydev->mdio.dev);
	if (ret < 0)
		return ret;

	ret = request_firmware(&ur_entry, updater_ucode, &phydev->mdio.dev);
	if (ret < 0)
		goto out_release_pr;

	/* Microcode size must be word-aligned */
	if (((pr_entry->size % sizeof(u32)) != 0) ||
	    ((ur_entry->size % sizeof(u32)) != 0)) {
		dev_err(&phydev->mdio.dev, "firmware file invalid");
		ret = -EINVAL;
		goto out_release_all;
	}

	ret = 0;
	ret |= mv3310_ptp_set_udata(priv, pr_entry->data, pr_entry->size,
				    MV_V2_PTP_PARSER_EG_UDATA);
	cond_resched();
	ret |= mv3310_ptp_set_udata(priv, ur_entry->data, ur_entry->size,
				    MV_V2_PTP_UPDATER_EG_UDATA);
	cond_resched();
	ret |= mv3310_ptp_set_udata(priv, pr_entry->data, pr_entry->size,
				    MV_V2_PTP_PARSER_IG_UDATA);
	cond_resched();
	ret |= mv3310_ptp_set_udata(priv, ur_entry->data, ur_entry->size,
				    MV_V2_PTP_UPDATER_IG_UDATA);

out_release_all:
	release_firmware(ur_entry);
out_release_pr:
	release_firmware(pr_entry);
	return ret;
}

static int mv3310_ptp_check_ucode(struct mv3310_ptp_priv *priv)
{
	struct phy_device *phydev = priv->phydev;
	u32 ig_parser_check = 0;
	u32 eg_parser_check = 0;
	u32 ig_updater_check = 0;
	u32 eg_updater_check = 0;

	/* Check if the microcode is already loaded */
	mutex_lock(&priv->lock);
	mv3310_read_ptp_reg(phydev, MV_V2_PTP_PARSER_EG_UDATA,
			    &eg_parser_check);
	mv3310_read_ptp_reg(phydev, MV_V2_PTP_UPDATER_EG_UDATA,
			    &eg_updater_check);
	mv3310_read_ptp_reg(phydev, MV_V2_PTP_PARSER_IG_UDATA,
			    &ig_parser_check);
	mv3310_read_ptp_reg(phydev, MV_V2_PTP_UPDATER_IG_UDATA,
			    &ig_updater_check);
	mutex_unlock(&priv->lock);

	if ((eg_parser_check != MV_V2_PTP_UDATA_EMPTY) &&
	    (eg_updater_check != MV_V2_PTP_UDATA_EMPTY) &&
	    (ig_parser_check != MV_V2_PTP_UDATA_EMPTY) &&
	    (ig_updater_check != MV_V2_PTP_UDATA_EMPTY))
		return 0;

	dev_info(&phydev->mdio.dev, "loading PTP parser & updater microcode\n");
	return mv3310_ptp_load_ucode(priv);
}

/*
 * Match PTPv2 event messages (Sync, Delay_Req, Pdelay_Req, Pdelay_Resp) in the
 * Ingress/Egress LUT. Only these messages require an accurate timestamp.
*/
static int mv3310_ptp_set_lut(struct phy_device *phydev)
{
	int ret;

	/* Set Ingress/Egress LUT Match Key.
	 *   MESSAGETYPE  VERSIONPTP ...(zeros)... FLAGPTPV2
	 *      0000      0000 0010                    1
	 *     Event          2                      PTPv2
	 * Sync = 0000, Delay_Req = 0001, Pdelay_Req = 0010, Pdelay_Resp = 0011
	 * => MESSAGETYPE (value) = 00** (use 0 as *).
	 * Ignore TRANSPORTSPECIFIC, FLAGFIELD, DOMAINNUMBER. */
	const u32 PTP_V2_LUT_MATCH_KEY = 0x00020001;

	/* Set Ingress/Egress LUT Match Enable. This is mask. Set to 1 bit positions
	 * from LUT Match Key above.
	 * Check MESSAGETYPE, VERSIONPTP and FLAGPTPV2:
	 *   MESSAGETYPE  VERSIONPTP ...(zeros)... FLAGPTPV2
	 *       1100      0000 1111                    1
	 *      Event          2                      PTPv2
	 * Sync = 0000, Delay_Req = 0001, Pdelay_Req = 0010, Pdelay_Resp = 0011
	 * => MESSAGETYPE (mask) = 1100. */
	const u32 PTP_V2_LUT_MATCH_ENABLE = 0x0c0f0001;

	ret = mv3310_write_ptp_lut_reg(phydev, MV_V2_PTP_LUT_KEY_EG_BASE,
				       PTP_V2_LUT_MATCH_KEY);
	if (ret < 0)
		return ret;

	ret = mv3310_write_ptp_lut_reg(phydev, MV_V2_PTP_LUT_KEY_EG_BASE + 8,
				       PTP_V2_LUT_MATCH_ENABLE);
	if (ret < 0)
		return ret;

	ret = mv3310_write_ptp_lut_reg(phydev, MV_V2_PTP_LUT_KEY_IG_BASE,
				       PTP_V2_LUT_MATCH_KEY);
	if (ret < 0)
		return ret;

	ret = mv3310_write_ptp_lut_reg(phydev, MV_V2_PTP_LUT_KEY_IG_BASE + 8,
				       PTP_V2_LUT_MATCH_ENABLE);
	if (ret < 0)
		return ret;

	return 0;
}

static int mv3310_ptp_set_lut_actions(struct phy_device *phydev, bool enable_tx,
				      bool enable_rx)
{
	int ret;

	/* Set Ingress (RX) LUT Action: INIPIGGYBACK
	   Set Egress  (TX) LUT Action: UPDATERESIDENCE */
	ret = mv3310_write_ptp_reg(phydev, MV_V2_PTP_LUT_ACTION_IG_BASE,
				   enable_rx ? BIT(12) : 0);
	if (ret < 0)
		return ret;

	ret = mv3310_write_ptp_reg(phydev, MV_V2_PTP_LUT_ACTION_EG_BASE,
				   enable_tx ? BIT(11) : 0);
	if (ret < 0)
		return ret;

	return 0;
}

static int mv3310_ts_hwtstamp(struct mii_timestamper *mii_ts,
			      struct kernel_hwtstamp_config *cfg,
			      struct netlink_ext_ack *extack)
{
	int ret;
	bool enable_tx, enable_rx;
	struct mv3310_ptp_priv *priv =
		container_of(mii_ts, struct mv3310_ptp_priv, mii_ts);

	/* reserved for future extensions */
	if (cfg->flags)
		return -EINVAL;

	switch (cfg->tx_type) {
	case HWTSTAMP_TX_OFF:
		enable_tx = false;
		break;
	case HWTSTAMP_TX_ON:
		enable_tx = true;
		break;
	default:
		return -ERANGE;
	}

	switch (cfg->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		enable_rx = false;
		break;
	case HWTSTAMP_FILTER_ALL:
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
		return -ERANGE;
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		enable_rx = true;
		cfg->rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
		break;
	default:
		return -ERANGE;
	}

	mutex_lock(&priv->lock);
	ret = mv3310_ptp_set_lut_actions(priv->phydev, enable_tx, enable_rx);
	mutex_unlock(&priv->lock);

	if (ret < 0) {
		dev_err(&priv->phydev->mdio.dev,
			"failed to set PTP LUT actions: %d\n", ret);
	}

	return 0;
}

static int mv3310_ts_info(struct mii_timestamper *mii_ts,
			  struct kernel_ethtool_ts_info *ts_info)
{
	struct mv3310_ptp_priv *priv =
		container_of(mii_ts, struct mv3310_ptp_priv, mii_ts);

	ts_info->so_timestamping =
		SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE;
	ts_info->phc_index = ptp_clock_index(priv->clock);
	ts_info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON);
	ts_info->rx_filters =
		BIT(HWTSTAMP_FILTER_NONE) | BIT(HWTSTAMP_FILTER_PTP_V2_EVENT);

	return 0;
}

MODULE_FIRMWARE("mrvl/x3310uc_pr.hdr");
MODULE_FIRMWARE("mrvl/x3310uc_ur.hdr");
