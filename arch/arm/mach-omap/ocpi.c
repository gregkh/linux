/*
 * linux/arch/arm/mach-omap/ocpi.c
 *
 * Minimal OCP bus support for OMAP-1610 and OMAP-5912
 *
 * Copyright (C) 2003 - 2004 Nokia Corporation
 * Written by Tony Lindgren <tony@atomide.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>

#include <asm/io.h>
#include <asm/arch/hardware.h>

#define OCPI_BASE		0xfffec320
#define OCPI_FAULT		(OCPI_BASE + 0x00)
#define OCPI_CMD_FAULT		(OCPI_BASE + 0x04)
#define OCPI_SINT0		(OCPI_BASE + 0x08)
#define OCPI_TABORT		(OCPI_BASE + 0x0c)
#define OCPI_SINT1		(OCPI_BASE + 0x10)
#define OCPI_PROT		(OCPI_BASE + 0x14)
#define OCPI_SEC		(OCPI_BASE + 0x18)

#define EN_OCPI_CK		(1 << 0)
#define IDLOCPI_ARM		(1 << 1)

/* USB OHCI OCPI access error registers */
#define HOSTUEADDR	0xfffba0e0
#define HOSTUESTATUS	0xfffba0e4

/*
 * Enables device access to OMAP buses via the OCPI bridge
 * FIXME: Add locking
 */
int ocpi_enable(void)
{
	unsigned int val;

	/* Make sure there's clock for OCPI */

#if defined(CONFIG_ARCH_OMAP16XX)
        if (cpu_is_omap1610() || cpu_is_omap1710()) {
		val = omap_readl(OMAP16XX_ARM_IDLECT3);
		val |= EN_OCPI_CK;
		val &= ~IDLOCPI_ARM;
		omap_writel(val, OMAP16XX_ARM_IDLECT3);
        }
#endif
	/* Enable access for OHCI in OCPI */
	val = omap_readl(OCPI_PROT);
	val &= ~0xff;
	//val &= (1 << 0);	/* Allow access only to EMIFS */
	omap_writel(val, OCPI_PROT);

	val = omap_readl(OCPI_SEC);
	val &= ~0xff;
	omap_writel(val, OCPI_SEC);

	return 0;
}
EXPORT_SYMBOL(ocpi_enable);

int ocpi_status(void)
{
	printk("OCPI: addr: 0x%08x cmd: 0x%08x\n"
	       "      ohci-addr: 0x%08x ohci-status: 0x%08x\n",
	       omap_readl(OCPI_FAULT), omap_readl(OCPI_CMD_FAULT),
	       omap_readl(HOSTUEADDR), omap_readl(HOSTUESTATUS));

	return 1;
}
EXPORT_SYMBOL(ocpi_status);

static int __init omap_ocpi_init(void)
{
	ocpi_enable();
	printk("OMAP OCPI interconnect driver loaded\n");

	return 0;
}

static void __exit omap_ocpi_exit(void)
{
	/* FIXME: Disable OCPI */
}

MODULE_AUTHOR("Tony Lindgren <tony@atomide.com>");
MODULE_DESCRIPTION("OMAP OCPI bus controller module");
MODULE_LICENSE("GPL");
module_init(omap_ocpi_init);
module_exit(omap_ocpi_exit);
