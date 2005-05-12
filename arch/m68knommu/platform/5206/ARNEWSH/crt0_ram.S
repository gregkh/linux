/*****************************************************************************/

/*
 *	crt0_ram.S -- startup code for MCF5206 ColdFire Arnewsh board.
 *
 *	(C) Copyright 1999-2002, Greg Ungerer (gerg@snapgear.com).
 *
 * 	1999/02/24 Modified for the 5307 processor David W. Miller
 */

/*****************************************************************************/

#include "linux/autoconf.h"
#include "asm/coldfire.h"
#include "asm/mcfsim.h"

/*****************************************************************************/

/*
 *	Arnewsh m5206 ColdFire eval board, chip select and memory setup.
 */

#define	MEM_BASE	0x00000000	/* Memory base at address 0 */
#define	VBR_BASE	MEM_BASE	/* Vector address */

/*
 *	The following define the limits within which to search for
 *	available RAM.
 */
#define	MEM_MIN		0x00100000	/* Search from 1Mb */
#define	MEM_MAX		0x02000000	/* Max DRAM 32Mb! */
#define	MEM_LUMP	0x00010000	/* 64 Kb chunks */

#define MEM_TMPSTACK	0x00010000	/* At 64k - for exceptions */

/*
 *	Chip Select setup.
 */
#define	CS0_ADDR	0x0000f000	/* CS0 connected to Flash ROM */
#define	CS0_MASK	0xf0000000	/*   is 1Mbyte */
#define	CS0_CTRL	0x00000000	/*   read-only */
#define	CS1_ADDR	0x00000000	/* CS1 not connected */
#define	CS1_MASK	0x00000000
#define	CS1_CTRL	0x00000000
#define	CS2_ADDR	0x00003000	/* CS2 connected to SRAM */
#define	CS2_MASK	0x0000f000	/*   is 1Mbyte */
#define	CS2_CTRL	0x00001903	/*   read-write */
#define	CS3_ADDR	0x00004000	/* CS3 connected to LED, par port */
#define	CS3_MASK	0x0000f000	/*   is 1Mbyte */
#define	CS3_CTRL	0x00000083	/*   read-write */
#define	CS4_ADDR	0x00000000	/* CS4 not connected */
#define	CS4_MASK	0x00000000
#define	CS4_CTRL	0x00000123
#define	CS5_ADDR	0x00000000	/* CS5 not connected */
#define	CS5_MASK	0x00000000
#define	CS5_CTRL	0x00000000
#define	CS6_ADDR	0x00000000	/* CS6 not connected */
#define	CS6_MASK	0x00000000
#define	CS6_CTRL	0x00000000
#define	CS7_ADDR	0x00000000	/* CS7 not connected */
#define	CS7_MASK	0x00000000
#define	CS7_CTRL	0x00000000
#define	DMC_CTRL	0x00000000	/* default memory control */

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
	 *
	 *	On the Arnewsh 5206 board we can probe for the amount
	 *	of DRAM present...
	 */
	move.l	#MEM_MIN, %a0			/* Start at bottom */
	move.l	#MEM_MAX, %a1			/* Set stop point */
	lea.l	MEM_TMPSTACK, %sp		/* Set up tmp stack ptr */

	move.l	#VBR_BASE+8, %a2		/* Address of bus trap */
	lea.l	_ram_buserr, %a3		/* Get RAM trap address */
	move.l	%a3, (%a2)			/* Set trap to local ptr */

_find_ram:
	move.l	(%a0), %d0			/* Attempt read */
	add.l	#MEM_LUMP, %a0			/* Try next bank */
	cmp.l	%a1, %a0			/* Check more? */
	bne	_find_ram

	/*
	 *	BUS error trap handler - used for RAM probing.
	 */
_ram_buserr:
	bra	_found_ram

_found_ram:					/* Vectored here on bus err */
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
