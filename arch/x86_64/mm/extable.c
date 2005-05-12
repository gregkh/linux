/*
 * linux/arch/x86_64/mm/extable.c
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/init.h>
#include <asm/uaccess.h>

/* Simple binary search */
const struct exception_table_entry *
search_extable(const struct exception_table_entry *first,
	       const struct exception_table_entry *last,
	       unsigned long value)
{
	/* Work around a B stepping K8 bug */
	if ((value >> 32) == 0)
		value |= 0xffffffffUL << 32; 

        while (first <= last) {
		const struct exception_table_entry *mid;
		long diff;

		mid = (last - first) / 2 + first;
		diff = mid->insn - value;
                if (diff == 0)
                        return mid;
                else if (diff < 0)
                        first = mid+1;
                else
                        last = mid-1;
        }
        return NULL;
}

/* When an exception handler is in an non standard section (like __init)
   the fixup table can end up unordered. Fix that here. */
void sort_extable(struct exception_table_entry *start,
		  struct exception_table_entry *finish)
{
	struct exception_table_entry *e;
	int change;

	/* The input is near completely presorted, which makes bubble sort the
	   best (and simplest) sort algorithm. */
	do {
		change = 0;
		for (e = start+1; e < finish; e++) {
			if (e->insn < e[-1].insn) {
				struct exception_table_entry tmp = e[-1];
				e[-1] = e[0];
				e[0] = tmp;
				change = 1;
			}
		}
	} while (change != 0);
}
