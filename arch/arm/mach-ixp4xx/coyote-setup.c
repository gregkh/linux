/*
 * arch/arm/mach-ixp4xx/coyote-setup.c
 *
 * Board setup for ADI Engineering and IXDGP425 boards
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
 * Only one serial port is connected on the Coyote & IXDPG425
 */
static struct uart_port coyote_serial_port = {
	.membase	= (char*)(IXP4XX_UART2_BASE_VIRT + REG_OFFSET),
	.mapbase	= (IXP4XX_UART2_BASE_PHYS),
	.irq		= IRQ_IXP4XX_UART2,
	.flags		= UPF_SKIP_TEST,
	.iotype		= UPIO_MEM,	
	.regshift	= 2,
	.uartclk	= IXP4XX_UART_XTAL,
	.line		= 0,
	.type		= PORT_XSCALE,
	.fifosize	= 32
};

void __init coyote_map_io(void)
{
	if (machine_is_ixdpg425()) {
		coyote_serial_port.membase =
			(char*)(IXP4XX_UART1_BASE_VIRT + REG_OFFSET);
		coyote_serial_port.mapbase = IXP4XX_UART1_BASE_PHYS;
		coyote_serial_port.irq = IRQ_IXP4XX_UART1;
	}

	early_serial_setup(&coyote_serial_port);

	ixp4xx_map_io();
}

static struct flash_platform_data coyote_flash_data = {
	.map_name	= "cfi_probe",
	.width		= 2,
};

static struct resource coyote_flash_resource = {
	.start		= COYOTE_FLASH_BASE,
	.end		= COYOTE_FLASH_BASE + COYOTE_FLASH_SIZE,
	.flags		= IORESOURCE_MEM,
};

static struct platform_device coyote_flash = {
	.name		= "IXP4XX-Flash",
	.id		= 0,
	.dev		= {
		.platform_data = &coyote_flash_data,
	},
	.num_resources	= 1,
	.resource	= &coyote_flash_resource,
};

static struct platform_device *coyote_devices[] __initdata = {
	&coyote_flash
};

static void __init coyote_init(void)
{
	*IXP4XX_EXP_CS0 |= IXP4XX_FLASH_WRITABLE;
	*IXP4XX_EXP_CS1 = *IXP4XX_EXP_CS0;

	ixp4xx_sys_init();
	platform_add_devices(coyote_devices, ARRAY_SIZE(coyote_devices));
}

#ifdef CONFIG_ARCH_ADI_COYOTE
MACHINE_START(ADI_COYOTE, "ADI Engineering Coyote")
        MAINTAINER("MontaVista Software, Inc.")
        BOOT_MEM(PHYS_OFFSET, IXP4XX_PERIPHERAL_BASE_PHYS,
                IXP4XX_PERIPHERAL_BASE_VIRT)
        MAPIO(coyote_map_io)
        INITIRQ(ixp4xx_init_irq)
	.timer		= &ixp4xx_timer,
        BOOT_PARAMS(0x0100)
	INIT_MACHINE(coyote_init)
MACHINE_END
#endif

/*
 * IXDPG425 is identical to Coyote except for which serial port
 * is connected.
 */
#ifdef CONFIG_MACH_IXDPG425
MACHINE_START(IXDPG425, "Intel IXDPG425")
        MAINTAINER("MontaVista Software, Inc.")
        BOOT_MEM(PHYS_OFFSET, IXP4XX_PERIPHERAL_BASE_PHYS,
                IXP4XX_PERIPHERAL_BASE_VIRT)
        MAPIO(coyote_map_io)
        INITIRQ(ixp4xx_init_irq)
	.timer		= &ixp4xx_timer,
        BOOT_PARAMS(0x0100)
	INIT_MACHINE(coyote_init)
MACHINE_END
#endif

