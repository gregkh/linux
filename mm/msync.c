/*
 *	linux/mm/msync.c
 *
 * Copyright (C) 1994-1999  Linus Torvalds
 */

/*
 * The msync() system call.
 */
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/hugetlb.h>
#include <linux/syscalls.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>

/*
 * Called with mm->page_table_lock held to protect against other
 * threads/the swapper from ripping pte's out from under us.
 */
static int filemap_sync_pte(pte_t *ptep, struct vm_area_struct *vma,
	unsigned long address, unsigned int flags)
{
	pte_t pte = *ptep;
	unsigned long pfn = pte_pfn(pte);
	struct page *page;

	if (pte_present(pte) && pfn_valid(pfn)) {
		page = pfn_to_page(pfn);
		if (!PageReserved(page) &&
		    (ptep_clear_flush_dirty(vma, address, ptep) ||
		     page_test_and_clear_dirty(page)))
			set_page_dirty(page);
	}
	return 0;
}

static int filemap_sync_pte_range(pmd_t * pmd,
	unsigned long address, unsigned long end, 
	struct vm_area_struct *vma, unsigned int flags)
{
	pte_t *pte;
	int error;

	if (pmd_none(*pmd))
		return 0;
	if (pmd_bad(*pmd)) {
		pmd_ERROR(*pmd);
		pmd_clear(pmd);
		return 0;
	}
	pte = pte_offset_map(pmd, address);
	if ((address & PMD_MASK) != (end & PMD_MASK))
		end = (address & PMD_MASK) + PMD_SIZE;
	error = 0;
	do {
		error |= filemap_sync_pte(pte, vma, address, flags);
		address += PAGE_SIZE;
		pte++;
	} while (address && (address < end));

	pte_unmap(pte - 1);

	return error;
}

static inline int filemap_sync_pmd_range(pud_t * pud,
	unsigned long address, unsigned long end, 
	struct vm_area_struct *vma, unsigned int flags)
{
	pmd_t * pmd;
	int error;

	if (pud_none(*pud))
		return 0;
	if (pud_bad(*pud)) {
		pud_ERROR(*pud);
		pud_clear(pud);
		return 0;
	}
	pmd = pmd_offset(pud, address);
	if ((address & PUD_MASK) != (end & PUD_MASK))
		end = (address & PUD_MASK) + PUD_SIZE;
	error = 0;
	do {
		error |= filemap_sync_pte_range(pmd, address, end, vma, flags);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address && (address < end));
	return error;
}

static inline int filemap_sync_pud_range(pgd_t *pgd,
	unsigned long address, unsigned long end,
	struct vm_area_struct *vma, unsigned int flags)
{
	pud_t *pud;
	int error;

	if (pgd_none(*pgd))
		return 0;
	if (pgd_bad(*pgd)) {
		pgd_ERROR(*pgd);
		pgd_clear(pgd);
		return 0;
	}
	pud = pud_offset(pgd, address);
	if ((address & PGDIR_MASK) != (end & PGDIR_MASK))
		end = (address & PGDIR_MASK) + PGDIR_SIZE;
	error = 0;
	do {
		error |= filemap_sync_pmd_range(pud, address, end, vma, flags);
		address = (address + PUD_SIZE) & PUD_MASK;
		pud++;
	} while (address && (address < end));
	return error;
}

static int __filemap_sync(struct vm_area_struct *vma, unsigned long address,
			size_t size, unsigned int flags)
{
	pgd_t *pgd;
	unsigned long end = address + size;
	unsigned long next;
	int i;
	int error = 0;

	/* Aquire the lock early; it may be possible to avoid dropping
	 * and reaquiring it repeatedly.
	 */
	spin_lock(&vma->vm_mm->page_table_lock);

	pgd = pgd_offset(vma->vm_mm, address);
	flush_cache_range(vma, address, end);

	/* For hugepages we can't go walking the page table normally,
	 * but that's ok, hugetlbfs is memory based, so we don't need
	 * to do anything more on an msync() */
	if (is_vm_hugetlb_page(vma))
		goto out;

	if (address >= end)
		BUG();
	for (i = pgd_index(address); i <= pgd_index(end-1); i++) {
		next = (address + PGDIR_SIZE) & PGDIR_MASK;
		if (next <= address || next > end)
			next = end;
		error |= filemap_sync_pud_range(pgd, address, next, vma, flags);
		address = next;
		pgd++;
	}
	/*
	 * Why flush ? filemap_sync_pte already flushed the tlbs with the
	 * dirty bits.
	 */
	flush_tlb_range(vma, end - size, end);
 out:
	spin_unlock(&vma->vm_mm->page_table_lock);

	return error;
}

#ifdef CONFIG_PREEMPT
static int filemap_sync(struct vm_area_struct *vma, unsigned long address,
			size_t size, unsigned int flags)
{
	const size_t chunk = 64 * 1024;	/* bytes */
	int error = 0;

	while (size) {
		size_t sz = min(size, chunk);

		error |= __filemap_sync(vma, address, sz, flags);
		cond_resched();
		address += sz;
		size -= sz;
	}
	return error;
}
#else
static int filemap_sync(struct vm_area_struct *vma, unsigned long address,
			size_t size, unsigned int flags)
{
	return __filemap_sync(vma, address, size, flags);
}
#endif

/*
 * MS_SYNC syncs the entire file - including mappings.
 *
 * MS_ASYNC does not start I/O (it used to, up to 2.5.67).  Instead, it just
 * marks the relevant pages dirty.  The application may now run fsync() to
 * write out the dirty pages and wait on the writeout and check the result.
 * Or the application may run fadvise(FADV_DONTNEED) against the fd to start
 * async writeout immediately.
 * So my _not_ starting I/O in MS_ASYNC we provide complete flexibility to
 * applications.
 */
static int msync_interval(struct vm_area_struct * vma,
	unsigned long start, unsigned long end, int flags)
{
	int ret = 0;
	struct file * file = vma->vm_file;

	if ((flags & MS_INVALIDATE) && (vma->vm_flags & VM_LOCKED))
		return -EBUSY;

	if (file && (vma->vm_flags & VM_SHARED)) {
		ret = filemap_sync(vma, start, end-start, flags);

		if (!ret && (flags & MS_SYNC)) {
			struct address_space *mapping = file->f_mapping;
			int err;

			ret = filemap_fdatawrite(mapping);
			if (file->f_op && file->f_op->fsync) {
				/*
				 * We don't take i_sem here because mmap_sem
				 * is already held.
				 */
				err = file->f_op->fsync(file,file->f_dentry,1);
				if (err && !ret)
					ret = err;
			}
			err = filemap_fdatawait(mapping);
			if (!ret)
				ret = err;
		}
	}
	return ret;
}

asmlinkage long sys_msync(unsigned long start, size_t len, int flags)
{
	unsigned long end;
	struct vm_area_struct * vma;
	int unmapped_error, error = -EINVAL;

	if (flags & MS_SYNC)
		current->flags |= PF_SYNCWRITE;

	down_read(&current->mm->mmap_sem);
	if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC))
		goto out;
	if (start & ~PAGE_MASK)
		goto out;
	if ((flags & MS_ASYNC) && (flags & MS_SYNC))
		goto out;
	error = -ENOMEM;
	len = (len + ~PAGE_MASK) & PAGE_MASK;
	end = start + len;
	if (end < start)
		goto out;
	error = 0;
	if (end == start)
		goto out;
	/*
	 * If the interval [start,end) covers some unmapped address ranges,
	 * just ignore them, but return -ENOMEM at the end.
	 */
	vma = find_vma(current->mm, start);
	unmapped_error = 0;
	for (;;) {
		/* Still start < end. */
		error = -ENOMEM;
		if (!vma)
			goto out;
		/* Here start < vma->vm_end. */
		if (start < vma->vm_start) {
			unmapped_error = -ENOMEM;
			start = vma->vm_start;
		}
		/* Here vma->vm_start <= start < vma->vm_end. */
		if (end <= vma->vm_end) {
			if (start < end) {
				error = msync_interval(vma, start, end, flags);
				if (error)
					goto out;
			}
			error = unmapped_error;
			goto out;
		}
		/* Here vma->vm_start <= start < vma->vm_end < end. */
		error = msync_interval(vma, start, vma->vm_end, flags);
		if (error)
			goto out;
		start = vma->vm_end;
		vma = vma->vm_next;
	}
out:
	up_read(&current->mm->mmap_sem);
	current->flags &= ~PF_SYNCWRITE;
	return error;
}
