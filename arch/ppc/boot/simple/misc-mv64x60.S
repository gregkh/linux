/*
 * arch/ppc/boot/simple/misc-mv64x60.S
 *
 * Code to change the base address of the host bridges and call board specific
 * init routine.
 *
 * Author: Mark Greer <mgreer@mvista.com>
 *
 * 2002 (c) MontaVista, Software, Inc.  This file is licensed under the terms
 * of the GNU General Public License version 2.  This program is licensed
 * "as is" without any warranty of any kind, whether express or implied.
 */

#include <linux/config.h>
#include <asm/ppc_asm.h>
#include <asm/processor.h>
#include <asm/cache.h>
#include <asm/mv64x60_defs.h>

	.globl	mv64x60_init
mv64x60_init:
	mflr	r27

#if (CONFIG_MV64X60_NEW_BASE != CONFIG_MV64X60_BASE)
	bl	move_base
#endif
	bl	mv64x60_board_init

	mtlr	r27
	blr

#if (CONFIG_MV64X60_NEW_BASE != CONFIG_MV64X60_BASE)
move_base:
	li	r20,0
	li	r23,20

	/* Relocate bridge's regs */
	addis	r25,0,CONFIG_MV64X60_BASE@h
	ori	r25,r25,MV64x60_INTERNAL_SPACE_DECODE
	lwbrx	r26,0,(r25)
	lis	r24,0xffff
	and	r26,r26,r24
	addis	r24,0,CONFIG_MV64X60_NEW_BASE@h
	srw	r24,r24,r23
	or	r26,r26,r24
	stwbrx	r26,0,(r25)
	sync

	/* Wait for write to take effect */
	addis	r25,0,CONFIG_MV64X60_NEW_BASE@h
	ori	r25,r25,MV64x60_INTERNAL_SPACE_DECODE
1:	lwbrx	r24,0,(r25)
	cmpw	r24,r26
	bne	1b

	blr
#endif
