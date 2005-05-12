/*
 * linux/include/asm-arm/arch-omap/tc.h
 *
 * OMAP Traffic Controller
 *
 * Copyright (C) 2004 Nokia Corporation
 * Author: Imre Deak <imre.deak@nokia.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifndef __ASM_ARCH_TC_H
#define __ASM_ARCH_TC_H

#define OMAP_TC_OCPT1_PRIOR	0xFFFECC00
#define OMAP_TC_EMIFS_PRIOR	0xFFFECC04
#define OMAP_TC_EMIFF_PRIOR	0xFFFECC08
#define OMAP_TC_OCPT2_PRIOR	0xFFFECCD0


/* EMIF Slow Interface Configuration Register */
#define	OMAP_EMIFS_CONFIG_REG	__REG32(EMIFS_CONFIG)

#define OMAP_EMIFS_CONFIG_FR		(1 << 4)
#define OMAP_EMIFS_CONFIG_PDE		(1 << 3)
#define OMAP_EMIFS_CONFIG_PWD_EN	(1 << 2)
#define OMAP_EMIFS_CONFIG_BM		(1 << 1)
#define OMAP_EMIFS_CONFIG_WP		(1 << 0)

/* external EMIFS chipselect regions */
#define	OMAP_CS1_PHYS		0x04000000
#define	OMAP_CS1_SIZE		SZ_64M

#define	OMAP_CS1A_PHYS		OMAP_CS1_PHYS
#define	OMAP_CS1A_SIZE		SZ_32M

#define	OMAP_CS1B_PHYS		(OMAP_CS1A_PHYS + OMAP_CS1A_SIZE)
#define	OMAP_CS1B_SIZE		SZ_32M

#define	OMAP_CS2_PHYS		0x08000000
#define	OMAP_CS2_SIZE		SZ_64M

#define	OMAP_CS2A_PHYS		OMAP_CS2_PHYS
#define	OMAP_CS2A_SIZE		SZ_32M

#define	OMAP_CS2B_PHYS		(OMAP_CS2A_PHYS + OMAP_CS2A_SIZE)
#define	OMAP_CS2B_SIZE		SZ_32M

#define	OMAP_CS3_PHYS		0x0c000000
#define	OMAP_CS3_SIZE		SZ_64M

#endif	/* __ASM_ARCH_TC_H */
