/*
 * SMP support for pSeries machines.
 *
 * Dave Engebretsen, Peter Bergner, and
 * Mike Corrigan {engebret|bergner|mikec}@us.ibm.com
 *
 * Plus various changes from other IBM teams...
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#undef DEBUG

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/cache.h>
#include <linux/err.h>
#include <linux/sysdev.h>
#include <linux/cpu.h>

#include <asm/ptrace.h>
#include <asm/atomic.h>
#include <asm/irq.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/io.h>
#include <asm/prom.h>
#include <asm/smp.h>
#include <asm/paca.h>
#include <asm/time.h>
#include <asm/machdep.h>
#include <asm/xics.h>
#include <asm/cputable.h>
#include <asm/system.h>
#include <asm/rtas.h>
#include <asm/plpar_wrappers.h>

#include "mpic.h"

#ifdef DEBUG
#define DBG(fmt...) udbg_printf(fmt)
#else
#define DBG(fmt...)
#endif

extern void pSeries_secondary_smp_init(unsigned long);

/* Get state of physical CPU.
 * Return codes:
 *	0	- The processor is in the RTAS stopped state
 *	1	- stop-self is in progress
 *	2	- The processor is not in the RTAS stopped state
 *	-1	- Hardware Error
 *	-2	- Hardware Busy, Try again later.
 */
static int query_cpu_stopped(unsigned int pcpu)
{
	int cpu_status;
	int status, qcss_tok;

	qcss_tok = rtas_token("query-cpu-stopped-state");
	if (qcss_tok == RTAS_UNKNOWN_SERVICE)
		return -1;
	status = rtas_call(qcss_tok, 1, 2, &cpu_status, pcpu);
	if (status != 0) {
		printk(KERN_ERR
		       "RTAS query-cpu-stopped-state failed: %i\n", status);
		return status;
	}

	return cpu_status;
}


#ifdef CONFIG_HOTPLUG_CPU

int pSeries_cpu_disable(void)
{
	systemcfg->processorCount--;

	/*fix boot_cpuid here*/
	if (smp_processor_id() == boot_cpuid)
		boot_cpuid = any_online_cpu(cpu_online_map);

	/* FIXME: abstract this to not be platform specific later on */
	xics_migrate_irqs_away();
	return 0;
}

void pSeries_cpu_die(unsigned int cpu)
{
	int tries;
	int cpu_status;
	unsigned int pcpu = get_hard_smp_processor_id(cpu);

	for (tries = 0; tries < 25; tries++) {
		cpu_status = query_cpu_stopped(pcpu);
		if (cpu_status == 0 || cpu_status == -1)
			break;
		msleep(200);
	}
	if (cpu_status != 0) {
		printk("Querying DEAD? cpu %i (%i) shows %i\n",
		       cpu, pcpu, cpu_status);
	}

	/* Isolation and deallocation are definatly done by
	 * drslot_chrp_cpu.  If they were not they would be
	 * done here.  Change isolate state to Isolate and
	 * change allocation-state to Unusable.
	 */
	paca[cpu].cpu_start = 0;
}

/* Search all cpu device nodes for an offline logical cpu.  If a
 * device node has a "ibm,my-drc-index" property (meaning this is an
 * LPAR), paranoid-check whether we own the cpu.  For each "thread"
 * of a cpu, if it is offline and has the same hw index as before,
 * grab that in preference.
 */
static unsigned int find_physical_cpu_to_start(unsigned int old_hwindex)
{
	struct device_node *np = NULL;
	unsigned int best = -1U;

	while ((np = of_find_node_by_type(np, "cpu"))) {
		int nr_threads, len;
		u32 *index = (u32 *)get_property(np, "ibm,my-drc-index", NULL);
		u32 *tid = (u32 *)
			get_property(np, "ibm,ppc-interrupt-server#s", &len);

		if (!tid)
			tid = (u32 *)get_property(np, "reg", &len);

		if (!tid)
			continue;

		/* If there is a drc-index, make sure that we own
		 * the cpu.
		 */
		if (index) {
			int state;
			int rc = rtas_get_sensor(9003, *index, &state);
			if (rc != 0 || state != 1)
				continue;
		}

		nr_threads = len / sizeof(u32);

		while (nr_threads--) {
			if (0 == query_cpu_stopped(tid[nr_threads])) {
				best = tid[nr_threads];
				if (best == old_hwindex)
					goto out;
			}
		}
	}
out:
	of_node_put(np);
	return best;
}

/**
 * smp_startup_cpu() - start the given cpu
 *
 * At boot time, there is nothing to do.  At run-time, call RTAS with
 * the appropriate start location, if the cpu is in the RTAS stopped
 * state.
 *
 * Returns:
 *	0	- failure
 *	1	- success
 */
static inline int __devinit smp_startup_cpu(unsigned int lcpu)
{
	int status;
	unsigned long start_here = __pa((u32)*((unsigned long *)
					       pSeries_secondary_smp_init));
	unsigned int pcpu;

	/* At boot time the cpus are already spinning in hold
	 * loops, so nothing to do. */
 	if (system_state < SYSTEM_RUNNING)
		return 1;

	pcpu = find_physical_cpu_to_start(get_hard_smp_processor_id(lcpu));
	if (pcpu == -1U) {
		printk(KERN_INFO "No more cpus available, failing\n");
		return 0;
	}

	/* Fixup atomic count: it exited inside IRQ handler. */
	paca[lcpu].__current->thread_info->preempt_count	= 0;

	/* At boot this is done in prom.c. */
	paca[lcpu].hw_cpu_id = pcpu;

	status = rtas_call(rtas_token("start-cpu"), 3, 1, NULL,
			   pcpu, start_here, lcpu);
	if (status != 0) {
		printk(KERN_ERR "start-cpu failed: %i\n", status);
		return 0;
	}
	return 1;
}
#else /* ... CONFIG_HOTPLUG_CPU */
static inline int __devinit smp_startup_cpu(unsigned int lcpu)
{
	return 1;
}
#endif /* CONFIG_HOTPLUG_CPU */

static inline void smp_xics_do_message(int cpu, int msg)
{
	set_bit(msg, &xics_ipi_message[cpu].value);
	mb();
	xics_cause_IPI(cpu);
}

static void smp_xics_message_pass(int target, int msg)
{
	unsigned int i;

	if (target < NR_CPUS) {
		smp_xics_do_message(target, msg);
	} else {
		for_each_online_cpu(i) {
			if (target == MSG_ALL_BUT_SELF
			    && i == smp_processor_id())
				continue;
			smp_xics_do_message(i, msg);
		}
	}
}

static int __init smp_xics_probe(void)
{
	xics_request_IPIs();

	return cpus_weight(cpu_possible_map);
}

static void __devinit smp_xics_setup_cpu(int cpu)
{
	if (cpu != boot_cpuid)
		xics_setup_cpu();

	if (cur_cpu_spec->firmware_features & FW_FEATURE_SPLPAR)
		vpa_init(cpu);

	/*
	 * Put the calling processor into the GIQ.  This is really only
	 * necessary from a secondary thread as the OF start-cpu interface
	 * performs this function for us on primary threads.
	 */
	rtas_set_indicator(GLOBAL_INTERRUPT_QUEUE,
		(1UL << interrupt_server_size) - 1 - default_distrib_server, 1);
}

static DEFINE_SPINLOCK(timebase_lock);
static unsigned long timebase = 0;

static void __devinit pSeries_give_timebase(void)
{
	spin_lock(&timebase_lock);
	rtas_call(rtas_token("freeze-time-base"), 0, 1, NULL);
	timebase = get_tb();
	spin_unlock(&timebase_lock);

	while (timebase)
		barrier();
	rtas_call(rtas_token("thaw-time-base"), 0, 1, NULL);
}

static void __devinit pSeries_take_timebase(void)
{
	while (!timebase)
		barrier();
	spin_lock(&timebase_lock);
	set_tb(timebase >> 32, timebase & 0xffffffff);
	timebase = 0;
	spin_unlock(&timebase_lock);
}

static void __devinit smp_pSeries_kick_cpu(int nr)
{
	BUG_ON(nr < 0 || nr >= NR_CPUS);

	if (!smp_startup_cpu(nr))
		return;

	/*
	 * The processor is currently spinning, waiting for the
	 * cpu_start field to become non-zero After we set cpu_start,
	 * the processor will continue on to secondary_start
	 */
	paca[nr].cpu_start = 1;
}

static struct smp_ops_t pSeries_mpic_smp_ops = {
	.message_pass	= smp_mpic_message_pass,
	.probe		= smp_mpic_probe,
	.kick_cpu	= smp_pSeries_kick_cpu,
	.setup_cpu	= smp_mpic_setup_cpu,
};

static struct smp_ops_t pSeries_xics_smp_ops = {
	.message_pass	= smp_xics_message_pass,
	.probe		= smp_xics_probe,
	.kick_cpu	= smp_pSeries_kick_cpu,
	.setup_cpu	= smp_xics_setup_cpu,
};

/* This is called very early */
void __init smp_init_pSeries(void)
{
	int ret, i;

	DBG(" -> smp_init_pSeries()\n");

	if (ppc64_interrupt_controller == IC_OPEN_PIC)
		smp_ops = &pSeries_mpic_smp_ops;
	else
		smp_ops = &pSeries_xics_smp_ops;

#ifdef CONFIG_HOTPLUG_CPU
	smp_ops->cpu_disable = pSeries_cpu_disable;
	smp_ops->cpu_die = pSeries_cpu_die;
#endif

	/* Start secondary threads on SMT systems; primary threads
	 * are already in the running state.
	 */
	for_each_present_cpu(i) {
		if (query_cpu_stopped(get_hard_smp_processor_id(i)) == 0) {
			printk("%16.16x : starting thread\n", i);
			DBG("%16.16x : starting thread\n", i);
			rtas_call(rtas_token("start-cpu"), 3, 1, &ret,
				  get_hard_smp_processor_id(i),
				  __pa((u32)*((unsigned long *)
					      pSeries_secondary_smp_init)),
				  i);
		}
	}

	/* Non-lpar has additional take/give timebase */
	if (rtas_token("freeze-time-base") != RTAS_UNKNOWN_SERVICE) {
		smp_ops->give_timebase = pSeries_give_timebase;
		smp_ops->take_timebase = pSeries_take_timebase;
	}

	DBG(" <- smp_init_pSeries()\n");
}

