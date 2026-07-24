// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm_init.c - Memory initialisation verification and debugging
 *
 * Copyright 2008 IBM Corporation, 2008
 * Author Mel Gorman <mel@csn.ul.ie>
 *
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/export.h>
#include <linux/memory.h>
#include <linux/notifier.h>
#include <linux/sched.h>
#include <linux/mman.h>
#include <linux/jiffies.h>
#include <linux/memremap.h>
#include <linux/page-isolation.h>
#include "internal.h"

#ifdef CONFIG_DEBUG_MEMORY_INIT
int __meminitdata mminit_loglevel;

/* The zonelists are simply reported, validation is manual. */
void __init mminit_verify_zonelist(void)
{
	int nid;

	if (mminit_loglevel < MMINIT_VERIFY)
		return;

	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);
		struct zone *zone;
		struct zoneref *z;
		struct zonelist *zonelist;
		int i, listid, zoneid;

		BUILD_BUG_ON(MAX_ZONELISTS > 2);
		for (i = 0; i < MAX_ZONELISTS * MAX_NR_ZONES; i++) {

			/* Identify the zone and nodelist */
			zoneid = i % MAX_NR_ZONES;
			listid = i / MAX_NR_ZONES;
			zonelist = &pgdat->node_zonelists[listid];
			zone = &pgdat->node_zones[zoneid];
			if (!populated_zone(zone))
				continue;

			/* Print information about the zonelist */
			printk(KERN_DEBUG "mminit::zonelist %s %d:%s = ",
				listid > 0 ? "thisnode" : "general", nid,
				zone->name);

			/* Iterate the zonelist */
			for_each_zone_zonelist(zone, z, zonelist, zoneid)
				pr_cont("%d:%s ", zone_to_nid(zone), zone->name);
			pr_cont("\n");
		}
	}
}

void __init mminit_verify_pageflags_layout(void)
{
	int shift, width;
	unsigned long or_mask, add_mask;

	shift = 8 * sizeof(unsigned long);
	width = shift - SECTIONS_WIDTH - NODES_WIDTH - ZONES_WIDTH
		- LAST_CPUPID_SHIFT - KASAN_TAG_WIDTH - LRU_GEN_WIDTH - LRU_REFS_WIDTH;
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_widths",
		"Section %d Node %d Zone %d Lastcpupid %d Kasantag %d Gen %d Tier %d Flags %d\n",
		SECTIONS_WIDTH,
		NODES_WIDTH,
		ZONES_WIDTH,
		LAST_CPUPID_WIDTH,
		KASAN_TAG_WIDTH,
		LRU_GEN_WIDTH,
		LRU_REFS_WIDTH,
		NR_PAGEFLAGS);
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_shifts",
		"Section %d Node %d Zone %d Lastcpupid %d Kasantag %d\n",
		SECTIONS_SHIFT,
		NODES_SHIFT,
		ZONES_SHIFT,
		LAST_CPUPID_SHIFT,
		KASAN_TAG_WIDTH);
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_pgshifts",
		"Section %lu Node %lu Zone %lu Lastcpupid %lu Kasantag %lu\n",
		(unsigned long)SECTIONS_PGSHIFT,
		(unsigned long)NODES_PGSHIFT,
		(unsigned long)ZONES_PGSHIFT,
		(unsigned long)LAST_CPUPID_PGSHIFT,
		(unsigned long)KASAN_TAG_PGSHIFT);
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_nodezoneid",
		"Node/Zone ID: %lu -> %lu\n",
		(unsigned long)(ZONEID_PGOFF + ZONEID_SHIFT),
		(unsigned long)ZONEID_PGOFF);
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_usage",
		"location: %d -> %d layout %d -> %d unused %d -> %d page-flags\n",
		shift, width, width, NR_PAGEFLAGS, NR_PAGEFLAGS, 0);
#ifdef NODE_NOT_IN_PAGE_FLAGS
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_nodeflags",
		"Node not in page flags");
#endif
#ifdef LAST_CPUPID_NOT_IN_PAGE_FLAGS
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_nodeflags",
		"Last cpupid not in page flags");
#endif

	if (SECTIONS_WIDTH) {
		shift -= SECTIONS_WIDTH;
		BUG_ON(shift != SECTIONS_PGSHIFT);
	}
	if (NODES_WIDTH) {
		shift -= NODES_WIDTH;
		BUG_ON(shift != NODES_PGSHIFT);
	}
	if (ZONES_WIDTH) {
		shift -= ZONES_WIDTH;
		BUG_ON(shift != ZONES_PGSHIFT);
	}

	/* Check for bitmask overlaps */
	or_mask = (ZONES_MASK << ZONES_PGSHIFT) |
			(NODES_MASK << NODES_PGSHIFT) |
			(SECTIONS_MASK << SECTIONS_PGSHIFT);
	add_mask = (ZONES_MASK << ZONES_PGSHIFT) +
			(NODES_MASK << NODES_PGSHIFT) +
			(SECTIONS_MASK << SECTIONS_PGSHIFT);
	BUG_ON(or_mask != add_mask);
}

static __init int set_mminit_loglevel(char *str)
{
	get_option(&str, &mminit_loglevel);
	return 0;
}
early_param("mminit_loglevel", set_mminit_loglevel);
#endif /* CONFIG_DEBUG_MEMORY_INIT */

struct kobject *mm_kobj;
EXPORT_SYMBOL_GPL(mm_kobj);

#ifdef CONFIG_SMP
s32 vm_committed_as_batch = 32;

void mm_compute_batch(int overcommit_policy)
{
	u64 memsized_batch;
	s32 nr = num_present_cpus();
	s32 batch = max_t(s32, nr*2, 32);
	unsigned long ram_pages = totalram_pages();

	/*
	 * For policy OVERCOMMIT_NEVER, set batch size to 0.4% of
	 * (total memory/#cpus), and lift it to 25% for other policies
	 * to easy the possible lock contention for percpu_counter
	 * vm_committed_as, while the max limit is INT_MAX
	 */
	if (overcommit_policy == OVERCOMMIT_NEVER)
		memsized_batch = min_t(u64, ram_pages/nr/256, INT_MAX);
	else
		memsized_batch = min_t(u64, ram_pages/nr/4, INT_MAX);

	vm_committed_as_batch = max_t(s32, memsized_batch, batch);
}

static int __meminit mm_compute_batch_notifier(struct notifier_block *self,
					unsigned long action, void *arg)
{
	switch (action) {
	case MEM_ONLINE:
	case MEM_OFFLINE:
		mm_compute_batch(sysctl_overcommit_memory);
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block compute_batch_nb __meminitdata = {
	.notifier_call = mm_compute_batch_notifier,
	.priority = IPC_CALLBACK_PRI, /* use lowest priority */
};

static int __init mm_compute_batch_init(void)
{
	mm_compute_batch(sysctl_overcommit_memory);
	register_hotmemory_notifier(&compute_batch_nb);

	return 0;
}

__initcall(mm_compute_batch_init);

#endif

static int __init mm_sysfs_init(void)
{
	mm_kobj = kobject_create_and_add("mm", kernel_kobj);
	if (!mm_kobj)
		return -ENOMEM;

	return 0;
}
postcore_initcall(mm_sysfs_init);

#ifdef CONFIG_ZONE_DEVICE
static void __ref __init_zone_device_page(struct page *page, unsigned long pfn,
					  unsigned long zone_idx, int nid,
					  struct dev_pagemap *pgmap)
{
	__init_single_page(page, pfn, zone_idx, nid);

	/*
	 * Mark page reserved as it will need to wait for onlining
	 * phase for it to be fully associated with a zone.
	 *
	 * We can use the non-atomic __set_bit operation for setting
	 * the flag as we are still initializing the pages.
	 */
	__SetPageReserved(page);

	/*
	 * ZONE_DEVICE pages union ->lru with a ->pgmap back pointer
	 * and zone_device_data.  It is a bug if a ZONE_DEVICE page is
	 * ever freed or placed on a driver-private list.
	 */
	page->pgmap = pgmap;
	page->zone_device_data = NULL;

	/*
	 * ZONE_DEVICE pages are released directly to the driver page allocator
	 * which will set the page count to 1 when allocating the page.
	 */
	if (pgmap->type == MEMORY_DEVICE_PRIVATE ||
	    pgmap->type == MEMORY_DEVICE_COHERENT)
		set_page_count(page, 0);
}

#define pageblock_migratetype_init_range(start_pfn, nr_pages, migratetype) \
do {									\
	unsigned long __start = (start_pfn);				\
	unsigned long __pfn = pageblock_align(__start);			\
	unsigned long __end = __start + (nr_pages);			\
									\
	for (; __pfn < __end; __pfn += pageblock_nr_pages) {		\
		set_pageblock_migratetype(pfn_to_page(__pfn), migratetype); \
		if (IS_ALIGNED(__pfn, PAGES_PER_SECTION))			\
			cond_resched();					\
	}								\
} while (0)

/*
 * With compound page geometry and when struct pages are stored in ram most
 * tail pages are reused. Consequently, the amount of unique struct pages to
 * initialize is a lot smaller that the total amount of struct pages being
 * mapped. This is a paired / mild layering violation with explicit knowledge
 * of how the sparse_vmemmap internals handle compound pages in the lack
 * of an altmap. See vmemmap_populate_compound_pages().
 */
static inline unsigned long compound_nr_pages(unsigned long pfn,
					      struct vmem_altmap *altmap,
					      struct dev_pagemap *pgmap)
{
	/*
	 * If DAX memory is hot-plugged into an unoccupied subsection
	 * of an early section, the unoptimized boot memmap is reused.
	 * See section_activate().
	 */
	if (early_section(__pfn_to_section(pfn)) ||
	    !vmemmap_can_optimize(altmap, pgmap))
		return pgmap_vmemmap_nr(pgmap);

	return VMEMMAP_RESERVE_NR * (PAGE_SIZE / sizeof(struct page));
}

static void __ref memmap_init_compound(struct page *head,
				       unsigned long head_pfn,
				       unsigned long zone_idx, int nid,
				       struct dev_pagemap *pgmap,
				       unsigned long nr_pages)
{
	unsigned long pfn, end_pfn = head_pfn + nr_pages;
	unsigned int order = pgmap->vmemmap_shift;

	__SetPageHead(head);
	for (pfn = head_pfn + 1; pfn < end_pfn; pfn++) {
		struct page *page = pfn_to_page(pfn);

		__init_zone_device_page(page, pfn, zone_idx, nid, pgmap);
		prep_compound_tail(head, pfn - head_pfn);
		set_page_count(page, 0);

		/*
		 * The first tail page stores compound_mapcount_ptr() and
		 * compound_order() and the second tail page stores
		 * compound_pincount_ptr(). Call prep_compound_head() after
		 * the first and second tail pages have been initialized to
		 * not have the data overwritten.
		 */
		if (pfn == head_pfn + 2)
			prep_compound_head(head, order);
	}
}

void __ref memmap_init_zone_device(struct zone *zone,
				   unsigned long start_pfn,
				   unsigned long nr_pages,
				   struct dev_pagemap *pgmap)
{
	unsigned long pfn, end_pfn = start_pfn + nr_pages;
	struct pglist_data *pgdat = zone->zone_pgdat;
	struct vmem_altmap *altmap = pgmap_altmap(pgmap);
	unsigned int pfns_per_compound = pgmap_vmemmap_nr(pgmap);
	unsigned long zone_idx = zone_idx(zone);
	unsigned long start = jiffies;
	int nid = pgdat->node_id;

	if (WARN_ON_ONCE(!pgmap || zone_idx != ZONE_DEVICE))
		return;

	/*
	 * The call to memmap_init should have already taken care
	 * of the pages reserved for the memmap, so we can just jump to
	 * the end of that region and start processing the device pages.
	 */
	if (altmap) {
		start_pfn = altmap->base_pfn + vmem_altmap_offset(altmap);
		nr_pages = end_pfn - start_pfn;
	}

	for (pfn = start_pfn; pfn < end_pfn; pfn += pfns_per_compound) {
		struct page *page = pfn_to_page(pfn);

		__init_zone_device_page(page, pfn, zone_idx, nid, pgmap);

		if (IS_ALIGNED(pfn, PAGES_PER_SECTION))
			cond_resched();

		if (pfns_per_compound == 1)
			continue;

		memmap_init_compound(page, pfn, zone_idx, nid, pgmap,
				     compound_nr_pages(pfn, altmap, pgmap));
	}

	pageblock_migratetype_init_range(start_pfn, nr_pages, MIGRATE_MOVABLE);

	pr_debug("%s initialised %lu pages in %ums\n", __func__,
		 nr_pages, jiffies_to_msecs(jiffies - start));
}
#endif
