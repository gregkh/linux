/*
 *  serial.c, Serial Interface Unit routines for NEC VR4100 series.
 *
 *  Copyright (C) 2002  MontaVista Software Inc.
 *    Author: Yoichi Yuasa <yyuasa@mvista.com or source@mvista.com>
 *  Copyright (C) 2003-2004  Yoichi Yuasa <yuasa@hh.iij4u.or.jp>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
/*
 * Changes:
 *  MontaVista Software Inc. <yyuasa@mvista.com> or <source@mvista.com>
 *  - New creation, NEC VR4122 and VR4131 are supported.
 *  - Added support for NEC VR4111 and VR4121.
 *
 *  Yoichi Yuasa <yuasa@hh.iij4u.or.jp>
 *  - Added support for NEC VR4133.
 */
#include <linux/init.h>
#include <linux/types.h>
#include <linux/tty.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/smp.h>

#include <asm/addrspace.h>
#include <asm/cpu.h>
#include <asm/io.h>
#include <asm/vr41xx/vr41xx.h>

#define SIUIRSEL_TYPE1		KSEG1ADDR(0x0c000008)
#define SIUIRSEL_TYPE2		KSEG1ADDR(0x0f000808)
 #define USE_RS232C		0x00
 #define USE_IRDA		0x01
 #define SIU_USES_IRDA		0x00
 #define FIR_USES_IRDA		0x02
 #define IRDA_MODULE_SHARP	0x00
 #define IRDA_MODULE_TEMIC	0x04
 #define IRDA_MODULE_HP		0x08
 #define TMICTX			0x10
 #define TMICMODE		0x20

#define SIU_BASE_TYPE1		0x0c000000UL	/* VR4111 and VR4121 */
#define SIU_BASE_TYPE2		0x0f000800UL	/* VR4122, VR4131 and VR4133 */
#define SIU_SIZE		0x8UL

#define SIU_BASE_BAUD		1152000

/* VR4122, VR4131 and VR4133 DSIU Registers */
#define DSIU_BASE		0x0f000820UL
#define DSIU_SIZE		0x8UL

#define DSIU_BASE_BAUD		1152000

int vr41xx_serial_ports = 0;

void vr41xx_select_siu_interface(siu_interface_t interface,
                                 irda_module_t module)
{
	uint16_t val = USE_RS232C;	/* Select RS-232C */

	/* Select IrDA */
	if (interface == SIU_IRDA) {
		switch (module) {
		case IRDA_SHARP:
			val = IRDA_MODULE_SHARP;
			break;
		case IRDA_TEMIC:
			val = IRDA_MODULE_TEMIC;
			break;
		case IRDA_HP:
			val = IRDA_MODULE_HP;
			break;
		default:
			printk(KERN_ERR "SIU: unknown IrDA module\n");
			return;
		}
		val |= USE_IRDA | SIU_USES_IRDA;
	}

	switch (current_cpu_data.cputype) {
	case CPU_VR4111:
	case CPU_VR4121:
		writew(val, SIUIRSEL_TYPE1);
		break;
	case CPU_VR4122:
	case CPU_VR4131:
	case CPU_VR4133:
		writew(val, SIUIRSEL_TYPE2);
		break;
	default:
		printk(KERN_ERR "SIU: unsupported CPU of NEC VR4100 series\n");
		break;
	}
}

void __init vr41xx_siu_init(void)
{
	struct uart_port port;

	memset(&port, 0, sizeof(port));

	port.line = vr41xx_serial_ports;
	port.uartclk = SIU_BASE_BAUD * 16;
	port.irq = SIU_IRQ;
	port.flags = UPF_BOOT_AUTOCONF | UPF_SKIP_TEST;
	switch (current_cpu_data.cputype) {
	case CPU_VR4111:
	case CPU_VR4121:
		port.mapbase = SIU_BASE_TYPE1;
		break;
	case CPU_VR4122:
	case CPU_VR4131:
	case CPU_VR4133:
		port.mapbase = SIU_BASE_TYPE2;
		break;
	default:
		printk(KERN_ERR "SIU: unsupported CPU of NEC VR4100 series\n");
		return;
	}
	port.regshift = 0;
	port.iotype = UPIO_MEM;
	port.membase = ioremap(port.mapbase, SIU_SIZE);
	if (port.membase != NULL) {
		if (early_serial_setup(&port) == 0) {
			vr41xx_supply_clock(SIU_CLOCK);
			vr41xx_serial_ports++;
			return;
		}
	}

	printk(KERN_ERR "SIU: setup failed!\n");
}

void __init vr41xx_dsiu_init(void)
{
	struct uart_port port;

	if (current_cpu_data.cputype != CPU_VR4122 &&
	    current_cpu_data.cputype != CPU_VR4131 &&
	    current_cpu_data.cputype != CPU_VR4133) {
		printk(KERN_ERR "DSIU: unsupported CPU of NEC VR4100 series\n");
		return;
	}

	memset(&port, 0, sizeof(port));

	port.line = vr41xx_serial_ports;
	port.uartclk = DSIU_BASE_BAUD * 16;
	port.irq = DSIU_IRQ;
	port.flags = UPF_BOOT_AUTOCONF | UPF_SKIP_TEST;
	port.mapbase = DSIU_BASE;
	port.regshift = 0;
	port.iotype = UPIO_MEM;
	port.membase = ioremap(port.mapbase, DSIU_SIZE);
	if (port.membase != NULL) {
		if (early_serial_setup(&port) == 0) {
			vr41xx_supply_clock(DSIU_CLOCK);
			vr41xx_enable_dsiuint(DSIUINT_ALL);
			vr41xx_serial_ports++;
			return;
		}
	}

	printk(KERN_ERR "DSIU: setup failed!\n");
}
