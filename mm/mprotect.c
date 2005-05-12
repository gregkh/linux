/*
 *  mm/mprotect.c
 *
 *  (C) Copyright 1994 Linus Torvalds
 *  (C) Copyright 2002 Christoph Hellwig
 *
 *  Address space accounting code	<alan@redhat.com>
 *  (C) Copyright 2002 Red Hat Inc, All Rights Reserved
 */

#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/slab.h>
#include <linux/shm.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/security.h>
#include <linux/mempolicy.h>
#include <linux/personality.h>
#include <linux/syscalls.h>

#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

static inline void
change_pte_range(pmd_t *pmd, unsigned long address,
		unsigned long size, pgprot_t newprot)
{
	pte_t * pte;
	unsigned long end;

	if (pmd_none(*pmd))
		return;
	if (pmd_bad(*pmd)) {
		pmd_ERROR(*pmd);
		pmd_clear(pmd);
		return;
	}
	pte = pte_offset_map(pmd, address);
	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	do {
		if (pte_present(*pte)) {
			pte_t entry;

			/* Avoid an SMP race with hardware updated dirty/clean
			 * bits by wiping the pte and then setting the new pte
			 * into place.
			 */
			entry = ptep_get_and_clear(pte);
			set_pte(pte, pte_modify(entry, newprot));
		}
		address += PAGE_SIZE;
		pte++;
	} while (address && (address < end));
	pte_unmap(pte - 1);
}

static inline void
change_pmd_range(pud_t *pud, unsigned long address,
		unsigned long size, pgprot_t newprot)
{
	pmd_t * pmd;
	unsigned long end;

	if (pud_none(*pud))
		return;
	if (pud_bad(*pud)) {
		pud_ERROR(*pud);
		pud_clear(pud);
		return;
	}
	pmd = pmd_offset(pud, address);
	address &= ~PUD_MASK;
	end = address + size;
	if (end > PUD_SIZE)
		end = PUD_SIZE;
	do {
		change_pte_range(pmd, address, end - address, newprot);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address && (address < end));
}

static inline void
change_pud_range(pgd_t *pgd, unsigned long address,
		unsigned long size, pgprot_t newprot)
{
	pud_t * pud;
	unsigned long end;

	if (pgd_none(*pgd))
		return;
	if (pgd_bad(*pgd)) {
		pgd_ERROR(*pgd);
		pgd_clear(pgd);
		return;
	}
	pud = pud_offset(pgd, address);
	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	do {
		change_pmd_range(pud, address, end - address, newprot);
		address = (address + PUD_SIZE) & PUD_MASK;
		pud++;
	} while (address && (address < end));
}

static void
change_protection(struct vm_area_struct *vma, unsigned long start,
		unsigned long end, pgprot_t newprot)
{
	struct mm_struct *mm = current->mm;
	pgd_t *pgd;
	unsigned long beg = start, next;
	int i;

	pgd = pgd_offset(mm, start);
	flush_cache_range(vma, beg, end);
	BUG_ON(start >= end);
	spin_lock(&mm->page_table_lock);
	for (i = pgd_index(start); i <= pgd_index(end-1); i++) {
		next = (start + PGDIR_SIZE) & PGDIR_MASK;
		if (next <= start || next > end)
			next = end;
		change_pud_range(pgd, start, next - start, newprot);
		start = next;
		pgd++;
	}
	flush_tlb_range(vma, beg, end);
	spin_unlock(&mm->page_table_lock);
}

static int
mprotect_fixup(struct vm_area_struct *vma, struct vm_area_struct **pprev,
	unsigned long start, unsigned long end, unsigned long newflags)
{
	struct mm_struct * mm = vma->vm_mm;
	unsigned long oldflags = vma->vm_flags;
	long nrpages = (end - start) >> PAGE_SHIFT;
	unsigned long charged = 0;
	pgprot_t newprot;
	pgoff_t pgoff;
	int error;

	if (newflags == oldflags) {
		*pprev = vma;
		return 0;
	}

	/*
	 * If we make a private mapping writable we increase our commit;
	 * but (without finer accounting) cannot reduce our commit if we
	 * make it unwritable again.
	 *
	 * FIXME? We haven't defined a VM_NORESERVE flag, so mprotecting
	 * a MAP_NORESERVE private mapping to writable will now reserve.
	 */
	if (newflags & VM_WRITE) {
		if (!(oldflags & (VM_ACCOUNT|VM_WRITE|VM_SHARED|VM_HUGETLB))) {
			charged = nrpages;
			if (security_vm_enough_memory(charged))
				return -ENOMEM;
			newflags |= VM_ACCOUNT;
		}
	}

	newprot = protection_map[newflags & 0xf];

	/*
	 * First try to merge with previous and/or next vma.
	 */
	pgoff = vma->vm_pgoff + ((start - vma->vm_start) >> PAGE_SHIFT);
	*pprev = vma_merge(mm, *pprev, start, end, newflags,
			vma->anon_vma, vma->vm_file, pgoff, vma_policy(vma));
	if (*pprev) {
		vma = *pprev;
		goto success;
	}

	if (start != vma->vm_start) {
		error = split_vma(mm, vma, start, 1);
		if (error)
			goto fail;
	}
	/*
	 * Unless it returns an error, this function always sets *pprev to
	 * the first vma for which vma->vm_end >= end.
	 */
	*pprev = vma;

	if (end != vma->vm_end) {
		error = split_vma(mm, vma, end, 0);
		if (error)
			goto fail;
	}

success:
	/*
	 * vm_flags and vm_page_prot are protected by the mmap_sem
	 * held in write mode.
	 */
	vma->vm_flags = newflags;
	vma->vm_page_prot = newprot;
	change_protection(vma, start, end, newprot);
	__vm_stat_account(mm, oldflags, vma->vm_file, -nrpages);
	__vm_stat_account(mm, newflags, vma->vm_file, nrpages);
	return 0;

fail:
	vm_unacct_memory(charged);
	return error;
}

asmlinkage long
sys_mprotect(unsigned long start, size_t len, unsigned long prot)
{
	unsigned long vm_flags, nstart, end, tmp;
	struct vm_area_struct *vma, *prev;
	int error = -EINVAL;
	const int grows = prot & (PROT_GROWSDOWN|PROT_GROWSUP);
	prot &= ~(PROT_GROWSDOWN|PROT_GROWSUP);
	if (grows == (PROT_GROWSDOWN|PROT_GROWSUP)) /* can't be both */
		return -EINVAL;

	if (start & ~PAGE_MASK)
		return -EINVAL;
	len = PAGE_ALIGN(len);
	end = start + len;
	if (end < start)
		return -ENOMEM;
	if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_SEM))
		return -EINVAL;
	if (end == start)
		return 0;
	/*
	 * Does the application expect PROT_READ to imply PROT_EXEC:
	 */
	if (unlikely((prot & PROT_READ) &&
			(current->personality & READ_IMPLIES_EXEC)))
		prot |= PROT_EXEC;

	vm_flags = calc_vm_prot_bits(prot);

	down_write(&current->mm->mmap_sem);

	vma = find_vma_prev(current->mm, start, &prev);
	error = -ENOMEM;
	if (!vma)
		goto out;
	if (unlikely(grows & PROT_GROWSDOWN)) {
		if (vma->vm_start >= end)
			goto out;
		start = vma->vm_start;
		error = -EINVAL;
		if (!(vma->vm_flags & VM_GROWSDOWN))
			goto out;
	}
	else {
		if (vma->vm_start > start)
			goto out;
		if (unlikely(grows & PROT_GROWSUP)) {
			end = vma->vm_end;
			error = -EINVAL;
			if (!(vma->vm_flags & VM_GROWSUP))
				goto out;
		}
	}
	if (start > vma->vm_start)
		prev = vma;

	for (nstart = start ; ; ) {
		unsigned long newflags;

		/* Here we know that  vma->vm_start <= nstart < vma->vm_end. */

		if (is_vm_hugetlb_page(vma)) {
			error = -EACCES;
			goto out;
		}

		newflags = vm_flags | (vma->vm_flags & ~(VM_READ | VM_WRITE | VM_EXEC));

		if ((newflags & ~(newflags >> 4)) & 0xf) {
			error = -EACCES;
			goto out;
		}

		error = security_file_mprotect(vma, prot);
		if (error)
			goto out;

		tmp = vma->vm_end;
		if (tmp > end)
			tmp = end;
		error = mprotect_fixup(vma, &prev, nstart, tmp, newflags);
		if (error)
			goto out;
		nstart = tmp;

		if (nstart < prev->vm_end)
			nstart = prev->vm_end;
		if (nstart >= end)
			goto out;

		vma = prev->vm_next;
		if (!vma || vma->vm_start != nstart) {
			error = -ENOMEM;
			goto out;
		}
	}
out:
	up_write(&current->mm->mmap_sem);
	return error;
}
