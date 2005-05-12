#ifndef _LINUX_SWSUSP_H
#define _LINUX_SWSUSP_H

#if defined(CONFIG_X86) || defined(CONFIG_FRV)
#include <asm/suspend.h>
#endif
#include <linux/swap.h>
#include <linux/notifier.h>
#include <linux/config.h>
#include <linux/init.h>
#include <linux/pm.h>

/* page backup entry */
typedef struct pbe {
	unsigned long address;		/* address of the copy */
	unsigned long orig_address;	/* original address of page */
	swp_entry_t swap_address;	
	swp_entry_t dummy;		/* we need scratch space at 
					 * end of page (see link, diskpage)
					 */
} suspend_pagedir_t;

#define SWAP_FILENAME_MAXLENGTH	32


#define SUSPEND_PD_PAGES(x)     (((x)*sizeof(struct pbe))/PAGE_SIZE+1)
   
/* mm/vmscan.c */
extern int shrink_mem(void);

/* mm/page_alloc.c */
extern void drain_local_pages(void);
extern void mark_free_pages(struct zone *zone);

#ifdef CONFIG_PM
/* kernel/power/swsusp.c */
extern int software_suspend(void);

extern int pm_prepare_console(void);
extern void pm_restore_console(void);

#else
static inline int software_suspend(void)
{
	printk("Warning: fake suspend called\n");
	return -EPERM;
}
#endif

#ifdef CONFIG_SMP
extern void disable_nonboot_cpus(void);
extern void enable_nonboot_cpus(void);
#else
static inline void disable_nonboot_cpus(void) {}
static inline void enable_nonboot_cpus(void) {}
#endif

void save_processor_state(void);
void restore_processor_state(void);
struct saved_context;
void __save_processor_state(struct saved_context *ctxt);
void __restore_processor_state(struct saved_context *ctxt);

#endif /* _LINUX_SWSUSP_H */
