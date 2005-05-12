/*****************************************************************************/

/*
 *	crt0_ram.S -- startup code for MCF5307 ColdFire based NETtel.
 *
 *	(C) Copyright 1999-2002, Greg Ungerer (gerg@snapgear.com).
 * 	Copyright (C) 2000  Lineo Inc. (www.lineo.com) 
 *
 * 	1999/02/24 Modified for the 5307 processor David W. Miller
 */

/*****************************************************************************/

#include "linux/autoconf.h"
#include "asm/coldfire.h"
#include "asm/mcfsim.h"
#include "asm/nettel.h"

/*****************************************************************************/

/*
 *	Lineo NETtel board memory setup.
 */
#define	MEM_BASE	0x00000000	/* Memory base at address 0 */
#define	VBR_BASE	MEM_BASE	/* Vector address */

#if defined(CONFIG_RAM16MB)
#define	MEM_SIZE	0x01000000	/* Memory size 16Mb */
#elif defined(CONFIG_RAM8MB)
#define	MEM_SIZE	0x00800000	/* Memory size 8Mb */
#else
#define	MEM_SIZE	0x00400000	/* Memory size 4Mb */
#endif

/*****************************************************************************/

.global	_start
.global _rambase
.global _ramvec
.global	_ramstart
.global	_ramend

/*****************************************************************************/

.data

/*
 *	Set up the usable of RAM stuff. Size of RAM is determined then
 *	an initial stack set up at the end.
 */
_rambase:
.long	0
_ramvec:
.long	0
_ramstart:
.long	0
_ramend:
.long	0

/*****************************************************************************/

/*
 *	The NETtel platform has some funky LEDs!
 */
.global ppdata
ppdata:
.short	0x0000

.global ledbank
ledbank:
.byte	0xff

/*****************************************************************************/

.text

/*
 *	This is the codes first entry point. This is where it all
 *	begins...
 */

_start:
	nop					/* Filler */
	move.w	#0x2700, %sr			/* No interrupts */

	/*
	 * Setup VBR here, otherwise buserror remap will not work.
	 * if dBug was active before (on my SBC with dBug 1.1 of Dec 16 1996)
	 *
	 * bkr@cut.de 19990306
	 *
	 * Note: this is because dBUG points VBR to ROM, making vectors read
	 * only, so the bus trap can't be changed. (RS)
	 */
	move.l	#VBR_BASE, %a7			/* Note VBR can't be read */
	movec   %a7, %VBR
	move.l	%a7, _ramvec			/* Set up vector addr */
	move.l	%a7, _rambase			/* Set up base RAM addr */


	/*
	 *	Determine size of RAM, then set up initial stack.
	 */
	move.l	#MEM_SIZE, %a0

	move.l	%a0, %d0			/* Mem end addr is in a0 */
	move.l	%d0, %sp			/* Set up initial stack ptr */
	move.l	%d0, _ramend			/* Set end ram addr */

	/*
	 *	Enable CPU internal cache.
	 */
	move.l	#0x01000000, %d0		/* Invalidate cache cmd */
	movec	%d0, %CACR			/* Invalidate cache */
	move.l	#0x80000100, %d0		/* Setup cache mask */
	movec	%d0, %CACR			/* Enable cache */
	nop


#ifdef CONFIG_ROMFS_FS
#ifdef CONFIG_ROMFS_FROM_ROM
	/*
	 *	check for an in RAM romfs
	 */
	lea.l	_sbss, %a0			/* Get start of bss */
	mov.l	(%a0), %d0
	cmp.l	#0x2d726f6d, %d0	/* check for "-rom" */
	bne		use_xip_romfs
	add.l	#4, %a0
	mov.l	(%a0), %d0
	cmp.l	#0x3166732d, %d0	/* check for "1fs-" */
	bne		use_xip_romfs
#endif

	/*
	 *	Move ROM filesystem above bss :-)
	 */
	lea.l	_sbss, %a0			/* Get start of bss */
	lea.l	_ebss, %a1			/* Set up destination  */
	move.l	%a0, %a2			/* Copy of bss start */

	move.l	8(%a0), %d0			/* Get size of ROMFS */
	addq.l	#8, %d0				/* Allow for rounding */
	and.l	#0xfffffffc, %d0		/* Whole words */

	add.l	%d0, %a0			/* Copy from end */
	add.l	%d0, %a1			/* Copy from end */
	move.l	%a1, _ramstart			/* Set start of ram */

_copy_romfs:
	move.l	-(%a0), %d0			/* Copy dword */
	move.l	%d0, -(%a1)
	cmp.l	%a0, %a2			/* Check if at end */
	bne	_copy_romfs

#ifdef CONFIG_ROMFS_FROM_ROM
	bra		done_romfs
  use_xip_romfs:
	lea.l	_ebss, %a1			/* Set up destination  */
	mov.l	#0, (%a1)			/* make sure we don't use an old RAM version */
	move.l	%a1, _ramstart		/* Set start of ram */
  done_romfs:
#endif

#else /* CONFIG_ROMFS_FS */
	lea.l	_ebss, %a1
	move.l	%a1, _ramstart
#endif /* CONFIG_ROMFS_FS */


	/*
	 *	Zero out the bss region.
	 */
	lea.l	_sbss, %a0			/* Get start of bss */
	lea.l	_ebss, %a1			/* Get end of bss */
	clr.l	%d0				/* Set value */
_clear_bss:
	move.l	%d0, (%a0)+			/* Clear each word */
	cmp.l	%a0, %a1			/* Check if at end */
	bne	_clear_bss

	/*
	 * load the current task pointer and stack
	 */
	lea	init_thread_union, %a0
	lea	0x2000(%a0), %sp

	/*
	 *	Assember start up done, start code proper.
	 */
	jsr	start_kernel			/* Start Linux kernel */

_exit:
	jmp	_exit				/* Should never get here */

/*****************************************************************************/
