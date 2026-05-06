// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *	DEC platform devices.
 *
 *	Copyright (c) 2014  Maciej W. Rozycki
 */

#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mc146818rtc.h>
#include <linux/platform_device.h>

#include <asm/bootinfo.h>

#include <asm/dec/interrupts.h>
#include <asm/dec/kn01.h>
#include <asm/dec/kn02.h>
#include <asm/dec/system.h>

static struct resource dec_rtc_resources[] = {
	{
		.name = "rtc",
		.flags = IORESOURCE_MEM,
	},
};

static struct cmos_rtc_board_info dec_rtc_info = {
	.flags = CMOS_RTC_FLAGS_NOFREQ,
	.address_space = 64,
};

static struct platform_device dec_rtc_device = {
	.name = "rtc_cmos",
	.id = PLATFORM_DEVID_NONE,
	.dev.platform_data = &dec_rtc_info,
	.resource = dec_rtc_resources,
	.num_resources = ARRAY_SIZE(dec_rtc_resources),
};

static struct resource dec_dz_resources[] = {
	{ .name = "dz", .flags = IORESOURCE_MEM, },
	{ .name = "dz", .flags = IORESOURCE_IRQ, },
};

static struct platform_device dec_dz_device = {
	.name = "dz",
	.id = PLATFORM_DEVID_NONE,
	.resource = dec_dz_resources,
	.num_resources = ARRAY_SIZE(dec_dz_resources),
};

static struct platform_device *dec_dz_devices[] __initdata = {
	&dec_dz_device,
};

static int __init dec_add_devices(void)
{
	int ret1, ret2;
	int num_dz;
	int irq, i;

	dec_rtc_resources[0].start = RTC_PORT(0);
	dec_rtc_resources[0].end = RTC_PORT(0) + dec_kn_slot_size - 1;

	i = 0;
	irq = dec_interrupt[DEC_IRQ_DZ11];
	if (IS_ENABLED(CONFIG_32BIT) && irq >= 0) {
		resource_size_t base;

		switch (mips_machtype) {
		case MACH_DS23100:
		case MACH_DS5100:
			base = dec_kn_slot_base + KN01_DZ11;
			break;
		default:
			base = dec_kn_slot_base + KN02_DZ11;
			break;
		}
		dec_dz_device.resource[0].start = base;
		dec_dz_device.resource[0].end = base + dec_kn_slot_size - 1;
		dec_dz_device.resource[1].start = irq;
		dec_dz_device.resource[1].end = irq;
		i++;
	}
	num_dz = i;

	ret1 = platform_device_register(&dec_rtc_device);
	ret2 = IS_ENABLED(CONFIG_32BIT) ?
	       platform_add_devices(dec_dz_devices, num_dz) : 0;
	return ret1 ? ret1 : ret2;
}

device_initcall(dec_add_devices);
