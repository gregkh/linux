#ifndef __ASM_HARDIRQ_H
#define __ASM_HARDIRQ_H

#include <linux/config.h>
#include <linux/threads.h>
#include <linux/irq.h>

typedef struct {
	unsigned int __softirq_pending;
	unsigned long idle_timestamp;
	unsigned int __nmi_count;	/* arch dependent */
	unsigned int apic_timer_irqs;	/* arch dependent */
} ____cacheline_aligned irq_cpustat_t;

#include <linux/irq_cpustat.h>	/* Standard mappings for irq_cpustat_t above */

void ack_bad_irq(unsigned int irq);

#endif /* __ASM_HARDIRQ_H */
