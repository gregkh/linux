/*
 * Device driver for the SYMBIOS/LSILOGIC 53C8XX and 53C1010 family 
 * of PCI-SCSI IO processors.
 *
 * Copyright (C) 1999-2001  Gerard Roudier <groudier@free.fr>
 *
 * This driver is derived from the Linux sym53c8xx driver.
 * Copyright (C) 1998-2000  Gerard Roudier
 *
 * The sym53c8xx driver is derived from the ncr53c8xx driver that had been 
 * a port of the FreeBSD ncr driver to Linux-1.2.13.
 *
 * The original ncr driver has been written for 386bsd and FreeBSD by
 *         Wolfgang Stanglmeier        <wolf@cologne.de>
 *         Stefan Esser                <se@mi.Uni-Koeln.de>
 * Copyright (C) 1994  Wolfgang Stanglmeier
 *
 * Other major contributions:
 *
 * NVRAM detection and reading.
 * Copyright (C) 1997 Richard Waltham <dormouse@farsrobt.demon.co.uk>
 *
 *-----------------------------------------------------------------------------
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sym_glue.h"

#ifdef	SYM_OPT_ANNOUNCE_TRANSFER_RATE
/*
 *  Announce transfer rate if anything changed since last announcement.
 */
void sym_announce_transfer_rate(struct sym_hcb *np, int target)
{
	tcb_p tp = &np->target[target];

#define __tprev	tp->tinfo.prev
#define __tcurr	tp->tinfo.curr

	if (__tprev.options  == __tcurr.options &&
	    __tprev.width    == __tcurr.width   &&
	    __tprev.offset   == __tcurr.offset  &&
	    !(__tprev.offset && __tprev.period != __tcurr.period))
		return;

	__tprev.options  = __tcurr.options;
	__tprev.width    = __tcurr.width;
	__tprev.offset   = __tcurr.offset;
	__tprev.period   = __tcurr.period;

	if (__tcurr.offset && __tcurr.period) {
		u_int period, f10, mb10;
		char *scsi;

		period = f10 = mb10 = 0;
		scsi = "FAST-5";

		if (__tcurr.period <= 9) {
			scsi = "FAST-80";
			period = 125;
			mb10 = 1600;
		}
		else {
			if	(__tcurr.period <= 11) {
				scsi = "FAST-40";
				period = 250;
				if (__tcurr.period == 11)
					period = 303;
			}
			else if	(__tcurr.period < 25) {
				scsi = "FAST-20";
				if (__tcurr.period == 12)
					period = 500;
			}
			else if	(__tcurr.period <= 50) {
				scsi = "FAST-10";
			}
			if (!period)
				period = 40 * __tcurr.period;
			f10 = 100000 << (__tcurr.width ? 1 : 0);
			mb10 = (f10 + period/2) / period;
		}
		printf_info (
		    "%s:%d: %s %sSCSI %d.%d MB/s %s%s%s (%d.%d ns, offset %d)\n",
		    sym_name(np), target, scsi, __tcurr.width? "WIDE " : "",
		    mb10/10, mb10%10,
		    (__tcurr.options & PPR_OPT_DT) ? "DT" : "ST",
		    (__tcurr.options & PPR_OPT_IU) ? " IU" : "",
		    (__tcurr.options & PPR_OPT_QAS) ? " QAS" : "",
		    period/10, period%10, __tcurr.offset);
	}
	else
		printf_info ("%s:%d: %sasynchronous.\n", 
		             sym_name(np), target, __tcurr.width? "wide " : "");
}
#undef __tprev
#undef __tcurr
#endif	/* SYM_OPT_ANNOUNCE_TRANSFER_RATE */
