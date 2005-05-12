/* $Id: pgalloc.h,v 1.30 2001/12/21 04:56:17 davem Exp $ */
#ifndef _SPARC64_PGALLOC_H
#define _SPARC64_PGALLOC_H

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>

#include <asm/page.h>
#include <asm/spitfire.h>
#include <asm/pgtable.h>
#include <asm/cpudata.h>

/* Page table allocation/freeing. */
#ifdef CONFIG_SMP
/* Sliiiicck */
#define pgt_quicklists	cpu_data(smp_processor_id())
#else
extern struct pgtable_cache_struct {
	unsigned long *pgd_cache;
	unsigned long *pte_cache[2];
	unsigned int pgcache_size;
	unsigned int pgdcache_size;
} pgt_quicklists;
#endif
#define pgd_quicklist		(pgt_quicklists.pgd_cache)
#define pmd_quicklist		((unsigned long *)0)
#define pte_quicklist		(pgt_quicklists.pte_cache)
#define pgtable_cache_size	(pgt_quicklists.pgcache_size)
#define pgd_cache_size		(pgt_quicklists.pgdcache_size)

#ifndef CONFIG_SMP

static __inline__ void free_pgd_fast(pgd_t *pgd)
{
	struct page *page = virt_to_page(pgd);

	preempt_disable();
	if (!page->lru.prev) {
		page->lru.next = (void *) pgd_quicklist;
		pgd_quicklist = (unsigned long *)page;
	}
	page->lru.prev = (void *)
	  (((unsigned long)page->lru.prev) |
	   (((unsigned long)pgd & (PAGE_SIZE / 2)) ? 2 : 1));
	pgd_cache_size++;
	preempt_enable();
}

static __inline__ pgd_t *get_pgd_fast(void)
{
        struct page *ret;

	preempt_disable();
        if ((ret = (struct page *)pgd_quicklist) != NULL) {
                unsigned long mask = (unsigned long)ret->lru.prev;
		unsigned long off = 0;

		if (mask & 1)
			mask &= ~1;
		else {
			off = PAGE_SIZE / 2;
			mask &= ~2;
		}
		ret->lru.prev = (void *) mask;
		if (!mask)
			pgd_quicklist = (unsigned long *)ret->lru.next;
                ret = (struct page *)(__page_address(ret) + off);
                pgd_cache_size--;
		preempt_enable();
        } else {
		struct page *page;

		preempt_enable();
		page = alloc_page(GFP_KERNEL|__GFP_REPEAT|__GFP_ZERO);
		if (page) {
			ret = (struct page *)page_address(page);
			page->lru.prev = (void *) 2UL;

			preempt_disable();
			page->lru.next = (void *) pgd_quicklist;
			pgd_quicklist = (unsigned long *)page;
			pgd_cache_size++;
			preempt_enable();
		}
        }
        return (pgd_t *)ret;
}

#else /* CONFIG_SMP */

static __inline__ void free_pgd_fast(pgd_t *pgd)
{
	preempt_disable();
	*(unsigned long *)pgd = (unsigned long) pgd_quicklist;
	pgd_quicklist = (unsigned long *) pgd;
	pgtable_cache_size++;
	preempt_enable();
}

static __inline__ pgd_t *get_pgd_fast(void)
{
	unsigned long *ret;

	preempt_disable();
	if((ret = pgd_quicklist) != NULL) {
		pgd_quicklist = (unsigned long *)(*ret);
		ret[0] = 0;
		pgtable_cache_size--;
		preempt_enable();
	} else {
		preempt_enable();
		ret = (unsigned long *) __get_free_page(GFP_KERNEL|__GFP_REPEAT);
		if(ret)
			memset(ret, 0, PAGE_SIZE);
	}
	return (pgd_t *)ret;
}

static __inline__ void free_pgd_slow(pgd_t *pgd)
{
	free_page((unsigned long)pgd);
}

#endif /* CONFIG_SMP */

#if (L1DCACHE_SIZE > PAGE_SIZE)			/* is there D$ aliasing problem */
#define VPTE_COLOR(address)		(((address) >> (PAGE_SHIFT + 10)) & 1UL)
#define DCACHE_COLOR(address)		(((address) >> PAGE_SHIFT) & 1UL)
#else
#define VPTE_COLOR(address)		0
#define DCACHE_COLOR(address)		0
#endif

#define pud_populate(MM, PUD, PMD)	pud_set(PUD, PMD)

static __inline__ pmd_t *pmd_alloc_one_fast(struct mm_struct *mm, unsigned long address)
{
	unsigned long *ret;
	int color = 0;

	preempt_disable();
	if (pte_quicklist[color] == NULL)
		color = 1;

	if((ret = (unsigned long *)pte_quicklist[color]) != NULL) {
		pte_quicklist[color] = (unsigned long *)(*ret);
		ret[0] = 0;
		pgtable_cache_size--;
	}
	preempt_enable();

	return (pmd_t *)ret;
}

static __inline__ pmd_t *pmd_alloc_one(struct mm_struct *mm, unsigned long address)
{
	pmd_t *pmd;

	pmd = pmd_alloc_one_fast(mm, address);
	if (!pmd) {
		pmd = (pmd_t *)__get_free_page(GFP_KERNEL|__GFP_REPEAT);
		if (pmd)
			memset(pmd, 0, PAGE_SIZE);
	}
	return pmd;
}

static __inline__ void free_pmd_fast(pmd_t *pmd)
{
	unsigned long color = DCACHE_COLOR((unsigned long)pmd);

	preempt_disable();
	*(unsigned long *)pmd = (unsigned long) pte_quicklist[color];
	pte_quicklist[color] = (unsigned long *) pmd;
	pgtable_cache_size++;
	preempt_enable();
}

static __inline__ void free_pmd_slow(pmd_t *pmd)
{
	free_page((unsigned long)pmd);
}

#define pmd_populate_kernel(MM, PMD, PTE)	pmd_set(PMD, PTE)
#define pmd_populate(MM,PMD,PTE_PAGE)		\
	pmd_populate_kernel(MM,PMD,page_address(PTE_PAGE))

extern pte_t *__pte_alloc_one_kernel(struct mm_struct *mm, unsigned long address);

static inline pte_t *pte_alloc_one_kernel(struct mm_struct *mm, unsigned long address)
{
	pte_t *pte = __pte_alloc_one_kernel(mm, address);
	if (pte) {
		struct page *page = virt_to_page(pte);
		page->mapping = (void *) mm;
		page->index = address & PMD_MASK;
	}
	return pte;
}

static inline struct page *
pte_alloc_one(struct mm_struct *mm, unsigned long addr)
{
	pte_t *pte = __pte_alloc_one_kernel(mm, addr);
	if (pte) {
		struct page *page = virt_to_page(pte);
		page->mapping = (void *) mm;
		page->index = addr & PMD_MASK;
		return page;
	}
	return NULL;
}

static __inline__ pte_t *pte_alloc_one_fast(struct mm_struct *mm, unsigned long address)
{
	unsigned long color = VPTE_COLOR(address);
	unsigned long *ret;

	preempt_disable();
	if((ret = (unsigned long *)pte_quicklist[color]) != NULL) {
		pte_quicklist[color] = (unsigned long *)(*ret);
		ret[0] = 0;
		pgtable_cache_size--;
	}
	preempt_enable();
	return (pte_t *)ret;
}

static __inline__ void free_pte_fast(pte_t *pte)
{
	unsigned long color = DCACHE_COLOR((unsigned long)pte);

	preempt_disable();
	*(unsigned long *)pte = (unsigned long) pte_quicklist[color];
	pte_quicklist[color] = (unsigned long *) pte;
	pgtable_cache_size++;
	preempt_enable();
}

static __inline__ void free_pte_slow(pte_t *pte)
{
	free_page((unsigned long)pte);
}

static inline void pte_free_kernel(pte_t *pte)
{
	virt_to_page(pte)->mapping = NULL;
	free_pte_fast(pte);
}

static inline void pte_free(struct page *ptepage)
{
	ptepage->mapping = NULL;
	free_pte_fast(page_address(ptepage));
}

#define pmd_free(pmd)		free_pmd_fast(pmd)
#define pgd_free(pgd)		free_pgd_fast(pgd)
#define pgd_alloc(mm)		get_pgd_fast()

#endif /* _SPARC64_PGALLOC_H */
