/*
 * arch/ppc/boot/simple/misc-ev64260.S
 *
 * Host bridge init code for the Marvell/Galileo EV-64260-BP evaluation board
 * with a GT64260 onboard.
 *
 * Author: Mark Greer <mgreer@mvista.com>
 *
 * 2001 (c) MontaVista Software, Inc. This file is licensed under
 * the terms of the GNU General Public License version 2. This program
 * is licensed "as is" without any warranty of any kind, whether express
 * or implied.
 */

#include <asm/ppc_asm.h>
#include <asm/processor.h>
#include <asm/cache.h>
#include <asm/mv64x60_defs.h>
#include <platforms/ev64260.h>

	.globl	mv64x60_board_init
mv64x60_board_init:
	/* DINK doesn't enable 745x timebase, so enable here (Adrian Cox) */
	mfspr	r25,PVR
	srwi	r25,r25,16
	cmplwi	r25,(PVR_7450 >> 16)
	bne	1f
	mfspr	r25,HID0
	oris	r25,r25,(HID0_TBEN >> 16)
	mtspr	HID0,r25
1:
#if (CONFIG_MV64X60_NEW_BASE != CONFIG_MV64X60_BASE)
	li	r23,20

	/*
	 * Change the CS2 window for the UART so that the bootloader
	 * can do I/O thru the UARTs.
	 */
	addis	r25,0,CONFIG_MV64X60_NEW_BASE@h
	ori	r25,r25,MV64x60_CPU2DEV_2_BASE
	addis	r26,0,EV64260_UART_BASE@h
	srw	r26,r26,r23
	stwbrx	r26,0,(r25)
	sync

	addis	r25,0,CONFIG_MV64X60_NEW_BASE@h
	ori	r25,r25,MV64x60_CPU2DEV_2_SIZE
	addis	r26,0,EV64260_UART_END@h
	srw	r26,r26,r23
	stwbrx	r26,0,(r25)
	sync
#endif
	blr

#if defined(CONFIG_SERIAL_MPSC_CONSOLE)
.data
	.globl	mv64x60_console_baud
mv64x60_console_baud:
.long	EV64260_DEFAULT_BAUD

	.globl	mv64x60_mpsc_clk_src
mv64x60_mpsc_clk_src:
.long	EV64260_MPSC_CLK_SRC

	.globl	mv64x60_mpsc_clk_freq
mv64x60_mpsc_clk_freq:
.long	EV64260_MPSC_CLK_FREQ
#endif
