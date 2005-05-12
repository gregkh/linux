/*****************************************************************************/

/*
 *	crt0_ram.S -- startup code for Motorola M5249C3 eval board.
 *
 *	(C) Copyright 1999-2002, Greg Ungerer (gerg@snapgear.com).
 */

/*****************************************************************************/

#include "linux/autoconf.h"
#include "asm/coldfire.h"
#include "asm/mcfsim.h"

/*****************************************************************************/

/*
 *	Motorola M5249C3 ColdFire eval board, chip select and memory setup.
 */
#define	MEM_BASE	0x00000000	/* Memory base at address 0 */
#define	MEM_SIZE	0x00800000	/* Memory size 8MB */
#define	VBR_BASE	MEM_BASE	/* Vector address */

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
	 *	Set MBAR1 and MBAR2, just incase they are not set.
	 */
	move.l  #0x10000001, %a0
	movec   %a0, %MBAR			/* Map MBAR region */
	subq.l  #1, %a0				/* Get MBAR address in a0 */

	move.l  #0x80000001, %a1
	movec   %a1, #3086			/* Map MBAR2 region */
	subq.l  #1, %a1				/* Get MBAR2 address in a1 */

	/*
	 *      Move secondary interrupts to base at 128.
	 */
	move.b  #0x80, %d0
	move.b  %d0, 0x16b(%a1)			/* Interrupt base register */

#if 1
	/*
	 *      Work around broken CSMR0/DRAM vector problem.
	 */
	move.l  #0x001F0021, %d0		/* Disable C/I bit */
	move.l  %d0, 0x84(%a0)			/* Set CSMR0 */
#endif

	/*
	 *	Disable the PLL firstly. (Who knows what state it is
	 *	in here!).
	 */
	move.l  0x180(%a1), %d0			/* Get current PLL value */
	and.l   #0xfffffffe, %d0		/* PLL bypass first */
	move.l  %d0, 0x180(%a1)			/* Set PLL register */
	nop

#ifdef CONFIG_CLOCK_140MHz
	/*
	 *	Set initial clock frequency. This assumes M5249C3 board
	 *	is fitted with 11.2896MHz crystal. It will program the
	 *	PLL for 140MHz. Lets go fast :-)
	 */
	move.l  #0x125a40f0, %d0		/* Set for 140MHz */
	move.l  %d0, 0x180(%a1)			/* Set PLL register */
	or.l    #0x1, %d0
	move.l  %d0, 0x180(%a1)			/* Set PLL register */
#endif

	/*
	 *	Setup CS1 for ethernet controller.
	 *	(Setup as per M5249C3 doco).
	 */
	move.l  #0xe0000000, %d0		/* CS1 mapped at 0xe0000000 */
	move.l  %d0, 0x8c(%a0)
	move.l  #0x001f0021, %d0		/* CS1 size of 1Mb */
	move.l  %d0, 0x90(%a0)
	move.w  #0x0080, %d0			/* CS1 = 16bit port, AA */
	move.w  %d0, 0x96(%a0)

	/*
	 *	Setup CS2 for IDE interface.
	 */
	move.l	#0x50000000, %d0		/* CS2 mapped at 0x50000000 */
	move.l	%d0, 0x98(%a0)
	move.l	#0x001f0001, %d0		/* CS2 size of 1MB */
	move.l	%d0, 0x9c(%a0)
	move.w	#0x0080, %d0			/* CS2 = 16bit, TA */
	move.w	%d0, 0xa2(%a0)

	move.l	#0x00107000, %d0		/* IDEconfig1 */
	move.l	%d0, 0x18c(%a1)
	move.l	#0x000c0400, %d0		/* IDEconfig2 */
	move.l	%d0, 0x190(%a1)

	move.l	#0x00080000, %d0		/* GPIO19, IDE reset bit */
	or.l	%d0, 0xc(%a1)			/* Function GPIO19 */
	or.l	%d0, 0x8(%a1)			/* Enable GPIO19 as output */
        or.l	%d0, 0x4(%a1)			/* De-assert IDE reset */


	/*
	 *	Setup VBR as per eval board (really dBUG does this).
	 *	These settings must match it.
	 */
	move.l	#VBR_BASE, %a0			/* Note VBR can't be read */
	movec   %a0, %VBR
	move.l	%a0, _ramvec			/* Set up vector addr */
	move.l	%a0, _rambase			/* Set up base RAM addr */


	/*
	 *	Set the memory size, and then set a temporary stack.
	 */
	move.l	#MEM_SIZE, %a0

	move.l	%a0, %d0			/* Mem end addr is in a0 */
	move.l	%d0, %sp			/* Set up initial stack ptr */
	move.l	%d0, _ramend			/* Set end ram addr */


	/*
	 *	Enable CPU internal cache.
	 */
	move.l  #0x01000000, %d0		/* Invalidate whole cache */
	movec   %d0, %CACR
	nop

	move.l  #0x0000c000, %d0		/* Set SDRAM cached only */
	movec   %d0, %ACR0
	move.l  #0x00000000, %d0		/* No other regions cached */
	movec   %d0, %ACR1

	move.l  #0xa0000200, %d0		/* Enable cache... */
	movec   %d0, %CACR
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
	 * Load the current thread pointer and stack.
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
