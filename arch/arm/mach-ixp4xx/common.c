/*
 * arch/arm/mach-ixp4xx/common.c
 *
 * Generic code shared across all IXP4XX platforms
 *
 * Maintainer: Deepak Saxena <dsaxena@plexity.net>
 *
 * Copyright 2002 (c) Intel Corporation
 * Copyright 2003-2004 (c) MontaVista, Software, Inc. 
 * 
 * This file is licensed under  the terms of the GNU General Public 
 * License version 2. This program is licensed "as is" without any 
 * warranty of any kind, whether express or implied.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/serial.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/serial_core.h>
#include <linux/bootmem.h>
#include <linux/interrupt.h>
#include <linux/bitops.h>
#include <linux/time.h>
#include <linux/timex.h>

#include <asm/hardware.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/page.h>
#include <asm/irq.h>

#include <asm/mach/map.h>
#include <asm/mach/irq.h>
#include <asm/mach/time.h>


/*************************************************************************
 * GPIO acces functions
 *************************************************************************/

/*
 * Configure GPIO line for input, interrupt, or output operation
 *
 * TODO: Enable/disable the irq_desc based on interrupt or output mode.
 * TODO: Should these be named ixp4xx_gpio_?
 */
void gpio_line_config(u8 line, u32 style)
{
	u32 enable;
	volatile u32 *int_reg;
	u32 int_style;

	enable = *IXP4XX_GPIO_GPOER;

	if (style & IXP4XX_GPIO_OUT) {
		enable &= ~((1) << line);
	} else if (style & IXP4XX_GPIO_IN) {
		enable |= ((1) << line);

		switch (style & IXP4XX_GPIO_INTSTYLE_MASK)
		{
		case (IXP4XX_GPIO_ACTIVE_HIGH):
			int_style = IXP4XX_GPIO_STYLE_ACTIVE_HIGH;
			break;
		case (IXP4XX_GPIO_ACTIVE_LOW):
			int_style = IXP4XX_GPIO_STYLE_ACTIVE_LOW;
			break;
		case (IXP4XX_GPIO_RISING_EDGE):
			int_style = IXP4XX_GPIO_STYLE_RISING_EDGE;
			break;
		case (IXP4XX_GPIO_FALLING_EDGE):
			int_style = IXP4XX_GPIO_STYLE_FALLING_EDGE;
			break;
		case (IXP4XX_GPIO_TRANSITIONAL):
			int_style = IXP4XX_GPIO_STYLE_TRANSITIONAL;
			break;
		default:
			int_style = IXP4XX_GPIO_STYLE_ACTIVE_HIGH;
			break;
		}

		if (line >= 8) {	/* pins 8-15 */ 
			line -= 8;
			int_reg = IXP4XX_GPIO_GPIT2R;
		}
		else {			/* pins 0-7 */
			int_reg = IXP4XX_GPIO_GPIT1R;
		}

		/* Clear the style for the appropriate pin */
		*int_reg &= ~(IXP4XX_GPIO_STYLE_CLEAR << 
		    		(line * IXP4XX_GPIO_STYLE_SIZE));

		/* Set the new style */
		*int_reg |= (int_style << (line * IXP4XX_GPIO_STYLE_SIZE));
	}

	*IXP4XX_GPIO_GPOER = enable;
}

EXPORT_SYMBOL(gpio_line_config);

/*************************************************************************
 * IXP4xx chipset I/O mapping
 *************************************************************************/
static struct map_desc ixp4xx_io_desc[] __initdata = {
	{	/* UART, Interrupt ctrl, GPIO, timers, NPEs, MACs, USB .... */
		.virtual	= IXP4XX_PERIPHERAL_BASE_VIRT,
		.physical	= IXP4XX_PERIPHERAL_BASE_PHYS,
		.length		= IXP4XX_PERIPHERAL_REGION_SIZE,
		.type		= MT_DEVICE
	}, {	/* Expansion Bus Config Registers */
		.virtual	= IXP4XX_EXP_CFG_BASE_VIRT,
		.physical	= IXP4XX_EXP_CFG_BASE_PHYS,
		.length		= IXP4XX_EXP_CFG_REGION_SIZE,
		.type		= MT_DEVICE
	}, {	/* PCI Registers */
		.virtual	= IXP4XX_PCI_CFG_BASE_VIRT,
		.physical	= IXP4XX_PCI_CFG_BASE_PHYS,
		.length		= IXP4XX_PCI_CFG_REGION_SIZE,
		.type		= MT_DEVICE
	}
};

void __init ixp4xx_map_io(void)
{
  	iotable_init(ixp4xx_io_desc, ARRAY_SIZE(ixp4xx_io_desc));
}


/*************************************************************************
 * IXP4xx chipset IRQ handling
 *
 * TODO: GPIO IRQs should be marked invalid until the user of the IRQ
 *       (be it PCI or something else) configures that GPIO line
 *       as an IRQ. Also, we should use a different chip structure for 
 *       level-based GPIO vs edge-based GPIO. Currently nobody needs this as 
 *       all HW that's publically available uses level IRQs, so we'll
 *       worry about it if/when we have HW to test.
 **************************************************************************/
static void ixp4xx_irq_mask(unsigned int irq)
{
	if (cpu_is_ixp46x() && irq >= 32)
		*IXP4XX_ICMR2 &= ~(1 << (irq - 32));
	else
		*IXP4XX_ICMR &= ~(1 << irq);
}

static void ixp4xx_irq_mask_ack(unsigned int irq)
{
	ixp4xx_irq_mask(irq);
}

static void ixp4xx_irq_unmask(unsigned int irq)
{
	static int irq2gpio[32] = {
		-1, -1, -1, -1, -1, -1,  0,  1,
		-1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1,  2,  3,  4,  5,  6,
		 7,  8,  9, 10, 11, 12, -1, -1,
	};
	int line = (irq < 32) ? irq2gpio[irq] : -1;

	/*
	 * This only works for LEVEL gpio IRQs as per the IXP4xx developer's
	 * manual. If edge-triggered, need to move it to the mask_ack.
	 * Nobody seems to be using the edge-triggered mode on the GPIOs. 
	 */
	if (line >= 0)
		gpio_line_isr_clear(line);

	if (cpu_is_ixp46x() && irq >= 32)
		*IXP4XX_ICMR2 |= (1 << (irq - 32));
	else
		*IXP4XX_ICMR |= (1 << irq);
}

static struct irqchip ixp4xx_irq_chip = {
	.ack	= ixp4xx_irq_mask_ack,
	.mask	= ixp4xx_irq_mask,
	.unmask	= ixp4xx_irq_unmask,
};

void __init ixp4xx_init_irq(void)
{
	int i = 0;

	/* Route all sources to IRQ instead of FIQ */
	*IXP4XX_ICLR = 0x0;

	/* Disable all interrupt */
	*IXP4XX_ICMR = 0x0; 

	if (cpu_is_ixp46x()) {
		/* Route upper 32 sources to IRQ instead of FIQ */
		*IXP4XX_ICLR2 = 0x00;

		/* Disable upper 32 interrupts */
		*IXP4XX_ICMR2 = 0x00;
	}

	for(i = 0; i < NR_IRQS; i++)
	{
		set_irq_chip(i, &ixp4xx_irq_chip);
		set_irq_handler(i, do_level_IRQ);
		set_irq_flags(i, IRQF_VALID);
	}
}


/*************************************************************************
 * IXP4xx timer tick
 * We use OS timer1 on the CPU for the timer tick and the timestamp 
 * counter as a source of real clock ticks to account for missed jiffies.
 *************************************************************************/

static unsigned volatile last_jiffy_time;

#define CLOCK_TICKS_PER_USEC	((CLOCK_TICK_RATE + USEC_PER_SEC/2) / USEC_PER_SEC)

/* IRQs are disabled before entering here from do_gettimeofday() */
static unsigned long ixp4xx_gettimeoffset(void)
{
	u32 elapsed;

	elapsed = *IXP4XX_OSTS - last_jiffy_time;

	return elapsed / CLOCK_TICKS_PER_USEC;
}

static irqreturn_t ixp4xx_timer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	write_seqlock(&xtime_lock);

	/* Clear Pending Interrupt by writing '1' to it */
	*IXP4XX_OSST = IXP4XX_OSST_TIMER_1_PEND;

	/*
	 * Catch up with the real idea of time
	 */
	while ((*IXP4XX_OSTS - last_jiffy_time) > LATCH) {
		timer_tick(regs);
		last_jiffy_time += LATCH;
	}

	write_sequnlock(&xtime_lock);

	return IRQ_HANDLED;
}

static struct irqaction ixp4xx_timer_irq = {
	.name		= "IXP4xx Timer Tick",
	.flags		= SA_INTERRUPT,
	.handler	= ixp4xx_timer_interrupt
};

static void __init ixp4xx_timer_init(void)
{
	/* Clear Pending Interrupt by writing '1' to it */
	*IXP4XX_OSST = IXP4XX_OSST_TIMER_1_PEND;

	/* Setup the Timer counter value */
	*IXP4XX_OSRT1 = (LATCH & ~IXP4XX_OST_RELOAD_MASK) | IXP4XX_OST_ENABLE;

	/* Reset time-stamp counter */
	*IXP4XX_OSTS = 0;
	last_jiffy_time = 0;

	/* Connect the interrupt handler and enable the interrupt */
	setup_irq(IRQ_IXP4XX_TIMER1, &ixp4xx_timer_irq);
}

struct sys_timer ixp4xx_timer = {
	.init		= ixp4xx_timer_init,
	.offset		= ixp4xx_gettimeoffset,
};

static struct resource ixp46x_i2c_resources[] = {
	[0] = {
		.start 	= 0xc8011000,
		.end	= 0xc801101c,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start 	= IRQ_IXP4XX_I2C,
		.end	= IRQ_IXP4XX_I2C,
		.flags	= IORESOURCE_IRQ
	}
};

/*
 * I2C controller. The IXP46x uses the same block as the IOP3xx, so
 * we just use the same device name.
 */
static struct platform_device ixp46x_i2c_controller = {
	.name		= "IOP3xx-I2C",
	.id		= 0,
	.num_resources	= 2,
	.resource	= ixp46x_i2c_resources
};

static struct platform_device *ixp46x_devices[] __initdata = {
	&ixp46x_i2c_controller
};

void __init ixp4xx_sys_init(void)
{
	if (cpu_is_ixp46x()) {
		platform_add_devices(ixp46x_devices,
				ARRAY_SIZE(ixp46x_devices));
	}
}

