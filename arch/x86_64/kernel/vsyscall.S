/*
 * Code for the vsyscall page.  This version uses the syscall instruction.
 */

#include <asm/ia32_unistd.h>
#include <asm/offset.h>

	.text
	.globl __kernel_vsyscall
	.type __kernel_vsyscall,@function
__kernel_vsyscall:
.LSTART_vsyscall:
	push	%ebp
.Lpush_ebp:
	movl	%ecx, %ebp
	syscall
	popl	%ebp
.Lpop_ebp:
	ret
.LEND_vsyscall:
	.size __kernel_vsyscall,.-.LSTART_vsyscall

	.balign 32
	.globl __kernel_sigreturn
	.type __kernel_sigreturn,@function
__kernel_sigreturn:
.LSTART_sigreturn:
	popl %eax
	movl $__NR_ia32_sigreturn, %eax
	syscall
.LEND_sigreturn:
	.size __kernel_sigreturn,.-.LSTART_sigreturn

	.balign 32
	.globl __kernel_rt_sigreturn
	.type __kernel_rt_sigreturn,@function
__kernel_rt_sigreturn:
.LSTART_rt_sigreturn:
	movl $__NR_ia32_rt_sigreturn, %eax
	syscall
.LEND_rt_sigreturn:
	.size __kernel_rt_sigreturn,.-.LSTART_rt_sigreturn

	.section .eh_frame,"a",@progbits
.LSTARTFRAME:
	.long .LENDCIE-.LSTARTCIE
.LSTARTCIE:
	.long 0			/* CIE ID */
	.byte 1			/* Version number */
	.string "zR"		/* NUL-terminated augmentation string */
	.uleb128 1		/* Code alignment factor */
	.sleb128 -4		/* Data alignment factor */
	.byte 8			/* Return address register column */
	.uleb128 1		/* Augmentation value length */
	.byte 0x1b		/* DW_EH_PE_pcrel|DW_EH_PE_sdata4. */
	.byte 0x0c		/* DW_CFA_def_cfa */
	.uleb128 4
	.uleb128 4
	.byte 0x88		/* DW_CFA_offset, column 0x8 */
	.uleb128 1
	.align 4
.LENDCIE:

	.long .LENDFDE1-.LSTARTFDE1	/* Length FDE */
.LSTARTFDE1:
	.long .LSTARTFDE1-.LSTARTFRAME	/* CIE pointer */
	.long .LSTART_vsyscall-.	/* PC-relative start address */
	.long .LEND_vsyscall-.LSTART_vsyscall
	.uleb128 0			/* Augmentation length */
	/* What follows are the instructions for the table generation.
	   We have to record all changes of the stack pointer.  */
	.byte 0x40 + .Lpush_ebp-.LSTART_vsyscall /* DW_CFA_advance_loc */
	.byte 0x0e		/* DW_CFA_def_cfa_offset */
	.uleb128 8
	.byte 0x85, 0x02	/* DW_CFA_offset %ebp -8 */
	.byte 0x40 + .Lpop_ebp-.Lpush_ebp /* DW_CFA_advance_loc */
	.byte 0xc5		/* DW_CFA_restore %ebp */
	.byte 0x0e		/* DW_CFA_def_cfa_offset */
	.uleb128 4
	.align 4
.LENDFDE1:

	.long .LENDFDE2-.LSTARTFDE2	/* Length FDE */
.LSTARTFDE2:
	.long .LSTARTFDE2-.LSTARTFRAME	/* CIE pointer */
	/* HACK: The dwarf2 unwind routines will subtract 1 from the
	   return address to get an address in the middle of the
	   presumed call instruction.  Since we didn't get here via
	   a call, we need to include the nop before the real start
	   to make up for it.  */
	.long .LSTART_sigreturn-1-.	/* PC-relative start address */
	.long .LEND_sigreturn-.LSTART_sigreturn+1
	.uleb128 0			/* Augmentation length */
	/* What follows are the instructions for the table generation.
	   We record the locations of each register saved.  This is
	   complicated by the fact that the "CFA" is always assumed to
	   be the value of the stack pointer in the caller.  This means
	   that we must define the CFA of this body of code to be the
	   saved value of the stack pointer in the sigcontext.  Which
	   also means that there is no fixed relation to the other 
	   saved registers, which means that we must use DW_CFA_expression
	   to compute their addresses.  It also means that when we 
	   adjust the stack with the popl, we have to do it all over again.  */

#define do_cfa_expr(offset)						\
	.byte 0x0f;			/* DW_CFA_def_cfa_expression */	\
	.uleb128 1f-0f;			/*   length */			\
0:	.byte 0x74;			/*     DW_OP_breg4 */		\
	.sleb128 offset;		/*      offset */		\
	.byte 0x06;			/*     DW_OP_deref */		\
1:

#define do_expr(regno, offset)						\
	.byte 0x10;			/* DW_CFA_expression */		\
	.uleb128 regno;			/*   regno */			\
	.uleb128 1f-0f;			/*   length */			\
0:	.byte 0x74;			/*     DW_OP_breg4 */		\
	.sleb128 offset;		/*       offset */		\
1:

	do_cfa_expr(IA32_SIGCONTEXT_esp+4)
	do_expr(0, IA32_SIGCONTEXT_eax+4)
	do_expr(1, IA32_SIGCONTEXT_ecx+4)
	do_expr(2, IA32_SIGCONTEXT_edx+4)
	do_expr(3, IA32_SIGCONTEXT_ebx+4)
	do_expr(5, IA32_SIGCONTEXT_ebp+4)
	do_expr(6, IA32_SIGCONTEXT_esi+4)
	do_expr(7, IA32_SIGCONTEXT_edi+4)
	do_expr(8, IA32_SIGCONTEXT_eip+4)

	.byte 0x42	/* DW_CFA_advance_loc 2 -- nop; popl eax. */

	do_cfa_expr(IA32_SIGCONTEXT_esp)
	do_expr(0, IA32_SIGCONTEXT_eax)
	do_expr(1, IA32_SIGCONTEXT_ecx)
	do_expr(2, IA32_SIGCONTEXT_edx)
	do_expr(3, IA32_SIGCONTEXT_ebx)
	do_expr(5, IA32_SIGCONTEXT_ebp)
	do_expr(6, IA32_SIGCONTEXT_esi)
	do_expr(7, IA32_SIGCONTEXT_edi)
	do_expr(8, IA32_SIGCONTEXT_eip)

	.align 4
.LENDFDE2:

	.long .LENDFDE3-.LSTARTFDE3	/* Length FDE */
.LSTARTFDE3:
	.long .LSTARTFDE3-.LSTARTFRAME	/* CIE pointer */
	/* HACK: See above wrt unwind library assumptions.  */
	.long .LSTART_rt_sigreturn-1-.	/* PC-relative start address */
	.long .LEND_rt_sigreturn-.LSTART_rt_sigreturn+1
	.uleb128 0			/* Augmentation */
	/* What follows are the instructions for the table generation.
	   We record the locations of each register saved.  This is
	   slightly less complicated than the above, since we don't
	   modify the stack pointer in the process.  */

	do_cfa_expr(IA32_RT_SIGFRAME_sigcontext-4 + IA32_SIGCONTEXT_esp)
	do_expr(0, IA32_RT_SIGFRAME_sigcontext-4 + IA32_SIGCONTEXT_eax)
	do_expr(1, IA32_RT_SIGFRAME_sigcontext-4 + IA32_SIGCONTEXT_ecx)
	do_expr(2, IA32_RT_SIGFRAME_sigcontext-4 + IA32_SIGCONTEXT_edx)
	do_expr(3, IA32_RT_SIGFRAME_sigcontext-4 + IA32_SIGCONTEXT_ebx)
	do_expr(5, IA32_RT_SIGFRAME_sigcontext-4 + IA32_SIGCONTEXT_ebp)
	do_expr(6, IA32_RT_SIGFRAME_sigcontext-4 + IA32_SIGCONTEXT_esi)
	do_expr(7, IA32_RT_SIGFRAME_sigcontext-4 + IA32_SIGCONTEXT_edi)
	do_expr(8, IA32_RT_SIGFRAME_sigcontext-4 + IA32_SIGCONTEXT_eip)

	.align 4
.LENDFDE3:
