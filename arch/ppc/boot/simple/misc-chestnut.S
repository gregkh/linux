/*
 * arch/ppc/boot/simple/misc-chestnut.S
 *
 * Setup for the IBM Chestnut (ibm-750fxgx_eval)
 *
 * Author: <source@mvista.com>
 *
 * <2004> (c) MontaVista Software, Inc. This file is licensed under
 * the terms of the GNU General Public License version 2. This program
 * is licensed "as is" without any warranty of any kind, whether express
 * or implied.
 */


#include <asm/ppc_asm.h>
#include <asm/mv64x60_defs.h>
#include <platforms/chestnut.h>

	.globl	mv64x60_board_init
mv64x60_board_init:
	/*
	 * move UART to 0xffc00000
	 */

	li      r23,16

	addis	r25,0,CONFIG_MV64X60_BASE@h
	ori     r25,r25,MV64x60_CPU2DEV_2_BASE
	addis   r26,0,CHESTNUT_UART_BASE@h
	srw     r26,r26,r23
	stwbrx  r26,0,(r25)
	sync

	addis	r25,0,CONFIG_MV64X60_BASE@h
	ori     r25,r25,MV64x60_CPU2DEV_2_SIZE
	addis   r26,0,0x00100000@h
	srw     r26,r26,r23
	stwbrx  r26,0,(r25)
	sync

	blr
