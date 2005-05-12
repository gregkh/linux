/* Kernel module help for sparc64.
 *
 * Copyright (C) 2001 Rusty Russell.
 * Copyright (C) 2002 David S. Miller.
 */

#include <linux/moduleloader.h>
#include <linux/kernel.h>
#include <linux/elf.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>

#include <asm/processor.h>
#include <asm/spitfire.h>

static struct vm_struct * modvmlist = NULL;

static void module_unmap(void * addr)
{
	struct vm_struct **p, *tmp;
	int i;

	if (!addr)
		return;
	if ((PAGE_SIZE-1) & (unsigned long) addr) {
		printk("Trying to unmap module with bad address (%p)\n", addr);
		return;
	}

	for (p = &modvmlist; (tmp = *p) != NULL; p = &tmp->next) {
		if (tmp->addr == addr) {
			*p = tmp->next;
			goto found;
		}
	}
	printk("Trying to unmap nonexistent module vm area (%p)\n", addr);
	return;

found:
	unmap_vm_area(tmp);
	
	for (i = 0; i < tmp->nr_pages; i++) {
		if (unlikely(!tmp->pages[i]))
			BUG();
		__free_page(tmp->pages[i]);
	}

	kfree(tmp->pages);
	kfree(tmp);
}


static void *module_map(unsigned long size)
{
	struct vm_struct **p, *tmp, *area;
	struct page **pages;
	void * addr;
	unsigned int nr_pages, array_size, i;

	size = PAGE_ALIGN(size);
	if (!size || size > MODULES_LEN)
		return NULL;
		
	addr = (void *) MODULES_VADDR;
	for (p = &modvmlist; (tmp = *p) != NULL; p = &tmp->next) {
		if (size + (unsigned long) addr < (unsigned long) tmp->addr)
			break;
		addr = (void *) (tmp->size + (unsigned long) tmp->addr);
	}
	if ((unsigned long) addr + size >= MODULES_END)
		return NULL;
	
	area = (struct vm_struct *) kmalloc(sizeof(*area), GFP_KERNEL);
	if (!area)
		return NULL;
	area->size = size + PAGE_SIZE;
	area->addr = addr;
	area->next = *p;
	area->pages = NULL;
	area->nr_pages = 0;
	area->phys_addr = 0;
	*p = area;

	nr_pages = size >> PAGE_SHIFT;
	array_size = (nr_pages * sizeof(struct page *));

	area->nr_pages = nr_pages;
	area->pages = pages = kmalloc(array_size, GFP_KERNEL);
	if (!area->pages)
		goto fail;

	memset(area->pages, 0, array_size);

	for (i = 0; i < area->nr_pages; i++) {
		area->pages[i] = alloc_page(GFP_KERNEL);
		if (unlikely(!area->pages[i]))
			goto fail;
	}
	
	if (map_vm_area(area, PAGE_KERNEL, &pages)) {
		unmap_vm_area(area);
		goto fail;
	}

	return area->addr;

fail:
	if (area->pages) {
		for (i = 0; i < area->nr_pages; i++) {
			if (area->pages[i])
				__free_page(area->pages[i]);
		}
		kfree(area->pages);
	}
	kfree(area);

	return NULL;
}

void *module_alloc(unsigned long size)
{
	void *ret;

	/* We handle the zero case fine, unlike vmalloc */
	if (size == 0)
		return NULL;

	ret = module_map(size);
	if (!ret)
		ret = ERR_PTR(-ENOMEM);
	else
		memset(ret, 0, size);

	return ret;
}

/* Free memory returned from module_core_alloc/module_init_alloc */
void module_free(struct module *mod, void *module_region)
{
	write_lock(&vmlist_lock);
	module_unmap(module_region);
	write_unlock(&vmlist_lock);
	/* FIXME: If module_region == mod->init_region, trim exception
           table entries. */
}

/* Make generic code ignore STT_REGISTER dummy undefined symbols.  */
int module_frob_arch_sections(Elf_Ehdr *hdr,
			      Elf_Shdr *sechdrs,
			      char *secstrings,
			      struct module *mod)
{
	unsigned int symidx;
	Elf64_Sym *sym;
	const char *strtab;
	int i;

	for (symidx = 0; sechdrs[symidx].sh_type != SHT_SYMTAB; symidx++) {
		if (symidx == hdr->e_shnum-1) {
			printk("%s: no symtab found.\n", mod->name);
			return -ENOEXEC;
		}
	}
	sym = (Elf64_Sym *)sechdrs[symidx].sh_addr;
	strtab = (char *)sechdrs[sechdrs[symidx].sh_link].sh_addr;

	for (i = 1; i < sechdrs[symidx].sh_size / sizeof(Elf_Sym); i++) {
		if (sym[i].st_shndx == SHN_UNDEF &&
		    ELF64_ST_TYPE(sym[i].st_info) == STT_REGISTER)
			sym[i].st_shndx = SHN_ABS;
	}
	return 0;
}

int apply_relocate(Elf64_Shdr *sechdrs,
		   const char *strtab,
		   unsigned int symindex,
		   unsigned int relsec,
		   struct module *me)
{
	printk(KERN_ERR "module %s: non-ADD RELOCATION unsupported\n",
	       me->name);
	return -ENOEXEC;
}

int apply_relocate_add(Elf64_Shdr *sechdrs,
		       const char *strtab,
		       unsigned int symindex,
		       unsigned int relsec,
		       struct module *me)
{
	unsigned int i;
	Elf64_Rela *rel = (void *)sechdrs[relsec].sh_addr;
	Elf64_Sym *sym;
	u8 *location;
	u32 *loc32;

	for (i = 0; i < sechdrs[relsec].sh_size / sizeof(*rel); i++) {
		Elf64_Addr v;

		/* This is where to make the change */
		location = (u8 *)sechdrs[sechdrs[relsec].sh_info].sh_addr
			+ rel[i].r_offset;
		loc32 = (u32 *) location;

		BUG_ON(((u64)location >> (u64)32) != (u64)0);

		/* This is the symbol it is referring to.  Note that all
		   undefined symbols have been resolved.  */
		sym = (Elf64_Sym *)sechdrs[symindex].sh_addr
			+ ELF64_R_SYM(rel[i].r_info);
		v = sym->st_value + rel[i].r_addend;

		switch (ELF64_R_TYPE(rel[i].r_info) & 0xff) {
		case R_SPARC_64:
			location[0] = v >> 56;
			location[1] = v >> 48;
			location[2] = v >> 40;
			location[3] = v >> 32;
			location[4] = v >> 24;
			location[5] = v >> 16;
			location[6] = v >>  8;
			location[7] = v >>  0;
			break;

		case R_SPARC_32:
			location[0] = v >> 24;
			location[1] = v >> 16;
			location[2] = v >>  8;
			location[3] = v >>  0;
			break;

		case R_SPARC_WDISP30:
			v -= (Elf64_Addr) location;
			*loc32 = (*loc32 & ~0x3fffffff) |
				((v >> 2) & 0x3fffffff);
			break;

		case R_SPARC_WDISP22:
			v -= (Elf64_Addr) location;
			*loc32 = (*loc32 & ~0x3fffff) |
				((v >> 2) & 0x3fffff);
			break;

		case R_SPARC_WDISP19:
			v -= (Elf64_Addr) location;
			*loc32 = (*loc32 & ~0x7ffff) |
				((v >> 2) & 0x7ffff);
			break;

		case R_SPARC_LO10:
			*loc32 = (*loc32 & ~0x3ff) | (v & 0x3ff);
			break;

		case R_SPARC_HI22:
			*loc32 = (*loc32 & ~0x3fffff) |
				((v >> 10) & 0x3fffff);
			break;

		case R_SPARC_OLO10:
			*loc32 = (*loc32 & ~0x1fff) |
				(((v & 0x3ff) +
				  (ELF64_R_TYPE(rel[i].r_info) >> 8))
				 & 0x1fff);
			break;

		default:
			printk(KERN_ERR "module %s: Unknown relocation: %x\n",
			       me->name,
			       (int) (ELF64_R_TYPE(rel[i].r_info) & 0xff));
			return -ENOEXEC;
		};
	}
	return 0;
}

int module_finalize(const Elf_Ehdr *hdr,
		    const Elf_Shdr *sechdrs,
		    struct module *me)
{
	/* Cheetah's I-cache is fully coherent.  */
	if (tlb_type == spitfire) {
		unsigned long va;

		flushw_all();
		for (va =  0; va < (PAGE_SIZE << 1); va += 32)
			spitfire_put_icache_tag(va, 0x0);
		__asm__ __volatile__("flush %g6");
	}

	return 0;
}

void module_arch_cleanup(struct module *mod)
{
}
