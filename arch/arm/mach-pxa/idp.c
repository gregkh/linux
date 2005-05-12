/*
 *  linux/arch/arm/mach-pxa/idp.c
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  Copyright (c) 2001 Cliff Brake, Accelent Systems Inc.
 *
 *  2001-09-13: Cliff Brake <cbrake@accelent.com>
 *              Initial code
 *
 * Expected command line: mem=32M initrd=0xa1000000,4M root=/dev/ram ramdisk=8192
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/major.h>
#include <linux/fs.h>
#include <linux/interrupt.h>

#include <asm/setup.h>
#include <asm/memory.h>
#include <asm/mach-types.h>
#include <asm/hardware.h>
#include <asm/irq.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include <asm/arch/pxa-regs.h>
#include <asm/arch/idp.h>

#include "generic.h"

#ifndef PXA_IDP_REV02
/* shadow registers for write only registers */
unsigned int idp_cpld_led_control_shadow = 0x1;
unsigned int idp_cpld_periph_pwr_shadow = 0xd;
unsigned int ipd_cpld_cir_shadow = 0;
unsigned int idp_cpld_kb_col_high_shadow = 0;
unsigned int idp_cpld_kb_col_low_shadow = 0;
unsigned int idp_cpld_pccard_en_shadow = 0xC3;
unsigned int idp_cpld_gpioh_dir_shadow = 0;
unsigned int idp_cpld_gpioh_value_shadow = 0;
unsigned int idp_cpld_gpiol_dir_shadow = 0;
unsigned int idp_cpld_gpiol_value_shadow = 0;

/*
 * enable all LCD signals -- they should still be on
 * write protect flash
 * enable all serial port transceivers
 */

unsigned int idp_control_port_shadow = ((0x7 << 21) | 		/* LCD power */
					(0x1 << 19) |		/* disable flash write enable */
					(0x7 << 9));		/* enable serial port transeivers */

#endif

static void __init idp_init(void)
{
	printk("idp_init()\n");
}

static void __init idp_init_irq(void)
{
	pxa_init_irq();
}

static struct map_desc idp_io_desc[] __initdata = {
 /* virtual     physical    length      type */


#ifndef PXA_IDP_REV02
  { IDP_CTRL_PORT_BASE,
    IDP_CTRL_PORT_PHYS,
    IDP_CTRL_PORT_SIZE,
    MT_DEVICE },
#endif

  { IDP_IDE_BASE,
    IDP_IDE_PHYS,
    IDP_IDE_SIZE,
    MT_DEVICE },
  { IDP_ETH_BASE,
    IDP_ETH_PHYS,
    IDP_ETH_SIZE,
    MT_DEVICE },
  { IDP_COREVOLT_BASE,
    IDP_COREVOLT_PHYS,
    IDP_COREVOLT_SIZE,
    MT_DEVICE },
  { IDP_CPLD_BASE,
    IDP_CPLD_PHYS,
    IDP_CPLD_SIZE,
    MT_DEVICE }
};

static void __init idp_map_io(void)
{
	pxa_map_io();
	iotable_init(idp_io_desc, ARRAY_SIZE(idp_io_desc));

	set_irq_type(TOUCH_PANEL_IRQ, TOUCH_PANEL_IRQ_EDGE);

	// serial ports 2 & 3
	pxa_gpio_mode(GPIO42_BTRXD_MD);
	pxa_gpio_mode(GPIO43_BTTXD_MD);
	pxa_gpio_mode(GPIO44_BTCTS_MD);
	pxa_gpio_mode(GPIO45_BTRTS_MD);
	pxa_gpio_mode(GPIO46_STRXD_MD);
	pxa_gpio_mode(GPIO47_STTXD_MD);

}


MACHINE_START(PXA_IDP, "Accelent Xscale IDP")
	MAINTAINER("Accelent Systems Inc.")
	BOOT_MEM(0xa0000000, 0x40000000, io_p2v(0x40000000))
	MAPIO(idp_map_io)
	INITIRQ(idp_init_irq)
	.timer		= &pxa_timer,
	INIT_MACHINE(idp_init)
MACHINE_END
