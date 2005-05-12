/*
 * arch/arm/mach-ixp4xx/prpmc1100-pci.c 
 *
 * PrPMC1100 PCI initialization
 *
 * Copyright (C) 2003-2004 MontaVista Sofwtare, Inc. 
 * Based on IXDP425 code originally (C) Intel Corporation
 *
 * Author: Deepak Saxena <dsaxena@plexity.net>
 *
 * PrPMC1100 PCI init code.  GPIO usage is similar to that on 
 * IXDP425, but the IRQ routing is completely different and
 * depends on what carrier you are using. This code is written
 * to work on the Motorola PrPMC800 ATX carrier board.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/kernel.h>
#include <linux/config.h>
#include <linux/pci.h>
#include <linux/init.h>

#include <asm/mach-types.h>
#include <asm/irq.h>
#include <asm/hardware.h>

#include <asm/mach/pci.h>


void __init prpmc1100_pci_preinit(void)
{
	gpio_line_config(PRPMC1100_PCI_INTA_PIN, 
				IXP4XX_GPIO_IN | IXP4XX_GPIO_ACTIVE_LOW);
	gpio_line_config(PRPMC1100_PCI_INTB_PIN, 
				IXP4XX_GPIO_IN | IXP4XX_GPIO_ACTIVE_LOW);
	gpio_line_config(PRPMC1100_PCI_INTC_PIN, 
				IXP4XX_GPIO_IN | IXP4XX_GPIO_ACTIVE_LOW);
	gpio_line_config(PRPMC1100_PCI_INTD_PIN, 
				IXP4XX_GPIO_IN | IXP4XX_GPIO_ACTIVE_LOW);

	gpio_line_isr_clear(PRPMC1100_PCI_INTA_PIN);
	gpio_line_isr_clear(PRPMC1100_PCI_INTB_PIN);
	gpio_line_isr_clear(PRPMC1100_PCI_INTC_PIN);
	gpio_line_isr_clear(PRPMC1100_PCI_INTD_PIN);

	ixp4xx_pci_preinit();
}


static int __init prpmc1100_map_irq(struct pci_dev *dev, u8 slot, u8 pin)
{
	int irq = -1;

	static int pci_irq_table[][4] = { 
		{	/* IDSEL 16 - PMC A1 */
			IRQ_PRPMC1100_PCI_INTD, 
			IRQ_PRPMC1100_PCI_INTA, 
			IRQ_PRPMC1100_PCI_INTB, 
			IRQ_PRPMC1100_PCI_INTC
		}, {	/* IDSEL 17 - PRPMC-A-B */
			IRQ_PRPMC1100_PCI_INTD, 
			IRQ_PRPMC1100_PCI_INTA, 
			IRQ_PRPMC1100_PCI_INTB, 
			IRQ_PRPMC1100_PCI_INTC
		}, { 	/* IDSEL 18 - PMC A1-B */
			IRQ_PRPMC1100_PCI_INTA, 
			IRQ_PRPMC1100_PCI_INTB, 
			IRQ_PRPMC1100_PCI_INTC, 
			IRQ_PRPMC1100_PCI_INTD
		}, {	/* IDSEL 19 - Unused */
			0, 0, 0, 0 
		}, {	/* IDSEL 20 - P2P Bridge */
			IRQ_PRPMC1100_PCI_INTA, 
			IRQ_PRPMC1100_PCI_INTB, 
			IRQ_PRPMC1100_PCI_INTC, 
			IRQ_PRPMC1100_PCI_INTD
		}, {	/* IDSEL 21 - PMC A2 */
			IRQ_PRPMC1100_PCI_INTC, 
			IRQ_PRPMC1100_PCI_INTD, 
			IRQ_PRPMC1100_PCI_INTA, 
			IRQ_PRPMC1100_PCI_INTB
		}, {	/* IDSEL 22 - PMC A2-B */
			IRQ_PRPMC1100_PCI_INTD, 
			IRQ_PRPMC1100_PCI_INTA, 
			IRQ_PRPMC1100_PCI_INTB, 
			IRQ_PRPMC1100_PCI_INTC
		},
	};

	if (slot >= PRPMC1100_PCI_MIN_DEVID && slot <= PRPMC1100_PCI_MAX_DEVID 
		&& pin >= 1 && pin <= PRPMC1100_PCI_IRQ_LINES) {
		irq = pci_irq_table[slot - PRPMC1100_PCI_MIN_DEVID][pin - 1];
	}

	return irq;
}


struct hw_pci prpmc1100_pci __initdata = {
	.nr_controllers = 1,
	.preinit =	  prpmc1100_pci_preinit,
	.swizzle =	  pci_std_swizzle,
	.setup =	  ixp4xx_setup,
	.scan =		  ixp4xx_scan_bus,
	.map_irq =	  prpmc1100_map_irq,
};

int __init prpmc1100_pci_init(void)
{
	if (machine_is_prpmc1100())
		pci_common_init(&prpmc1100_pci);
	return 0;
}

subsys_initcall(prpmc1100_pci_init);

