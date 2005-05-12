/* arch/sparc64/mm/tlb.c
 *
 * Copyright (C) 2004 David S. Miller <davem@redhat.com>
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/percpu.h>
#include <linux/mm.h>
#include <linux/swap.h>

#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/tlb.h>

/* Heavily inspired by the ppc64 code.  */

DEFINE_PER_CPU(struct mmu_gather, mmu_gathers) =
	{ NULL, 0, 0, 0, 0, 0, { 0 }, { NULL }, };

void flush_tlb_pending(void)
{
	struct mmu_gather *mp = &__get_cpu_var(mmu_gathers);

	if (mp->tlb_nr) {
		unsigned long context = mp->mm->context;

		if (CTX_VALID(context)) {
#ifdef CONFIG_SMP
			smp_flush_tlb_pending(mp->mm, mp->tlb_nr,
					      &mp->vaddrs[0]);
#else
			__flush_tlb_pending(CTX_HWBITS(context), mp->tlb_nr,
					    &mp->vaddrs[0]);
#endif
		}
		mp->tlb_nr = 0;
	}
}

void tlb_batch_add(pte_t *ptep, pte_t orig)
{
	struct mmu_gather *mp = &__get_cpu_var(mmu_gathers);
	struct page *ptepage;
	struct mm_struct *mm;
	unsigned long vaddr, nr;

	ptepage = virt_to_page(ptep);
	mm = (struct mm_struct *) ptepage->mapping;

	/* It is more efficient to let flush_tlb_kernel_range()
	 * handle these cases.
	 */
	if (mm == &init_mm)
		return;

	vaddr = ptepage->index +
		(((unsigned long)ptep & ~PAGE_MASK) * PTRS_PER_PTE);
	if (pte_exec(orig))
		vaddr |= 0x1UL;

	if (pte_dirty(orig)) {
		unsigned long paddr, pfn = pte_pfn(orig);
		struct address_space *mapping;
		struct page *page;

		if (!pfn_valid(pfn))
			goto no_cache_flush;

		page = pfn_to_page(pfn);
		if (PageReserved(page))
			goto no_cache_flush;

		/* A real file page? */
		mapping = page_mapping(page);
		if (!mapping)
			goto no_cache_flush;

		paddr = (unsigned long) page_address(page);
		if ((paddr ^ vaddr) & (1 << 13))
			flush_dcache_page_all(mm, page);
	}

no_cache_flush:
	if (mp->tlb_frozen)
		return;

	nr = mp->tlb_nr;

	if (unlikely(nr != 0 && mm != mp->mm)) {
		flush_tlb_pending();
		nr = 0;
	}

	if (nr == 0)
		mp->mm = mm;

	mp->vaddrs[nr] = vaddr;
	mp->tlb_nr = ++nr;
	if (nr >= TLB_BATCH_NR)
		flush_tlb_pending();
}

void flush_tlb_pgtables(struct mm_struct *mm, unsigned long start, unsigned long end)
{
	struct mmu_gather *mp = &__get_cpu_var(mmu_gathers);
	unsigned long nr = mp->tlb_nr;
	long s = start, e = end, vpte_base;

	if (mp->tlb_frozen)
		return;

	/* Nobody should call us with start below VM hole and end above.
	 * See if it is really true.
	 */
	BUG_ON(s > e);

	s &= PMD_MASK;
	e = (e + PMD_SIZE - 1) & PMD_MASK;

	vpte_base = (tlb_type == spitfire ?
		     VPTE_BASE_SPITFIRE :
		     VPTE_BASE_CHEETAH);

	if (unlikely(nr != 0 && mm != mp->mm)) {
		flush_tlb_pending();
		nr = 0;
	}

	if (nr == 0)
		mp->mm = mm;

	start = vpte_base + (s >> (PAGE_SHIFT - 3));
	end = vpte_base + (e >> (PAGE_SHIFT - 3));
	while (start < end) {
		mp->vaddrs[nr] = start;
		mp->tlb_nr = ++nr;
		if (nr >= TLB_BATCH_NR) {
			flush_tlb_pending();
			nr = 0;
		}
		start += PAGE_SIZE;
	}
	if (nr)
		flush_tlb_pending();
}

unsigned long __ptrs_per_pmd(void)
{
	if (test_thread_flag(TIF_32BIT))
		return (1UL << (32 - (PAGE_SHIFT-3) - PAGE_SHIFT));
	return REAL_PTRS_PER_PMD;
}
