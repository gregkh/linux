#ifndef _PPC_KERNEL_CPM2_H
#define _PPC_KERNEL_CPM2_H

extern struct hw_interrupt_type cpm2_pic;
extern int cpm2_get_irq(struct pt_regs *regs);

#endif /* _PPC_KERNEL_CPM2_H */
