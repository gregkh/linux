/*
 *  cmu.c, Clock Mask Unit routines for the NEC VR4100 series.
 *
 *  Copyright (C) 2001-2002  MontaVista Software Inc.
 *    Author: Yoichi Yuasa <yyuasa@mvista.com or source@mvista.com>
 *  Copuright (C) 2003-2004  Yoichi Yuasa <yuasa@hh.iij4u.or.jp>
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
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <asm/cpu.h>
#include <asm/io.h>
#include <asm/vr41xx/vr41xx.h>

#define CMUCLKMSK_TYPE1	KSEG1ADDR(0x0b000060)
#define CMUCLKMSK_TYPE2	KSEG1ADDR(0x0f000060)
 #define MSKPIU		0x0001
 #define MSKSIU		0x0002
 #define MSKAIU		0x0004
 #define MSKKIU		0x0008
 #define MSKFIR		0x0010
 #define MSKDSIU	0x0820
 #define MSKCSI		0x0040
 #define MSKPCIU	0x0080
 #define MSKSSIU	0x0100
 #define MSKSHSP	0x0200
 #define MSKFFIR	0x0400
 #define MSKSCSI	0x1000
 #define MSKPPCIU	0x2000
#define CMUCLKMSK2	KSEG1ADDR(0x0f000064)
 #define MSKCEU		0x0001
 #define MSKMAC0	0x0002
 #define MSKMAC1	0x0004

static uint32_t cmu_base;
static uint16_t cmuclkmsk, cmuclkmsk2;
static spinlock_t cmu_lock;

#define read_cmuclkmsk()	readw(cmu_base)
#define read_cmuclkmsk2()	readw(CMUCLKMSK2)
#define write_cmuclkmsk()	writew(cmuclkmsk, cmu_base)
#define write_cmuclkmsk2()	writew(cmuclkmsk2, CMUCLKMSK2)

void vr41xx_supply_clock(vr41xx_clock_t clock)
{
	spin_lock_irq(&cmu_lock);

	switch (clock) {
	case PIU_CLOCK:
		cmuclkmsk |= MSKPIU;
		break;
	case SIU_CLOCK:
		cmuclkmsk |= MSKSIU | MSKSSIU;
		break;
	case AIU_CLOCK:
		cmuclkmsk |= MSKAIU;
		break;
	case KIU_CLOCK:
		cmuclkmsk |= MSKKIU;
		break;
	case FIR_CLOCK:
		cmuclkmsk |= MSKFIR | MSKFFIR;
		break;
	case DSIU_CLOCK:
		if (current_cpu_data.cputype == CPU_VR4111 ||
		    current_cpu_data.cputype == CPU_VR4121)
			cmuclkmsk |= MSKDSIU;
		else
			cmuclkmsk |= MSKSIU | MSKDSIU;
		break;
	case CSI_CLOCK:
		cmuclkmsk |= MSKCSI | MSKSCSI;
		break;
	case PCIU_CLOCK:
		cmuclkmsk |= MSKPCIU;
		break;
	case HSP_CLOCK:
		cmuclkmsk |= MSKSHSP;
		break;
	case PCI_CLOCK:
		cmuclkmsk |= MSKPPCIU;
		break;
	case CEU_CLOCK:
		cmuclkmsk2 |= MSKCEU;
		break;
	case ETHER0_CLOCK:
		cmuclkmsk2 |= MSKMAC0;
		break;
	case ETHER1_CLOCK:
		cmuclkmsk2 |= MSKMAC1;
		break;
	default:
		break;
	}

	if (clock == CEU_CLOCK || clock == ETHER0_CLOCK ||
	    clock == ETHER1_CLOCK)
		write_cmuclkmsk2();
	else
		write_cmuclkmsk();

	spin_unlock_irq(&cmu_lock);
}

void vr41xx_mask_clock(vr41xx_clock_t clock)
{
	spin_lock_irq(&cmu_lock);

	switch (clock) {
	case PIU_CLOCK:
		cmuclkmsk &= ~MSKPIU;
		break;
	case SIU_CLOCK:
		if (current_cpu_data.cputype == CPU_VR4111 ||
		    current_cpu_data.cputype == CPU_VR4121) {
			cmuclkmsk &= ~(MSKSIU | MSKSSIU);
		} else {
			if (cmuclkmsk & MSKDSIU)
				cmuclkmsk &= ~MSKSSIU;
			else
				cmuclkmsk &= ~(MSKSIU | MSKSSIU);
		}
		break;
	case AIU_CLOCK:
		cmuclkmsk &= ~MSKAIU;
		break;
	case KIU_CLOCK:
		cmuclkmsk &= ~MSKKIU;
		break;
	case FIR_CLOCK:
		cmuclkmsk &= ~(MSKFIR | MSKFFIR);
		break;
	case DSIU_CLOCK:
		if (current_cpu_data.cputype == CPU_VR4111 ||
		    current_cpu_data.cputype == CPU_VR4121) {
			cmuclkmsk &= ~MSKDSIU;
		} else {
			if (cmuclkmsk & MSKSIU)
				cmuclkmsk &= ~MSKDSIU;
			else
				cmuclkmsk &= ~(MSKSIU | MSKDSIU);
		}
		break;
	case CSI_CLOCK:
		cmuclkmsk &= ~(MSKCSI | MSKSCSI);
		break;
	case PCIU_CLOCK:
		cmuclkmsk &= ~MSKPCIU;
		break;
	case HSP_CLOCK:
		cmuclkmsk &= ~MSKSHSP;
		break;
	case PCI_CLOCK:
		cmuclkmsk &= ~MSKPPCIU;
		break;
	case CEU_CLOCK:
		cmuclkmsk2 &= ~MSKCEU;
		break;
	case ETHER0_CLOCK:
		cmuclkmsk2 &= ~MSKMAC0;
		break;
	case ETHER1_CLOCK:
		cmuclkmsk2 &= ~MSKMAC1;
		break;
	default:
		break;
	}

	if (clock == CEU_CLOCK || clock == ETHER0_CLOCK ||
	    clock == ETHER1_CLOCK)
		write_cmuclkmsk2();
	else
		write_cmuclkmsk();

	spin_unlock_irq(&cmu_lock);
}

static int __init vr41xx_cmu_init(void)
{
	switch (current_cpu_data.cputype) {
        case CPU_VR4111:
        case CPU_VR4121:
                cmu_base = CMUCLKMSK_TYPE1;
                break;
        case CPU_VR4122:
        case CPU_VR4131:
                cmu_base = CMUCLKMSK_TYPE2;
                break;
        case CPU_VR4133:
                cmu_base = CMUCLKMSK_TYPE2;
		cmuclkmsk2 = read_cmuclkmsk2();
                break;
	default:
		panic("Unexpected CPU of NEC VR4100 series");
		break;
        }

	cmuclkmsk = read_cmuclkmsk();

	spin_lock_init(&cmu_lock);

	return 0;
}

early_initcall(vr41xx_cmu_init);
