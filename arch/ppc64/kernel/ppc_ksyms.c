/* 
 * c 2001 PPC 64 Team, IBM Corp
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/threads.h>
#include <linux/smp.h>
#include <linux/elfcore.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/console.h>
#include <linux/irq.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/smp_lock.h>
#include <linux/syscalls.h>
#include <linux/bitops.h>

#include <asm/page.h>
#include <asm/semaphore.h>
#include <asm/processor.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/atomic.h>
#include <asm/checksum.h>
#include <asm/pgtable.h>
#include <asm/prom.h>
#include <asm/system.h>
#include <asm/pci-bridge.h>
#include <asm/irq.h>
#include <asm/dma.h>
#include <asm/machdep.h>
#include <asm/hw_irq.h>
#include <asm/abs_addr.h>
#include <asm/cacheflush.h>
#ifdef CONFIG_PPC_ISERIES
#include <asm/iSeries/HvCallSc.h>
#include <asm/iSeries/LparData.h>
#endif

extern int do_signal(sigset_t *, struct pt_regs *);

EXPORT_SYMBOL(do_signal);

EXPORT_SYMBOL(isa_io_base);
EXPORT_SYMBOL(pci_io_base);

EXPORT_SYMBOL(strcpy);
EXPORT_SYMBOL(strncpy);
EXPORT_SYMBOL(strcat);
EXPORT_SYMBOL(strncat);
EXPORT_SYMBOL(strchr);
EXPORT_SYMBOL(strrchr);
EXPORT_SYMBOL(strpbrk);
EXPORT_SYMBOL(strstr);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strnlen);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strncmp);

EXPORT_SYMBOL(__down_interruptible);
EXPORT_SYMBOL(__up);
EXPORT_SYMBOL(__down);

EXPORT_SYMBOL(csum_partial);
EXPORT_SYMBOL(csum_partial_copy_generic);
EXPORT_SYMBOL(ip_fast_csum);
EXPORT_SYMBOL(csum_tcpudp_magic);

EXPORT_SYMBOL(__copy_tofrom_user);
EXPORT_SYMBOL(__clear_user);
EXPORT_SYMBOL(__strncpy_from_user);
EXPORT_SYMBOL(__strnlen_user);

EXPORT_SYMBOL(clear_user_page);

#ifdef CONFIG_MSCHUNKS
EXPORT_SYMBOL(msChunks);
#endif
EXPORT_SYMBOL(reloc_offset);

#ifdef CONFIG_PPC_ISERIES
EXPORT_SYMBOL(HvCall0);
EXPORT_SYMBOL(HvCall1);
EXPORT_SYMBOL(HvCall2);
EXPORT_SYMBOL(HvCall3);
EXPORT_SYMBOL(HvCall4);
EXPORT_SYMBOL(HvCall5);
EXPORT_SYMBOL(HvCall6);
EXPORT_SYMBOL(HvCall7);
#endif

EXPORT_SYMBOL(_insb);
EXPORT_SYMBOL(_outsb);
EXPORT_SYMBOL(_insw);
EXPORT_SYMBOL(_outsw);
EXPORT_SYMBOL(_insl);
EXPORT_SYMBOL(_outsl);
EXPORT_SYMBOL(_insw_ns);
EXPORT_SYMBOL(_outsw_ns);
EXPORT_SYMBOL(_insl_ns);
EXPORT_SYMBOL(_outsl_ns);
EXPORT_SYMBOL(ioremap);
EXPORT_SYMBOL(__ioremap);
EXPORT_SYMBOL(iounmap);

EXPORT_SYMBOL(start_thread);
EXPORT_SYMBOL(kernel_thread);

EXPORT_SYMBOL(giveup_fpu);
#ifdef CONFIG_ALTIVEC
EXPORT_SYMBOL(giveup_altivec);
#endif
EXPORT_SYMBOL(flush_icache_range);
EXPORT_SYMBOL(flush_icache_user_range);
EXPORT_SYMBOL(flush_dcache_page);
#ifdef CONFIG_SMP
#ifdef CONFIG_PPC_ISERIES
EXPORT_SYMBOL(local_get_flags);
EXPORT_SYMBOL(local_irq_disable);
EXPORT_SYMBOL(local_irq_restore);
#endif
#endif

EXPORT_SYMBOL(ppc_md);

#ifdef CONFIG_PPC_MULTIPLATFORM
EXPORT_SYMBOL(find_devices);
EXPORT_SYMBOL(find_type_devices);
EXPORT_SYMBOL(find_compatible_devices);
EXPORT_SYMBOL(find_path_device);
EXPORT_SYMBOL(device_is_compatible);
EXPORT_SYMBOL(machine_is_compatible);
EXPORT_SYMBOL(find_all_nodes);
EXPORT_SYMBOL(get_property);
#endif

EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memmove);
EXPORT_SYMBOL(memscan);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(memchr);

EXPORT_SYMBOL(timer_interrupt);
EXPORT_SYMBOL(irq_desc);
EXPORT_SYMBOL(get_wchan);
EXPORT_SYMBOL(console_drivers);

EXPORT_SYMBOL(tb_ticks_per_usec);
EXPORT_SYMBOL(paca);
EXPORT_SYMBOL(cur_cpu_spec);
