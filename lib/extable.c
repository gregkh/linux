/*
 * lib/extable.c
 * Derived from arch/ppc/mm/extable.c and arch/i386/mm/extable.c.
 *
 * Copyright (C) 2004 Paul Mackerras, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <asm/uaccess.h>

extern struct exception_table_entry __start___ex_table[];
extern struct exception_table_entry __stop___ex_table[];

#ifndef ARCH_HAS_SORT_EXTABLE
/*
 * The exception table needs to be sorted so that the binary
 * search that we use to find entries in it works properly.
 * This is used both for the kernel exception table and for
 * the exception tables of modules that get loaded.
 */
void sort_extable(struct exception_table_entry *start,
		  struct exception_table_entry *finish)
{
	struct exception_table_entry el, *p, *q;

	/* insertion sort */
	for (p = start + 1; p < finish; ++p) {
		/* start .. p-1 is sorted */
		if (p[0].insn < p[-1].insn) {
			/* move element p down to its right place */
			el = *p;
			q = p;
			do {
				/* el comes before q[-1], move q[-1] up one */
				q[0] = q[-1];
				--q;
			} while (q > start && el.insn < q[-1].insn);
			*q = el;
		}
	}
}
#endif

#ifndef ARCH_HAS_SEARCH_EXTABLE
/*
 * Search one exception table for an entry corresponding to the
 * given instruction address, and return the address of the entry,
 * or NULL if none is found.
 * We use a binary search, and thus we assume that the table is
 * already sorted.
 */
const struct exception_table_entry *
search_extable(const struct exception_table_entry *first,
	       const struct exception_table_entry *last,
	       unsigned long value)
{
	while (first <= last) {
		const struct exception_table_entry *mid;

		mid = (last - first) / 2 + first;
		/*
		 * careful, the distance between entries can be
		 * larger than 2GB:
		 */
		if (mid->insn < value)
			first = mid + 1;
		else if (mid->insn > value)
			last = mid - 1;
		else
			return mid;
        }
        return NULL;
}
#endif
