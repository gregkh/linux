/*
 * arch/ppc/boot/simple/mv64x60_stub.c
 *
 * Stub for board_init() routine called from mv64x60_init().
 *
 * Author: Mark A. Greer <mgreer@mvista.com>
 *
 * 2002 (c) MontaVista, Software, Inc.  This file is licensed under the terms
 * of the GNU General Public License version 2.  This program is licensed
 * "as is" without any warranty of any kind, whether express or implied.
 */

#include <linux/config.h>

#if defined(CONFIG_SERIAL_MPSC_CONSOLE)
long __attribute__ ((weak))	mv64x60_console_baud = 9600;
long __attribute__ ((weak))	mv64x60_mpsc_clk_src = 8; /* TCLK */
long __attribute__ ((weak))	mv64x60_mpsc_clk_freq = 100000000;
#endif

void __attribute__ ((weak))
mv64x60_board_init(void)
{
}
