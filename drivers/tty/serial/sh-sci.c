// SPDX-License-Identifier: GPL-2.0
/*
 * SuperH on-chip serial module support.  (SCI with no FIFO / with FIFO)
 *
 *  Copyright (C) 2002 - 2011  Paul Mundt
 *  Copyright (C) 2015 Glider bvba
 *  Modified to support SH7720 SCIF. Markus Brunner, Mark Jonas (Jul 2007).
 *
 * based off of the old drivers/char/sh-sci.c by:
 *
 *   Copyright (C) 1999, 2000  Niibe Yutaka
 *   Copyright (C) 2000  Sugioka Toshinobu
 *   Modified to support multiple serial ports. Stuart Menefy (May 2000).
 *   Modified to support SecureEdge. David McCullough (2002)
 *   Modified to support SH7300 SCIF. Takashi Kusuda (Jun 2003).
 *   Removed SH7300 support (Jul 2007).
 */
#undef DEBUG

#include <linux/clk.h>
#include <linux/console.h>
#include <linux/ctype.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/ktime.h>
#include <linux/major.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/scatterlist.h>
#include <linux/serial.h>
#include <linux/serial_sci.h>
#include <linux/sh_dma.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysrq.h>
#include <linux/timer.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#ifdef CONFIG_SUPERH
#include <asm/sh_bios.h>
#include <asm/platform_early.h>
#endif

#include "rsci.h"
#include "serial_mctrl_gpio.h"
#include "sh-sci.h"
#include "sh-sci-common.h"

#define SCIx_IRQ_IS_MUXED(port)			\
	((port)->irqs[SCIx_ERI_IRQ] ==	\
	 (port)->irqs[SCIx_RXI_IRQ]) ||	\
	((port)->irqs[SCIx_ERI_IRQ] &&	\
	 ((port)->irqs[SCIx_RXI_IRQ] < 0))

#define SCI_SR_SCIFAB		SCI_SR(5) | SCI_SR(7) | SCI_SR(11) | \
				SCI_SR(13) | SCI_SR(16) | SCI_SR(17) | \
				SCI_SR(19) | SCI_SR(27)

/* Iterate over all supported sampling rates, from high to low */
#define for_each_sr(_sr, _port)						\
	for ((_sr) = max_sr(_port); (_sr) >= min_sr(_port); (_sr)--)	\
		if ((_port)->sampling_rate_mask & SCI_SR((_sr)))

#define SCI_NPORTS CONFIG_SERIAL_SH_SCI_NR_UARTS

#define SCI_PUBLIC_PORT_ID(port) (((port) & BIT(7)) ? PORT_GENERIC : (port))

static struct sci_port sci_ports[SCI_NPORTS];
static unsigned long sci_ports_in_use;
static struct uart_driver sci_uart_driver;
static bool sci_uart_earlycon;
static bool sci_uart_earlycon_dev_probing;

static const struct sci_port_params_bits sci_sci_port_params_bits = {
	.rxtx_enable = SCSCR_RE | SCSCR_TE,
	.te_clear = SCSCR_TE | SCSCR_TEIE,
	.poll_sent_bits = SCI_TDRE | SCI_TEND
};

static const struct sci_port_params_bits sci_scif_port_params_bits = {
	.rxtx_enable = SCSCR_RE | SCSCR_TE,
	.te_clear = SCSCR_TE | SCSCR_TEIE,
	.poll_sent_bits = SCIF_TDFE | SCIF_TEND
};

static const struct sci_common_regs sci_common_regs = {
	.status = SCxSR,
	.control = SCSCR,
};

struct sci_suspend_regs {
	u16 scdl;
	u16 sccks;
	u16 scsmr;
	u16 scscr;
	u16 scfcr;
	u16 scsptr;
	u16 hssrr;
	u16 scpcr;
	u16 scpdr;
	u8 scbrr;
	u8 semr;
};

static size_t sci_suspend_regs_size(void)
{
	return sizeof(struct sci_suspend_regs);
}

static const struct sci_port_params sci_port_params[SCIx_NR_REGTYPES] = {
	/*
	 * Common SCI definitions, dependent on the port's regshift
	 * value.
	 */
	[SCIx_SCI_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00,  8 },
			[SCBRR]		= { 0x01,  8 },
			[SCSCR]		= { 0x02,  8 },
			[SCxTDR]	= { 0x03,  8 },
			[SCxSR]		= { 0x04,  8 },
			[SCxRDR]	= { 0x05,  8 },
		},
		.fifosize = 1,
		.overrun_reg = SCxSR,
		.overrun_mask = SCI_ORER,
		.sampling_rate_mask = SCI_SR(32),
		.error_mask = SCI_DEFAULT_ERROR_MASK | SCI_ORER,
		.error_clear = SCI_ERROR_CLEAR & ~SCI_ORER,
		.param_bits = &sci_sci_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * Common definitions for legacy IrDA ports.
	 */
	[SCIx_IRDA_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00,  8 },
			[SCBRR]		= { 0x02,  8 },
			[SCSCR]		= { 0x04,  8 },
			[SCxTDR]	= { 0x06,  8 },
			[SCxSR]		= { 0x08, 16 },
			[SCxRDR]	= { 0x0a,  8 },
			[SCFCR]		= { 0x0c,  8 },
			[SCFDR]		= { 0x0e, 16 },
		},
		.fifosize = 1,
		.overrun_reg = SCxSR,
		.overrun_mask = SCI_ORER,
		.sampling_rate_mask = SCI_SR(32),
		.error_mask = SCI_DEFAULT_ERROR_MASK | SCI_ORER,
		.error_clear = SCI_ERROR_CLEAR & ~SCI_ORER,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * Common SCIFA definitions.
	 */
	[SCIx_SCIFA_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00, 16 },
			[SCBRR]		= { 0x04,  8 },
			[SCSCR]		= { 0x08, 16 },
			[SCxTDR]	= { 0x20,  8 },
			[SCxSR]		= { 0x14, 16 },
			[SCxRDR]	= { 0x24,  8 },
			[SCFCR]		= { 0x18, 16 },
			[SCFDR]		= { 0x1c, 16 },
			[SCPCR]		= { 0x30, 16 },
			[SCPDR]		= { 0x34, 16 },
		},
		.fifosize = 64,
		.overrun_reg = SCxSR,
		.overrun_mask = SCIFA_ORER,
		.sampling_rate_mask = SCI_SR_SCIFAB,
		.error_mask = SCIF_DEFAULT_ERROR_MASK | SCIFA_ORER,
		.error_clear = SCIF_ERROR_CLEAR & ~SCIFA_ORER,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * Common SCIFB definitions.
	 */
	[SCIx_SCIFB_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00, 16 },
			[SCBRR]		= { 0x04,  8 },
			[SCSCR]		= { 0x08, 16 },
			[SCxTDR]	= { 0x40,  8 },
			[SCxSR]		= { 0x14, 16 },
			[SCxRDR]	= { 0x60,  8 },
			[SCFCR]		= { 0x18, 16 },
			[SCTFDR]	= { 0x38, 16 },
			[SCRFDR]	= { 0x3c, 16 },
			[SCPCR]		= { 0x30, 16 },
			[SCPDR]		= { 0x34, 16 },
		},
		.fifosize = 256,
		.overrun_reg = SCxSR,
		.overrun_mask = SCIFA_ORER,
		.sampling_rate_mask = SCI_SR_SCIFAB,
		.error_mask = SCIF_DEFAULT_ERROR_MASK | SCIFA_ORER,
		.error_clear = SCIF_ERROR_CLEAR & ~SCIFA_ORER,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * Common SH-2(A) SCIF definitions for ports with FIFO data
	 * count registers.
	 */
	[SCIx_SH2_SCIF_FIFODATA_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00, 16 },
			[SCBRR]		= { 0x04,  8 },
			[SCSCR]		= { 0x08, 16 },
			[SCxTDR]	= { 0x0c,  8 },
			[SCxSR]		= { 0x10, 16 },
			[SCxRDR]	= { 0x14,  8 },
			[SCFCR]		= { 0x18, 16 },
			[SCFDR]		= { 0x1c, 16 },
			[SCSPTR]	= { 0x20, 16 },
			[SCLSR]		= { 0x24, 16 },
		},
		.fifosize = 16,
		.overrun_reg = SCLSR,
		.overrun_mask = SCLSR_ORER,
		.sampling_rate_mask = SCI_SR(32),
		.error_mask = SCIF_DEFAULT_ERROR_MASK,
		.error_clear = SCIF_ERROR_CLEAR,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * The "SCIFA" that is in RZ/A2, RZ/G2L and RZ/T1.
	 * It looks like a normal SCIF with FIFO data, but with a
	 * compressed address space. Also, the break out of interrupts
	 * are different: ERI/BRI, RXI, TXI, TEI, DRI.
	 */
	[SCIx_RZ_SCIFA_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00, 16 },
			[SCBRR]		= { 0x02,  8 },
			[SCSCR]		= { 0x04, 16 },
			[SCxTDR]	= { 0x06,  8 },
			[SCxSR]		= { 0x08, 16 },
			[SCxRDR]	= { 0x0A,  8 },
			[SCFCR]		= { 0x0C, 16 },
			[SCFDR]		= { 0x0E, 16 },
			[SCSPTR]	= { 0x10, 16 },
			[SCLSR]		= { 0x12, 16 },
			[SEMR]		= { 0x14, 8 },
		},
		.fifosize = 16,
		.overrun_reg = SCLSR,
		.overrun_mask = SCLSR_ORER,
		.sampling_rate_mask = SCI_SR(32),
		.error_mask = SCIF_DEFAULT_ERROR_MASK,
		.error_clear = SCIF_ERROR_CLEAR,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * The "SCIF" that is in RZ/V2H(P) SoC is similar to one found on RZ/G2L SoC
	 * with below differences,
	 * - Break out of interrupts are different: ERI, BRI, RXI, TXI, TEI, DRI,
	 *   TEI-DRI, RXI-EDGE and TXI-EDGE.
	 * - SCSMR register does not have CM bit (BIT(7)) ie it does not support synchronous mode.
	 * - SCFCR register does not have SCFCR_MCE bit.
	 * - SCSPTR register has only bits SCSPTR_SPB2DT and SCSPTR_SPB2IO.
	 */
	[SCIx_RZV2H_SCIF_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00, 16 },
			[SCBRR]		= { 0x02,  8 },
			[SCSCR]		= { 0x04, 16 },
			[SCxTDR]	= { 0x06,  8 },
			[SCxSR]		= { 0x08, 16 },
			[SCxRDR]	= { 0x0a,  8 },
			[SCFCR]		= { 0x0c, 16 },
			[SCFDR]		= { 0x0e, 16 },
			[SCSPTR]	= { 0x10, 16 },
			[SCLSR]		= { 0x12, 16 },
			[SEMR]		= { 0x14, 8 },
		},
		.fifosize = 16,
		.overrun_reg = SCLSR,
		.overrun_mask = SCLSR_ORER,
		.sampling_rate_mask = SCI_SR(32),
		.error_mask = SCIF_DEFAULT_ERROR_MASK,
		.error_clear = SCIF_ERROR_CLEAR,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * Common SH-3 SCIF definitions.
	 */
	[SCIx_SH3_SCIF_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00,  8 },
			[SCBRR]		= { 0x02,  8 },
			[SCSCR]		= { 0x04,  8 },
			[SCxTDR]	= { 0x06,  8 },
			[SCxSR]		= { 0x08, 16 },
			[SCxRDR]	= { 0x0a,  8 },
			[SCFCR]		= { 0x0c,  8 },
			[SCFDR]		= { 0x0e, 16 },
		},
		.fifosize = 16,
		.overrun_reg = SCLSR,
		.overrun_mask = SCLSR_ORER,
		.sampling_rate_mask = SCI_SR(32),
		.error_mask = SCIF_DEFAULT_ERROR_MASK,
		.error_clear = SCIF_ERROR_CLEAR,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * Common SH-4(A) SCIF(B) definitions.
	 */
	[SCIx_SH4_SCIF_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00, 16 },
			[SCBRR]		= { 0x04,  8 },
			[SCSCR]		= { 0x08, 16 },
			[SCxTDR]	= { 0x0c,  8 },
			[SCxSR]		= { 0x10, 16 },
			[SCxRDR]	= { 0x14,  8 },
			[SCFCR]		= { 0x18, 16 },
			[SCFDR]		= { 0x1c, 16 },
			[SCSPTR]	= { 0x20, 16 },
			[SCLSR]		= { 0x24, 16 },
		},
		.fifosize = 16,
		.overrun_reg = SCLSR,
		.overrun_mask = SCLSR_ORER,
		.sampling_rate_mask = SCI_SR(32),
		.error_mask = SCIF_DEFAULT_ERROR_MASK,
		.error_clear = SCIF_ERROR_CLEAR,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * Common SCIF definitions for ports with a Baud Rate Generator for
	 * External Clock (BRG).
	 */
	[SCIx_SH4_SCIF_BRG_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00, 16 },
			[SCBRR]		= { 0x04,  8 },
			[SCSCR]		= { 0x08, 16 },
			[SCxTDR]	= { 0x0c,  8 },
			[SCxSR]		= { 0x10, 16 },
			[SCxRDR]	= { 0x14,  8 },
			[SCFCR]		= { 0x18, 16 },
			[SCFDR]		= { 0x1c, 16 },
			[SCSPTR]	= { 0x20, 16 },
			[SCLSR]		= { 0x24, 16 },
			[SCDL]		= { 0x30, 16 },
			[SCCKS]		= { 0x34, 16 },
		},
		.fifosize = 16,
		.overrun_reg = SCLSR,
		.overrun_mask = SCLSR_ORER,
		.sampling_rate_mask = SCI_SR(32),
		.error_mask = SCIF_DEFAULT_ERROR_MASK,
		.error_clear = SCIF_ERROR_CLEAR,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * Common HSCIF definitions.
	 */
	[SCIx_HSCIF_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00, 16 },
			[SCBRR]		= { 0x04,  8 },
			[SCSCR]		= { 0x08, 16 },
			[SCxTDR]	= { 0x0c,  8 },
			[SCxSR]		= { 0x10, 16 },
			[SCxRDR]	= { 0x14,  8 },
			[SCFCR]		= { 0x18, 16 },
			[SCFDR]		= { 0x1c, 16 },
			[SCSPTR]	= { 0x20, 16 },
			[SCLSR]		= { 0x24, 16 },
			[HSSRR]		= { 0x40, 16 },
			[SCDL]		= { 0x30, 16 },
			[SCCKS]		= { 0x34, 16 },
			[HSRTRGR]	= { 0x54, 16 },
			[HSTTRGR]	= { 0x58, 16 },
		},
		.fifosize = 128,
		.overrun_reg = SCLSR,
		.overrun_mask = SCLSR_ORER,
		.sampling_rate_mask = SCI_SR_RANGE(8, 32),
		.error_mask = SCIF_DEFAULT_ERROR_MASK,
		.error_clear = SCIF_ERROR_CLEAR,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * Common SH-4(A) SCIF(B) definitions for ports without an SCSPTR
	 * register.
	 */
	[SCIx_SH4_SCIF_NO_SCSPTR_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00, 16 },
			[SCBRR]		= { 0x04,  8 },
			[SCSCR]		= { 0x08, 16 },
			[SCxTDR]	= { 0x0c,  8 },
			[SCxSR]		= { 0x10, 16 },
			[SCxRDR]	= { 0x14,  8 },
			[SCFCR]		= { 0x18, 16 },
			[SCFDR]		= { 0x1c, 16 },
			[SCLSR]		= { 0x24, 16 },
		},
		.fifosize = 16,
		.overrun_reg = SCLSR,
		.overrun_mask = SCLSR_ORER,
		.sampling_rate_mask = SCI_SR(32),
		.error_mask = SCIF_DEFAULT_ERROR_MASK,
		.error_clear = SCIF_ERROR_CLEAR,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * Common SH-4(A) SCIF(B) definitions for ports with FIFO data
	 * count registers.
	 */
	[SCIx_SH4_SCIF_FIFODATA_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00, 16 },
			[SCBRR]		= { 0x04,  8 },
			[SCSCR]		= { 0x08, 16 },
			[SCxTDR]	= { 0x0c,  8 },
			[SCxSR]		= { 0x10, 16 },
			[SCxRDR]	= { 0x14,  8 },
			[SCFCR]		= { 0x18, 16 },
			[SCFDR]		= { 0x1c, 16 },
			[SCTFDR]	= { 0x1c, 16 },	/* aliased to SCFDR */
			[SCRFDR]	= { 0x20, 16 },
			[SCSPTR]	= { 0x24, 16 },
			[SCLSR]		= { 0x28, 16 },
		},
		.fifosize = 16,
		.overrun_reg = SCLSR,
		.overrun_mask = SCLSR_ORER,
		.sampling_rate_mask = SCI_SR(32),
		.error_mask = SCIF_DEFAULT_ERROR_MASK,
		.error_clear = SCIF_ERROR_CLEAR,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},

	/*
	 * SH7705-style SCIF(B) ports, lacking both SCSPTR and SCLSR
	 * registers.
	 */
	[SCIx_SH7705_SCIF_REGTYPE] = {
		.regs = {
			[SCSMR]		= { 0x00, 16 },
			[SCBRR]		= { 0x04,  8 },
			[SCSCR]		= { 0x08, 16 },
			[SCxTDR]	= { 0x20,  8 },
			[SCxSR]		= { 0x14, 16 },
			[SCxRDR]	= { 0x24,  8 },
			[SCFCR]		= { 0x18, 16 },
			[SCFDR]		= { 0x1c, 16 },
		},
		.fifosize = 64,
		.overrun_reg = SCxSR,
		.overrun_mask = SCIFA_ORER,
		.sampling_rate_mask = SCI_SR(16),
		.error_mask = SCIF_DEFAULT_ERROR_MASK | SCIFA_ORER,
		.error_clear = SCIF_ERROR_CLEAR & ~SCIFA_ORER,
		.param_bits = &sci_scif_port_params_bits,
		.common_regs = &sci_common_regs,
	},
};

#define sci_getreg(up, offset)		(&to_sci_port(up)->params->regs[offset])

/*
 * The "offset" here is rather misleading, in that it refers to an enum
 * value relative to the port mapping rather than the fixed offset
 * itself, which needs to be manually retrieved from the platform's
 * register map for the given port.
 */
static unsigned int sci_serial_in(struct uart_port *p, int offset)
{
	const struct plat_sci_reg *reg = sci_getreg(p, offset);

	if (reg->size == 8)
		return ioread8(p->membase + (reg->offset << p->regshift));
	else if (reg->size == 16)
		return ioread16(p->membase + (reg->offset << p->regshift));
	else
		WARN(1, "Invalid register access\n");

	return 0;
}

static void sci_serial_out(struct uart_port *p, int offset, int value)
{
	const struct plat_sci_reg *reg = sci_getreg(p, offset);

	if (reg->size == 8)
		iowrite8(value, p->membase + (reg->offset << p->regshift));
	else if (reg->size == 16)
		iowrite16(value, p->membase + (reg->offset << p->regshift));
	else
		WARN(1, "Invalid register access\n");
}

void sci_port_enable(struct sci_port *sci_port)
{
	unsigned int i;

	if (!sci_port->port.dev)
		return;

	pm_runtime_get_sync(sci_port->port.dev);

	for (i = 0; i < SCI_NUM_CLKS; i++) {
		clk_prepare_enable(sci_port->clks[i]);
		sci_port->clk_rates[i] = clk_get_rate(sci_port->clks[i]);
	}
	sci_port->port.uartclk = sci_port->clk_rates[SCI_FCK];
}
EXPORT_SYMBOL_NS_GPL(sci_port_enable, "SH_SCI");

void sci_port_disable(struct sci_port *sci_port)
{
	unsigned int i;

	if (!sci_port->port.dev)
		return;

	for (i = SCI_NUM_CLKS; i-- > 0; )
		clk_disable_unprepare(sci_port->clks[i]);

	pm_runtime_put_sync(sci_port->port.dev);
}
EXPORT_SYMBOL_NS_GPL(sci_port_disable, "SH_SCI");

static inline unsigned long port_rx_irq_mask(struct uart_port *port)
{
	/*
	 * Not all ports (such as SCIFA) will support REIE. Rather than
	 * special-casing the port type, we check the port initialization
	 * IRQ enable mask to see whether the IRQ is desired at all. If
	 * it's unset, it's logically inferred that there's no point in
	 * testing for it.
	 */
	return SCSCR_RIE | (to_sci_port(port)->cfg->scscr & SCSCR_REIE);
}

static void sci_start_tx(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	unsigned short ctrl;

#ifdef CONFIG_SERIAL_SH_SCI_DMA
	if (s->type == PORT_SCIFA || s->type == PORT_SCIFB) {
		u16 new, scr = sci_serial_in(port, SCSCR);
		if (s->chan_tx)
			new = scr | SCSCR_TDRQE;
		else
			new = scr & ~SCSCR_TDRQE;
		if (new != scr)
			sci_serial_out(port, SCSCR, new);
	}

	if (s->chan_tx && !kfifo_is_empty(&port->state->port.xmit_fifo) &&
	    dma_submit_error(s->cookie_tx)) {
		if (s->regtype == SCIx_RZ_SCIFA_REGTYPE)
			/* Switch irq from SCIF to DMA */
			disable_irq_nosync(s->irqs[SCIx_TXI_IRQ]);

		s->cookie_tx = 0;
		schedule_work(&s->work_tx);
	}
#endif

	if (!s->chan_tx || s->regtype == SCIx_RZ_SCIFA_REGTYPE ||
	    s->type == PORT_SCIFA || s->type == PORT_SCIFB) {
		/* Set TIE (Transmit Interrupt Enable) bit in SCSCR */
		ctrl = sci_serial_in(port, SCSCR);

		/*
		 * For SCI, TE (transmit enable) must be set after setting TIE
		 * (transmit interrupt enable) or in the same instruction to start
		 * the transmit process.
		 */
		if (s->type == PORT_SCI)
			ctrl |= SCSCR_TE;

		sci_serial_out(port, SCSCR, ctrl | SCSCR_TIE);
	}
}

static void sci_stop_tx(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	unsigned short ctrl;

	/* Clear TIE (Transmit Interrupt Enable) bit in SCSCR */
	ctrl = sci_serial_in(port, SCSCR);

	if (s->type == PORT_SCIFA || s->type == PORT_SCIFB)
		ctrl &= ~SCSCR_TDRQE;

	ctrl &= ~SCSCR_TIE;

	sci_serial_out(port, SCSCR, ctrl);

#ifdef CONFIG_SERIAL_SH_SCI_DMA
	if (s->chan_tx &&
	    !dma_submit_error(s->cookie_tx)) {
		dmaengine_terminate_async(s->chan_tx);
		s->cookie_tx = -EINVAL;
	}
#endif
}

static void sci_start_rx(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	unsigned short ctrl;

	ctrl = sci_serial_in(port, SCSCR) | port_rx_irq_mask(port);

	if (s->type == PORT_SCIFA || s->type == PORT_SCIFB)
		ctrl &= ~SCSCR_RDRQE;

	sci_serial_out(port, SCSCR, ctrl);
}

static void sci_stop_rx(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	unsigned short ctrl;

	ctrl = sci_serial_in(port, SCSCR);

	if (s->type == PORT_SCIFA || s->type == PORT_SCIFB)
		ctrl &= ~SCSCR_RDRQE;

	ctrl &= ~port_rx_irq_mask(port);

	sci_serial_out(port, SCSCR, ctrl);
}

static void sci_clear_SCxSR(struct uart_port *port, unsigned int mask)
{
	struct sci_port *s = to_sci_port(port);

	if (s->type == PORT_SCI) {
		/* Just store the mask */
		sci_serial_out(port, SCxSR, mask);
	} else if (s->params->overrun_mask == SCIFA_ORER) {
		/* SCIFA/SCIFB and SCIF on SH7705/SH7720/SH7721 */
		/* Only clear the status bits we want to clear */
		sci_serial_out(port, SCxSR, sci_serial_in(port, SCxSR) & mask);
	} else {
		/* Store the mask, clear parity/framing errors */
		sci_serial_out(port, SCxSR, mask & ~(SCIF_FERC | SCIF_PERC));
	}
}

#if defined(CONFIG_CONSOLE_POLL) || defined(CONFIG_SERIAL_SH_SCI_CONSOLE) || \
    defined(CONFIG_SERIAL_SH_SCI_EARLYCON)

#ifdef CONFIG_CONSOLE_POLL
static int sci_poll_get_char(struct uart_port *port)
{
	unsigned short status;
	struct sci_port *s = to_sci_port(port);
	int c;

	do {
		status = sci_serial_in(port, SCxSR);
		if (status & SCxSR_ERRORS(port)) {
			s->ops->clear_SCxSR(port, SCxSR_ERROR_CLEAR(port));
			continue;
		}
		break;
	} while (1);

	if (!(status & SCxSR_RDxF(port)))
		return NO_POLL_CHAR;

	c = sci_serial_in(port, SCxRDR);

	/* Dummy read */
	sci_serial_in(port, SCxSR);
	s->ops->clear_SCxSR(port, SCxSR_RDxF_CLEAR(port));

	return c;
}
#endif

static void sci_poll_put_char(struct uart_port *port, unsigned char c)
{
	struct sci_port *s = to_sci_port(port);
	const struct sci_common_regs *regs = s->params->common_regs;
	unsigned int status;

	do {
		status = s->ops->read_reg(port, regs->status);
	} while (!(status & SCxSR_TDxE(port)));

	sci_serial_out(port, SCxTDR, c);
	s->ops->clear_SCxSR(port, SCxSR_TDxE_CLEAR(port) & ~SCxSR_TEND(port));
}
#endif /* CONFIG_CONSOLE_POLL || CONFIG_SERIAL_SH_SCI_CONSOLE ||
	  CONFIG_SERIAL_SH_SCI_EARLYCON */

static void sci_init_pins(struct uart_port *port, unsigned int cflag)
{
	struct sci_port *s = to_sci_port(port);

	/*
	 * Use port-specific handler if provided.
	 */
	if (s->cfg->ops && s->cfg->ops->init_pins) {
		s->cfg->ops->init_pins(port, cflag);
		return;
	}

	if (s->type == PORT_SCIFA || s->type == PORT_SCIFB) {
		u16 data = sci_serial_in(port, SCPDR);
		u16 ctrl = sci_serial_in(port, SCPCR);

		/* Enable RXD and TXD pin functions */
		ctrl &= ~(SCPCR_RXDC | SCPCR_TXDC);
		if (s->has_rtscts) {
			/* RTS# is output, active low, unless autorts */
			if (!(port->mctrl & TIOCM_RTS)) {
				ctrl |= SCPCR_RTSC;
				data |= SCPDR_RTSD;
			} else if (!s->autorts) {
				ctrl |= SCPCR_RTSC;
				data &= ~SCPDR_RTSD;
			} else {
				/* Enable RTS# pin function */
				ctrl &= ~SCPCR_RTSC;
			}
			/* Enable CTS# pin function */
			ctrl &= ~SCPCR_CTSC;
		}
		sci_serial_out(port, SCPDR, data);
		sci_serial_out(port, SCPCR, ctrl);
	} else if (sci_getreg(port, SCSPTR)->size && s->regtype != SCIx_RZV2H_SCIF_REGTYPE) {
		u16 status = sci_serial_in(port, SCSPTR);

		/* RTS# is always output; and active low, unless autorts */
		status |= SCSPTR_RTSIO;
		if (!(port->mctrl & TIOCM_RTS))
			status |= SCSPTR_RTSDT;
		else if (!s->autorts)
			status &= ~SCSPTR_RTSDT;
		/* CTS# and SCK are inputs */
		status &= ~(SCSPTR_CTSIO | SCSPTR_SCKIO);
		sci_serial_out(port, SCSPTR, status);
	}
}

static int sci_txfill(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	unsigned int fifo_mask = (s->params->fifosize << 1) - 1;
	const struct plat_sci_reg *reg;

	reg = sci_getreg(port, SCTFDR);
	if (reg->size)
		return sci_serial_in(port, SCTFDR) & fifo_mask;

	reg = sci_getreg(port, SCFDR);
	if (reg->size)
		return sci_serial_in(port, SCFDR) >> 8;

	return !(sci_serial_in(port, SCxSR) & SCI_TDRE);
}

static int sci_txroom(struct uart_port *port)
{
	return port->fifosize - sci_txfill(port);
}

static int sci_rxfill(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	unsigned int fifo_mask = (s->params->fifosize << 1) - 1;
	const struct plat_sci_reg *reg;

	reg = sci_getreg(port, SCRFDR);
	if (reg->size)
		return sci_serial_in(port, SCRFDR) & fifo_mask;

	reg = sci_getreg(port, SCFDR);
	if (reg->size)
		return sci_serial_in(port, SCFDR) & fifo_mask;

	return (sci_serial_in(port, SCxSR) & SCxSR_RDxF(port)) != 0;
}

/* ********************************************************************** *
 *                   the interrupt related routines                       *
 * ********************************************************************** */

static void sci_transmit_chars(struct uart_port *port)
{
	struct tty_port *tport = &port->state->port;
	unsigned int stopped = uart_tx_stopped(port);
	struct sci_port *s = to_sci_port(port);
	unsigned short status;
	unsigned short ctrl;
	int count;

	status = sci_serial_in(port, SCxSR);
	if (!(status & SCxSR_TDxE(port))) {
		ctrl = sci_serial_in(port, SCSCR);
		if (kfifo_is_empty(&tport->xmit_fifo))
			ctrl &= ~SCSCR_TIE;
		else
			ctrl |= SCSCR_TIE;
		sci_serial_out(port, SCSCR, ctrl);
		return;
	}

	count = sci_txroom(port);

	do {
		unsigned char c;

		if (port->x_char) {
			c = port->x_char;
			port->x_char = 0;
		} else if (stopped || !kfifo_get(&tport->xmit_fifo, &c)) {
			if (s->type == PORT_SCI &&
			    kfifo_is_empty(&tport->xmit_fifo)) {
				ctrl = sci_serial_in(port, SCSCR);
				ctrl &= ~SCSCR_TE;
				sci_serial_out(port, SCSCR, ctrl);
				return;
			}
			break;
		}

		sci_serial_out(port, SCxTDR, c);
		s->tx_occurred = true;

		port->icount.tx++;
	} while (--count > 0);

	s->ops->clear_SCxSR(port, SCxSR_TDxE_CLEAR(port));

	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(port);
	if (kfifo_is_empty(&tport->xmit_fifo)) {
		if (s->type == PORT_SCI) {
			ctrl = sci_serial_in(port, SCSCR);
			ctrl &= ~SCSCR_TIE;
			ctrl |= SCSCR_TEIE;
			sci_serial_out(port, SCSCR, ctrl);
		}

		sci_stop_tx(port);
	}
}

static void sci_receive_chars(struct uart_port *port)
{
	struct tty_port *tport = &port->state->port;
	struct sci_port *s = to_sci_port(port);
	int i, count, copied = 0;
	unsigned short status;
	unsigned char flag;

	status = sci_serial_in(port, SCxSR);
	if (!(status & SCxSR_RDxF(port)))
		return;

	while (1) {
		/* Don't copy more bytes than there is room for in the buffer */
		count = tty_buffer_request_room(tport, sci_rxfill(port));

		/* If for any reason we can't copy more data, we're done! */
		if (count == 0)
			break;

		if (s->type == PORT_SCI) {
			char c = sci_serial_in(port, SCxRDR);
			if (uart_handle_sysrq_char(port, c))
				count = 0;
			else
				tty_insert_flip_char(tport, c, TTY_NORMAL);
		} else {
			for (i = 0; i < count; i++) {
				char c;

				if (s->type == PORT_SCIF ||
				    s->type == PORT_HSCIF) {
					status = sci_serial_in(port, SCxSR);
					c = sci_serial_in(port, SCxRDR);
				} else {
					c = sci_serial_in(port, SCxRDR);
					status = sci_serial_in(port, SCxSR);
				}
				if (uart_handle_sysrq_char(port, c)) {
					count--; i--;
					continue;
				}

				/* Store data and status */
				if (status & SCxSR_FER(port)) {
					flag = TTY_FRAME;
					port->icount.frame++;
				} else if (status & SCxSR_PER(port)) {
					flag = TTY_PARITY;
					port->icount.parity++;
				} else
					flag = TTY_NORMAL;

				tty_insert_flip_char(tport, c, flag);
			}
		}

		sci_serial_in(port, SCxSR); /* dummy read */
		s->ops->clear_SCxSR(port, SCxSR_RDxF_CLEAR(port));

		copied += count;
		port->icount.rx += count;
	}

	if (copied) {
		/* Tell the rest of the system the news. New characters! */
		tty_flip_buffer_push(tport);
	} else {
		/* TTY buffers full; read from RX reg to prevent lockup */
		sci_serial_in(port, SCxRDR);
		sci_serial_in(port, SCxSR); /* dummy read */
		s->ops->clear_SCxSR(port, SCxSR_RDxF_CLEAR(port));
	}
}

static int sci_handle_errors(struct uart_port *port)
{
	int copied = 0;
	struct sci_port *s = to_sci_port(port);
	const struct sci_common_regs *regs = s->params->common_regs;
	unsigned int status = s->ops->read_reg(port, regs->status);
	struct tty_port *tport = &port->state->port;

	/* Handle overruns */
	if (status & s->params->overrun_mask) {
		port->icount.overrun++;

		/* overrun error */
		if (tty_insert_flip_char(tport, 0, TTY_OVERRUN))
			copied++;
	}

	if (status & SCxSR_FER(port)) {
		/* frame error */
		port->icount.frame++;

		if (tty_insert_flip_char(tport, 0, TTY_FRAME))
			copied++;
	}

	if (status & SCxSR_PER(port)) {
		/* parity error */
		port->icount.parity++;

		if (tty_insert_flip_char(tport, 0, TTY_PARITY))
			copied++;
	}

	if (copied)
		tty_flip_buffer_push(tport);

	return copied;
}

static int sci_handle_fifo_overrun(struct uart_port *port)
{
	struct tty_port *tport = &port->state->port;
	struct sci_port *s = to_sci_port(port);
	const struct plat_sci_reg *reg;
	int copied = 0;
	u32 status;

	if (s->type != SCI_PORT_RSCI) {
		reg = sci_getreg(port, s->params->overrun_reg);
		if (!reg->size)
			return 0;
	}

	status = s->ops->read_reg(port, s->params->overrun_reg);
	if (status & s->params->overrun_mask) {
		status &= ~s->params->overrun_mask;
		s->ops->write_reg(port, s->params->overrun_reg, status);

		port->icount.overrun++;

		tty_insert_flip_char(tport, 0, TTY_OVERRUN);
		tty_flip_buffer_push(tport);
		copied++;
	}

	return copied;
}

static int sci_handle_breaks(struct uart_port *port)
{
	int copied = 0;
	unsigned short status = sci_serial_in(port, SCxSR);
	struct tty_port *tport = &port->state->port;

	if (uart_handle_break(port))
		return 0;

	if (status & SCxSR_BRK(port)) {
		port->icount.brk++;

		/* Notify of BREAK */
		if (tty_insert_flip_char(tport, 0, TTY_BREAK))
			copied++;
	}

	if (copied)
		tty_flip_buffer_push(tport);

	copied += sci_handle_fifo_overrun(port);

	return copied;
}

static int scif_set_rtrg(struct uart_port *port, int rx_trig)
{
	struct sci_port *s = to_sci_port(port);
	unsigned int bits;

	if (rx_trig >= port->fifosize)
		rx_trig = port->fifosize - 1;
	if (rx_trig < 1)
		rx_trig = 1;

	/* HSCIF can be set to an arbitrary level. */
	if (sci_getreg(port, HSRTRGR)->size) {
		sci_serial_out(port, HSRTRGR, rx_trig);
		return rx_trig;
	}

	switch (s->type) {
	case PORT_SCIF:
		if (rx_trig < 4) {
			bits = 0;
			rx_trig = 1;
		} else if (rx_trig < 8) {
			bits = SCFCR_RTRG0;
			rx_trig = 4;
		} else if (rx_trig < 14) {
			bits = SCFCR_RTRG1;
			rx_trig = 8;
		} else {
			bits = SCFCR_RTRG0 | SCFCR_RTRG1;
			rx_trig = 14;
		}
		break;
	case PORT_SCIFA:
	case PORT_SCIFB:
		if (rx_trig < 16) {
			bits = 0;
			rx_trig = 1;
		} else if (rx_trig < 32) {
			bits = SCFCR_RTRG0;
			rx_trig = 16;
		} else if (rx_trig < 48) {
			bits = SCFCR_RTRG1;
			rx_trig = 32;
		} else {
			bits = SCFCR_RTRG0 | SCFCR_RTRG1;
			rx_trig = 48;
		}
		break;
	default:
		WARN(1, "unknown FIFO configuration");
		return 1;
	}

	sci_serial_out(port, SCFCR,
		       (sci_serial_in(port, SCFCR) &
			~(SCFCR_RTRG1 | SCFCR_RTRG0)) | bits);

	return rx_trig;
}

static int scif_rtrg_enabled(struct uart_port *port)
{
	if (sci_getreg(port, HSRTRGR)->size)
		return sci_serial_in(port, HSRTRGR) != 0;
	else
		return (sci_serial_in(port, SCFCR) &
			(SCFCR_RTRG0 | SCFCR_RTRG1)) != 0;
}

static void rx_fifo_timer_fn(struct timer_list *t)
{
	struct sci_port *s = timer_container_of(s, t, rx_fifo_timer);
	struct uart_port *port = &s->port;

	dev_dbg(port->dev, "Rx timed out\n");
	s->ops->set_rtrg(port, 1);
}

static ssize_t rx_fifo_trigger_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct uart_port *port = dev_get_drvdata(dev);
	struct sci_port *sci = to_sci_port(port);

	return sprintf(buf, "%d\n", sci->rx_trigger);
}

static ssize_t rx_fifo_trigger_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct uart_port *port = dev_get_drvdata(dev);
	struct sci_port *sci = to_sci_port(port);
	int ret;
	long r;

	ret = kstrtol(buf, 0, &r);
	if (ret)
		return ret;

	sci->rx_trigger = sci->ops->set_rtrg(port, r);
	if (sci->type == PORT_SCIFA || sci->type == PORT_SCIFB)
		sci->ops->set_rtrg(port, 1);

	return count;
}

static DEVICE_ATTR_RW(rx_fifo_trigger);

static ssize_t rx_fifo_timeout_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	struct uart_port *port = dev_get_drvdata(dev);
	struct sci_port *sci = to_sci_port(port);
	int v;

	if (sci->type == PORT_HSCIF)
		v = sci->hscif_tot >> HSSCR_TOT_SHIFT;
	else
		v = sci->rx_fifo_timeout;

	return sprintf(buf, "%d\n", v);
}

static ssize_t rx_fifo_timeout_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf,
				size_t count)
{
	struct uart_port *port = dev_get_drvdata(dev);
	struct sci_port *sci = to_sci_port(port);
	int ret;
	long r;

	ret = kstrtol(buf, 0, &r);
	if (ret)
		return ret;

	if (sci->type == PORT_HSCIF) {
		if (r < 0 || r > 3)
			return -EINVAL;
		sci->hscif_tot = r << HSSCR_TOT_SHIFT;
	} else {
		sci->rx_fifo_timeout = r;
		sci->ops->set_rtrg(port, 1);
		if (r > 0)
			timer_setup(&sci->rx_fifo_timer, rx_fifo_timer_fn, 0);
	}

	return count;
}

static DEVICE_ATTR_RW(rx_fifo_timeout);


#ifdef CONFIG_SERIAL_SH_SCI_DMA
static void sci_dma_tx_complete(void *arg)
{
	struct sci_port *s = arg;
	struct uart_port *port = &s->port;
	struct tty_port *tport = &port->state->port;
	unsigned long flags;

	dev_dbg(port->dev, "%s(%d)\n", __func__, port->line);

	uart_port_lock_irqsave(port, &flags);

	uart_xmit_advance(port, s->tx_dma_len);

	if (kfifo_len(&tport->xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	s->tx_occurred = true;

	if (!kfifo_is_empty(&tport->xmit_fifo)) {
		s->cookie_tx = 0;
		schedule_work(&s->work_tx);
	} else {
		s->cookie_tx = -EINVAL;
		if (s->type == PORT_SCIFA || s->type == PORT_SCIFB ||
		    s->regtype == SCIx_RZ_SCIFA_REGTYPE) {
			u16 ctrl = sci_serial_in(port, SCSCR);
			sci_serial_out(port, SCSCR, ctrl & ~SCSCR_TIE);
			if (s->regtype == SCIx_RZ_SCIFA_REGTYPE) {
				/* Switch irq from DMA to SCIF */
				dmaengine_pause(s->chan_tx_saved);
				enable_irq(s->irqs[SCIx_TXI_IRQ]);
			}
		}
	}

	uart_port_unlock_irqrestore(port, flags);
}

/* Locking: called with port lock held */
static int sci_dma_rx_push(struct sci_port *s, void *buf, size_t count)
{
	struct uart_port *port = &s->port;
	struct tty_port *tport = &port->state->port;
	int copied;

	copied = tty_insert_flip_string(tport, buf, count);
	if (copied < count)
		port->icount.buf_overrun++;

	port->icount.rx += copied;

	return copied;
}

static int sci_dma_rx_find_active(struct sci_port *s)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(s->cookie_rx); i++)
		if (s->active_rx == s->cookie_rx[i])
			return i;

	return -1;
}

/* Must only be called with uart_port_lock taken */
static void sci_dma_rx_chan_invalidate(struct sci_port *s)
{
	unsigned int i;

	s->chan_rx = NULL;
	for (i = 0; i < ARRAY_SIZE(s->cookie_rx); i++)
		s->cookie_rx[i] = -EINVAL;
	s->active_rx = 0;
}

static void sci_dma_rx_release(struct sci_port *s)
{
	struct dma_chan *chan = s->chan_rx_saved;
	struct uart_port *port = &s->port;
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);
	s->chan_rx_saved = NULL;
	sci_dma_rx_chan_invalidate(s);
	uart_port_unlock_irqrestore(port, flags);

	dmaengine_terminate_sync(chan);
	dma_free_coherent(chan->device->dev, s->buf_len_rx * 2, s->rx_buf[0],
			  sg_dma_address(&s->sg_rx[0]));
	dma_release_channel(chan);
}

static void start_hrtimer_us(struct hrtimer *hrt, unsigned long usec)
{
	long sec = usec / 1000000;
	long nsec = (usec % 1000000) * 1000;
	ktime_t t = ktime_set(sec, nsec);

	hrtimer_start(hrt, t, HRTIMER_MODE_REL);
}

static void sci_dma_rx_reenable_irq(struct sci_port *s)
{
	struct uart_port *port = &s->port;
	u16 scr;

	/* Direct new serial port interrupts back to CPU */
	scr = sci_serial_in(port, SCSCR);
	if (s->type == PORT_SCIFA || s->type == PORT_SCIFB ||
	    s->regtype == SCIx_RZ_SCIFA_REGTYPE) {
		enable_irq(s->irqs[SCIx_RXI_IRQ]);
		if (s->regtype == SCIx_RZ_SCIFA_REGTYPE)
			s->ops->set_rtrg(port, s->rx_trigger);
		else
			scr &= ~SCSCR_RDRQE;
	}
	sci_serial_out(port, SCSCR, scr | SCSCR_RIE);
}

static void sci_dma_rx_complete(void *arg)
{
	struct sci_port *s = arg;
	struct dma_chan *chan = s->chan_rx;
	struct uart_port *port = &s->port;
	struct dma_async_tx_descriptor *desc;
	unsigned long flags;
	int active, count = 0;

	dev_dbg(port->dev, "%s(%d) active cookie %d\n", __func__, port->line,
		s->active_rx);

	hrtimer_cancel(&s->rx_timer);

	uart_port_lock_irqsave(port, &flags);

	active = sci_dma_rx_find_active(s);
	if (active >= 0)
		count = sci_dma_rx_push(s, s->rx_buf[active], s->buf_len_rx);

	if (count)
		tty_flip_buffer_push(&port->state->port);

	desc = dmaengine_prep_slave_sg(s->chan_rx, &s->sg_rx[active], 1,
				       DMA_DEV_TO_MEM,
				       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		goto fail;

	desc->callback = sci_dma_rx_complete;
	desc->callback_param = s;
	s->cookie_rx[active] = dmaengine_submit(desc);
	if (dma_submit_error(s->cookie_rx[active]))
		goto fail;

	s->active_rx = s->cookie_rx[!active];

	dma_async_issue_pending(chan);

	uart_port_unlock_irqrestore(port, flags);
	dev_dbg(port->dev, "%s: cookie %d #%d, new active cookie %d\n",
		__func__, s->cookie_rx[active], active, s->active_rx);

	start_hrtimer_us(&s->rx_timer, s->rx_timeout);

	return;

fail:
	/* Switch to PIO */
	dmaengine_terminate_async(chan);
	sci_dma_rx_chan_invalidate(s);
	sci_dma_rx_reenable_irq(s);
	uart_port_unlock_irqrestore(port, flags);
	dev_warn(port->dev, "Failed submitting Rx DMA descriptor\n");
}

static void sci_dma_tx_release(struct sci_port *s)
{
	struct dma_chan *chan = s->chan_tx_saved;

	cancel_work_sync(&s->work_tx);
	s->chan_tx_saved = s->chan_tx = NULL;
	s->cookie_tx = -EINVAL;
	dmaengine_terminate_sync(chan);
	dma_unmap_single(chan->device->dev, s->tx_dma_addr, UART_XMIT_SIZE,
			 DMA_TO_DEVICE);
	dma_release_channel(chan);
}

static int sci_dma_rx_submit(struct sci_port *s, bool port_lock_held)
{
	struct dma_chan *chan = s->chan_rx;
	struct uart_port *port = &s->port;
	unsigned long flags;
	int i;

	for (i = 0; i < 2; i++) {
		struct scatterlist *sg = &s->sg_rx[i];
		struct dma_async_tx_descriptor *desc;

		desc = dmaengine_prep_slave_sg(chan,
			sg, 1, DMA_DEV_TO_MEM,
			DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
		if (!desc)
			goto fail;

		desc->callback = sci_dma_rx_complete;
		desc->callback_param = s;
		s->cookie_rx[i] = dmaengine_submit(desc);
		if (dma_submit_error(s->cookie_rx[i]))
			goto fail;

	}

	s->active_rx = s->cookie_rx[0];

	dma_async_issue_pending(chan);
	return 0;

fail:
	/* Switch to PIO */
	if (!port_lock_held)
		uart_port_lock_irqsave(port, &flags);
	if (i)
		dmaengine_terminate_async(chan);
	sci_dma_rx_chan_invalidate(s);
	sci_start_rx(port);
	if (!port_lock_held)
		uart_port_unlock_irqrestore(port, flags);
	return -EAGAIN;
}

static void sci_dma_tx_work_fn(struct work_struct *work)
{
	struct sci_port *s = container_of(work, struct sci_port, work_tx);
	struct dma_async_tx_descriptor *desc;
	struct dma_chan *chan = s->chan_tx;
	struct uart_port *port = &s->port;
	struct tty_port *tport = &port->state->port;
	unsigned long flags;
	unsigned int tail;
	dma_addr_t buf;

	/*
	 * DMA is idle now.
	 * Port xmit buffer is already mapped, and it is one page... Just adjust
	 * offsets and lengths. Since it is a circular buffer, we have to
	 * transmit till the end, and then the rest. Take the port lock to get a
	 * consistent xmit buffer state.
	 */
	uart_port_lock_irq(port);
	s->tx_dma_len = kfifo_out_linear(&tport->xmit_fifo, &tail,
			UART_XMIT_SIZE);
	buf = s->tx_dma_addr + tail;
	if (!s->tx_dma_len) {
		/* Transmit buffer has been flushed */
		uart_port_unlock_irq(port);
		return;
	}

	desc = dmaengine_prep_slave_single(chan, buf, s->tx_dma_len,
					   DMA_MEM_TO_DEV,
					   DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		uart_port_unlock_irq(port);
		dev_warn(port->dev, "Failed preparing Tx DMA descriptor\n");
		goto switch_to_pio;
	}

	dma_sync_single_for_device(chan->device->dev, buf, s->tx_dma_len,
				   DMA_TO_DEVICE);

	desc->callback = sci_dma_tx_complete;
	desc->callback_param = s;
	s->cookie_tx = dmaengine_submit(desc);
	if (dma_submit_error(s->cookie_tx)) {
		uart_port_unlock_irq(port);
		dev_warn(port->dev, "Failed submitting Tx DMA descriptor\n");
		goto switch_to_pio;
	}

	uart_port_unlock_irq(port);
	dev_dbg(port->dev, "%s: %p: %u, cookie %d\n",
		__func__, tport->xmit_buf, tail, s->cookie_tx);

	dma_async_issue_pending(chan);
	return;

switch_to_pio:
	uart_port_lock_irqsave(port, &flags);
	s->chan_tx = NULL;
	sci_start_tx(port);
	uart_port_unlock_irqrestore(port, flags);
	return;
}

static enum hrtimer_restart sci_dma_rx_timer_fn(struct hrtimer *t)
{
	struct sci_port *s = container_of(t, struct sci_port, rx_timer);
	struct dma_chan *chan = s->chan_rx;
	struct uart_port *port = &s->port;
	struct dma_tx_state state;
	enum dma_status status;
	unsigned long flags;
	unsigned int read;
	int active, count;

	dev_dbg(port->dev, "DMA Rx timed out\n");

	uart_port_lock_irqsave(port, &flags);

	active = sci_dma_rx_find_active(s);
	if (active < 0) {
		uart_port_unlock_irqrestore(port, flags);
		return HRTIMER_NORESTART;
	}

	status = dmaengine_tx_status(s->chan_rx, s->active_rx, &state);
	if (status == DMA_COMPLETE) {
		uart_port_unlock_irqrestore(port, flags);
		dev_dbg(port->dev, "Cookie %d #%d has already completed\n",
			s->active_rx, active);

		/* Let packet complete handler take care of the packet */
		return HRTIMER_NORESTART;
	}

	dmaengine_pause(chan);

	/*
	 * sometimes DMA transfer doesn't stop even if it is stopped and
	 * data keeps on coming until transaction is complete so check
	 * for DMA_COMPLETE again
	 * Let packet complete handler take care of the packet
	 */
	status = dmaengine_tx_status(s->chan_rx, s->active_rx, &state);
	if (status == DMA_COMPLETE) {
		uart_port_unlock_irqrestore(port, flags);
		dev_dbg(port->dev, "Transaction complete after DMA engine was stopped");
		return HRTIMER_NORESTART;
	}

	/* Handle incomplete DMA receive */
	dmaengine_terminate_async(s->chan_rx);
	read = sg_dma_len(&s->sg_rx[active]) - state.residue;

	if (read) {
		count = sci_dma_rx_push(s, s->rx_buf[active], read);
		if (count)
			tty_flip_buffer_push(&port->state->port);
	}

	if (s->type == PORT_SCIFA || s->type == PORT_SCIFB ||
	    s->regtype == SCIx_RZ_SCIFA_REGTYPE)
		sci_dma_rx_submit(s, true);

	sci_dma_rx_reenable_irq(s);

	uart_port_unlock_irqrestore(port, flags);

	return HRTIMER_NORESTART;
}

static struct dma_chan *sci_request_dma_chan(struct uart_port *port,
					     enum dma_transfer_direction dir)
{
	struct dma_chan *chan;
	struct dma_slave_config cfg;
	int ret;

	chan = dma_request_chan(port->dev, dir == DMA_MEM_TO_DEV ? "tx" : "rx");
	if (IS_ERR(chan)) {
		dev_dbg(port->dev, "dma_request_chan failed\n");
		return NULL;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.direction = dir;
	cfg.dst_addr = port->mapbase +
		(sci_getreg(port, SCxTDR)->offset << port->regshift);
	cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	cfg.src_addr = port->mapbase +
		(sci_getreg(port, SCxRDR)->offset << port->regshift);
	cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;

	ret = dmaengine_slave_config(chan, &cfg);
	if (ret) {
		dev_warn(port->dev, "dmaengine_slave_config failed %d\n", ret);
		dma_release_channel(chan);
		return NULL;
	}

	return chan;
}

static void sci_request_dma(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	struct tty_port *tport = &port->state->port;
	struct dma_chan *chan;

	dev_dbg(port->dev, "%s: port %d\n", __func__, port->line);

	/*
	 * DMA on console may interfere with Kernel log messages which use
	 * plain putchar(). So, simply don't use it with a console.
	 */
	if (uart_console(port))
		return;

	if (!port->dev->of_node)
		return;

	s->cookie_tx = -EINVAL;

	/*
	 * Don't request a dma channel if no channel was specified
	 * in the device tree.
	 */
	if (!of_property_present(port->dev->of_node, "dmas"))
		return;

	chan = sci_request_dma_chan(port, DMA_MEM_TO_DEV);
	dev_dbg(port->dev, "%s: TX: got channel %p\n", __func__, chan);
	if (chan) {
		/* UART circular tx buffer is an aligned page. */
		s->tx_dma_addr = dma_map_single(chan->device->dev,
						tport->xmit_buf,
						UART_XMIT_SIZE,
						DMA_TO_DEVICE);
		if (dma_mapping_error(chan->device->dev, s->tx_dma_addr)) {
			dev_warn(port->dev, "Failed mapping Tx DMA descriptor\n");
			dma_release_channel(chan);
		} else {
			dev_dbg(port->dev, "%s: mapped %lu@%p to %pad\n",
				__func__, UART_XMIT_SIZE,
				tport->xmit_buf, &s->tx_dma_addr);

			INIT_WORK(&s->work_tx, sci_dma_tx_work_fn);
			s->chan_tx_saved = s->chan_tx = chan;
		}
	}

	chan = sci_request_dma_chan(port, DMA_DEV_TO_MEM);
	dev_dbg(port->dev, "%s: RX: got channel %p\n", __func__, chan);
	if (chan) {
		unsigned int i;
		dma_addr_t dma;
		void *buf;

		s->buf_len_rx = 2 * max_t(size_t, 16, port->fifosize);
		buf = dma_alloc_coherent(chan->device->dev, s->buf_len_rx * 2,
					 &dma, GFP_KERNEL);
		if (!buf) {
			dev_warn(port->dev,
				 "Failed to allocate Rx dma buffer, using PIO\n");
			dma_release_channel(chan);
			return;
		}

		for (i = 0; i < 2; i++) {
			struct scatterlist *sg = &s->sg_rx[i];

			sg_init_table(sg, 1);
			s->rx_buf[i] = buf;
			sg_dma_address(sg) = dma;
			sg_dma_len(sg) = s->buf_len_rx;

			buf += s->buf_len_rx;
			dma += s->buf_len_rx;
		}

		hrtimer_setup(&s->rx_timer, sci_dma_rx_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

		s->chan_rx_saved = s->chan_rx = chan;

		if (s->type == PORT_SCIFA || s->type == PORT_SCIFB ||
		    s->regtype == SCIx_RZ_SCIFA_REGTYPE)
			sci_dma_rx_submit(s, false);
	}
}

static void sci_free_dma(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);

	if (s->chan_tx_saved)
		sci_dma_tx_release(s);
	if (s->chan_rx_saved)
		sci_dma_rx_release(s);
}

static void sci_flush_buffer(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);

	/*
	 * In uart_flush_buffer(), the xmit circular buffer has just been
	 * cleared, so we have to reset tx_dma_len accordingly, and stop any
	 * pending transfers
	 */
	s->tx_dma_len = 0;
	if (s->chan_tx) {
		dmaengine_terminate_async(s->chan_tx);
		s->cookie_tx = -EINVAL;
	}
}

static void sci_dma_check_tx_occurred(struct sci_port *s)
{
	struct dma_tx_state state;
	enum dma_status status;

	if (!s->chan_tx)
		return;

	status = dmaengine_tx_status(s->chan_tx, s->cookie_tx, &state);
	if (status == DMA_COMPLETE || status == DMA_IN_PROGRESS)
		s->tx_occurred = true;
}
#else /* !CONFIG_SERIAL_SH_SCI_DMA */
static inline void sci_request_dma(struct uart_port *port)
{
}

static inline void sci_free_dma(struct uart_port *port)
{
}

static void sci_dma_check_tx_occurred(struct sci_port *s)
{
}

#define sci_flush_buffer	NULL
#endif /* !CONFIG_SERIAL_SH_SCI_DMA */

static irqreturn_t sci_rx_interrupt(int irq, void *ptr)
{
	struct uart_port *port = ptr;
	struct sci_port *s = to_sci_port(port);

#ifdef CONFIG_SERIAL_SH_SCI_DMA
	if (s->chan_rx) {
		u16 scr = sci_serial_in(port, SCSCR);
		u16 ssr = sci_serial_in(port, SCxSR);

		/* Disable future Rx interrupts */
		if (s->type == PORT_SCIFA || s->type == PORT_SCIFB ||
		    s->regtype == SCIx_RZ_SCIFA_REGTYPE) {
			disable_irq_nosync(s->irqs[SCIx_RXI_IRQ]);
			if (s->regtype == SCIx_RZ_SCIFA_REGTYPE) {
				s->ops->set_rtrg(port, 1);
				scr |= SCSCR_RIE;
			} else {
				scr |= SCSCR_RDRQE;
			}
		} else {
			if (sci_dma_rx_submit(s, false) < 0)
				goto handle_pio;

			scr &= ~SCSCR_RIE;
		}
		sci_serial_out(port, SCSCR, scr);
		/* Clear current interrupt */
		sci_serial_out(port, SCxSR,
			       ssr & ~(SCIF_DR | SCxSR_RDxF(port)));
		dev_dbg(port->dev, "Rx IRQ %lu: setup t-out in %u us\n",
			jiffies, s->rx_timeout);
		start_hrtimer_us(&s->rx_timer, s->rx_timeout);

		return IRQ_HANDLED;
	}

handle_pio:
#endif

	if (s->rx_trigger > 1 && s->rx_fifo_timeout > 0) {
		if (!s->ops->rtrg_enabled(port))
			s->ops->set_rtrg(port, s->rx_trigger);

		mod_timer(&s->rx_fifo_timer, jiffies + DIV_ROUND_UP(
			  s->rx_frame * HZ * s->rx_fifo_timeout, 1000000));
	}

	/* I think sci_receive_chars has to be called irrespective
	 * of whether the I_IXOFF is set, otherwise, how is the interrupt
	 * to be disabled?
	 */
	s->ops->receive_chars(port);

	return IRQ_HANDLED;
}

static irqreturn_t sci_tx_interrupt(int irq, void *ptr)
{
	struct uart_port *port = ptr;
	unsigned long flags;
	struct sci_port *s = to_sci_port(port);

	uart_port_lock_irqsave(port, &flags);
	s->ops->transmit_chars(port);
	uart_port_unlock_irqrestore(port, flags);

	return IRQ_HANDLED;
}

static irqreturn_t sci_tx_end_interrupt(int irq, void *ptr)
{
	struct uart_port *port = ptr;
	struct sci_port *s = to_sci_port(port);
	const struct sci_common_regs *regs = s->params->common_regs;
	unsigned long flags;
	u32 ctrl;

	if (s->type != PORT_SCI && s->type != SCI_PORT_RSCI)
		return sci_tx_interrupt(irq, ptr);

	uart_port_lock_irqsave(port, &flags);
	ctrl = s->ops->read_reg(port, regs->control) &
		~(s->params->param_bits->te_clear);
	s->ops->write_reg(port, regs->control, ctrl);
	uart_port_unlock_irqrestore(port, flags);

	return IRQ_HANDLED;
}

static irqreturn_t sci_br_interrupt(int irq, void *ptr)
{
	struct uart_port *port = ptr;
	struct sci_port *s = to_sci_port(port);

	/* Handle BREAKs */
	sci_handle_breaks(port);

	/* drop invalid character received before break was detected */
	sci_serial_in(port, SCxRDR);

	s->ops->clear_SCxSR(port, SCxSR_BREAK_CLEAR(port));

	return IRQ_HANDLED;
}

static irqreturn_t sci_er_interrupt(int irq, void *ptr)
{
	struct uart_port *port = ptr;
	struct sci_port *s = to_sci_port(port);

	if (s->irqs[SCIx_ERI_IRQ] == s->irqs[SCIx_BRI_IRQ]) {
		/* Break and Error interrupts are muxed */
		unsigned short ssr_status = sci_serial_in(port, SCxSR);

		/* Break Interrupt */
		if (ssr_status & SCxSR_BRK(port))
			sci_br_interrupt(irq, ptr);

		/* Break only? */
		if (!(ssr_status & SCxSR_ERRORS(port)))
			return IRQ_HANDLED;
	}

	/* Handle errors */
	if (s->type == PORT_SCI) {
		if (sci_handle_errors(port)) {
			/* discard character in rx buffer */
			sci_serial_in(port, SCxSR);
			s->ops->clear_SCxSR(port, SCxSR_RDxF_CLEAR(port));
		}
	} else {
		sci_handle_fifo_overrun(port);
		if (!s->chan_rx)
			s->ops->receive_chars(port);
	}

	s->ops->clear_SCxSR(port, SCxSR_ERROR_CLEAR(port));

	/* Kick the transmission */
	if (!s->chan_tx)
		sci_tx_interrupt(irq, ptr);

	return IRQ_HANDLED;
}

static irqreturn_t sci_mpxed_interrupt(int irq, void *ptr)
{
	unsigned short ssr_status, scr_status, err_enabled, orer_status = 0;
	struct uart_port *port = ptr;
	struct sci_port *s = to_sci_port(port);
	irqreturn_t ret = IRQ_NONE;

	ssr_status = sci_serial_in(port, SCxSR);
	scr_status = sci_serial_in(port, SCSCR);
	if (s->params->overrun_reg == SCxSR)
		orer_status = ssr_status;
	else if (sci_getreg(port, s->params->overrun_reg)->size)
		orer_status = sci_serial_in(port, s->params->overrun_reg);

	err_enabled = scr_status & port_rx_irq_mask(port);

	/* Tx Interrupt */
	if ((ssr_status & SCxSR_TDxE(port)) && (scr_status & SCSCR_TIE) &&
	    !s->chan_tx)
		ret = sci_tx_interrupt(irq, ptr);

	/*
	 * Rx Interrupt: if we're using DMA, the DMA controller clears RDF /
	 * DR flags
	 */
	if (((ssr_status & SCxSR_RDxF(port)) || s->chan_rx) &&
	    (scr_status & SCSCR_RIE))
		ret = sci_rx_interrupt(irq, ptr);

	/* Error Interrupt */
	if ((ssr_status & SCxSR_ERRORS(port)) && err_enabled)
		ret = sci_er_interrupt(irq, ptr);

	/* Break Interrupt */
	if (s->irqs[SCIx_ERI_IRQ] != s->irqs[SCIx_BRI_IRQ] &&
	    (ssr_status & SCxSR_BRK(port)) && err_enabled)
		ret = sci_br_interrupt(irq, ptr);

	/* Overrun Interrupt */
	if (orer_status & s->params->overrun_mask) {
		sci_handle_fifo_overrun(port);
		ret = IRQ_HANDLED;
	}

	return ret;
}

static const struct sci_irq_desc {
	const char	*desc;
	irq_handler_t	handler;
} sci_irq_desc[] = {
	/*
	 * Split out handlers, the default case.
	 */
	[SCIx_ERI_IRQ] = {
		.desc = "rx err",
		.handler = sci_er_interrupt,
	},

	[SCIx_RXI_IRQ] = {
		.desc = "rx full",
		.handler = sci_rx_interrupt,
	},

	[SCIx_TXI_IRQ] = {
		.desc = "tx empty",
		.handler = sci_tx_interrupt,
	},

	[SCIx_BRI_IRQ] = {
		.desc = "break",
		.handler = sci_br_interrupt,
	},

	[SCIx_DRI_IRQ] = {
		.desc = "rx ready",
		.handler = sci_rx_interrupt,
	},

	[SCIx_TEI_IRQ] = {
		.desc = "tx end",
		.handler = sci_tx_end_interrupt,
	},

	/*
	 * Special muxed handler.
	 */
	[SCIx_MUX_IRQ] = {
		.desc = "mux",
		.handler = sci_mpxed_interrupt,
	},
};

static int sci_request_irq(struct sci_port *port)
{
	struct uart_port *up = &port->port;
	int i, j, w, ret = 0;

	for (i = j = 0; i < SCIx_NR_IRQS; i++, j++) {
		const struct sci_irq_desc *desc;
		int irq;

		/* Check if already registered (muxed) */
		for (w = 0; w < i; w++)
			if (port->irqs[w] == port->irqs[i])
				w = i + 1;
		if (w > i)
			continue;

		if (SCIx_IRQ_IS_MUXED(port)) {
			i = SCIx_MUX_IRQ;
			irq = up->irq;
		} else {
			irq = port->irqs[i];

			/*
			 * Certain port types won't support all of the
			 * available interrupt sources.
			 */
			if (unlikely(irq < 0))
				continue;
		}

		desc = sci_irq_desc + i;
		port->irqstr[j] = kasprintf(GFP_KERNEL, "%s:%s",
					    dev_name(up->dev), desc->desc);
		if (!port->irqstr[j]) {
			ret = -ENOMEM;
			goto out_nomem;
		}

		ret = request_irq(irq, desc->handler, up->irqflags,
				  port->irqstr[j], port);
		if (unlikely(ret)) {
			dev_err(up->dev, "Can't allocate %s IRQ\n", desc->desc);
			goto out_noirq;
		}
	}

	return 0;

out_noirq:
	while (--i >= 0)
		free_irq(port->irqs[i], port);

out_nomem:
	while (--j >= 0)
		kfree(port->irqstr[j]);

	return ret;
}

static void sci_free_irq(struct sci_port *port)
{
	int i, j;

	/*
	 * Intentionally in reverse order so we iterate over the muxed
	 * IRQ first.
	 */
	for (i = 0; i < SCIx_NR_IRQS; i++) {
		int irq = port->irqs[i];

		/*
		 * Certain port types won't support all of the available
		 * interrupt sources.
		 */
		if (unlikely(irq < 0))
			continue;

		/* Check if already freed (irq was muxed) */
		for (j = 0; j < i; j++)
			if (port->irqs[j] == irq)
				j = i + 1;
		if (j > i)
			continue;

		free_irq(port->irqs[i], port);
		kfree(port->irqstr[i]);

		if (SCIx_IRQ_IS_MUXED(port)) {
			/* If there's only one IRQ, we're done. */
			return;
		}
	}
}

static unsigned int sci_tx_empty(struct uart_port *port)
{
	unsigned short status = sci_serial_in(port, SCxSR);
	unsigned short in_tx_fifo = sci_txfill(port);
	struct sci_port *s = to_sci_port(port);

	sci_dma_check_tx_occurred(s);

	if (!s->tx_occurred)
		return TIOCSER_TEMT;

	return (status & SCxSR_TEND(port)) && !in_tx_fifo ? TIOCSER_TEMT : 0;
}

static void sci_set_rts(struct uart_port *port, bool state)
{
	struct sci_port *s = to_sci_port(port);

	if (s->type == PORT_SCIFA || s->type == PORT_SCIFB) {
		u16 data = sci_serial_in(port, SCPDR);

		/* Active low */
		if (state)
			data &= ~SCPDR_RTSD;
		else
			data |= SCPDR_RTSD;
		sci_serial_out(port, SCPDR, data);

		/* RTS# is output */
		sci_serial_out(port, SCPCR,
			       sci_serial_in(port, SCPCR) | SCPCR_RTSC);
	} else if (sci_getreg(port, SCSPTR)->size) {
		u16 ctrl = sci_serial_in(port, SCSPTR);

		/* Active low */
		if (state)
			ctrl &= ~SCSPTR_RTSDT;
		else
			ctrl |= SCSPTR_RTSDT;
		sci_serial_out(port, SCSPTR, ctrl);
	}
}

static bool sci_get_cts(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);

	if (s->type == PORT_SCIFA || s->type == PORT_SCIFB) {
		/* Active low */
		return !(sci_serial_in(port, SCPDR) & SCPDR_CTSD);
	} else if (sci_getreg(port, SCSPTR)->size) {
		/* Active low */
		return !(sci_serial_in(port, SCSPTR) & SCSPTR_CTSDT);
	}

	return true;
}

/*
 * Modem control is a bit of a mixed bag for SCI(F) ports. Generally
 * CTS/RTS is supported in hardware by at least one port and controlled
 * via SCSPTR (SCxPCR for SCIFA/B parts), or external pins (presently
 * handled via the ->init_pins() op, which is a bit of a one-way street,
 * lacking any ability to defer pin control -- this will later be
 * converted over to the GPIO framework).
 *
 * Other modes (such as loopback) are supported generically on certain
 * port types, but not others. For these it's sufficient to test for the
 * existence of the support register and simply ignore the port type.
 */
static void sci_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct sci_port *s = to_sci_port(port);

	if (mctrl & TIOCM_LOOP) {
		const struct plat_sci_reg *reg;

		/*
		 * Standard loopback mode for SCFCR ports.
		 */
		reg = sci_getreg(port, SCFCR);
		if (reg->size)
			sci_serial_out(port, SCFCR,
				       sci_serial_in(port, SCFCR) | SCFCR_LOOP);
	}

	mctrl_gpio_set(s->gpios, mctrl);

	if (!s->has_rtscts)
		return;

	if (!(mctrl & TIOCM_RTS)) {
		/* Disable Auto RTS */
		if (s->regtype != SCIx_RZV2H_SCIF_REGTYPE)
			sci_serial_out(port, SCFCR,
				       sci_serial_in(port, SCFCR) & ~SCFCR_MCE);

		/* Clear RTS */
		sci_set_rts(port, 0);
	} else if (s->autorts) {
		if (s->type == PORT_SCIFA || s->type == PORT_SCIFB) {
			/* Enable RTS# pin function */
			sci_serial_out(port, SCPCR,
				sci_serial_in(port, SCPCR) & ~SCPCR_RTSC);
		}

		/* Enable Auto RTS */
		if (s->regtype != SCIx_RZV2H_SCIF_REGTYPE)
			sci_serial_out(port, SCFCR,
				       sci_serial_in(port, SCFCR) | SCFCR_MCE);
	} else {
		/* Set RTS */
		sci_set_rts(port, 1);
	}
}

static unsigned int sci_get_mctrl(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	struct mctrl_gpios *gpios = s->gpios;
	unsigned int mctrl = 0;

	mctrl_gpio_get(gpios, &mctrl);

	/*
	 * CTS/RTS is handled in hardware when supported, while nothing
	 * else is wired up.
	 */
	if (s->autorts) {
		if (sci_get_cts(port))
			mctrl |= TIOCM_CTS;
	} else if (!mctrl_gpio_to_gpiod(gpios, UART_GPIO_CTS)) {
		mctrl |= TIOCM_CTS;
	}
	if (!mctrl_gpio_to_gpiod(gpios, UART_GPIO_DSR))
		mctrl |= TIOCM_DSR;
	if (!mctrl_gpio_to_gpiod(gpios, UART_GPIO_DCD))
		mctrl |= TIOCM_CAR;

	return mctrl;
}

static void sci_enable_ms(struct uart_port *port)
{
	mctrl_gpio_enable_ms(to_sci_port(port)->gpios);
}

static void sci_break_ctl(struct uart_port *port, int break_state)
{
	unsigned short scscr, scsptr;
	unsigned long flags;

	/* check whether the port has SCSPTR */
	if (!sci_getreg(port, SCSPTR)->size) {
		/*
		 * Not supported by hardware. Most parts couple break and rx
		 * interrupts together, with break detection always enabled.
		 */
		return;
	}

	uart_port_lock_irqsave(port, &flags);
	scsptr = sci_serial_in(port, SCSPTR);
	scscr = sci_serial_in(port, SCSCR);

	if (break_state == -1) {
		scsptr = (scsptr | SCSPTR_SPB2IO) & ~SCSPTR_SPB2DT;
		scscr &= ~SCSCR_TE;
	} else {
		scsptr = (scsptr | SCSPTR_SPB2DT) & ~SCSPTR_SPB2IO;
		scscr |= SCSCR_TE;
	}

	sci_serial_out(port, SCSPTR, scsptr);
	sci_serial_out(port, SCSCR, scscr);
	uart_port_unlock_irqrestore(port, flags);
}

static void sci_shutdown_complete(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	u16 scr;

	scr = sci_serial_in(port, SCSCR);
	sci_serial_out(port, SCSCR,
		       scr & (SCSCR_CKE1 | SCSCR_CKE0 | s->hscif_tot));
}

int sci_startup(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	int ret;

	dev_dbg(port->dev, "%s(%d)\n", __func__, port->line);

	s->tx_occurred = false;
	sci_request_dma(port);

	ret = sci_request_irq(s);
	if (unlikely(ret < 0)) {
		sci_free_dma(port);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_NS_GPL(sci_startup, "SH_SCI");

void sci_shutdown(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	unsigned long flags;

	dev_dbg(port->dev, "%s(%d)\n", __func__, port->line);

	s->autorts = false;
	mctrl_gpio_disable_ms_sync(to_sci_port(port)->gpios);

	uart_port_lock_irqsave(port, &flags);
	s->port.ops->stop_rx(port);
	s->port.ops->stop_tx(port);
	s->ops->shutdown_complete(port);
	uart_port_unlock_irqrestore(port, flags);

#ifdef CONFIG_SERIAL_SH_SCI_DMA
	if (s->chan_rx_saved) {
		dev_dbg(port->dev, "%s(%d) deleting rx_timer\n", __func__,
			port->line);
		hrtimer_cancel(&s->rx_timer);
	}
#endif

	if (s->rx_trigger > 1 && s->rx_fifo_timeout > 0)
		timer_delete_sync(&s->rx_fifo_timer);
	sci_free_irq(s);
	sci_free_dma(port);
}
EXPORT_SYMBOL_NS_GPL(sci_shutdown, "SH_SCI");

static int sci_sck_calc(struct sci_port *s, unsigned int bps,
			unsigned int *srr)
{
	unsigned long freq = s->clk_rates[SCI_SCK];
	int err, min_err = INT_MAX;
	unsigned int sr;

	if (s->type != PORT_HSCIF)
		freq *= 2;

	for_each_sr(sr, s) {
		err = DIV_ROUND_CLOSEST(freq, sr) - bps;
		if (abs(err) >= abs(min_err))
			continue;

		min_err = err;
		*srr = sr - 1;

		if (!err)
			break;
	}

	dev_dbg(s->port.dev, "SCK: %u%+d bps using SR %u\n", bps, min_err,
		*srr + 1);
	return min_err;
}

static int sci_brg_calc(struct sci_port *s, unsigned int bps,
			unsigned long freq, unsigned int *dlr,
			unsigned int *srr)
{
	int err, min_err = INT_MAX;
	unsigned int sr, dl;

	if (s->type != PORT_HSCIF)
		freq *= 2;

	for_each_sr(sr, s) {
		dl = DIV_ROUND_CLOSEST(freq, sr * bps);
		dl = clamp(dl, 1U, 65535U);

		err = DIV_ROUND_CLOSEST(freq, sr * dl) - bps;
		if (abs(err) >= abs(min_err))
			continue;

		min_err = err;
		*dlr = dl;
		*srr = sr - 1;

		if (!err)
			break;
	}

	dev_dbg(s->port.dev, "BRG: %u%+d bps using DL %u SR %u\n", bps,
		min_err, *dlr, *srr + 1);
	return min_err;
}

/* calculate sample rate, BRR, and clock select */
static int sci_scbrr_calc(struct sci_port *s, unsigned int bps,
		   unsigned int *brr, unsigned int *srr,
		   unsigned int *cks)
{
	unsigned long freq = s->clk_rates[SCI_FCK];
	unsigned int sr, br, prediv, scrate, c;
	int err, min_err = INT_MAX;

	if (s->type != PORT_HSCIF)
		freq *= 2;

	/*
	 * Find the combination of sample rate and clock select with the
	 * smallest deviation from the desired baud rate.
	 * Prefer high sample rates to maximise the receive margin.
	 *
	 * M: Receive margin (%)
	 * N: Ratio of bit rate to clock (N = sampling rate)
	 * D: Clock duty (D = 0 to 1.0)
	 * L: Frame length (L = 9 to 12)
	 * F: Absolute value of clock frequency deviation
	 *
	 *  M = |(0.5 - 1 / 2 * N) - ((L - 0.5) * F) -
	 *      (|D - 0.5| / N * (1 + F))|
	 *  NOTE: Usually, treat D for 0.5, F is 0 by this calculation.
	 */
	for_each_sr(sr, s) {
		for (c = 0; c <= 3; c++) {
			/* integerized formulas from HSCIF documentation */
			prediv = sr << (2 * c + 1);

			/*
			 * We need to calculate:
			 *
			 *     br = freq / (prediv * bps) clamped to [1..256]
			 *     err = freq / (br * prediv) - bps
			 *
			 * Watch out for overflow when calculating the desired
			 * sampling clock rate!
			 */
			if (bps > UINT_MAX / prediv)
				break;

			scrate = prediv * bps;
			br = DIV_ROUND_CLOSEST(freq, scrate);
			br = clamp(br, 1U, 256U);

			err = DIV_ROUND_CLOSEST(freq, br * prediv) - bps;
			if (abs(err) >= abs(min_err))
				continue;

			min_err = err;
			*brr = br - 1;
			*srr = sr - 1;
			*cks = c;

			if (!err)
				goto found;
		}
	}

found:
	dev_dbg(s->port.dev, "BRR: %u%+d bps using N %u SR %u cks %u\n", bps,
		min_err, *brr, *srr + 1, *cks);
	return min_err;
}

static void sci_reset(struct uart_port *port)
{
	const struct plat_sci_reg *reg;
	unsigned int status;
	struct sci_port *s = to_sci_port(port);

	sci_serial_out(port, SCSCR, s->hscif_tot);	/* TE=0, RE=0, CKE1=0 */

	reg = sci_getreg(port, SCFCR);
	if (reg->size)
		sci_serial_out(port, SCFCR, SCFCR_RFRST | SCFCR_TFRST);

	s->ops->clear_SCxSR(port,
			    SCxSR_RDxF_CLEAR(port) & SCxSR_ERROR_CLEAR(port) &
			    SCxSR_BREAK_CLEAR(port));
	if (sci_getreg(port, SCLSR)->size) {
		status = sci_serial_in(port, SCLSR);
		status &= ~(SCLSR_TO | SCLSR_ORER);
		sci_serial_out(port, SCLSR, status);
	}

	if (s->rx_trigger > 1) {
		if (s->rx_fifo_timeout) {
			s->ops->set_rtrg(port, 1);
			timer_setup(&s->rx_fifo_timer, rx_fifo_timer_fn, 0);
		} else {
			if (s->type == PORT_SCIFA ||
			    s->type == PORT_SCIFB)
				s->ops->set_rtrg(port, 1);
			else
				s->ops->set_rtrg(port, s->rx_trigger);
		}
	}
}

static void sci_set_termios(struct uart_port *port, struct ktermios *termios,
		            const struct ktermios *old)
{
	unsigned int baud, smr_val = SCSMR_ASYNC, scr_val = 0, i, bits;
	unsigned int brr = 255, cks = 0, srr = 15, dl = 0, sccks = 0;
	unsigned int brr1 = 255, cks1 = 0, srr1 = 15, dl1 = 0;
	struct sci_port *s = to_sci_port(port);
	const struct plat_sci_reg *reg;
	int min_err = INT_MAX, err;
	unsigned long max_freq = 0;
	int best_clk = -1;
	unsigned long flags;

	if ((termios->c_cflag & CSIZE) == CS7) {
		smr_val |= SCSMR_CHR;
	} else {
		termios->c_cflag &= ~CSIZE;
		termios->c_cflag |= CS8;
	}
	if (termios->c_cflag & PARENB)
		smr_val |= SCSMR_PE;
	if (termios->c_cflag & PARODD)
		smr_val |= SCSMR_PE | SCSMR_ODD;
	if (termios->c_cflag & CSTOPB)
		smr_val |= SCSMR_STOP;

	/*
	 * earlyprintk comes here early on with port->uartclk set to zero.
	 * the clock framework is not up and running at this point so here
	 * we assume that 115200 is the maximum baud rate. please note that
	 * the baud rate is not programmed during earlyprintk - it is assumed
	 * that the previous boot loader has enabled required clocks and
	 * setup the baud rate generator hardware for us already.
	 */
	if (!port->uartclk) {
		baud = uart_get_baud_rate(port, termios, old, 0, 115200);
		goto done;
	}

	for (i = 0; i < SCI_NUM_CLKS; i++)
		max_freq = max(max_freq, s->clk_rates[i]);

	baud = uart_get_baud_rate(port, termios, old, 0, max_freq / min_sr(s));
	if (!baud)
		goto done;

	/*
	 * There can be multiple sources for the sampling clock.  Find the one
	 * that gives us the smallest deviation from the desired baud rate.
	 */

	/* Optional Undivided External Clock */
	if (s->clk_rates[SCI_SCK] && s->type != PORT_SCIFA &&
	    s->type != PORT_SCIFB) {
		err = sci_sck_calc(s, baud, &srr1);
		if (abs(err) < abs(min_err)) {
			best_clk = SCI_SCK;
			scr_val = SCSCR_CKE1;
			sccks = SCCKS_CKS;
			min_err = err;
			srr = srr1;
			if (!err)
				goto done;
		}
	}

	/* Optional BRG Frequency Divided External Clock */
	if (s->clk_rates[SCI_SCIF_CLK] && sci_getreg(port, SCDL)->size) {
		err = sci_brg_calc(s, baud, s->clk_rates[SCI_SCIF_CLK], &dl1,
				   &srr1);
		if (abs(err) < abs(min_err)) {
			best_clk = SCI_SCIF_CLK;
			scr_val = SCSCR_CKE1;
			sccks = 0;
			min_err = err;
			dl = dl1;
			srr = srr1;
			if (!err)
				goto done;
		}
	}

	/* Optional BRG Frequency Divided Internal Clock */
	if (s->clk_rates[SCI_BRG_INT] && sci_getreg(port, SCDL)->size) {
		err = sci_brg_calc(s, baud, s->clk_rates[SCI_BRG_INT], &dl1,
				   &srr1);
		if (abs(err) < abs(min_err)) {
			best_clk = SCI_BRG_INT;
			scr_val = SCSCR_CKE1;
			sccks = SCCKS_XIN;
			min_err = err;
			dl = dl1;
			srr = srr1;
			if (!min_err)
				goto done;
		}
	}

	/* Divided Functional Clock using standard Bit Rate Register */
	err = sci_scbrr_calc(s, baud, &brr1, &srr1, &cks1);
	if (abs(err) < abs(min_err)) {
		best_clk = SCI_FCK;
		scr_val = 0;
		min_err = err;
		brr = brr1;
		srr = srr1;
		cks = cks1;
	}

done:
	if (best_clk >= 0)
		dev_dbg(port->dev, "Using clk %pC for %u%+d bps\n",
			s->clks[best_clk], baud, min_err);

	sci_port_enable(s);

	/*
	 * Program the optional External Baud Rate Generator (BRG) first.
	 * It controls the mux to select (H)SCK or frequency divided clock.
	 */
	if (best_clk >= 0 && sci_getreg(port, SCCKS)->size) {
		sci_serial_out(port, SCDL, dl);
		sci_serial_out(port, SCCKS, sccks);
	}

	uart_port_lock_irqsave(port, &flags);

	sci_reset(port);

	uart_update_timeout(port, termios->c_cflag, baud);

	/* byte size and parity */
	bits = tty_get_frame_size(termios->c_cflag);

	if (sci_getreg(port, SEMR)->size)
		sci_serial_out(port, SEMR, 0);

	if (best_clk >= 0) {
		if (s->type == PORT_SCIFA || s->type == PORT_SCIFB)
			switch (srr + 1) {
			case 5:  smr_val |= SCSMR_SRC_5;  break;
			case 7:  smr_val |= SCSMR_SRC_7;  break;
			case 11: smr_val |= SCSMR_SRC_11; break;
			case 13: smr_val |= SCSMR_SRC_13; break;
			case 16: smr_val |= SCSMR_SRC_16; break;
			case 17: smr_val |= SCSMR_SRC_17; break;
			case 19: smr_val |= SCSMR_SRC_19; break;
			case 27: smr_val |= SCSMR_SRC_27; break;
			}
		smr_val |= cks;
		sci_serial_out(port, SCSCR, scr_val | s->hscif_tot);
		sci_serial_out(port, SCSMR, smr_val);
		sci_serial_out(port, SCBRR, brr);
		if (sci_getreg(port, HSSRR)->size) {
			unsigned int hssrr = srr | HSCIF_SRE;
			/* Calculate deviation from intended rate at the
			 * center of the last stop bit in sampling clocks.
			 */
			int last_stop = bits * 2 - 1;
			int deviation = DIV_ROUND_CLOSEST(min_err * last_stop *
							  (int)(srr + 1),
							  2 * (int)baud);

			if (abs(deviation) >= 2) {
				/* At least two sampling clocks off at the
				 * last stop bit; we can increase the error
				 * margin by shifting the sampling point.
				 */
				int shift = clamp(deviation / 2, -8, 7);

				hssrr |= (shift << HSCIF_SRHP_SHIFT) &
					 HSCIF_SRHP_MASK;
				hssrr |= HSCIF_SRDE;
			}
			sci_serial_out(port, HSSRR, hssrr);
		}

		/* Wait one bit interval */
		udelay((1000000 + (baud - 1)) / baud);
	} else {
		/* Don't touch the bit rate configuration */
		scr_val = s->cfg->scscr & (SCSCR_CKE1 | SCSCR_CKE0);
		smr_val |= sci_serial_in(port, SCSMR) &
			   (SCSMR_CKEDG | SCSMR_SRC_MASK | SCSMR_CKS);
		sci_serial_out(port, SCSCR, scr_val | s->hscif_tot);
		sci_serial_out(port, SCSMR, smr_val);
	}

	sci_init_pins(port, termios->c_cflag);

	port->status &= ~UPSTAT_AUTOCTS;
	s->autorts = false;
	reg = sci_getreg(port, SCFCR);
	if (reg->size) {
		unsigned short ctrl = sci_serial_in(port, SCFCR);

		if ((port->flags & UPF_HARD_FLOW) &&
		    (termios->c_cflag & CRTSCTS)) {
			/* There is no CTS interrupt to restart the hardware */
			port->status |= UPSTAT_AUTOCTS;
			/* MCE is enabled when RTS is raised */
			s->autorts = true;
		}

		/*
		 * As we've done a sci_reset() above, ensure we don't
		 * interfere with the FIFOs while toggling MCE. As the
		 * reset values could still be set, simply mask them out.
		 */
		ctrl &= ~(SCFCR_RFRST | SCFCR_TFRST);

		sci_serial_out(port, SCFCR, ctrl);
	}
	if (port->flags & UPF_HARD_FLOW) {
		/* Refresh (Auto) RTS */
		sci_set_mctrl(port, port->mctrl);
	}

	/*
	 * For SCI, TE (transmit enable) must be set after setting TIE
	 * (transmit interrupt enable) or in the same instruction to
	 * start the transmitting process. So skip setting TE here for SCI.
	 */
	if (s->type != PORT_SCI)
		scr_val |= SCSCR_TE;
	scr_val |= SCSCR_RE | (s->cfg->scscr & ~(SCSCR_CKE1 | SCSCR_CKE0));
	sci_serial_out(port, SCSCR, scr_val | s->hscif_tot);
	if ((srr + 1 == 5) &&
	    (s->type == PORT_SCIFA || s->type == PORT_SCIFB)) {
		/*
		 * In asynchronous mode, when the sampling rate is 1/5, first
		 * received data may become invalid on some SCIFA and SCIFB.
		 * To avoid this problem wait more than 1 serial data time (1
		 * bit time x serial data number) after setting SCSCR.RE = 1.
		 */
		udelay(DIV_ROUND_UP(10 * 1000000, baud));
	}

	/* Calculate delay for 2 DMA buffers (4 FIFO). */
	s->rx_frame = (10000 * bits) / (baud / 100);
#ifdef CONFIG_SERIAL_SH_SCI_DMA
	s->rx_timeout = s->buf_len_rx * 2 * s->rx_frame;
#endif

	if ((termios->c_cflag & CREAD) != 0)
		sci_start_rx(port);

	uart_port_unlock_irqrestore(port, flags);

	sci_port_disable(s);

	if (UART_ENABLE_MS(port, termios->c_cflag))
		sci_enable_ms(port);
}

void sci_pm(struct uart_port *port, unsigned int state,
		   unsigned int oldstate)
{
	struct sci_port *sci_port = to_sci_port(port);

	switch (state) {
	case UART_PM_STATE_OFF:
		sci_port_disable(sci_port);
		break;
	default:
		sci_port_enable(sci_port);
		break;
	}
}
EXPORT_SYMBOL_NS_GPL(sci_pm, "SH_SCI");

static const char *sci_type(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);

	switch (s->type) {
	case PORT_IRDA:
		return "irda";
	case PORT_SCI:
		return "sci";
	case PORT_SCIF:
		return "scif";
	case PORT_SCIFA:
		return "scifa";
	case PORT_SCIFB:
		return "scifb";
	case PORT_HSCIF:
		return "hscif";
	}

	return NULL;
}

static int sci_remap_port(struct uart_port *port)
{
	struct sci_port *sport = to_sci_port(port);

	/*
	 * Nothing to do if there's already an established membase.
	 */
	if (port->membase)
		return 0;

	if (port->dev->of_node || (port->flags & UPF_IOREMAP)) {
		port->membase = ioremap(port->mapbase, sport->reg_size);
		if (unlikely(!port->membase)) {
			dev_err(port->dev, "can't remap port#%d\n", port->line);
			return -ENXIO;
		}
	} else {
		/*
		 * For the simple (and majority of) cases where we don't
		 * need to do any remapping, just cast the cookie
		 * directly.
		 */
		port->membase = (void __iomem *)(uintptr_t)port->mapbase;
	}

	return 0;
}

void sci_release_port(struct uart_port *port)
{
	struct sci_port *sport = to_sci_port(port);

	if (port->dev->of_node || (port->flags & UPF_IOREMAP)) {
		iounmap(port->membase);
		port->membase = NULL;
	}

	release_mem_region(port->mapbase, sport->reg_size);
}
EXPORT_SYMBOL_NS_GPL(sci_release_port, "SH_SCI");

int sci_request_port(struct uart_port *port)
{
	struct resource *res;
	struct sci_port *sport = to_sci_port(port);
	int ret;

	res = request_mem_region(port->mapbase, sport->reg_size,
				 dev_name(port->dev));
	if (unlikely(res == NULL)) {
		dev_err(port->dev, "request_mem_region failed.");
		return -EBUSY;
	}

	ret = sci_remap_port(port);
	if (unlikely(ret != 0)) {
		release_resource(res);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_NS_GPL(sci_request_port, "SH_SCI");

void sci_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE) {
		struct sci_port *sport = to_sci_port(port);
		port->type = SCI_PUBLIC_PORT_ID(sport->type);
		sci_request_port(port);
	}
}
EXPORT_SYMBOL_NS_GPL(sci_config_port, "SH_SCI");

int sci_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	if (ser->baud_base < 2400)
		/* No paper tape reader for Mitch.. */
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(sci_verify_port, "SH_SCI");

static void sci_prepare_console_write(struct uart_port *port, u32 ctrl)
{
	struct sci_port *s = to_sci_port(port);
	u32 ctrl_temp =
		s->params->param_bits->rxtx_enable |
		(s->cfg->scscr & ~(SCSCR_CKE1 | SCSCR_CKE0)) |
		(ctrl & (SCSCR_CKE1 | SCSCR_CKE0)) |
		s->hscif_tot;
	sci_serial_out(port, SCSCR, ctrl_temp);
}

static void sci_console_save(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	struct sci_suspend_regs *regs = s->suspend_regs;

	if (sci_getreg(port, SCDL)->size)
		regs->scdl = sci_serial_in(port, SCDL);
	if (sci_getreg(port, SCCKS)->size)
		regs->sccks = sci_serial_in(port, SCCKS);
	if (sci_getreg(port, SCSMR)->size)
		regs->scsmr = sci_serial_in(port, SCSMR);
	if (sci_getreg(port, SCSCR)->size)
		regs->scscr = sci_serial_in(port, SCSCR);
	if (sci_getreg(port, SCFCR)->size)
		regs->scfcr = sci_serial_in(port, SCFCR);
	if (sci_getreg(port, SCSPTR)->size)
		regs->scsptr = sci_serial_in(port, SCSPTR);
	if (sci_getreg(port, SCBRR)->size)
		regs->scbrr = sci_serial_in(port, SCBRR);
	if (sci_getreg(port, HSSRR)->size)
		regs->hssrr = sci_serial_in(port, HSSRR);
	if (sci_getreg(port, SCPCR)->size)
		regs->scpcr = sci_serial_in(port, SCPCR);
	if (sci_getreg(port, SCPDR)->size)
		regs->scpdr = sci_serial_in(port, SCPDR);
	if (sci_getreg(port, SEMR)->size)
		regs->semr = sci_serial_in(port, SEMR);
}

static void sci_console_restore(struct uart_port *port)
{
	struct sci_port *s = to_sci_port(port);
	struct sci_suspend_regs *regs = s->suspend_regs;

	if (sci_getreg(port, SCDL)->size)
		sci_serial_out(port, SCDL, regs->scdl);
	if (sci_getreg(port, SCCKS)->size)
		sci_serial_out(port, SCCKS, regs->sccks);
	if (sci_getreg(port, SCSMR)->size)
		sci_serial_out(port, SCSMR, regs->scsmr);
	if (sci_getreg(port, SCSCR)->size)
		sci_serial_out(port, SCSCR, regs->scscr);
	if (sci_getreg(port, SCFCR)->size)
		sci_serial_out(port, SCFCR, regs->scfcr);
	if (sci_getreg(port, SCSPTR)->size)
		sci_serial_out(port, SCSPTR, regs->scsptr);
	if (sci_getreg(port, SCBRR)->size)
		sci_serial_out(port, SCBRR, regs->scbrr);
	if (sci_getreg(port, HSSRR)->size)
		sci_serial_out(port, HSSRR, regs->hssrr);
	if (sci_getreg(port, SCPCR)->size)
		sci_serial_out(port, SCPCR, regs->scpcr);
	if (sci_getreg(port, SCPDR)->size)
		sci_serial_out(port, SCPDR, regs->scpdr);
	if (sci_getreg(port, SEMR)->size)
		sci_serial_out(port, SEMR, regs->semr);
}

static const struct uart_ops sci_uart_ops = {
	.tx_empty	= sci_tx_empty,
	.set_mctrl	= sci_set_mctrl,
	.get_mctrl	= sci_get_mctrl,
	.start_tx	= sci_start_tx,
	.stop_tx	= sci_stop_tx,
	.stop_rx	= sci_stop_rx,
	.enable_ms	= sci_enable_ms,
	.break_ctl	= sci_break_ctl,
	.startup	= sci_startup,
	.shutdown	= sci_shutdown,
	.flush_buffer	= sci_flush_buffer,
	.set_termios	= sci_set_termios,
	.pm		= sci_pm,
	.type		= sci_type,
	.release_port	= sci_release_port,
	.request_port	= sci_request_port,
	.config_port	= sci_config_port,
	.verify_port	= sci_verify_port,
#ifdef CONFIG_CONSOLE_POLL
	.poll_get_char	= sci_poll_get_char,
	.poll_put_char	= sci_poll_put_char,
#endif
};

static const struct sci_port_ops sci_port_ops = {
	.read_reg		= sci_serial_in,
	.write_reg		= sci_serial_out,
	.clear_SCxSR		= sci_clear_SCxSR,
	.transmit_chars		= sci_transmit_chars,
	.receive_chars		= sci_receive_chars,
#if defined(CONFIG_SERIAL_SH_SCI_CONSOLE) || \
    defined(CONFIG_SERIAL_SH_SCI_EARLYCON)
	.poll_put_char		= sci_poll_put_char,
#endif
	.set_rtrg		= scif_set_rtrg,
	.rtrg_enabled		= scif_rtrg_enabled,
	.shutdown_complete	= sci_shutdown_complete,
	.prepare_console_write	= sci_prepare_console_write,
	.console_save		= sci_console_save,
	.console_restore	= sci_console_restore,
	.suspend_regs_size	= sci_suspend_regs_size,
};

static int sci_init_clocks(struct sci_port *sci_port, struct device *dev)
{
	const char *clk_names[] = {
		[SCI_FCK] = "fck",
		[SCI_SCK] = "sck",
		[SCI_BRG_INT] = "brg_int",
		[SCI_SCIF_CLK] = "scif_clk",
	};
	struct clk *clk;
	unsigned int i;

	if (sci_port->type == PORT_HSCIF) {
		clk_names[SCI_SCK] = "hsck";
	} else if (sci_port->type == SCI_PORT_RSCI) {
		clk_names[SCI_FCK] = "operation";
		clk_names[SCI_BRG_INT] = "bus";
	}

	for (i = 0; i < SCI_NUM_CLKS; i++) {
		const char *name = clk_names[i];

		clk = devm_clk_get_optional(dev, name);
		if (IS_ERR(clk))
			return PTR_ERR(clk);

		if (!clk && sci_port->type == SCI_PORT_RSCI &&
		    (i == SCI_FCK || i == SCI_BRG_INT)) {
			return dev_err_probe(dev, -ENODEV,
					     "failed to get %s\n",
					     name);
		}

		if (!clk && i == SCI_FCK) {
			/*
			 * Not all SH platforms declare a clock lookup entry
			 * for SCI devices, in which case we need to get the
			 * global "peripheral_clk" clock.
			 */
			clk = devm_clk_get(dev, "peripheral_clk");
			if (IS_ERR(clk))
				return dev_err_probe(dev, PTR_ERR(clk),
						     "failed to get %s\n",
						     name);
		}

		if (!clk)
			dev_dbg(dev, "failed to get %s\n", name);
		else
			dev_dbg(dev, "clk %s is %pC rate %lu\n", name,
				clk, clk_get_rate(clk));
		sci_port->clks[i] = clk;
	}
	return 0;
}

static const struct sci_port_params *
sci_probe_regmap(const struct plat_sci_port *cfg, struct sci_port *sci_port)
{
	unsigned int regtype;

	sci_port->ops = &sci_port_ops;
	sci_port->port.ops = &sci_uart_ops;

	if (cfg->regtype != SCIx_PROBE_REGTYPE)
		return &sci_port_params[cfg->regtype];

	switch (cfg->type) {
	case PORT_SCI:
		regtype = SCIx_SCI_REGTYPE;
		break;
	case PORT_IRDA:
		regtype = SCIx_IRDA_REGTYPE;
		break;
	case PORT_SCIFA:
		regtype = SCIx_SCIFA_REGTYPE;
		break;
	case PORT_SCIFB:
		regtype = SCIx_SCIFB_REGTYPE;
		break;
	case PORT_SCIF:
		/*
		 * The SH-4 is a bit of a misnomer here, although that's
		 * where this particular port layout originated. This
		 * configuration (or some slight variation thereof)
		 * remains the dominant model for all SCIFs.
		 */
		regtype = SCIx_SH4_SCIF_REGTYPE;
		break;
	case PORT_HSCIF:
		regtype = SCIx_HSCIF_REGTYPE;
		break;
	default:
		pr_err("Can't probe register map for given port\n");
		return NULL;
	}

	return &sci_port_params[regtype];
}

static int sci_init_single(struct platform_device *dev,
			   struct sci_port *sci_port, unsigned int index,
			   const struct plat_sci_port *p, bool early)
{
	struct uart_port *port = &sci_port->port;
	const struct resource *res;
	unsigned int i;
	int ret;

	sci_port->cfg	= p;

	sci_port->type	= p->type;
	sci_port->regtype = p->regtype;

	port->iotype	= UPIO_MEM;
	port->line	= index;
	port->has_sysrq = IS_ENABLED(CONFIG_SERIAL_SH_SCI_CONSOLE);

	res = platform_get_resource(dev, IORESOURCE_MEM, 0);
	if (res == NULL)
		return -ENOMEM;

	port->mapbase = res->start;
	sci_port->reg_size = resource_size(res);

	for (i = 0; i < ARRAY_SIZE(sci_port->irqs); ++i) {
		if (i)
			sci_port->irqs[i] = platform_get_irq_optional(dev, i);
		else
			sci_port->irqs[i] = platform_get_irq(dev, i);
	}

	/*
	 * The fourth interrupt on SCI and RSCI port is transmit end interrupt, so
	 * shuffle the interrupts.
	 */
	if (p->type == PORT_SCI || p->type == SCI_PORT_RSCI)
		swap(sci_port->irqs[SCIx_BRI_IRQ], sci_port->irqs[SCIx_TEI_IRQ]);

	/* The SCI generates several interrupts. They can be muxed together or
	 * connected to different interrupt lines. In the muxed case only one
	 * interrupt resource is specified as there is only one interrupt ID.
	 * In the non-muxed case, up to 6 interrupt signals might be generated
	 * from the SCI, however those signals might have their own individual
	 * interrupt ID numbers, or muxed together with another interrupt.
	 */
	if (sci_port->irqs[0] < 0)
		return -ENXIO;

	if (sci_port->irqs[1] < 0)
		for (i = 1; i < ARRAY_SIZE(sci_port->irqs); i++)
			sci_port->irqs[i] = sci_port->irqs[0];

	switch (p->type) {
	case PORT_SCIFB:
		sci_port->rx_trigger = 48;
		break;
	case PORT_HSCIF:
		sci_port->rx_trigger = 64;
		break;
	case PORT_SCIFA:
		sci_port->rx_trigger = 32;
		break;
	case PORT_SCIF:
		if (p->regtype == SCIx_SH7705_SCIF_REGTYPE)
			/* RX triggering not implemented for this IP */
			sci_port->rx_trigger = 1;
		else
			sci_port->rx_trigger = 8;
		break;
	case SCI_PORT_RSCI:
		sci_port->rx_trigger = 15;
		break;
	default:
		sci_port->rx_trigger = 1;
		break;
	}

	sci_port->rx_fifo_timeout = 0;
	sci_port->hscif_tot = 0;

	/* SCIFA on sh7723 and sh7724 need a custom sampling rate that doesn't
	 * match the SoC datasheet, this should be investigated. Let platform
	 * data override the sampling rate for now.
	 */
	sci_port->sampling_rate_mask = p->sampling_rate
				     ? SCI_SR(p->sampling_rate)
				     : sci_port->params->sampling_rate_mask;

	if (!early) {
		ret = sci_init_clocks(sci_port, &dev->dev);
		if (ret < 0)
			return ret;
	}

	port->type		= SCI_PUBLIC_PORT_ID(p->type);
	port->flags		= UPF_FIXED_PORT | UPF_BOOT_AUTOCONF | p->flags;
	port->fifosize		= sci_port->params->fifosize;

	if (p->type == PORT_SCI && !dev->dev.of_node) {
		if (sci_port->reg_size >= 0x20)
			port->regshift = 2;
		else
			port->regshift = 1;
	}

	/*
	 * The UART port needs an IRQ value, so we peg this to the RX IRQ
	 * for the multi-IRQ ports, which is where we are primarily
	 * concerned with the shutdown path synchronization.
	 *
	 * For the muxed case there's nothing more to do.
	 */
	port->irq		= sci_port->irqs[SCIx_RXI_IRQ];
	port->irqflags		= 0;

	return 0;
}

#if defined(CONFIG_SERIAL_SH_SCI_CONSOLE) || \
    defined(CONFIG_SERIAL_SH_SCI_EARLYCON)
static void serial_console_putchar(struct uart_port *port, unsigned char ch)
{
	to_sci_port(port)->ops->poll_put_char(port, ch);
}

/*
 *	Print a string to the serial port trying not to disturb
 *	any possible real use of the port...
 */
static void serial_console_write(struct console *co, const char *s,
				 unsigned count)
{
	struct sci_port *sci_port = &sci_ports[co->index];
	struct uart_port *port = &sci_port->port;
	const struct sci_common_regs *regs = sci_port->params->common_regs;
	unsigned int bits;
	u32 ctrl;
	unsigned long flags;
	int locked = 1;

	if (port->sysrq)
		locked = 0;
	else if (oops_in_progress)
		locked = uart_port_trylock_irqsave(port, &flags);
	else
		uart_port_lock_irqsave(port, &flags);

	/* first save SCSCR then disable interrupts, keep clock source */

	ctrl = sci_port->ops->read_reg(port, regs->control);
	sci_port->ops->prepare_console_write(port, ctrl);

	uart_console_write(port, s, count, serial_console_putchar);

	/* wait until fifo is empty and last bit has been transmitted */

	bits = sci_port->params->param_bits->poll_sent_bits;

	while ((sci_port->ops->read_reg(port, regs->status) & bits) != bits)
		cpu_relax();

	/* restore the SCSCR */
	sci_port->ops->write_reg(port, regs->control, ctrl);

	if (locked)
		uart_port_unlock_irqrestore(port, flags);
}

static int serial_console_setup(struct console *co, char *options)
{
	struct sci_port *sci_port;
	struct uart_port *port;
	int baud = 115200;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';
	int ret;

	/*
	 * Refuse to handle any bogus ports.
	 */
	if (co->index < 0 || co->index >= SCI_NPORTS)
		return -ENODEV;

	sci_port = &sci_ports[co->index];
	port = &sci_port->port;

	/*
	 * Refuse to handle uninitialized ports.
	 */
	if (!port->ops)
		return -ENODEV;

	ret = sci_remap_port(port);
	if (unlikely(ret != 0))
		return ret;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(port, co, baud, parity, bits, flow);
}

static struct console serial_console = {
	.name		= "ttySC",
	.device		= uart_console_device,
	.write		= serial_console_write,
	.setup		= serial_console_setup,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
	.data		= &sci_uart_driver,
};

#ifdef CONFIG_SUPERH
static char early_serial_buf[32];

static int early_serial_console_setup(struct console *co, char *options)
{
	/*
	 * This early console is always registered using the earlyprintk=
	 * parameter, which does not call add_preferred_console(). Thus
	 * @options is always NULL and the options for this early console
	 * are passed using a custom buffer.
	 */
	WARN_ON(options);

	return serial_console_setup(co, early_serial_buf);
}

static struct console early_serial_console = {
	.name           = "early_ttySC",
	.write          = serial_console_write,
	.setup		= early_serial_console_setup,
	.flags          = CON_PRINTBUFFER,
	.index		= -1,
};

static int sci_probe_earlyprintk(struct platform_device *pdev)
{
	const struct plat_sci_port *cfg = dev_get_platdata(&pdev->dev);
	struct sci_port *sp = &sci_ports[pdev->id];

	if (early_serial_console.data)
		return -EEXIST;

	early_serial_console.index = pdev->id;

	sp->params = sci_probe_regmap(cfg, sp);
	if (!sp->params)
		return -ENODEV;

	sci_init_single(pdev, sp, pdev->id, cfg, true);

	if (!strstr(early_serial_buf, "keep"))
		early_serial_console.flags |= CON_BOOT;

	register_console(&early_serial_console);
	return 0;
}
#endif

#define SCI_CONSOLE	(&serial_console)

#else
static inline int sci_probe_earlyprintk(struct platform_device *pdev)
{
	return -EINVAL;
}

#define SCI_CONSOLE	NULL

#endif /* CONFIG_SERIAL_SH_SCI_CONSOLE || CONFIG_SERIAL_SH_SCI_EARLYCON */

static const char banner[] __initconst = "SuperH (H)SCI(F) driver initialized";

static DEFINE_MUTEX(sci_uart_registration_lock);
static struct uart_driver sci_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "sci",
	.dev_name	= "ttySC",
	.major		= SCI_MAJOR,
	.minor		= SCI_MINOR_START,
	.nr		= SCI_NPORTS,
	.cons		= SCI_CONSOLE,
};

static void sci_remove(struct platform_device *dev)
{
	struct sci_port *s = platform_get_drvdata(dev);
	unsigned int type = s->type;	/* uart_remove_... clears it */

	sci_ports_in_use &= ~BIT(s->port.line);
	uart_remove_one_port(&sci_uart_driver, &s->port);

	if (s->port.fifosize > 1)
		device_remove_file(&dev->dev, &dev_attr_rx_fifo_trigger);
	if (type == PORT_SCIFA || type == PORT_SCIFB || type == PORT_HSCIF ||
	    type == SCI_PORT_RSCI)
		device_remove_file(&dev->dev, &dev_attr_rx_fifo_timeout);
}

static const struct sci_of_data of_sci_scif_sh2 = {
	.type = PORT_SCIF,
	.regtype = SCIx_SH2_SCIF_FIFODATA_REGTYPE,
	.ops = &sci_port_ops,
	.uart_ops = &sci_uart_ops,
	.params = &sci_port_params[SCIx_SH2_SCIF_FIFODATA_REGTYPE],
};

static const struct sci_of_data of_sci_scif_rz_scifa = {
	.type = PORT_SCIF,
	.regtype = SCIx_RZ_SCIFA_REGTYPE,
	.ops = &sci_port_ops,
	.uart_ops = &sci_uart_ops,
	.params = &sci_port_params[SCIx_RZ_SCIFA_REGTYPE],
};

static const struct sci_of_data of_sci_scif_rzv2h = {
	.type = PORT_SCIF,
	.regtype = SCIx_RZV2H_SCIF_REGTYPE,
	.ops = &sci_port_ops,
	.uart_ops = &sci_uart_ops,
	.params = &sci_port_params[SCIx_RZV2H_SCIF_REGTYPE],
};

static const struct sci_of_data of_sci_rcar_scif = {
	.type = PORT_SCIF,
	.regtype = SCIx_SH4_SCIF_BRG_REGTYPE,
	.ops = &sci_port_ops,
	.uart_ops = &sci_uart_ops,
	.params = &sci_port_params[SCIx_SH4_SCIF_BRG_REGTYPE],
};

static const struct sci_of_data of_sci_scif_sh4 = {
	.type = PORT_SCIF,
	.regtype = SCIx_SH4_SCIF_REGTYPE,
	.ops = &sci_port_ops,
	.uart_ops = &sci_uart_ops,
	.params = &sci_port_params[SCIx_SH4_SCIF_REGTYPE],
};

static const struct sci_of_data of_sci_scifa = {
	.type = PORT_SCIFA,
	.regtype = SCIx_SCIFA_REGTYPE,
	.ops = &sci_port_ops,
	.uart_ops = &sci_uart_ops,
	.params = &sci_port_params[SCIx_SCIFA_REGTYPE],
};

static const struct sci_of_data of_sci_scifb = {
	.type = PORT_SCIFB,
	.regtype = SCIx_SCIFB_REGTYPE,
	.ops = &sci_port_ops,
	.uart_ops = &sci_uart_ops,
	.params = &sci_port_params[SCIx_SCIFB_REGTYPE],
};

static const struct sci_of_data of_sci_hscif = {
	.type = PORT_HSCIF,
	.regtype = SCIx_HSCIF_REGTYPE,
	.ops = &sci_port_ops,
	.uart_ops = &sci_uart_ops,
	.params = &sci_port_params[SCIx_HSCIF_REGTYPE],
};

static const struct sci_of_data of_sci_sci = {
	.type = PORT_SCI,
	.regtype = SCIx_SCI_REGTYPE,
	.ops = &sci_port_ops,
	.uart_ops = &sci_uart_ops,
	.params = &sci_port_params[SCIx_SCI_REGTYPE],
};

static const struct of_device_id of_sci_match[] __maybe_unused = {
	/* SoC-specific types */
	{
		.compatible = "renesas,scif-r7s72100",
		.data = &of_sci_scif_sh2,
	},
	{
		.compatible = "renesas,scif-r7s9210",
		.data = &of_sci_scif_rz_scifa,
	},
	{
		.compatible = "renesas,scif-r9a07g044",
		.data = &of_sci_scif_rz_scifa,
	},
	{
		.compatible = "renesas,scif-r9a09g057",
		.data = &of_sci_scif_rzv2h,
	},
#ifdef CONFIG_SERIAL_RSCI
	{
		.compatible = "renesas,r9a09g077-rsci",
		.data = &of_sci_rsci_data,
	},
#endif	/* CONFIG_SERIAL_RSCI */
	/* Family-specific types */
	{
		.compatible = "renesas,rcar-gen1-scif",
		.data = &of_sci_rcar_scif,
	}, {
		.compatible = "renesas,rcar-gen2-scif",
		.data = &of_sci_rcar_scif,
	}, {
		.compatible = "renesas,rcar-gen3-scif",
		.data = &of_sci_rcar_scif
	}, {
		.compatible = "renesas,rcar-gen4-scif",
		.data = &of_sci_rcar_scif
	}, {
		.compatible = "renesas,rcar-gen5-scif",
		.data = &of_sci_rcar_scif
	},
	/* Generic types */
	{
		.compatible = "renesas,scif",
		.data = &of_sci_scif_sh4,
	}, {
		.compatible = "renesas,scifa",
		.data = &of_sci_scifa,
	}, {
		.compatible = "renesas,scifb",
		.data = &of_sci_scifb,
	}, {
		.compatible = "renesas,hscif",
		.data = &of_sci_hscif,
	}, {
		.compatible = "renesas,sci",
		.data = &of_sci_sci,
	}, {
		/* Terminator */
	},
};
MODULE_DEVICE_TABLE(of, of_sci_match);

static void sci_reset_control_assert(void *data)
{
	reset_control_assert(data);
}

static struct plat_sci_port *sci_parse_dt(struct platform_device *pdev,
					  unsigned int *dev_id)
{
	struct device_node *np = pdev->dev.of_node;
	struct reset_control *rstc;
	struct plat_sci_port *p;
	struct sci_port *sp;
	const struct sci_of_data *data;
	int id, ret;

	if (!IS_ENABLED(CONFIG_OF) || !np)
		return ERR_PTR(-EINVAL);

	data = of_device_get_match_data(&pdev->dev);

	rstc = devm_reset_control_get_optional_exclusive(&pdev->dev, NULL);
	if (IS_ERR(rstc))
		return ERR_PTR(dev_err_probe(&pdev->dev, PTR_ERR(rstc),
					     "failed to get reset ctrl\n"));

	ret = reset_control_deassert(rstc);
	if (ret) {
		dev_err(&pdev->dev, "failed to deassert reset %d\n", ret);
		return ERR_PTR(ret);
	}

	ret = devm_add_action_or_reset(&pdev->dev, sci_reset_control_assert, rstc);
	if (ret) {
		dev_err(&pdev->dev, "failed to register assert devm action, %d\n",
			ret);
		return ERR_PTR(ret);
	}

	p = devm_kzalloc(&pdev->dev, sizeof(struct plat_sci_port), GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);

	/* Get the line number from the aliases node. */
	id = of_alias_get_id(np, "serial");
	if (id < 0 && ~sci_ports_in_use)
		id = ffz(sci_ports_in_use);
	if (id < 0) {
		dev_err(&pdev->dev, "failed to get alias id (%d)\n", id);
		return ERR_PTR(-EINVAL);
	}
	if (id >= ARRAY_SIZE(sci_ports)) {
		dev_err(&pdev->dev, "serial%d out of range\n", id);
		return ERR_PTR(-EINVAL);
	}

	sp = &sci_ports[id];
	sp->rstc = rstc;
	*dev_id = id;

	p->type = data->type;
	p->regtype = data->regtype;

	sp->ops = data->ops;
	sp->port.ops = data->uart_ops;
	sp->params = data->params;

	sp->has_rtscts = of_property_read_bool(np, "uart-has-rtscts");

	return p;
}

static int sci_probe_single(struct platform_device *dev,
				      unsigned int index,
				      struct plat_sci_port *p,
				      struct sci_port *sciport,
				      struct resource *sci_res)
{
	int ret;

	/* Sanity check */
	if (unlikely(index >= SCI_NPORTS)) {
		dev_notice(&dev->dev, "Attempting to register port %d when only %d are available\n",
			   index+1, SCI_NPORTS);
		dev_notice(&dev->dev, "Consider bumping CONFIG_SERIAL_SH_SCI_NR_UARTS!\n");
		return -EINVAL;
	}
	BUILD_BUG_ON(SCI_NPORTS > sizeof(sci_ports_in_use) * 8);
	if (sci_ports_in_use & BIT(index))
		return -EBUSY;

	mutex_lock(&sci_uart_registration_lock);
	if (!sci_uart_driver.state) {
		ret = uart_register_driver(&sci_uart_driver);
		if (ret) {
			mutex_unlock(&sci_uart_registration_lock);
			return ret;
		}
	}
	mutex_unlock(&sci_uart_registration_lock);

	ret = sci_init_single(dev, sciport, index, p, false);
	if (ret)
		return ret;

	sciport->port.dev = &dev->dev;
	ret = devm_pm_runtime_enable(&dev->dev);
	if (ret)
		return ret;

	sciport->gpios = mctrl_gpio_init(&sciport->port, 0);
	if (IS_ERR(sciport->gpios))
		return PTR_ERR(sciport->gpios);

	if (sciport->has_rtscts) {
		if (mctrl_gpio_to_gpiod(sciport->gpios, UART_GPIO_CTS) ||
		    mctrl_gpio_to_gpiod(sciport->gpios, UART_GPIO_RTS)) {
			dev_err(&dev->dev, "Conflicting RTS/CTS config\n");
			return -EINVAL;
		}
		sciport->port.flags |= UPF_HARD_FLOW;
	}

	if (sci_uart_earlycon && sci_ports[0].port.mapbase == sci_res->start) {
		/*
		 * In case:
		 * - this is the earlycon port (mapped on index 0 in sci_ports[]) and
		 * - it now maps to an alias other than zero and
		 * - the earlycon is still alive (e.g., "earlycon keep_bootcon" is
		 *   available in bootargs)
		 *
		 * we need to avoid disabling clocks and PM domains through the runtime
		 * PM APIs called in __device_attach(). For this, increment the runtime
		 * PM reference counter (the clocks and PM domains were already enabled
		 * by the bootloader). Otherwise the earlycon may access the HW when it
		 * has no clocks enabled leading to failures (infinite loop in
		 * sci_poll_put_char()).
		 */
		pm_runtime_get_noresume(&dev->dev);

		/*
		 * Skip cleanup the sci_port[0] in early_console_exit(), this
		 * port is the same as the earlycon one.
		 */
		sci_uart_earlycon_dev_probing = true;
	}

	return uart_add_one_port(&sci_uart_driver, &sciport->port);
}

static int sci_probe(struct platform_device *dev)
{
	struct plat_sci_port *p;
	struct resource *res;
	struct sci_port *sp;
	unsigned int dev_id;
	int ret;

	/*
	 * If we've come here via earlyprintk initialization, head off to
	 * the special early probe. We don't have sufficient device state
	 * to make it beyond this yet.
	 */
#ifdef CONFIG_SUPERH
	if (is_sh_early_platform_device(dev))
		return sci_probe_earlyprintk(dev);
#endif

	if (dev->dev.of_node) {
		p = sci_parse_dt(dev, &dev_id);
		if (IS_ERR(p))
			return PTR_ERR(p);
		sp = &sci_ports[dev_id];
	} else {
		p = dev->dev.platform_data;
		if (p == NULL) {
			dev_err(&dev->dev, "no platform data supplied\n");
			return -EINVAL;
		}

		dev_id = dev->id;
		sp = &sci_ports[dev_id];
		sp->params = sci_probe_regmap(p, sp);
		if (!sp->params)
			return -ENODEV;
	}

	sp->suspend_regs = devm_kzalloc(&dev->dev,
					sp->ops->suspend_regs_size(),
					GFP_KERNEL);
	if (!sp->suspend_regs)
		return -ENOMEM;

	/*
	 * In case:
	 * - the probed port alias is zero (as the one used by earlycon), and
	 * - the earlycon is still active (e.g., "earlycon keep_bootcon" in
	 *   bootargs)
	 *
	 * defer the probe of this serial. This is a debug scenario and the user
	 * must be aware of it.
	 *
	 * Except when the probed port is the same as the earlycon port.
	 */

	res = platform_get_resource(dev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	if (sci_uart_earlycon && sp == &sci_ports[0] && sp->port.mapbase != res->start)
		return dev_err_probe(&dev->dev, -EBUSY, "sci_port[0] is used by earlycon!\n");

	platform_set_drvdata(dev, sp);

	ret = sci_probe_single(dev, dev_id, p, sp, res);
	if (ret)
		return ret;

	if (sp->port.fifosize > 1) {
		ret = device_create_file(&dev->dev, &dev_attr_rx_fifo_trigger);
		if (ret)
			return ret;
	}
	if (sp->type == PORT_SCIFA || sp->type == PORT_SCIFB ||
	    sp->type == PORT_HSCIF || sp->type == SCI_PORT_RSCI) {
		ret = device_create_file(&dev->dev, &dev_attr_rx_fifo_timeout);
		if (ret) {
			if (sp->port.fifosize > 1) {
				device_remove_file(&dev->dev,
						   &dev_attr_rx_fifo_trigger);
			}
			return ret;
		}
	}

#ifdef CONFIG_SH_STANDARD_BIOS
	sh_bios_gdb_detach();
#endif

	sci_ports_in_use |= BIT(dev_id);
	return 0;
}

static int sci_suspend(struct device *dev)
{
	struct sci_port *sport = dev_get_drvdata(dev);

	if (sport) {
		uart_suspend_port(&sci_uart_driver, &sport->port);

		if (!console_suspend_enabled && uart_console(&sport->port)) {
			if (sport->ops->console_save)
				sport->ops->console_save(&sport->port);
		}
		else
			return reset_control_assert(sport->rstc);
	}

	return 0;
}

static int sci_resume(struct device *dev)
{
	struct sci_port *sport = dev_get_drvdata(dev);

	if (sport) {
		if (!console_suspend_enabled && uart_console(&sport->port)) {
			if (sport->ops->console_restore)
				sport->ops->console_restore(&sport->port);
		} else {
			int ret = reset_control_deassert(sport->rstc);

			if (ret)
				return ret;
		}

		uart_resume_port(&sci_uart_driver, &sport->port);
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(sci_dev_pm_ops, sci_suspend, sci_resume);

static struct platform_driver sci_driver = {
	.probe		= sci_probe,
	.remove		= sci_remove,
	.driver		= {
		.name	= "sh-sci",
		.pm	= pm_sleep_ptr(&sci_dev_pm_ops),
		.of_match_table = of_match_ptr(of_sci_match),
	},
};

static int __init sci_init(void)
{
	pr_info("%s\n", banner);

	return platform_driver_register(&sci_driver);
}

static void __exit sci_exit(void)
{
	platform_driver_unregister(&sci_driver);

	if (sci_uart_driver.state)
		uart_unregister_driver(&sci_uart_driver);
}

#if defined(CONFIG_SUPERH) && defined(CONFIG_SERIAL_SH_SCI_CONSOLE)
sh_early_platform_init_buffer("earlyprintk", &sci_driver,
			   early_serial_buf, ARRAY_SIZE(early_serial_buf));
#endif
#ifdef CONFIG_SERIAL_SH_SCI_EARLYCON
static struct plat_sci_port port_cfg;

static int early_console_exit(struct console *co)
{
	struct sci_port *sci_port = &sci_ports[0];

	/*
	 * Clean the slot used by earlycon. A new SCI device might
	 * map to this slot.
	 */
	if (!sci_uart_earlycon_dev_probing) {
		memset(sci_port, 0, sizeof(*sci_port));
		sci_uart_earlycon = false;
	}

	return 0;
}

int __init scix_early_console_setup(struct earlycon_device *device,
				    const struct sci_of_data *data)
{
	const struct sci_common_regs *regs;

	if (!device->port.membase)
		return -ENODEV;

	device->port.type = SCI_PUBLIC_PORT_ID(data->type);

	sci_ports[0].port = device->port;
	sci_ports[0].type = data->type;
	sci_ports[0].regtype = data->regtype;

	port_cfg.type = data->type;
	port_cfg.regtype = data->regtype;

	sci_ports[0].cfg = &port_cfg;
	sci_ports[0].params = data->params;
	sci_ports[0].ops = data->ops;
	sci_ports[0].port.ops = data->uart_ops;
	sci_uart_earlycon = true;
	regs = sci_ports[0].params->common_regs;

	port_cfg.scscr = sci_ports[0].ops->read_reg(&sci_ports[0].port, regs->control);
	sci_ports[0].ops->write_reg(&sci_ports[0].port,
				    regs->control,
				    sci_ports[0].params->param_bits->rxtx_enable | port_cfg.scscr);

	device->con->write = serial_console_write;
	device->con->exit = early_console_exit;

	return 0;
}
static int __init sci_early_console_setup(struct earlycon_device *device,
					  const char *opt)
{
	return scix_early_console_setup(device, &of_sci_sci);
}
static int __init scif_early_console_setup(struct earlycon_device *device,
					  const char *opt)
{
	return scix_early_console_setup(device, &of_sci_scif_sh4);
}
static int __init rzscifa_early_console_setup(struct earlycon_device *device,
					  const char *opt)
{
	return scix_early_console_setup(device, &of_sci_scif_rz_scifa);
}

static int __init rzv2hscif_early_console_setup(struct earlycon_device *device,
						const char *opt)
{
	return scix_early_console_setup(device, &of_sci_scif_rzv2h);
}

static int __init scifa_early_console_setup(struct earlycon_device *device,
					  const char *opt)
{
	return scix_early_console_setup(device, &of_sci_scifa);
}
static int __init scifb_early_console_setup(struct earlycon_device *device,
					  const char *opt)
{
	return scix_early_console_setup(device, &of_sci_scifb);
}
static int __init hscif_early_console_setup(struct earlycon_device *device,
					  const char *opt)
{
	return scix_early_console_setup(device, &of_sci_hscif);
}

OF_EARLYCON_DECLARE(sci, "renesas,sci", sci_early_console_setup);
OF_EARLYCON_DECLARE(scif, "renesas,scif", scif_early_console_setup);
OF_EARLYCON_DECLARE(scif, "renesas,scif-r7s9210", rzscifa_early_console_setup);
OF_EARLYCON_DECLARE(scif, "renesas,scif-r9a07g044", rzscifa_early_console_setup);
OF_EARLYCON_DECLARE(scif, "renesas,scif-r9a09g057", rzv2hscif_early_console_setup);
OF_EARLYCON_DECLARE(scifa, "renesas,scifa", scifa_early_console_setup);
OF_EARLYCON_DECLARE(scifb, "renesas,scifb", scifb_early_console_setup);
OF_EARLYCON_DECLARE(hscif, "renesas,hscif", hscif_early_console_setup);
#endif /* CONFIG_SERIAL_SH_SCI_EARLYCON */

module_init(sci_init);
module_exit(sci_exit);

MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:sh-sci");
MODULE_AUTHOR("Paul Mundt");
MODULE_DESCRIPTION("SuperH (H)SCI(F) serial driver");
