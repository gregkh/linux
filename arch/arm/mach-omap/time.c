/*
 * linux/arch/arm/mach-omap/time.c
 *
 * OMAP Timers
 *
 * Copyright (C) 2004 Nokia Corporation
 * Partial timer rewrite and additional VST timer support by
 * Tony Lindgen <tony@atomide.com> and
 * Tuukka Tikkanen <tuukka.tikkanen@elektrobit.com>
 *
 * MPU timer code based on the older MPU timer code for OMAP
 * Copyright (C) 2000 RidgeRun, Inc.
 * Author: Greg Lonnon <glonnon@ridgerun.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
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

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

#include <asm/system.h>
#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/leds.h>
#include <asm/irq.h>
#include <asm/mach/irq.h>
#include <asm/mach/time.h>

struct sys_timer omap_timer;

/*
 * ---------------------------------------------------------------------------
 * MPU timer
 * ---------------------------------------------------------------------------
 */
#define OMAP_MPU_TIMER1_BASE		(0xfffec500)
#define OMAP_MPU_TIMER2_BASE		(0xfffec600)
#define OMAP_MPU_TIMER3_BASE		(0xfffec700)
#define OMAP_MPU_TIMER_BASE		OMAP_MPU_TIMER1_BASE
#define OMAP_MPU_TIMER_OFFSET		0x100

#define MPU_TIMER_FREE			(1 << 6)
#define MPU_TIMER_CLOCK_ENABLE		(1 << 5)
#define MPU_TIMER_AR			(1 << 1)
#define MPU_TIMER_ST			(1 << 0)

/*
 * MPU_TICKS_PER_SEC must be an even number, otherwise machinecycles_to_usecs
 * will break. On P2, the timer count rate is 6.5 MHz after programming PTV
 * with 0. This divides the 13MHz input by 2, and is undocumented.
 */
#ifdef CONFIG_MACH_OMAP_PERSEUS2
/* REVISIT: This ifdef construct should be replaced by a query to clock
 * framework to see if timer base frequency is 12.0, 13.0 or 19.2 MHz.
 */
#define MPU_TICKS_PER_SEC		(13000000 / 2)
#else
#define MPU_TICKS_PER_SEC		(12000000 / 2)
#endif

#define MPU_TIMER_TICK_PERIOD		((MPU_TICKS_PER_SEC / HZ) - 1)

typedef struct {
	u32 cntl;			/* CNTL_TIMER, R/W */
	u32 load_tim;			/* LOAD_TIM,   W */
	u32 read_tim;			/* READ_TIM,   R */
} omap_mpu_timer_regs_t;

#define omap_mpu_timer_base(n)						\
((volatile omap_mpu_timer_regs_t*)IO_ADDRESS(OMAP_MPU_TIMER_BASE +	\
				 (n)*OMAP_MPU_TIMER_OFFSET))

static inline unsigned long omap_mpu_timer_read(int nr)
{
	volatile omap_mpu_timer_regs_t* timer = omap_mpu_timer_base(nr);
	return timer->read_tim;
}

static inline void omap_mpu_timer_start(int nr, unsigned long load_val)
{
	volatile omap_mpu_timer_regs_t* timer = omap_mpu_timer_base(nr);

	timer->cntl = MPU_TIMER_CLOCK_ENABLE;
	udelay(1);
	timer->load_tim = load_val;
        udelay(1);
	timer->cntl = (MPU_TIMER_CLOCK_ENABLE | MPU_TIMER_AR | MPU_TIMER_ST);
}

unsigned long omap_mpu_timer_ticks_to_usecs(unsigned long nr_ticks)
{
	/* Round up to nearest usec */
	return ((nr_ticks * 1000) / (MPU_TICKS_PER_SEC / 2 / 1000) + 1) >> 1;
}

/*
 * Last processed system timer interrupt
 */
static unsigned long omap_mpu_timer_last = 0;

/*
 * Returns elapsed usecs since last system timer interrupt
 */
static unsigned long omap_mpu_timer_gettimeoffset(void)
{
	unsigned long now = 0 - omap_mpu_timer_read(0);
	unsigned long elapsed = now - omap_mpu_timer_last;

	return omap_mpu_timer_ticks_to_usecs(elapsed);
}

/*
 * Elapsed time between interrupts is calculated using timer0.
 * Latency during the interrupt is calculated using timer1.
 * Both timer0 and timer1 are counting at 6MHz (P2 6.5MHz).
 */
static irqreturn_t omap_mpu_timer_interrupt(int irq, void *dev_id,
					struct pt_regs *regs)
{
	unsigned long now, latency;

	write_seqlock(&xtime_lock);
	now = 0 - omap_mpu_timer_read(0);
	latency = MPU_TICKS_PER_SEC / HZ - omap_mpu_timer_read(1);
	omap_mpu_timer_last = now - latency;
	timer_tick(regs);
	write_sequnlock(&xtime_lock);

	return IRQ_HANDLED;
}

static struct irqaction omap_mpu_timer_irq = {
	.name		= "mpu timer",
	.flags		= SA_INTERRUPT,
	.handler	= omap_mpu_timer_interrupt
};

static __init void omap_init_mpu_timer(void)
{
	omap_timer.offset = omap_mpu_timer_gettimeoffset;
	setup_irq(INT_TIMER2, &omap_mpu_timer_irq);
	omap_mpu_timer_start(0, 0xffffffff);
	omap_mpu_timer_start(1, MPU_TIMER_TICK_PERIOD);
}

/*
 * ---------------------------------------------------------------------------
 * Timer initialization
 * ---------------------------------------------------------------------------
 */
void __init omap_timer_init(void)
{
	omap_init_mpu_timer();
}

struct sys_timer omap_timer = {
	.init		= omap_timer_init,
	.offset		= NULL,		/* Initialized later */
};
