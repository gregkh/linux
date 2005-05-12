/*****************************************************************************/

/*
 *	crt0_ram.S -- startup code for Motorola 5407 eval board.
 *
 *	(C) Copyright 1999-2002, Greg Ungerer (gerg@snapgear.com).
 *	(C) Copyright 2000, Lineo (www.lineo.com).
 *
 * 	1999/02/24 Modified for the 5307 processor David W. Miller
 */

/*****************************************************************************/

#include "linux/autoconf.h"
#include "asm/coldfire.h"
#include "asm/mcfsim.h"

/*****************************************************************************/

/*
 *	Board setup info.
 */
#define	MEM_BASE	0x00000000	/* Memory base at address 0 */
#define	VBR_BASE	MEM_BASE	/* Vector address */

#define	MEM_SIZE	0x02000000	/* Memory size 32 */

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

.text

/*
 *	This is the codes first entry point. This is where it all
 *	begins...
 */

_start:
	nop					/* Filler */
	move.w	#0x2700, %sr			/* No interrupts */

	/*
	 *	Setup VBR as per eval board (really dBUG does this).
	 *	These settings must match it.
	 */
	move.l	#VBR_BASE, %a0			/* Note VBR can't be read */
	movec   %a0, %VBR
	move.l	%a0, _ramvec			/* Set up vector addr */
	move.l	%a0, _rambase			/* Set up base RAM addr */


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
	move.l	#0x01040100, %d0		/* Invalidate whole cache */
	movec	%d0,%CACR
	nop
	move.l	#0x000fc000, %d0		/* Set SDRAM cached only */
	movec	%d0, %ACR0
	move.l	#0x00000000, %d0		/* No other regions cached */
	movec	%d0, %ACR1
	move.l	#0x000fc000, %d0		/* Set SDRAM cached only */
	movec	%d0, %ACR2
	move.l	#0x00000000, %d0		/* No other regions cached */
	movec	%d0, %ACR3

	/* Enable cache */
	move.l	#0xb6088400, %d0		/* Enable caches */
	movec	%d0,%CACR
	nop


#ifdef CONFIG_ROMFS_FS
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
	 *	Load the current task pointer and stack.
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
