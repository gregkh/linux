/****************************************************************************/

/*
 *	semp.h -- SecureEdge MP3 hardware platform support.
 *
 *	(C) Copyright 2001-2002, Greg Ungerer (gerg@snapgear.com).
 */

/****************************************************************************/
#ifndef	semp3_h
#define	semp3_h
/****************************************************************************/

#include <linux/config.h>

/****************************************************************************/
#ifdef CONFIG_SECUREEDGEMP3
/****************************************************************************/

#include <asm/coldfire.h>
#include <asm/mcfsim.h>

/*
 *	The ColdFire UARTs do not have any support for DTR/DCD lines.
 *	We have wired them onto some of the parallel IO lines.
 */
#define	MCFPP_DCD1	0x0004
#define	MCFPP_DCD0	0x0000		/* No DCD line on port 0 */
#define	MCFPP_DTR1	0x0080
#define	MCFPP_DTR0	0x0000		/* No DTR line on port 0 */


#ifndef __ASSEMBLY__

extern volatile unsigned short ppdata;

/*
 *	These functions defined to give quasi generic access to the
 *	PPIO bits used for DTR/DCD.
 */
static __inline__ unsigned int mcf_getppdata(void)
{
	volatile unsigned short *pp;
	pp = (volatile unsigned short *) (MCF_MBAR + MCFSIM_PADAT);
	return((unsigned int) *pp);
}

static __inline__ void mcf_setppdata(unsigned int mask, unsigned int bits)
{
	volatile unsigned short *pp;
	pp = (volatile unsigned short *) (MCF_MBAR + MCFSIM_PADAT);
	ppdata = (ppdata & ~mask) | bits;
	*pp = ppdata;
}
#endif

/****************************************************************************/
#endif /* CONFIG_SECUREEDGEMP3 */
/****************************************************************************/
#endif	/* semp3_h */
