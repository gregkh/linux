/*  Kernel module help for x86-64
    Copyright (C) 2001 Rusty Russell.
    Copyright (C) 2002,2003 Andi Kleen, SuSE Labs.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include <linux/moduleloader.h>
#include <linux/elf.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include <asm/system.h>
#include <asm/page.h>
#include <asm/pgtable.h>

#define DEBUGP(fmt...) 
 
static struct vm_struct *mod_vmlist;

void module_free(struct module *mod, void *module_region)
{
	struct vm_struct **prevp, *map;
	int i;
	unsigned long addr = (unsigned long)module_region;

	if (!addr)
		return;
	write_lock(&vmlist_lock); 
	for (prevp = &mod_vmlist ; (map = *prevp) ; prevp = &map->next) {
		if ((unsigned long)map->addr == addr) {
			*prevp = map->next;
			goto found;
		}
	}
	write_unlock(&vmlist_lock); 
	printk("Trying to unmap nonexistent module vm area (%lx)\n", addr);
	return;
 found:
	unmap_vm_area(map);
	write_unlock(&vmlist_lock); 
	if (map->pages) {
		for (i = 0; i < map->nr_pages; i++)
			if (map->pages[i])
				__free_page(map->pages[i]);	
		kfree(map->pages);
	}
	kfree(map);					
}

void *module_alloc(unsigned long size)
{
	struct vm_struct **p, *tmp, *area;
	struct page **pages;
	void *addr;
	unsigned int nr_pages, array_size, i;

	if (!size)
		return NULL; 
	size = PAGE_ALIGN(size);
	if (size > MODULES_LEN)
		return NULL;

	area = (struct vm_struct *) kmalloc(sizeof(*area), GFP_KERNEL);
	if (!area)
		return NULL;
	memset(area, 0, sizeof(struct vm_struct));

	write_lock(&vmlist_lock);
	addr = (void *) MODULES_VADDR;
	for (p = &mod_vmlist; (tmp = *p); p = &tmp->next) {
		void *next; 
		DEBUGP("vmlist %p %lu addr %p\n", tmp->addr, tmp->size, addr);
		if (size + (unsigned long) addr + PAGE_SIZE < (unsigned long) tmp->addr)
			break;
		next = (void *) (tmp->size + (unsigned long) tmp->addr);
		if (next > addr) 
			addr = next;
	}

	if ((unsigned long)addr + size >= MODULES_END) {
		write_unlock(&vmlist_lock);
		kfree(area); 
		return NULL;
	}
	DEBUGP("addr %p\n", addr);

	area->next = *p;
	*p = area;
	area->size = size + PAGE_SIZE;
	area->addr = addr;
	write_unlock(&vmlist_lock);

	nr_pages = size >> PAGE_SHIFT;
	array_size = (nr_pages * sizeof(struct page *));

	area->nr_pages = nr_pages;
	area->pages = pages = kmalloc(array_size, GFP_KERNEL);
	if (!area->pages) 
		goto fail;

	memset(area->pages, 0, array_size);
	for (i = 0; i < nr_pages; i++) {
		area->pages[i] = alloc_page(GFP_KERNEL);
		if (area->pages[i] == NULL)
			goto fail;
	}
	
	if (map_vm_area(area, PAGE_KERNEL_EXEC, &pages))
		goto fail;
	
	memset(addr, 0, size);
	DEBUGP("module_alloc size %lu = %p\n", size, addr);
	return addr;

fail:
	module_free(NULL, addr);
	return NULL;
}

/* We don't need anything special. */
int module_frob_arch_sections(Elf_Ehdr *hdr,
			      Elf_Shdr *sechdrs,
			      char *secstrings,
			      struct module *mod)
{
	return 0;
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
	void *loc;
	u64 val; 

	DEBUGP("Applying relocate section %u to %u\n", relsec,
	       sechdrs[relsec].sh_info);
	for (i = 0; i < sechdrs[relsec].sh_size / sizeof(*rel); i++) {
		/* This is where to make the change */
		loc = (void *)sechdrs[sechdrs[relsec].sh_info].sh_addr
			+ rel[i].r_offset;

		/* This is the symbol it is referring to.  Note that all
		   undefined symbols have been resolved.  */
		sym = (Elf64_Sym *)sechdrs[symindex].sh_addr
			+ ELF64_R_SYM(rel[i].r_info);

	        DEBUGP("type %d st_value %Lx r_addend %Lx loc %Lx\n",
		       (int)ELF64_R_TYPE(rel[i].r_info), 
		       sym->st_value, rel[i].r_addend, (u64)loc);

		val = sym->st_value + rel[i].r_addend; 

		switch (ELF64_R_TYPE(rel[i].r_info)) {
		case R_X86_64_NONE:
			break;
		case R_X86_64_64:
			*(u64 *)loc = val;
			break;
		case R_X86_64_32:
			*(u32 *)loc = val;
			if (val != *(u32 *)loc)
				goto overflow;
			break;
		case R_X86_64_32S:
			*(s32 *)loc = val;
			if ((s64)val != *(s32 *)loc)
				goto overflow;
			break;
		case R_X86_64_PC32: 
			val -= (u64)loc;
			*(u32 *)loc = val;
#if 0
			if ((s64)val != *(s32 *)loc)
				goto overflow; 
#endif
			break;
		default:
			printk(KERN_ERR "module %s: Unknown rela relocation: %Lu\n",
			       me->name, ELF64_R_TYPE(rel[i].r_info));
			return -ENOEXEC;
		}
	}
	return 0;

overflow:
	printk(KERN_ERR "overflow in relocation type %d val %Lx\n", 
	       (int)ELF64_R_TYPE(rel[i].r_info), val);
	printk(KERN_ERR "`%s' likely not compiled with -mcmodel=kernel\n",
	       me->name);
	return -ENOEXEC;
}

int apply_relocate(Elf_Shdr *sechdrs,
		   const char *strtab,
		   unsigned int symindex,
		   unsigned int relsec,
		   struct module *me)
{
	printk("non add relocation not supported\n");
	return -ENOSYS;
} 

extern void apply_alternatives(void *start, void *end); 

int module_finalize(const Elf_Ehdr *hdr,
		    const Elf_Shdr *sechdrs,
		    struct module *me)
{
	const Elf_Shdr *s;
	char *secstrings = (void *)hdr + sechdrs[hdr->e_shstrndx].sh_offset;

	/* look for .altinstructions to patch */ 
	for (s = sechdrs; s < sechdrs + hdr->e_shnum; s++) { 
		void *seg; 		
		if (strcmp(".altinstructions", secstrings + s->sh_name))
			continue;
		seg = (void *)s->sh_addr; 
		apply_alternatives(seg, seg + s->sh_size); 
	} 	
	return 0;
}

void module_arch_cleanup(struct module *mod)
{
}
