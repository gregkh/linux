/*
 * linux/arch/arm/mach-omap/leds-h2p2-debug.c
 *
 * Copyright 2003 by Texas Instruments Incorporated
 *
 */
#include <linux/config.h>
#include <linux/init.h>
#include <linux/kernel_stat.h>
#include <linux/sched.h>
#include <linux/version.h>

#include <asm/io.h>
#include <asm/hardware.h>
#include <asm/leds.h>
#include <asm/system.h>

#include <asm/arch/fpga.h>

#include "leds.h"

void h2p2_dbg_leds_event(led_event_t evt)
{
	unsigned long flags;
	static unsigned long hw_led_state = 0;

	local_irq_save(flags);

	switch (evt) {
	case led_start:
		hw_led_state |= H2P2_DBG_FPGA_LED_STARTSTOP;
		break;

	case led_stop:
		hw_led_state &= ~H2P2_DBG_FPGA_LED_STARTSTOP;
		break;

	case led_claim:
		hw_led_state |= H2P2_DBG_FPGA_LED_CLAIMRELEASE;
		break;

	case led_release:
		hw_led_state &= ~H2P2_DBG_FPGA_LED_CLAIMRELEASE;
		break;

#ifdef CONFIG_LEDS_TIMER
	case led_timer:
		/*
		 * Toggle Timer LED
		 */
		if (hw_led_state & H2P2_DBG_FPGA_LED_TIMER)
			hw_led_state &= ~H2P2_DBG_FPGA_LED_TIMER;
		else
			hw_led_state |= H2P2_DBG_FPGA_LED_TIMER;
		break;
#endif

#ifdef CONFIG_LEDS_CPU
	case led_idle_start:
		hw_led_state |= H2P2_DBG_FPGA_LED_IDLE;
		break;

	case led_idle_end:
		hw_led_state &= ~H2P2_DBG_FPGA_LED_IDLE;
		break;
#endif

	case led_halted:
		if (hw_led_state & H2P2_DBG_FPGA_LED_HALTED)
			hw_led_state &= ~H2P2_DBG_FPGA_LED_HALTED;
		else
			hw_led_state |= H2P2_DBG_FPGA_LED_HALTED;
		break;

	case led_green_on:
		break;

	case led_green_off:
		break;

	case led_amber_on:
		break;

	case led_amber_off:
		break;

	case led_red_on:
		break;

	case led_red_off:
		break;

	default:
		break;
	}


	/*
	 *  Actually burn the LEDs
	 */
	__raw_writew(~hw_led_state & 0xffff, H2P2_DBG_FPGA_LEDS);

	local_irq_restore(flags);
}
