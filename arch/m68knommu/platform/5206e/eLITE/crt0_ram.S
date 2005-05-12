/*****************************************************************************/

/*
 *	crt0_ram.S -- startup code for MCF5206e ColdFire based eLITE boards.
 *
 *	(C) Copyright 1999-2002, Greg Ungerer (gerg@snapgear.com).
 *	Copyright (C) 1999 Rob Scott (rscott@mtrob.fdns.net)
 *
 * 	1999/02/24 Modified for the 5307 processor David W. Miller
 */

/*****************************************************************************/

#include "linux/autoconf.h"
#include "asm/coldfire.h"
#include "asm/mcfsim.h"

/*****************************************************************************/

/*
 *	M5206eLITE ColdFire eval board, chip select and memory setup.
 */

#ifdef CONFIG_SMALL
#define	MEM_BASE	0x30000000      /* Base memory for M5206eLITE */
#define	MEM_RESERVED	0x00020000      /* Don't use memory reserved by dBUG */
#define	MEM_SIZE	0x00100000      /* 1 MB of SRAM on M5206eLITE */
#else
#define	MEM_BASE	0x00000000      /* Base memory for M5206eLITE */
#define	MEM_RESERVED	0x00010000      /* Skip first MEM_LUMP for colilo */
#define	MEM_SIZE	0x02000000      /* Max DRAM 32Mb */
#endif
#define	MEM_MIN		MEM_BASE+MEM_RESERVED
/* Define end of probeable memory space */
#define	MEM_MAX		MEM_BASE+MEM_SIZE
#define	MEM_BUILTIN	0x20000000      /* Put built in SRAM at dBUG loc */
#define	MEM_TMPSTACK	MEM_BUILTIN+0x800 /* Use built in SRAM for tmp stack */
#define	MEM_LUMP	0x00010000	/* 64 Kb chunks */
#define	VBR_BASE	MEM_BUILTIN     /* Use built in SRAM for vectors */

#define	CS0_ADDR	0x0000ffe0	/* CS0 connected to Flash ROM */
#define	CS0_MASK	0x000f0000	/*   is 1Mbyte */
#define	CS0_CTRL	0x00001da3	/*   read-write (for flash) */
#define	CS1_ADDR	0x00000000	/* CS1 not connected */
#define	CS1_MASK	0x00000000
#define	CS1_CTRL	0x00000000
#define	CS2_ADDR	0x00003000	/* CS2 connected to SRAM */
#define	CS2_MASK	0x000f0000	/*   is 1Mbyte */
#define	CS2_CTRL	0x00001903	/*   read-write */
#define	CS3_ADDR	0x00004000	/* CS3 connected to LED, par port */
#define	CS3_MASK	0x000f0000	/*   is 1Mbyte */
#define	CS3_CTRL	0x00000183	/*   read-write */
#define	CS4_ADDR	0x00000000	/* CS4 not connected */
#define	CS4_MASK	0x00000000
#define	CS4_CTRL	0x00000000
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

#define	DCRR		0x00000034	/* Refresh period  */
/* DCTR definition:
     <15>: DAEM, 1 = Drive Multiplexed Address During External Master DRAM xfer
     <14>: EDO,  1 = EDO, 0 = Normal
     <12>: RCD,  1 = 2 clk RAS-to-CAS, 0 = 1.0 clk RAS-to-CAS
  <10:09>: RSH,  10 = 3.5 clk RAS low, 01 = 2.5 clk, 00 = 1.5 clk
  <06:05>: RP,   10 = 3.5 clk RAS Precharge, 01 = 2.5 clk, 00 = 1.5 clk
     <03>: CAS,  1 = 2.5 clk CAS assertion, 0 = 1.5 clk
     <01>: CP,   1 = 1.5 CAS clk precharge, 0 = .5 clk
     <00>: CSR,  1 = 2.0 clk CAS before RAS setup, 0 = 1.0 clk
*/
#define	DCTR		0x0000144B	/* Slow DRAM */
#define	DCAR0		0x00000000	/* DRAM0 address, 0 base addr */
#define	DCMR0		0x003e0000	/* DRAM0 mask, 4Mb DRAM */
#define	DCCR0		0x00000007	/* DRAM0 control, R/W, burst pg mde */
#define	DCAR1		0x00000000	/* DRAM1 address, 0 base addr */
#define	DCMR1		0x00000000	/* DRAM1 mask, no DRAM */
#define	DCCR1		0x00000000	/* DRAM1 control, off */

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

	move.l	#MCF_MBAR+1, %a0		/* Set I/O base addr */
	movec	%a0, %MBAR			/* Note: bit 0 is Validate */
	move.l	#MEM_BUILTIN+1,%a0		/* Set SRAM base addr */
	movec	%a0, %RAMBAR0			/* Note: bit 0 is Validate */

	move.l	#MCF_MBAR, %a0			/* Get I/O base addr */

	/* ----------------------- CS1 ----------------------- */
	move.w	#CS1_ADDR, %d0			/* CS1 address */
	move.w	%d0, MCFSIM_CSAR1(%a0)		/* CS1 address */
	move.l	#CS1_MASK, %d0			/* CS1 mask */
	move.l	%d0, MCFSIM_CSMR1(%a0)		/* CS1 mask */
	move.w	#CS1_CTRL, %d0			/* CS1 control */
	move.w	%d0, MCFSIM_CSCR1(%a0)		/* CS1 control */

	/* ----------------------- CS2 ----------------------- */
	move.w	#CS2_ADDR, %d0			/* CS2 address */
	move.w	%d0, MCFSIM_CSAR2(%a0)		/* CS2 address */
	move.l	#CS2_MASK, %d0			/* CS2 mask */
	move.l	%d0, MCFSIM_CSMR2(%a0)		/* CS2 mask */
	move.w	#CS2_CTRL, %d0			/* CS2 control */
	move.w	%d0, MCFSIM_CSCR2(%a0)		/* CS2 control */

	/* ----------------------- CS3 ----------------------- */
	move.w	#CS3_ADDR, %d0			/* CS3 address */
	move.w	%d0, MCFSIM_CSAR3(%a0)		/* CS3 address */
	move.l	#CS3_MASK, %d0			/* CS3 mask */
	move.l	%d0, MCFSIM_CSMR3(%a0)		/* CS3 mask */
	move.w	#CS3_CTRL, %d0			/* CS3 control */
	move.w	%d0, MCFSIM_CSCR3(%a0)		/* CS3 control */

	/* ----------------------- CS4 ----------------------- */
	move.w	#CS4_ADDR, %d0			/* CS4 address */
	move.w	%d0, MCFSIM_CSAR4(%a0)		/* CS4 address */
	move.l	#CS4_MASK, %d0			/* CS4 mask */
	move.l	%d0, MCFSIM_CSMR4(%a0)		/* CS4 mask */
	move.w	#CS4_CTRL, %d0			/* CS4 control */
	move.w	%d0, MCFSIM_CSCR4(%a0)		/* CS4 control */

	/* ----------------------- CS5 ----------------------- */
	move.w	#CS5_ADDR, %d0			/* CS5 address */
	move.w	%d0, MCFSIM_CSAR5(%a0)		/* CS5 address */
	move.l	#CS5_MASK, %d0			/* CS5 mask */
	move.l	%d0, MCFSIM_CSMR5(%a0)		/* CS5 mask */
	move.w	#CS5_CTRL, %d0			/* CS5 control */
	move.w	%d0, MCFSIM_CSCR5(%a0)		/* CS5 control */

	/* ----------------------- CS6 ----------------------- */
	move.w	#CS6_ADDR, %d0			/* CS6 address */
	move.w	%d0, MCFSIM_CSAR6(%a0)		/* CS6 address */
	move.l	#CS6_MASK, %d0			/* CS6 mask */
	move.l	%d0, MCFSIM_CSMR6(%a0)		/* CS6 mask */
	move.w	#CS6_CTRL, %d0			/* CS6 control */
	move.w	%d0, MCFSIM_CSCR6(%a0)		/* CS6 control */

	/* ----------------------- CS7 ----------------------- */
	move.w	#CS7_ADDR, %d0			/* CS7 address */
	move.w	%d0, MCFSIM_CSAR7(%a0)		/* CS7 address */
	move.l	#CS7_MASK, %d0			/* CS7 mask */
	move.l	%d0, MCFSIM_CSMR7(%a0)		/* CS7 mask */
	move.w	#CS7_CTRL, %d0			/* CS7 control */
	move.w	%d0, MCFSIM_CSCR7(%a0)		/* CS7 control */

	/* --------------------- Default --------------------- */
	move.w	#DMC_CTRL, %d0			/* Default control */
	move.w	%d0, MCFSIM_DMCR(%a0)		/* Default control */

	/* ----------------------- DRAM ------------------------ */
	move.w	#DCRR, %d0			/* Refresh period */
	move.w	%d0, MCFSIM_DCRR(%a0)		/* Refresh period */
	move.w	#DCTR, %d0			/* Timing address */
	move.w	%d0, MCFSIM_DCTR(%a0)		/* Timing address */
	move.w	#DCAR0, %d0			/* DRAM0 base address */
	move.w	%d0, MCFSIM_DCAR0(%a0)		/* DRAM0 base address */
	move.l	#DCMR0, %d0			/* DRAM0 mask */
	move.l	%d0, MCFSIM_DCMR0(%a0)		/* DRAM0 mask */
	move.b	#DCCR0, %d0			/* DRAM0 control */
	move.b	%d0, MCFSIM_DCCR0(%a0)		/* DRAM0 control */
	move.w	#DCAR1, %d0			/* DRAM1 base address */
	move.w	%d0, MCFSIM_DCAR1(%a0)		/* DRAM1 base address */
	move.l	#DCMR1, %d0			/* DRAM1 mask */
	move.l	%d0, MCFSIM_DCMR1(%a0)		/* DRAM1 mask */
	move.b	#DCCR1, %d0			/* DRAM1 control */
	move.b	%d0, MCFSIM_DCCR1(%a0)		/* DRAM1 control */

	/*
	 * ChipSelect 0 - ROM cs
	 *
	 * ChipSelect 0 is the global chip select coming out of system reset.
	 * CS0 is asserted for every access until CSMR0 is written.  Therefore,
	 * the entire ChipSelect must be properly set prior to asserting
	 * CSCR0_V.
	 */
	move.w	#CS0_ADDR, %d0			/* CS0 address */
	move.w	%d0, MCFSIM_CSAR0(%a0)		/* CS0 address */
	move.l	#CS0_MASK, %d0			/* CS0 mask */
	move.l	%d0, MCFSIM_CSMR0(%a0)		/* CS0 mask */
	move.w	#CS0_CTRL, %d0			/* CS0 control */
	move.w	%d0, MCFSIM_CSCR0(%a0)		/* CS0 control */


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
	 *	Determine size of RAM, then set up initial stack
	 *	Done differently for different eval boards and cpus.
	 */

#if defined(CONFIG_SMALL)
	/*
	 *	Set to SRAM size when configuring a minimal system
	 */
	move.l	#MEM_MAX, %a0

#else
	/*
	 *	On the Arnewsh 5206 board and the Motorola m5206eLITE board
	 *	we can probe for the amount of DRAM present...
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
#endif

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
	 *      Load the current task pointer and stack.
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
