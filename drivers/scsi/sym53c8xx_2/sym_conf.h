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

#ifndef SYM_CONF_H
#define SYM_CONF_H

#include "sym53c8xx.h"

/*
 *  Max number of targets.
 *  Maximum is 16 and you are advised not to change this value.
 */
#ifndef SYM_CONF_MAX_TARGET
#define SYM_CONF_MAX_TARGET	(16)
#endif

/*
 *  Max number of logical units.
 *  SPI-2 allows up to 64 logical units, but in real life, target
 *  that implements more that 7 logical units are pretty rare.
 *  Anyway, the cost of accepting up to 64 logical unit is low in 
 *  this driver, thus going with the maximum is acceptable.
 */
#ifndef SYM_CONF_MAX_LUN
#define SYM_CONF_MAX_LUN	(64)
#endif

/*
 *  Max number of IO control blocks queued to the controller.
 *  Each entry needs 8 bytes and the queues are allocated contiguously.
 *  Since we donnot want to allocate more than a page, the theorical 
 *  maximum is PAGE_SIZE/8. For safety, we announce a bit less to the 
 *  access method. :)
 *  When not supplied, as it is suggested, the driver compute some 
 *  good value for this parameter.
 */
/* #define SYM_CONF_MAX_START	(PAGE_SIZE/8 - 16) */

/*
 *  Support for Immediate Arbitration.
 *  Not advised.
 */
/* #define SYM_CONF_IARB_SUPPORT */

/*
 *  Only relevant if IARB support configured.
 *  - Max number of successive settings of IARB hints.
 *  - Set IARB on arbitration lost.
 */
#define SYM_CONF_IARB_MAX 3
#define SYM_CONF_SET_IARB_ON_ARB_LOST 1

/*
 *  Returning wrong residuals may make problems.
 *  When zero, this define tells the driver to 
 *  always return 0 as transfer residual.
 *  Btw, all my testings of residuals have succeeded.
 */
#define SYM_SETUP_RESIDUAL_SUPPORT 1

/*
 *  Supported maximum number of LUNs to announce to 
 *  the access method.
 *  The driver supports up to 64 LUNs per target as 
 *  required by SPI-2/SPI-3. However some SCSI devices  
 *  designed prior to these specifications or not being  
 *  conformant may be highly confused when they are 
 *  asked about a LUN > 7.
 */
#ifndef SYM_SETUP_MAX_LUN
#define SYM_SETUP_MAX_LUN	(8)
#endif

#endif /* SYM_CONF_H */
