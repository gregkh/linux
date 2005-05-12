/*
 * OMAP clock interface
 *
 * Copyright (C) 2001 RidgeRun, Inc
 * Written by Gordon McNutt <gmcnutt@ridgerun.com>
 * Updated 2004 for Linux 2.6 by Tony Lindgren <tony@atomide.com>
 *
 * This program is free software; you can redistribute	it and/or modify it
 * under  the terms of	the GNU General	 Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * You should have received a copy of the  GNU General Public License along
 * with this program; if not, write  to the Free Software Foundation, Inc.,
 * 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef __ASM_ARM_CLOCKS_H
#define __ASM_ARM_CLOCKS_H

#include <linux/config.h>

/* ARM_CKCTL bit shifts */
#define PERDIV			0
#define LCDDIV			2
#define ARMDIV			4
#define DSPDIV			6
#define TCDIV			8
#define DSPMMUDIV		10
#define ARM_TIMXO		12
#define EN_DSPCK		13
#define ARM_INTHCK_SEL		14 /* REVISIT: Where is this used? */

/* ARM_IDLECT1 bit shifts */
#define IDLWDT_ARM	0
#define IDLXORP_ARM	1
#define IDLPER_ARM	2
#define IDLLCD_ARM	3
#define IDLLB_ARM	4
#define IDLHSAB_ARM	5
#define IDLIF_ARM	6
#define IDLDPLL_ARM	7
#define IDLAPI_ARM	8
#define IDLTIM_ARM	9
#define SETARM_IDLE	11

/* ARM_IDLECT2 bit shifts */
#define EN_WDTCK	0
#define EN_XORPCK	1
#define EN_PERCK	2
#define EN_LCDCK	3
#define EN_LBCK		4
#define EN_HSABCK	5
#define EN_APICK	6
#define EN_TIMCK	7
#define DMACK_REQ	8
#define EN_GPIOCK	9
#define EN_LBFREECK	10

/*
 * OMAP clocks
 */
typedef enum {
	/* Fixed system clock */
	OMAP_CLKIN = 0,

	/* DPLL1 */
	OMAP_CK_GEN1, OMAP_CK_GEN2, OMAP_CK_GEN3,

	/* TC usually needs to be checked before anything else */
	OMAP_TC_CK,

	/* CLKM1 */
	OMAP_ARM_CK, OMAP_MPUPER_CK, OMAP_ARM_GPIO_CK, OMAP_MPUXOR_CK,
	OMAP_MPUTIM_CK, OMAP_MPUWD_CK,

	/* CLKM2 */
	OMAP_DSP_CK, OMAP_DSPMMU_CK,
#if 0
	/* Accessible only from the dsp */
	OMAP_DSPPER_CK, OMAP_GPIO_CK, OMAP_DSPXOR_CK, OMAP_DSPTIM_CK,
	OMAP_DSPWD_CK, OMAP_UART_CK,
#endif
	/* CLKM3 */
	OMAP_DMA_CK, OMAP_API_CK, OMAP_HSAB_CK, OMAP_LBFREE_CK,
	OMAP_LB_CK, OMAP_LCD_CK
} ck_t;

typedef enum {
	/* Reset the MPU */
	OMAP_ARM_RST,

	/* Reset the DSP */
	OMAP_DSP_RST,

	/* Reset priority registers, EMIF config, and MPUI control logic */
	OMAP_API_RST,

	/* Reset DSP, MPU, and Peripherals */
	OMAP_SW_RST,
} reset_t;

#define OMAP_CK_MIN			OMAP_CLKIN
#define OMAP_CK_MAX			OMAP_LCD_CK

#if defined(CONFIG_OMAP_ARM_30MHZ)
#define OMAP_CK_MAX_RATE		30
#elif defined(CONFIG_OMAP_ARM_60MHZ)
#define OMAP_CK_MAX_RATE		60
#elif defined(CONFIG_OMAP_ARM_96MHZ)
#define OMAP_CK_MAX_RATE		96
#elif defined(CONFIG_OMAP_ARM_120MHZ)
#define OMAP_CK_MAX_RATE		120
#elif defined(CONFIG_OMAP_ARM_168MHZ)
#define OMAP_CK_MAX_RATE		168
#elif defined(CONFIG_OMAP_ARM_182MHZ)
#define OMAP_CK_MAX_RATE		182
#elif defined(CONFIG_OMAP_ARM_192MHZ)
#define OMAP_CK_MAX_RATE		192
#elif defined(CONFIG_OMAP_ARM_195MHZ)
#define OMAP_CK_MAX_RATE		195
#endif

#define CK_DPLL_MASK			0x0fe0

/* Shared by CK and DSPC */
#define MPUI_STROBE_MAX_1509		24
#define MPUI_STROBE_MAX_1510		30

/*
 * ----------------------------------------------------------------------------
 * Clock interface functions
 * ----------------------------------------------------------------------------
 */

/*  Clock initialization.  */
int init_ck(void);

/*
 * For some clocks you have a choice of which "parent" clocks they are derived
 * from. Use this to select a "parent". See the platform documentation for
 * valid combinations.
 */
int ck_can_set_input(ck_t);
int ck_set_input(ck_t ck, ck_t input);
int ck_get_input(ck_t ck, ck_t *input);

/*
 * Use this to set a clock rate. If other clocks are derived from this one,
 * their rates will all change too. If this is a derived clock and I can't
 * change it to match your request unless I also change the parent clock, then
 * tough luck -- I won't change the parent automatically. I'll return an error
 * if I can't get the clock within 10% of what you want. Otherwise I'll return
 * the value I actually set it to. If I have to switch parents to get the rate
 * then I will do this automatically (since it only affects this clock and its
 * descendants).
 */
int ck_can_set_rate(ck_t);
int ck_set_rate(ck_t ck, int val_in_mhz);
int ck_get_rate(ck_t ck);

/*
 * Use this to get a bitmap of available rates for the clock. Caller allocates
 *  the buffer and passes in the length. Clock module fills up to len bytes of
 *  the buffer & passes back actual bytes used.
 */
int ck_get_rates(ck_t ck, void *buf, int len);
int ck_valid_rate(int rate);

/*
 * Idle a clock. What happens next depends on the clock ;). For example, if
 * you idle the ARM_CK you might well end up in sleep mode on some platforms.
 * If you try to idle a clock that doesn't support it I'll return an error.
 * Note that idling a clock does not always take affect until certain h/w
 * conditions are met. Consult the platform specs to learn more.
 */
int ck_can_idle(ck_t);
int ck_idle(ck_t);
int ck_activate(ck_t);
int ck_is_idle(ck_t);

/*
 * Enable/disable a clock. I'll return an error if the h/w doesn't support it.
 * If you disable a clock being used by an active device then you probably
 * just screwed it. YOU are responsible for making sure this doesn't happen.
 */
int ck_can_disable(ck_t);
int ck_enable(ck_t);
int ck_disable(ck_t);
int ck_is_enabled(ck_t);

/* Enable/reset ARM peripherals (remove/set reset signal) */
void ck_enable_peripherals(void);
void ck_reset_peripherals(void);

/* Generate/clear a MPU or DSP reset */
void ck_generate_reset(reset_t reset);
void ck_release_from_reset(reset_t reset);

/* This gets a string representation of the clock's name. Useful for proc. */
char *ck_get_name(ck_t);

extern void start_mputimer1(unsigned long);

#endif
