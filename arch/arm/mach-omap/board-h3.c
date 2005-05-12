/*
 * linux/arch/arm/mach-omap/board-h3.c
 *
 * This file contains OMAP1710 H3 specific code.
 *
 * Copyright (C) 2004 Texas Instruments, Inc.
 * Copyright (C) 2002 MontaVista Software, Inc.
 * Copyright (C) 2001 RidgeRun, Inc.
 * Author: RidgeRun, Inc.
 *         Greg Lonnon (glonnon@ridgerun.com) or info@ridgerun.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/major.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/errno.h>

#include <asm/setup.h>
#include <asm/page.h>
#include <asm/hardware.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/arch/irqs.h>
#include <asm/arch/mux.h>
#include <asm/arch/gpio.h>
#include <asm/mach-types.h>

#include "common.h"

extern int omap_gpio_init(void);

static int __initdata h3_serial_ports[OMAP_MAX_NR_PORTS] = {1, 1, 1};

static struct resource smc91x_resources[] = {
	[0] = {
		.start	= OMAP1710_ETHR_START,		/* Physical */
		.end	= OMAP1710_ETHR_START + OMAP1710_ETHR_SIZE,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start	= OMAP_GPIO_IRQ(40),
		.end	= OMAP_GPIO_IRQ(40),
		.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device smc91x_device = {
	.name		= "smc91x",
	.id		= 0,
	.num_resources	= ARRAY_SIZE(smc91x_resources),
	.resource	= smc91x_resources,
};

static struct platform_device *devices[] __initdata = {
        &smc91x_device,
};

static void __init h3_init(void)
{
	(void) platform_add_devices(devices, ARRAY_SIZE(devices));
}

static void __init h3_init_smc91x(void)
{
	omap_cfg_reg(W15_1710_GPIO40);
	if (omap_request_gpio(40) < 0) {
		printk("Error requesting gpio 40 for smc91x irq\n");
		return;
	}
	omap_set_gpio_edge_ctrl(40, OMAP_GPIO_FALLING_EDGE);
}

void h3_init_irq(void)
{
	omap_init_irq();
	omap_gpio_init();
	h3_init_smc91x();
}

static void __init h3_map_io(void)
{
	omap_map_io();
	omap_serial_init(h3_serial_ports);
}

MACHINE_START(OMAP_H3, "TI OMAP1710 H3 board")
	MAINTAINER("Texas Instruments, Inc.")
	BOOT_MEM(0x10000000, 0xfff00000, 0xfef00000)
	BOOT_PARAMS(0x10000100)
	MAPIO(h3_map_io)
	INITIRQ(h3_init_irq)
	INIT_MACHINE(h3_init)
	.timer		= &omap_timer,
MACHINE_END
