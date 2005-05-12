/*
 *  tb0219.c, Setup for the TANBAC TB0219
 *
 *  Copyright (C) 2003  Megasolution Inc. <matsu@megasolution.jp>
 *  Copyright (C) 2004  Yoichi Yuasa <yuasa@hh.iij4u.or.jp>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/init.h>

#include <asm/io.h>
#include <asm/reboot.h>

#define TB0219_RESET_REGS	KSEG1ADDR(0x0a00000e)

#define tb0219_hard_reset()	writew(0, TB0219_RESET_REGS)

static void tanbac_tb0219_restart(char *command)
{
	local_irq_disable();
	tb0219_hard_reset();
	while (1);
}

static int __init tanbac_tb0219_setup(void)
{
	_machine_restart = tanbac_tb0219_restart;

	return 0;
}

early_initcall(tanbac_tb0219_setup);
