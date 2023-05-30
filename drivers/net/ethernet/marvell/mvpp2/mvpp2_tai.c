// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell PP2.2 TAI support
 *
 * Note:
 *   In order to use the event capture support, please see the example
 *   in marvell,pp2.yaml.
 *   Do not manually (e.g. without pinctrl-1, as described in
 *   marvell,pp2.yaml) set the MPP muxes to allow PTP_EVENT_REQ to be
 *   used.
 *   It will disrupt the operation of this driver, and there is nothing
 *   that this driver can do to prevent that.  Even using PTP_EVENT_REQ
 *   as an output will be seen as a trigger input, which can't be masked.
 *   When ever a trigger input is seen, the action in the TCFCR0_TCF
 *   field will be performed - whether it is a set, increment, decrement
 *   read, or frequency update.
 *
 * Other notes (useful, not specified in the documentation):
 * - PTP_PULSE_OUT (PTP_EVENT_REQ MPP)
 *   It looks like the hardware can't generate a pulse at nsec=0. (The
 *   output doesn't trigger if the nsec field is zero.)
 *   Note: when configured as an output via the register at 0xfX441120,
 *   the input is still very much alive, and will trigger the current TCF
 *   function.
 * - PTP_CLK_OUT (PTP_TRIG_GEN MPP)
 *   This generates a "PPS" signal determined by the CCC registers. It
 *   seems this is not aligned to the TOD counter in any way (it may be
 *   initially, but if you specify a non-round second interval, it won't,
 *   and you can't easily get it back.)
 * - PTP_PCLK_OUT
 *   This generates a 50% duty cycle clock based on the TOD counter, and
 *   seems it can be set to any period of 1ns resolution. It is probably
 *   limited by the TOD step size. Its period is defined by the PCLK_CCC
 *   registers. Again, its alignment to the second is questionable.
 *
 * Consequently, we support none of these.
 */
#include "linux/spinlock.h"
#include <linux/io.h>
#include <linux/pinctrl/consumer.h>
#include <linux/ptp_clock.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/slab.h>

#include "mvpp2.h"

#define CR0_SW_NRESET			BIT(0)

#define TCFCR0_PHASE_UPDATE_ENABLE	BIT(8)
#define TCFCR0_TCF_MASK			(7 << 2)
#define TCFCR0_TCF_UPDATE		(0 << 2)
#define TCFCR0_TCF_FREQUPDATE		(1 << 2)
#define TCFCR0_TCF_INCREMENT		(2 << 2)
#define TCFCR0_TCF_DECREMENT		(3 << 2)
#define TCFCR0_TCF_CAPTURE		(4 << 2)
#define TCFCR0_TCF_NOP			(7 << 2)
#define TCFCR0_TCF_TRIGGER		BIT(0)

#define TCSR_CAPTURE_1_VALID		BIT(1)
#define TCSR_CAPTURE_0_VALID		BIT(0)

#define MVPP2_PINCTRL_EXTTS_STATE		"extts"
#define MAX_PINS 1
#define EXTTS_PERIOD_MS 95

struct mvpp2_tai {
	struct ptp_clock_info caps;
	struct ptp_clock *ptp_clock;
	void __iomem *base;
	spinlock_t lock;
	u64 period;		// nanosecond period in 32.32 fixed point
	/* This timestamp is updated every two seconds */
	struct timespec64 stamp;
	struct pinctrl *extts_pinctrl;
	struct pinctrl_state *default_pinctrl_state;
	struct pinctrl_state *extts_pinctrl_state;
	struct ptp_pin_desc pin_config[MAX_PINS];
	spinlock_t refcount_lock; /* Protects the poll_worker_refcount variable */
	u16 poll_worker_refcount;
	bool extts_enabled:1;
};

static void mvpp2_tai_modify(void __iomem *reg, u32 mask, u32 set)
{
	u32 val;

	val = readl_relaxed(reg) & ~mask;
	val |= set & mask;
	writel(val, reg);
}

/* mvpp2_tai_{pause,resume}_external_trigger are used as guards
 * to mask external triggers where it is undesirable. For example,
 * in case that the action is "increment", we may want to perform it
 * once; however, we may trigger it once internally and once from
 * an external pulse, which will cause an issue.
 * In order to work around this issue, we need perform the following sequence:
 *  1. call mvpp2_tai_pause_external_trigger
 *  2. save the current trigger operation.
 *  3. update the trigger operation.
 *  4. perform an internal trigger.
 *  5. restore the previous trigger operation.
 *  6. call mvpp2_tai_restore_external_trigger.
 */
static int mvpp2_tai_pause_external_trigger(struct mvpp2_tai *tai)
{
	if (tai->extts_enabled && tai->extts_pinctrl &&
	    tai->extts_pinctrl_state)
		return pinctrl_select_state(tai->extts_pinctrl,
					    tai->default_pinctrl_state);

	return 0;
}

static int mvpp2_tai_resume_external_trigger(struct mvpp2_tai *tai)
{
	if (tai->extts_enabled && tai->extts_pinctrl &&
	    tai->extts_pinctrl_state)
		return pinctrl_select_state(tai->extts_pinctrl,
					    tai->extts_pinctrl_state);
	return 0;
}

static void mvpp2_tai_write(u32 val, void __iomem *reg)
{
	writel_relaxed(val & 0xffff, reg);
}

static u32 mvpp2_tai_read(void __iomem *reg)
{
	return readl_relaxed(reg) & 0xffff;
}

static struct mvpp2_tai *ptp_to_tai(struct ptp_clock_info *ptp)
{
	return container_of(ptp, struct mvpp2_tai, caps);
}

static void mvpp22_tai_read_ts(struct timespec64 *ts, void __iomem *base)
{
	ts->tv_sec = (u64)mvpp2_tai_read(base + 0) << 32 |
		     mvpp2_tai_read(base + 4) << 16 |
		     mvpp2_tai_read(base + 8);

	ts->tv_nsec = mvpp2_tai_read(base + 12) << 16 |
		      mvpp2_tai_read(base + 16);

	/* Read and discard fractional part */
	readl_relaxed(base + 20);
	readl_relaxed(base + 24);
}

static int mvpp22_tai_try_read_ts(struct timespec64 *ts, void __iomem *base)
{
	long tcsr = readl(base + MVPP22_TAI_TCSR);
	/* If both captures are not valid, return EBUSY */
	int ret = -EBUSY;

	if (tcsr & TCSR_CAPTURE_1_VALID) {
		mvpp22_tai_read_ts(ts, base + MVPP22_TAI_TCV1_SEC_HIGH);
		ret = 0;
	}

	/* If both capture 1 and capture 0 are valid, use capture 0
	 * but also read and clear capture 1.
	 */
	if (tcsr & TCSR_CAPTURE_0_VALID) {
		mvpp22_tai_read_ts(ts, base + MVPP22_TAI_TCV0_SEC_HIGH);
		ret = 0;
	}

	return ret;
}

static void mvpp2_tai_write_tlv(const struct timespec64 *ts, u32 frac,
			        void __iomem *base)
{
	mvpp2_tai_write(ts->tv_sec >> 32, base + MVPP22_TAI_TLV_SEC_HIGH);
	mvpp2_tai_write(ts->tv_sec >> 16, base + MVPP22_TAI_TLV_SEC_MED);
	mvpp2_tai_write(ts->tv_sec, base + MVPP22_TAI_TLV_SEC_LOW);
	mvpp2_tai_write(ts->tv_nsec >> 16, base + MVPP22_TAI_TLV_NANO_HIGH);
	mvpp2_tai_write(ts->tv_nsec, base + MVPP22_TAI_TLV_NANO_LOW);
	mvpp2_tai_write(frac >> 16, base + MVPP22_TAI_TLV_FRAC_HIGH);
	mvpp2_tai_write(frac, base + MVPP22_TAI_TLV_FRAC_LOW);
}

static int mvpp2_tai_op(u32 op, void __iomem *base, struct mvpp2_tai *tai)
{
	u32 reg_val;
	int ret;

	reg_val = mvpp2_tai_read(base + MVPP22_TAI_TCFCR0);
	/* Trigger the operation. Note that an external unmaskable
	 * event on PTP_EVENT_REQ will also trigger this action.
	 * therefore, pause possible (known) external triggers.
	 */
	ret = mvpp2_tai_pause_external_trigger(tai);
	if (ret)
		goto out;

	mvpp2_tai_modify(base + MVPP22_TAI_TCFCR0,
			 TCFCR0_TCF_MASK | TCFCR0_TCF_TRIGGER,
			 op | TCFCR0_TCF_TRIGGER);
	mvpp2_tai_modify(base + MVPP22_TAI_TCFCR0,
			 TCFCR0_TCF_MASK | TCFCR0_TCF_TRIGGER, reg_val);

	mvpp2_tai_resume_external_trigger(tai);

out:
	return ret;
}

/* The adjustment has a range of +0.5ns to -0.5ns in 2^32 steps, so has units
 * of 2^-32 ns.
 *
 * units(s) = 1 / (2^32 * 10^9)
 * fractional = abs_scaled_ppm / (2^16 * 10^6)
 *
 * What we want to achieve:
 *  freq_adjusted = freq_nominal * (1 + fractional)
 *  freq_delta = freq_adjusted - freq_nominal => positive = faster
 *  freq_delta = freq_nominal * (1 + fractional) - freq_nominal
 * So: freq_delta = freq_nominal * fractional
 *
 * However, we are dealing with periods, so:
 *  period_adjusted = period_nominal / (1 + fractional)
 *  period_delta = period_nominal - period_adjusted => positive = faster
 *  period_delta = period_nominal * fractional / (1 + fractional)
 *
 * Hence:
 *  period_delta = period_nominal * abs_scaled_ppm /
 *		   (2^16 * 10^6 + abs_scaled_ppm)
 *
 * To avoid overflow, we reduce both sides of the divide operation by a factor
 * of 16.
 */
static u64 mvpp22_calc_frac_ppm(struct mvpp2_tai *tai, long abs_scaled_ppm)
{
	u64 val = tai->period * abs_scaled_ppm >> 4;

	return div_u64(val, (1000000 << 12) + (abs_scaled_ppm >> 4));
}

static s32 mvpp22_calc_max_adj(struct mvpp2_tai *tai)
{
	return 1000000;
}

static int mvpp22_tai_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct mvpp2_tai *tai = ptp_to_tai(ptp);
	unsigned long flags;
	void __iomem *base;
	bool neg_adj;
	s32 frac;
	u64 val;
	int ret;

	neg_adj = scaled_ppm < 0;
	if (neg_adj)
		scaled_ppm = -scaled_ppm;

	val = mvpp22_calc_frac_ppm(tai, scaled_ppm);

	/* Convert to a signed 32-bit adjustment */
	if (neg_adj) {
		/* -S32_MIN warns, -val < S32_MIN fails, so go for the easy
		 * solution.
		 */
		if (val > 0x80000000)
			return -ERANGE;

		frac = -val;
	} else {
		if (val > S32_MAX)
			return -ERANGE;

		frac = val;
	}

	base = tai->base;
	spin_lock_irqsave(&tai->lock, flags);
	mvpp2_tai_write(frac >> 16, base + MVPP22_TAI_TLV_FRAC_HIGH);
	mvpp2_tai_write(frac, base + MVPP22_TAI_TLV_FRAC_LOW);
	ret = mvpp2_tai_op(TCFCR0_TCF_FREQUPDATE, base, tai);
	spin_unlock_irqrestore(&tai->lock, flags);

	return ret;
}

static int mvpp22_tai_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct mvpp2_tai *tai = ptp_to_tai(ptp);
	struct timespec64 ts;
	unsigned long flags;
	void __iomem *base;
	u32 tcf;
	int ret;

	/* We can't deal with S64_MIN */
	if (delta == S64_MIN)
		return -ERANGE;

	if (delta < 0) {
		delta = -delta;
		tcf = TCFCR0_TCF_DECREMENT;
	} else {
		tcf = TCFCR0_TCF_INCREMENT;
	}

	ts = ns_to_timespec64(delta);

	base = tai->base;
	spin_lock_irqsave(&tai->lock, flags);
	mvpp2_tai_write_tlv(&ts, 0, base);
	ret = mvpp2_tai_op(tcf, base, tai);
	spin_unlock_irqrestore(&tai->lock, flags);

	return ret;
}

static int mvpp22_tai_gettimex64(struct ptp_clock_info *ptp,
				 struct timespec64 *ts,
				 struct ptp_system_timestamp *sts)
{
	struct mvpp2_tai *tai = ptp_to_tai(ptp);
	unsigned long flags;
	void __iomem *base;
	u32 reg_val;
	int ret;

	base = tai->base;
	spin_lock_irqsave(&tai->lock, flags);
	/* XXX: the only way to read the PTP time is for the CPU to trigger
	 * an event. However, there is no way to distinguish between the CPU
	 * triggered event, and an external event on PTP_EVENT_REQ. As a result
	 * we are pausing here external triggers by switching the pinctrl state
	 * to the default state (if applicable).
	 */
	ret = mvpp2_tai_pause_external_trigger(tai);
	if (ret)
		goto unlock_out;

	reg_val = mvpp2_tai_read(base + MVPP22_TAI_TCFCR0);
	ptp_read_system_prets(sts);
	mvpp2_tai_modify(base + MVPP22_TAI_TCFCR0,
			 TCFCR0_TCF_MASK | TCFCR0_TCF_TRIGGER,
			 TCFCR0_TCF_CAPTURE | TCFCR0_TCF_TRIGGER);
	ptp_read_system_postts(sts);
	mvpp2_tai_modify(base + MVPP22_TAI_TCFCR0,
			 TCFCR0_TCF_MASK | TCFCR0_TCF_TRIGGER, reg_val);

	ret = mvpp22_tai_try_read_ts(ts, base);
	mvpp2_tai_resume_external_trigger(tai);

unlock_out:
	spin_unlock_irqrestore(&tai->lock, flags);

	return ret;
}

static int mvpp22_tai_settime64(struct ptp_clock_info *ptp,
				const struct timespec64 *ts)
{
	struct mvpp2_tai *tai = ptp_to_tai(ptp);
	unsigned long flags;
	void __iomem *base;
	u32 reg_val;
	int ret;

	base = tai->base;
	spin_lock_irqsave(&tai->lock, flags);
	mvpp2_tai_write_tlv(ts, 0, base);

	ret = mvpp2_tai_pause_external_trigger(tai);
	if (ret)
		goto unlock_out;

	/* Trigger an update to load the value from the TLV registers
	 * into the TOD counter. Note that an external unmaskable event on
	 * PTP_EVENT_REQ will also trigger this action.
	 */
	reg_val = mvpp2_tai_read(base + MVPP22_TAI_TCFCR0);
	mvpp2_tai_modify(base + MVPP22_TAI_TCFCR0,
			 TCFCR0_PHASE_UPDATE_ENABLE | TCFCR0_TCF_MASK |
				 TCFCR0_TCF_TRIGGER,
			 TCFCR0_TCF_UPDATE | TCFCR0_TCF_TRIGGER);
	mvpp2_tai_modify(base + MVPP22_TAI_TCFCR0,
			 TCFCR0_PHASE_UPDATE_ENABLE | TCFCR0_TCF_MASK |
				 TCFCR0_TCF_TRIGGER,
			 reg_val);

	mvpp2_tai_resume_external_trigger(tai);

unlock_out:
	spin_unlock_irqrestore(&tai->lock, flags);

	return ret;
}

static void do_aux_work_extts(struct mvpp2_tai *tai)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&tai->lock, flags);

	ret = mvpp22_tai_try_read_ts(&tai->stamp, tai->base);
	/* We are not managed to read a TS, try again later */
	if (!ret) {
		struct ptp_clock_event event;

		/* Triggered - save timestamp */
		event.type = PTP_CLOCK_EXTTS;
		event.index = 0; /* We only have one channel */
		event.timestamp = timespec64_to_ns(&tai->stamp);
		ptp_clock_event(tai->ptp_clock, &event);
	}

	spin_unlock_irqrestore(&tai->lock, flags);
}

static long mvpp22_tai_aux_work(struct ptp_clock_info *ptp)
{
	struct mvpp2_tai *tai = ptp_to_tai(ptp);

	if (tai->extts_enabled) {
		do_aux_work_extts(tai);
		return msecs_to_jiffies(EXTTS_PERIOD_MS);
	}

	mvpp22_tai_gettimex64(ptp, &tai->stamp, NULL);
	return msecs_to_jiffies(2000);
}

static void mvpp22_tai_set_step(struct mvpp2_tai *tai)
{
	void __iomem *base = tai->base;
	u32 nano, frac;

	nano = upper_32_bits(tai->period);
	frac = lower_32_bits(tai->period);

	/* As the fractional nanosecond is a signed offset, if the MSB (sign)
	 * bit is set, we have to increment the whole nanoseconds.
	 */
	if (frac >= 0x80000000)
		nano += 1;

	mvpp2_tai_write(nano, base + MVPP22_TAI_TOD_STEP_NANO_CR);
	mvpp2_tai_write(frac >> 16, base + MVPP22_TAI_TOD_STEP_FRAC_HIGH);
	mvpp2_tai_write(frac, base + MVPP22_TAI_TOD_STEP_FRAC_LOW);
}

static void mvpp22_tai_init(struct mvpp2_tai *tai)
{
	void __iomem *base = tai->base;

	mvpp22_tai_set_step(tai);

	/* Release the TAI reset */
	mvpp2_tai_modify(base + MVPP22_TAI_CR0, CR0_SW_NRESET, CR0_SW_NRESET);
}

int mvpp22_tai_ptp_clock_index(struct mvpp2_tai *tai)
{
	return ptp_clock_index(tai->ptp_clock);
}

void mvpp22_tai_tstamp(struct mvpp2_tai *tai, u32 tstamp,
		       struct skb_shared_hwtstamps *hwtstamp)
{
	struct timespec64 ts;
	int delta;

	/* The tstamp consists of 2 bits of seconds and 30 bits of nanoseconds.
	 * We use our stored timestamp (tai->stamp) to form a full timestamp,
	 * and we must read the seconds exactly once.
	 */
	ts.tv_sec = READ_ONCE(tai->stamp.tv_sec);
	ts.tv_nsec = tstamp & 0x3fffffff;

	/* Calculate the delta in seconds between our stored timestamp and
	 * the value read from the queue. Allow timestamps one second in the
	 * past, otherwise consider them to be in the future.
	 */
	delta = ((tstamp >> 30) - (ts.tv_sec & 3)) & 3;
	if (delta == 3)
		delta -= 4;
	ts.tv_sec += delta;

	memset(hwtstamp, 0, sizeof(*hwtstamp));
	hwtstamp->hwtstamp = timespec64_to_ktime(ts);
}

void mvpp22_tai_start(struct mvpp2_tai *tai)
{
	unsigned long flags;

	spin_lock_irqsave(&tai->refcount_lock, flags);

	tai->poll_worker_refcount++;
	if (tai->poll_worker_refcount > 1)
		goto out_unlock;

	ptp_schedule_worker(tai->ptp_clock, 0);

out_unlock:
	spin_unlock_irqrestore(&tai->refcount_lock, flags);
}

void mvpp22_tai_stop(struct mvpp2_tai *tai)
{
	unsigned long flags;

	spin_lock_irqsave(&tai->refcount_lock, flags);

	tai->poll_worker_refcount--;
	if (tai->poll_worker_refcount)
		goto unlock_out;

	ptp_cancel_worker_sync(tai->ptp_clock);

unlock_out:
	spin_unlock_irqrestore(&tai->refcount_lock, flags);
}

static void mvpp22_tai_capture_enable(struct mvpp2_tai *tai, bool enable)
{
	mvpp2_tai_modify(tai->base + MVPP22_TAI_TCFCR0, TCFCR0_TCF_MASK,
			 enable ? TCFCR0_TCF_CAPTURE : TCFCR0_TCF_NOP);
}

static int mvpp22_tai_req_extts_enable(struct mvpp2_tai *tai,
				       struct ptp_clock_request *rq, int on)
{
	u8 index = rq->extts.index;
	int ret = 0;

	if (!tai->extts_pinctrl)
		return -EINVAL;

	/* Reject requests with unsupported flags */
	if (rq->extts.flags & ~(PTP_ENABLE_FEATURE | PTP_RISING_EDGE |
				PTP_FALLING_EDGE | PTP_STRICT_FLAGS))
		return -EOPNOTSUPP;

	/* Reject requests to enable time stamping on falling edge */
	if ((rq->extts.flags & PTP_ENABLE_FEATURE) &&
	    (rq->extts.flags & PTP_FALLING_EDGE))
		return -EOPNOTSUPP;

	if (index >= MAX_PINS)
		return -EINVAL;

	if (on)
		ret = pinctrl_select_state(tai->extts_pinctrl,
					   tai->extts_pinctrl_state);
	else
		ret = pinctrl_select_state(tai->extts_pinctrl,
					   tai->default_pinctrl_state);
	if (ret)
		goto out;

	tai->extts_enabled = on != 0;
	mvpp22_tai_capture_enable(tai, tai->extts_enabled);

	/* We need to enable the poll worker in order for events to be polled */
	if (on)
		mvpp22_tai_start(tai);
	else
		mvpp22_tai_stop(tai);

out:
	return ret;
}

static int mvpp22_tai_enable(struct ptp_clock_info *ptp,
			     struct ptp_clock_request *rq, int on)
{
	struct mvpp2_tai *tai = ptp_to_tai(ptp);
	int err = -EOPNOTSUPP;
	unsigned long flags;

	spin_lock_irqsave(&tai->lock, flags);

	switch (rq->type) {
	case PTP_CLK_REQ_EXTTS:
		err = mvpp22_tai_req_extts_enable(tai, rq, on);
		break;
	default:
		break;
	}

	spin_unlock_irqrestore(&tai->lock, flags);
	return err;
}

static int mvpp22_tai_verify_pin(struct ptp_clock_info *ptp, unsigned int pin,
				 enum ptp_pin_function func, unsigned int chan)
{
	if (chan != 0)
		return -1;

	switch (func) {
	case PTP_PF_NONE:
	case PTP_PF_EXTTS:
		break;
	case PTP_PF_PEROUT:
	case PTP_PF_PHYSYNC:
		return -1;
	}
	return 0;
}

static void mvpp22_tai_remove(void *priv)
{
	struct mvpp2_tai *tai = priv;

	if (!IS_ERR(tai->ptp_clock))
		ptp_clock_unregister(tai->ptp_clock);
}

int mvpp22_tai_probe(struct device *dev, struct mvpp2 *priv)
{
	struct mvpp2_tai *tai;
	int ret;
	int i;

	tai = devm_kzalloc(dev, sizeof(*tai), GFP_KERNEL);
	if (!tai)
		return -ENOMEM;

	spin_lock_init(&tai->lock);
	spin_lock_init(&tai->refcount_lock);

	tai->extts_pinctrl = devm_pinctrl_get_select_default(dev);
	if (!IS_ERR(tai->extts_pinctrl)) {
		tai->default_pinctrl_state = pinctrl_lookup_state
			(tai->extts_pinctrl, PINCTRL_STATE_DEFAULT);
		tai->extts_pinctrl_state = pinctrl_lookup_state
			(tai->extts_pinctrl, MVPP2_PINCTRL_EXTTS_STATE);

		if (IS_ERR(tai->default_pinctrl_state) ||
		    IS_ERR(tai->extts_pinctrl_state)) {
			pinctrl_put(tai->extts_pinctrl);
			tai->extts_pinctrl = NULL;
			tai->default_pinctrl_state = NULL;
			tai->extts_pinctrl_state = NULL;
		}
	} else {
		tai->extts_pinctrl = NULL;
	}

	tai->base = priv->iface_base;

	/* The step size consists of three registers - a 16-bit nanosecond step
	 * size, and a 32-bit fractional nanosecond step size split over two
	 * registers. The fractional nanosecond step size has units of 2^-32ns.
	 *
	 * To calculate this, we calculate:
	 *   (10^9 + freq / 2) / (freq * 2^-32)
	 * which gives us the nanosecond step to the nearest integer in 16.32
	 * fixed point format, and the fractional part of the step size with
	 * the MSB inverted.  With rounding of the fractional nanosecond, and
	 * simplification, this becomes:
	 *   (10^9 << 32 + freq << 31 + (freq + 1) >> 1) / freq
	 *
	 * So:
	 *   div = (10^9 << 32 + freq << 31 + (freq + 1) >> 1) / freq
	 *   nano = upper_32_bits(div);
	 *   frac = lower_32_bits(div) ^ 0x80000000;
	 * Will give the values for the registers.
	 *
	 * This is all seems perfect, but alas it is not when considering the
	 * whole story.  The system is clocked from 25MHz, which is multiplied
	 * by a PLL to 1GHz, and then divided by three, giving 333333333Hz
	 * (recurring).  This gives exactly 3ns, but using 333333333Hz with
	 * the above gives an error of 13*2^-32ns.
	 *
	 * Consequently, we use the period rather than calculating from the
	 * frequency.
	 */
	tai->period = 3ULL << 32;

	mvpp22_tai_init(tai);

	tai->caps.owner = THIS_MODULE;
	strscpy(tai->caps.name, "Marvell PP2.2", sizeof(tai->caps.name));
	tai->caps.n_ext_ts = MAX_PINS;
	tai->caps.n_pins = MAX_PINS;
	tai->caps.max_adj = mvpp22_calc_max_adj(tai);
	tai->caps.adjfine = mvpp22_tai_adjfine;
	tai->caps.adjtime = mvpp22_tai_adjtime;
	tai->caps.gettimex64 = mvpp22_tai_gettimex64;
	tai->caps.settime64 = mvpp22_tai_settime64;
	tai->caps.do_aux_work = mvpp22_tai_aux_work;
	tai->caps.enable = mvpp22_tai_enable;
	tai->caps.verify = mvpp22_tai_verify_pin;
	tai->caps.pin_config = tai->pin_config;

	for (i = 0; i < tai->caps.n_pins; ++i) {
		struct ptp_pin_desc *ppd = &tai->caps.pin_config[i];

		snprintf(ppd->name, sizeof(ppd->name), "PTP_PULSE_IN%d", i);
		ppd->index = i;
		ppd->func = PTP_PF_NONE;
		ppd->chan = 0;
	}

	ret = devm_add_action(dev, mvpp22_tai_remove, tai);
	if (ret)
		return ret;

	tai->ptp_clock = ptp_clock_register(&tai->caps, dev);
	if (IS_ERR(tai->ptp_clock))
		return PTR_ERR(tai->ptp_clock);

	priv->tai = tai;

	return 0;
}
