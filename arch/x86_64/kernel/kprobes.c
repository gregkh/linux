/*
 *  Kernel Probes (KProbes)
 *  arch/x86_64/kernel/kprobes.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) IBM Corporation, 2002, 2004
 *
 * 2002-Oct	Created by Vamsi Krishna S <vamsi_krishna@in.ibm.com> Kernel
 *		Probes initial implementation ( includes contributions from
 *		Rusty Russell).
 * 2004-July	Suparna Bhattacharya <suparna@in.ibm.com> added jumper probes
 *		interface to access function arguments.
 * 2004-Oct	Jim Keniston <kenistoj@us.ibm.com> and Prasanna S Panchamukhi
 *		<prasanna@in.ibm.com> adapted for x86_64
 */

#include <linux/config.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/preempt.h>
#include <linux/vmalloc.h>

#include <asm/pgtable.h>
#include <asm/kdebug.h>

static DECLARE_MUTEX(kprobe_mutex);

/* kprobe_status settings */
#define KPROBE_HIT_ACTIVE	0x00000001
#define KPROBE_HIT_SS		0x00000002

static struct kprobe *current_kprobe;
static unsigned long kprobe_status, kprobe_old_rflags, kprobe_saved_rflags;
static struct pt_regs jprobe_saved_regs;
static long *jprobe_saved_rsp;
static kprobe_opcode_t *get_insn_slot(void);
static void free_insn_slot(kprobe_opcode_t *slot);
void jprobe_return_end(void);

/* copy of the kernel stack at the probe fire time */
static kprobe_opcode_t jprobes_stack[MAX_STACK_SIZE];

/*
 * returns non-zero if opcode modifies the interrupt flag.
 */
static inline int is_IF_modifier(kprobe_opcode_t *insn)
{
	switch (*insn) {
	case 0xfa:		/* cli */
	case 0xfb:		/* sti */
	case 0xcf:		/* iret/iretd */
	case 0x9d:		/* popf/popfd */
		return 1;
	}

	if (*insn  >= 0x40 && *insn <= 0x4f && *++insn == 0xcf)
		return 1;
	return 0;
}

int arch_prepare_kprobe(struct kprobe *p)
{
	/* insn: must be on special executable page on x86_64. */
	up(&kprobe_mutex);
	p->ainsn.insn = get_insn_slot();
	down(&kprobe_mutex);
	if (!p->ainsn.insn) {
		return -ENOMEM;
	}
	return 0;
}

void arch_copy_kprobe(struct kprobe *p)
{
	memcpy(p->ainsn.insn, p->addr, MAX_INSN_SIZE);
}

void arch_remove_kprobe(struct kprobe *p)
{
	up(&kprobe_mutex);
	free_insn_slot(p->ainsn.insn);
	down(&kprobe_mutex);
}

static inline void disarm_kprobe(struct kprobe *p, struct pt_regs *regs)
{
	*p->addr = p->opcode;
	regs->rip = (unsigned long)p->addr;
}

static void prepare_singlestep(struct kprobe *p, struct pt_regs *regs)
{
	regs->eflags |= TF_MASK;
	regs->eflags &= ~IF_MASK;

	regs->rip = (unsigned long)p->ainsn.insn;
}

/*
 * Interrupts are disabled on entry as trap3 is an interrupt gate and they
 * remain disabled thorough out this function.
 */
int kprobe_handler(struct pt_regs *regs)
{
	struct kprobe *p;
	int ret = 0;
	kprobe_opcode_t *addr = (kprobe_opcode_t *)(regs->rip - sizeof(kprobe_opcode_t));

	/* We're in an interrupt, but this is clear and BUG()-safe. */
	preempt_disable();

	/* Check we're not actually recursing */
	if (kprobe_running()) {
		/* We *are* holding lock here, so this is safe.
		   Disarm the probe we just hit, and ignore it. */
		p = get_kprobe(addr);
		if (p) {
			disarm_kprobe(p, regs);
			ret = 1;
		} else {
			p = current_kprobe;
			if (p->break_handler && p->break_handler(p, regs)) {
				goto ss_probe;
			}
		}
		/* If it's not ours, can't be delete race, (we hold lock). */
		goto no_kprobe;
	}

	lock_kprobes();
	p = get_kprobe(addr);
	if (!p) {
		unlock_kprobes();
		if (*addr != BREAKPOINT_INSTRUCTION) {
			/*
			 * The breakpoint instruction was removed right
			 * after we hit it.  Another cpu has removed
			 * either a probepoint or a debugger breakpoint
			 * at this address.  In either case, no further
			 * handling of this interrupt is appropriate.
			 */
			ret = 1;
		}
		/* Not one of ours: let kernel handle it */
		goto no_kprobe;
	}

	kprobe_status = KPROBE_HIT_ACTIVE;
	current_kprobe = p;
	kprobe_saved_rflags = kprobe_old_rflags
	    = (regs->eflags & (TF_MASK | IF_MASK));
	if (is_IF_modifier(p->ainsn.insn))
		kprobe_saved_rflags &= ~IF_MASK;

	if (p->pre_handler(p, regs)) {
		/* handler has already set things up, so skip ss setup */
		return 1;
	}

      ss_probe:
	prepare_singlestep(p, regs);
	kprobe_status = KPROBE_HIT_SS;
	return 1;

      no_kprobe:
	preempt_enable_no_resched();
	return ret;
}

/*
 * Called after single-stepping.  p->addr is the address of the
 * instruction whose first byte has been replaced by the "int 3"
 * instruction.  To avoid the SMP problems that can occur when we
 * temporarily put back the original opcode to single-step, we
 * single-stepped a copy of the instruction.  The address of this
 * copy is p->ainsn.insn.
 *
 * This function prepares to return from the post-single-step
 * interrupt.  We have to fix up the stack as follows:
 *
 * 0) Except in the case of absolute or indirect jump or call instructions,
 * the new rip is relative to the copied instruction.  We need to make
 * it relative to the original instruction.
 *
 * 1) If the single-stepped instruction was pushfl, then the TF and IF
 * flags are set in the just-pushed eflags, and may need to be cleared.
 *
 * 2) If the single-stepped instruction was a call, the return address
 * that is atop the stack is the address following the copied instruction.
 * We need to make it the address following the original instruction.
 */
static void resume_execution(struct kprobe *p, struct pt_regs *regs)
{
	unsigned long *tos = (unsigned long *)regs->rsp;
	unsigned long next_rip = 0;
	unsigned long copy_rip = (unsigned long)p->ainsn.insn;
	unsigned long orig_rip = (unsigned long)p->addr;
	kprobe_opcode_t *insn = p->ainsn.insn;

	/*skip the REX prefix*/
	if (*insn >= 0x40 && *insn <= 0x4f)
		insn++;

	switch (*insn) {
	case 0x9c:		/* pushfl */
		*tos &= ~(TF_MASK | IF_MASK);
		*tos |= kprobe_old_rflags;
		break;
	case 0xe8:		/* call relative - Fix return addr */
		*tos = orig_rip + (*tos - copy_rip);
		break;
	case 0xff:
		if ((*insn & 0x30) == 0x10) {
			/* call absolute, indirect */
			/* Fix return addr; rip is correct. */
			next_rip = regs->rip;
			*tos = orig_rip + (*tos - copy_rip);
		} else if (((*insn & 0x31) == 0x20) ||	/* jmp near, absolute indirect */
			   ((*insn & 0x31) == 0x21)) {	/* jmp far, absolute indirect */
			/* rip is correct. */
			next_rip = regs->rip;
		}
		break;
	case 0xea:		/* jmp absolute -- rip is correct */
		next_rip = regs->rip;
		break;
	default:
		break;
	}

	regs->eflags &= ~TF_MASK;
	if (next_rip) {
		regs->rip = next_rip;
	} else {
		regs->rip = orig_rip + (regs->rip - copy_rip);
	}
}

/*
 * Interrupts are disabled on entry as trap1 is an interrupt gate and they
 * remain disabled thoroughout this function.  And we hold kprobe lock.
 */
int post_kprobe_handler(struct pt_regs *regs)
{
	if (!kprobe_running())
		return 0;

	if (current_kprobe->post_handler)
		current_kprobe->post_handler(current_kprobe, regs, 0);

	resume_execution(current_kprobe, regs);
	regs->eflags |= kprobe_saved_rflags;

	unlock_kprobes();
	preempt_enable_no_resched();

	/*
	 * if somebody else is singlestepping across a probe point, eflags
	 * will have TF set, in which case, continue the remaining processing
	 * of do_debug, as if this is not a probe hit.
	 */
	if (regs->eflags & TF_MASK)
		return 0;

	return 1;
}

/* Interrupts disabled, kprobe_lock held. */
int kprobe_fault_handler(struct pt_regs *regs, int trapnr)
{
	if (current_kprobe->fault_handler
	    && current_kprobe->fault_handler(current_kprobe, regs, trapnr))
		return 1;

	if (kprobe_status & KPROBE_HIT_SS) {
		resume_execution(current_kprobe, regs);
		regs->eflags |= kprobe_old_rflags;

		unlock_kprobes();
		preempt_enable_no_resched();
	}
	return 0;
}

/*
 * Wrapper routine for handling exceptions.
 */
int kprobe_exceptions_notify(struct notifier_block *self, unsigned long val,
			     void *data)
{
	struct die_args *args = (struct die_args *)data;
	switch (val) {
	case DIE_INT3:
		if (kprobe_handler(args->regs))
			return NOTIFY_STOP;
		break;
	case DIE_DEBUG:
		if (post_kprobe_handler(args->regs))
			return NOTIFY_STOP;
		break;
	case DIE_GPF:
		if (kprobe_running() &&
		    kprobe_fault_handler(args->regs, args->trapnr))
			return NOTIFY_STOP;
		break;
	case DIE_PAGE_FAULT:
		if (kprobe_running() &&
		    kprobe_fault_handler(args->regs, args->trapnr))
			return NOTIFY_STOP;
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

int setjmp_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct jprobe *jp = container_of(p, struct jprobe, kp);
	unsigned long addr;

	jprobe_saved_regs = *regs;
	jprobe_saved_rsp = (long *) regs->rsp;
	addr = (unsigned long)jprobe_saved_rsp;
	/*
	 * As Linus pointed out, gcc assumes that the callee
	 * owns the argument space and could overwrite it, e.g.
	 * tailcall optimization. So, to be absolutely safe
	 * we also save and restore enough stack bytes to cover
	 * the argument area.
	 */
	memcpy(jprobes_stack, (kprobe_opcode_t *) addr, MIN_STACK_SIZE(addr));
	regs->eflags &= ~IF_MASK;
	regs->rip = (unsigned long)(jp->entry);
	return 1;
}

void jprobe_return(void)
{
	preempt_enable_no_resched();
	asm volatile ("       xchg   %%rbx,%%rsp     \n"
		      "       int3			\n"
		      "       .globl jprobe_return_end	\n"
		      "       jprobe_return_end:	\n"
		      "       nop			\n"::"b"
		      (jprobe_saved_rsp):"memory");
}

int longjmp_break_handler(struct kprobe *p, struct pt_regs *regs)
{
	u8 *addr = (u8 *) (regs->rip - 1);
	unsigned long stack_addr = (unsigned long)jprobe_saved_rsp;
	struct jprobe *jp = container_of(p, struct jprobe, kp);

	if ((addr > (u8 *) jprobe_return) && (addr < (u8 *) jprobe_return_end)) {
		if ((long *)regs->rsp != jprobe_saved_rsp) {
			struct pt_regs *saved_regs =
			    container_of(jprobe_saved_rsp, struct pt_regs, rsp);
			printk("current rsp %p does not match saved rsp %p\n",
			       (long *)regs->rsp, jprobe_saved_rsp);
			printk("Saved registers for jprobe %p\n", jp);
			show_registers(saved_regs);
			printk("Current registers\n");
			show_registers(regs);
			BUG();
		}
		*regs = jprobe_saved_regs;
		memcpy((kprobe_opcode_t *) stack_addr, jprobes_stack,
		       MIN_STACK_SIZE(stack_addr));
		return 1;
	}
	return 0;
}

/*
 * kprobe->ainsn.insn points to the copy of the instruction to be single-stepped.
 * By default on x86_64, pages we get from kmalloc or vmalloc are not
 * executable.  Single-stepping an instruction on such a page yields an
 * oops.  So instead of storing the instruction copies in their respective
 * kprobe objects, we allocate a page, map it executable, and store all the
 * instruction copies there.  (We can allocate additional pages if somebody
 * inserts a huge number of probes.)  Each page can hold up to INSNS_PER_PAGE
 * instruction slots, each of which is MAX_INSN_SIZE*sizeof(kprobe_opcode_t)
 * bytes.
 */
#define INSNS_PER_PAGE (PAGE_SIZE/(MAX_INSN_SIZE*sizeof(kprobe_opcode_t)))
struct kprobe_insn_page {
	struct hlist_node hlist;
	kprobe_opcode_t *insns;		/* page of instruction slots */
	char slot_used[INSNS_PER_PAGE];
	int nused;
};

static struct hlist_head kprobe_insn_pages;

/**
 * get_insn_slot() - Find a slot on an executable page for an instruction.
 * We allocate an executable page if there's no room on existing ones.
 */
static kprobe_opcode_t *get_insn_slot(void)
{
	struct kprobe_insn_page *kip;
	struct hlist_node *pos;

	hlist_for_each(pos, &kprobe_insn_pages) {
		kip = hlist_entry(pos, struct kprobe_insn_page, hlist);
		if (kip->nused < INSNS_PER_PAGE) {
			int i;
			for (i = 0; i < INSNS_PER_PAGE; i++) {
				if (!kip->slot_used[i]) {
					kip->slot_used[i] = 1;
					kip->nused++;
					return kip->insns + (i*MAX_INSN_SIZE);
				}
			}
			/* Surprise!  No unused slots.  Fix kip->nused. */
			kip->nused = INSNS_PER_PAGE;
		}
	}

	/* All out of space.  Need to allocate a new page. Use slot 0.*/
	kip = kmalloc(sizeof(struct kprobe_insn_page), GFP_KERNEL);
	if (!kip) {
		return NULL;
	}
	kip->insns = (kprobe_opcode_t*) __vmalloc(PAGE_SIZE,
		GFP_KERNEL|__GFP_HIGHMEM, __pgprot(__PAGE_KERNEL_EXEC));
	if (!kip->insns) {
		kfree(kip);
		return NULL;
	}
	INIT_HLIST_NODE(&kip->hlist);
	hlist_add_head(&kip->hlist, &kprobe_insn_pages);
	memset(kip->slot_used, 0, INSNS_PER_PAGE);
	kip->slot_used[0] = 1;
	kip->nused = 1;
	return kip->insns;
}

/**
 * free_insn_slot() - Free instruction slot obtained from get_insn_slot().
 */
static void free_insn_slot(kprobe_opcode_t *slot)
{
	struct kprobe_insn_page *kip;
	struct hlist_node *pos;

	hlist_for_each(pos, &kprobe_insn_pages) {
		kip = hlist_entry(pos, struct kprobe_insn_page, hlist);
		if (kip->insns <= slot
		    && slot < kip->insns+(INSNS_PER_PAGE*MAX_INSN_SIZE)) {
			int i = (slot - kip->insns) / MAX_INSN_SIZE;
			kip->slot_used[i] = 0;
			kip->nused--;
			if (kip->nused == 0) {
				/*
				 * Page is no longer in use.  Free it unless
				 * it's the last one.  We keep the last one
				 * so as not to have to set it up again the
				 * next time somebody inserts a probe.
				 */
				hlist_del(&kip->hlist);
				if (hlist_empty(&kprobe_insn_pages)) {
					INIT_HLIST_NODE(&kip->hlist);
					hlist_add_head(&kip->hlist,
						&kprobe_insn_pages);
				} else {
					vfree(kip->insns);
					kfree(kip);
				}
			}
			return;
		}
	}
}
