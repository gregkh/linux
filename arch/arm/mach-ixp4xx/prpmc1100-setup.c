/*
 * arch/arm/mach-ixp4xx/prpmc1100-setup.c
 *
 * Motorola PrPMC1100 board setup
 *
 * Copyright (C) 2003-2004 MontaVista Software, Inc.
 *
 * Author: Deepak Saxena <dsaxena@plexity.net>
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/serial.h>
#include <linux/tty.h>
#include <linux/serial_core.h>

#include <asm/types.h>
#include <asm/setup.h>
#include <asm/memory.h>
#include <asm/hardware.h>
#include <asm/irq.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/flash.h>

#ifdef	__ARMEB__
#define	REG_OFFSET	3
#else
#define	REG_OFFSET	0
#endif

/*
 * Only one serial port is connected on the PrPMC1100
 */
static struct uart_port prpmc1100_serial_port = {
	.membase	= (char*)(IXP4XX_UART1_BASE_VIRT + REG_OFFSET),
	.mapbase	= (IXP4XX_UART1_BASE_PHYS),
	.irq		= IRQ_IXP4XX_UART1,
	.flags		= UPF_SKIP_TEST,
	.iotype		= UPIO_MEM,	
	.regshift	= 2,
	.uartclk	= IXP4XX_UART_XTAL,
	.line		= 0,
	.type		= PORT_XSCALE,
	.fifosize	= 32
};

void __init prpmc1100_map_io(void)
{
	early_serial_setup(&prpmc1100_serial_port);

	ixp4xx_map_io();
}

static struct flash_platform_data prpmc1100_flash_data = {
	.map_name	= "cfi_probe",
	.width		= 2,
};

static struct resource prpmc1100_flash_resource = {
	.start		= PRPMC1100_FLASH_BASE,
	.end		= PRPMC1100_FLASH_BASE + PRPMC1100_FLASH_SIZE,
	.flags		= IORESOURCE_MEM,
};

static struct platform_device prpmc1100_flash = {
	.name		= "IXP4XX-Flash",
	.id		= 0,
	.dev		= {
		.platform_data = &prpmc1100_flash_data,
	},
	.num_resources	= 1,
	.resource	= &prpmc1100_flash_resource,
};

static struct platform_device *prpmc1100_devices[] __initdata = {
	&prpmc1100_flash
};

static void __init prpmc1100_init(void)
{
	ixp4xx_sys_init();

	platform_add_devices(prpmc1100_devices, ARRAY_SIZE(prpmc1100_devices));
}

MACHINE_START(PRPMC1100, "Motorola PrPMC1100")
        MAINTAINER("MontaVista Software, Inc.")
        BOOT_MEM(PHYS_OFFSET, IXP4XX_PERIPHERAL_BASE_PHYS,
                IXP4XX_PERIPHERAL_BASE_VIRT)
        MAPIO(prpmc1100_map_io)
        INITIRQ(ixp4xx_init_irq)
	.timer		= &ixp4xx_timer,
        BOOT_PARAMS(0x0100)
	INIT_MACHINE(prpmc1100_init)
MACHINE_END

