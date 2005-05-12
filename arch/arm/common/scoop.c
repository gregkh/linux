/*
 * Support code for the SCOOP interface found on various Sharp PDAs
 *
 * Copyright (c) 2004 Richard Purdie
 *
 *	Based on code written by Sharp/Lineo for 2.4 kernels
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/device.h>
#include <asm/io.h>
#include <asm/hardware/scoop.h>

static void __iomem *scoop_io_base;

#define SCOOP_REG(adr) (*(volatile unsigned short*)(scoop_io_base+(adr)))

void reset_scoop(void)
{
	SCOOP_REG(SCOOP_MCR) = 0x0100;	// 00
	SCOOP_REG(SCOOP_CDR) = 0x0000;  // 04
	SCOOP_REG(SCOOP_CPR) = 0x0000;  // 0C
	SCOOP_REG(SCOOP_CCR) = 0x0000;  // 10
	SCOOP_REG(SCOOP_IMR) = 0x0000;  // 18
	SCOOP_REG(SCOOP_IRM) = 0x00FF;  // 14
	SCOOP_REG(SCOOP_ISR) = 0x0000;  // 1C
	SCOOP_REG(SCOOP_IRM) = 0x0000;
}

static DEFINE_SPINLOCK(scoop_lock);
static u32 scoop_gpwr;

unsigned short set_scoop_gpio(unsigned short bit)
{
	unsigned short gpio_bit;
	unsigned long flag;

	spin_lock_irqsave(&scoop_lock, flag);
	gpio_bit = SCOOP_REG(SCOOP_GPWR) | bit;
	SCOOP_REG(SCOOP_GPWR) = gpio_bit;
	spin_unlock_irqrestore(&scoop_lock, flag);

	return gpio_bit;
}

unsigned short reset_scoop_gpio(unsigned short bit)
{
	unsigned short gpio_bit;
	unsigned long flag;

	spin_lock_irqsave(&scoop_lock, flag);
	gpio_bit = SCOOP_REG(SCOOP_GPWR) & ~bit;
	SCOOP_REG(SCOOP_GPWR) = gpio_bit;
	spin_unlock_irqrestore(&scoop_lock, flag);

	return gpio_bit;
}

EXPORT_SYMBOL(set_scoop_gpio);
EXPORT_SYMBOL(reset_scoop_gpio);

unsigned short read_scoop_reg(unsigned short reg)
{
	return SCOOP_REG(reg);
}

void write_scoop_reg(unsigned short reg, unsigned short data)
{
	SCOOP_REG(reg)=data;
}

EXPORT_SYMBOL(reset_scoop);
EXPORT_SYMBOL(read_scoop_reg);
EXPORT_SYMBOL(write_scoop_reg);

static int scoop_suspend(struct device *dev, uint32_t state, uint32_t level)
{
	if (level == SUSPEND_POWER_DOWN) {
		scoop_gpwr = SCOOP_REG(SCOOP_GPWR);
		SCOOP_REG(SCOOP_GPWR) = 0;
	}
	return 0;
}

static int scoop_resume(struct device *dev, uint32_t level)
{
	if (level == RESUME_POWER_ON) {
		SCOOP_REG(SCOOP_GPWR) = scoop_gpwr;
	}
	return 0;
}

int __init scoop_probe(struct device *dev)
{
	struct scoop_config *inf;
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (!mem)
		return -EINVAL;

	inf = dev->platform_data;
	scoop_io_base = ioremap(mem->start, 0x1000);
	if (!scoop_io_base)
		return -ENOMEM;

	SCOOP_REG(SCOOP_MCR) = 0x0140;

	reset_scoop();

	SCOOP_REG(SCOOP_GPCR) = inf->io_dir & 0xffff;
	SCOOP_REG(SCOOP_GPWR) = inf->io_out & 0xffff;

	return 0;
}

static struct device_driver scoop_driver = {
	.name		= "sharp-scoop",
	.bus		= &platform_bus_type,
	.probe		= scoop_probe,
	.suspend	= scoop_suspend,
	.resume		= scoop_resume,
};

int __init scoop_init(void)
{
	return driver_register(&scoop_driver);
}

subsys_initcall(scoop_init);
