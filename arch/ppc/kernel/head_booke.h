#ifndef __HEAD_BOOKE_H__
#define __HEAD_BOOKE_H__

/*
 * Macros used for common Book-e exception handling
 */

#define SET_IVOR(vector_number, vector_label)		\
		li	r26,vector_label@l; 		\
		mtspr	SPRN_IVOR##vector_number,r26;	\
		sync

#define NORMAL_EXCEPTION_PROLOG						     \
	mtspr	SPRN_SPRG0,r10;		/* save two registers to work with */\
	mtspr	SPRN_SPRG1,r11;						     \
	mtspr	SPRN_SPRG4W,r1;						     \
	mfcr	r10;			/* save CR in r10 for now	   */\
	mfspr	r11,SPRN_SRR1;		/* check whether user or kernel    */\
	andi.	r11,r11,MSR_PR;						     \
	beq	1f;							     \
	mfspr	r1,SPRG3;		/* if from user, start at top of   */\
	lwz	r1,THREAD_INFO-THREAD(r1); /* this thread's kernel stack   */\
	addi	r1,r1,THREAD_SIZE;					     \
1:	subi	r1,r1,INT_FRAME_SIZE;	/* Allocate an exception frame     */\
	tophys(r11,r1);							     \
	stw	r10,_CCR(r11);          /* save various registers	   */\
	stw	r12,GPR12(r11);						     \
	stw	r9,GPR9(r11);						     \
	mfspr	r10,SPRG0;						     \
	stw	r10,GPR10(r11);						     \
	mfspr	r12,SPRG1;						     \
	stw	r12,GPR11(r11);						     \
	mflr	r10;							     \
	stw	r10,_LINK(r11);						     \
	mfspr	r10,SPRG4R;						     \
	mfspr	r12,SRR0;						     \
	stw	r10,GPR1(r11);						     \
	mfspr	r9,SRR1;						     \
	stw	r10,0(r11);						     \
	rlwinm	r9,r9,0,14,12;		/* clear MSR_WE (necessary?)	   */\
	stw	r0,GPR0(r11);						     \
	SAVE_4GPRS(3, r11);						     \
	SAVE_2GPRS(7, r11)

/*
 * Exception prolog for critical exceptions.  This is a little different
 * from the normal exception prolog above since a critical exception
 * can potentially occur at any point during normal exception processing.
 * Thus we cannot use the same SPRG registers as the normal prolog above.
 * Instead we use a couple of words of memory at low physical addresses.
 * This is OK since we don't support SMP on these processors. For Book E
 * processors, we also have a reserved register (SPRG2) that is only used
 * in critical exceptions so we can free up a GPR to use as the base for
 * indirect access to the critical exception save area.  This is necessary
 * since the MMU is always on and the save area is offset from KERNELBASE.
 */
#define CRITICAL_EXCEPTION_PROLOG					     \
	mtspr	SPRG2,r8;		/* SPRG2 only used in criticals */   \
	lis	r8,crit_save@ha;					     \
	stw	r10,crit_r10@l(r8);					     \
	stw	r11,crit_r11@l(r8);					     \
	mfspr	r10,SPRG0;						     \
	stw	r10,crit_sprg0@l(r8);					     \
	mfspr	r10,SPRG1;						     \
	stw	r10,crit_sprg1@l(r8);					     \
	mfspr	r10,SPRG4R;						     \
	stw	r10,crit_sprg4@l(r8);					     \
	mfspr	r10,SPRG5R;						     \
	stw	r10,crit_sprg5@l(r8);					     \
	mfspr	r10,SPRG7R;						     \
	stw	r10,crit_sprg7@l(r8);					     \
	mfspr	r10,SPRN_PID;						     \
	stw	r10,crit_pid@l(r8);					     \
	mfspr	r10,SRR0;						     \
	stw	r10,crit_srr0@l(r8);					     \
	mfspr	r10,SRR1;						     \
	stw	r10,crit_srr1@l(r8);					     \
	mfspr	r8,SPRG2;		/* SPRG2 only used in criticals */   \
	mfcr	r10;			/* save CR in r10 for now	   */\
	mfspr	r11,SPRN_CSRR1;		/* check whether user or kernel    */\
	andi.	r11,r11,MSR_PR;						     \
	lis	r11,critical_stack_top@h;				     \
	ori	r11,r11,critical_stack_top@l;				     \
	beq	1f;							     \
	/* COMING FROM USER MODE */					     \
	mfspr	r11,SPRG3;		/* if from user, start at top of   */\
	lwz	r11,THREAD_INFO-THREAD(r11); /* this thread's kernel stack */\
	addi	r11,r11,THREAD_SIZE;					     \
1:	subi	r11,r11,INT_FRAME_SIZE;	/* Allocate an exception frame     */\
	stw	r10,_CCR(r11);          /* save various registers	   */\
	stw	r12,GPR12(r11);						     \
	stw	r9,GPR9(r11);						     \
	mflr	r10;							     \
	stw	r10,_LINK(r11);						     \
	mfspr	r12,SPRN_DEAR;		/* save DEAR and ESR in the frame  */\
	stw	r12,_DEAR(r11);		/* since they may have had stuff   */\
	mfspr	r9,SPRN_ESR;		/* in them at the point where the  */\
	stw	r9,_ESR(r11);		/* exception was taken		   */\
	mfspr	r12,CSRR0;						     \
	stw	r1,GPR1(r11);						     \
	mfspr	r9,CSRR1;						     \
	stw	r1,0(r11);						     \
	tovirt(r1,r11);							     \
	rlwinm	r9,r9,0,14,12;		/* clear MSR_WE (necessary?)	   */\
	stw	r0,GPR0(r11);						     \
	SAVE_4GPRS(3, r11);						     \
	SAVE_2GPRS(7, r11)

/*
 * Exception prolog for machine check exceptions.  This is similar to
 * the critical exception prolog, except that machine check exceptions
 * have their own save area.  For Book E processors, we also have a
 * reserved register (SPRG6) that is only used in machine check exceptions
 * so we can free up a GPR to use as the base for indirect access to the
 * machine check exception save area.  This is necessary since the MMU
 * is always on and the save area is offset from KERNELBASE.
 */
#define MCHECK_EXCEPTION_PROLOG					     \
	mtspr	SPRG6W,r8;		/* SPRG6 used in machine checks */   \
	lis	r8,mcheck_save@ha;					     \
	stw	r10,mcheck_r10@l(r8);					     \
	stw	r11,mcheck_r11@l(r8);					     \
	mfspr	r10,SPRG0;						     \
	stw	r10,mcheck_sprg0@l(r8);					     \
	mfspr	r10,SPRG1;						     \
	stw	r10,mcheck_sprg1@l(r8);					     \
	mfspr	r10,SPRG4R;						     \
	stw	r10,mcheck_sprg4@l(r8);					     \
	mfspr	r10,SPRG5R;						     \
	stw	r10,mcheck_sprg5@l(r8);					     \
	mfspr	r10,SPRG7R;						     \
	stw	r10,mcheck_sprg7@l(r8);					     \
	mfspr	r10,SPRN_PID;						     \
	stw	r10,mcheck_pid@l(r8);					     \
	mfspr	r10,SRR0;						     \
	stw	r10,mcheck_srr0@l(r8);					     \
	mfspr	r10,SRR1;						     \
	stw	r10,mcheck_srr1@l(r8);					     \
	mfspr	r10,CSRR0;						     \
	stw	r10,mcheck_csrr0@l(r8);					     \
	mfspr	r10,CSRR1;						     \
	stw	r10,mcheck_csrr1@l(r8);					     \
	mfspr	r8,SPRG6R;		/* SPRG6 used in machine checks */   \
	mfcr	r10;			/* save CR in r10 for now	   */\
	mfspr	r11,SPRN_MCSRR1;	/* check whether user or kernel    */\
	andi.	r11,r11,MSR_PR;						     \
	lis	r11,mcheck_stack_top@h;					     \
	ori	r11,r11,mcheck_stack_top@l;				     \
	beq	1f;							     \
	/* COMING FROM USER MODE */					     \
	mfspr	r11,SPRG3;		/* if from user, start at top of   */\
	lwz	r11,THREAD_INFO-THREAD(r11); /* this thread's kernel stack */\
	addi	r11,r11,THREAD_SIZE;					     \
1:	subi	r11,r11,INT_FRAME_SIZE;	/* Allocate an exception frame     */\
	stw	r10,_CCR(r11);          /* save various registers	   */\
	stw	r12,GPR12(r11);						     \
	stw	r9,GPR9(r11);						     \
	mflr	r10;							     \
	stw	r10,_LINK(r11);						     \
	mfspr	r12,SPRN_DEAR;		/* save DEAR and ESR in the frame  */\
	stw	r12,_DEAR(r11);		/* since they may have had stuff   */\
	mfspr	r9,SPRN_ESR;		/* in them at the point where the  */\
	stw	r9,_ESR(r11);		/* exception was taken		   */\
	mfspr	r12,MCSRR0;						     \
	stw	r1,GPR1(r11);						     \
	mfspr	r9,MCSRR1;						     \
	stw	r1,0(r11);						     \
	tovirt(r1,r11);							     \
	rlwinm	r9,r9,0,14,12;		/* clear MSR_WE (necessary?)	   */\
	stw	r0,GPR0(r11);						     \
	SAVE_4GPRS(3, r11);						     \
	SAVE_2GPRS(7, r11)

/*
 * Exception vectors.
 */
#define	START_EXCEPTION(label)						     \
        .align 5;              						     \
label:

#define FINISH_EXCEPTION(func)					\
	bl	transfer_to_handler_full;			\
	.long	func;						\
	.long	ret_from_except_full

#define EXCEPTION(n, label, hdlr, xfer)				\
	START_EXCEPTION(label);					\
	NORMAL_EXCEPTION_PROLOG;				\
	addi	r3,r1,STACK_FRAME_OVERHEAD;			\
	xfer(n, hdlr)

#define CRITICAL_EXCEPTION(n, label, hdlr)			\
	START_EXCEPTION(label);					\
	CRITICAL_EXCEPTION_PROLOG;				\
	addi	r3,r1,STACK_FRAME_OVERHEAD;			\
	EXC_XFER_TEMPLATE(hdlr, n+2, (MSR_KERNEL & ~(MSR_ME|MSR_DE|MSR_CE)), \
			  NOCOPY, transfer_to_handler_full, \
			  ret_from_except_full)

#define MCHECK_EXCEPTION(n, label, hdlr)			\
	START_EXCEPTION(label);					\
	MCHECK_EXCEPTION_PROLOG;				\
	mfspr	r5,SPRN_ESR;					\
	stw	r5,_ESR(r11);					\
	addi	r3,r1,STACK_FRAME_OVERHEAD;			\
	EXC_XFER_TEMPLATE(hdlr, n+2, (MSR_KERNEL & ~(MSR_ME|MSR_DE|MSR_CE)), \
			  NOCOPY, mcheck_transfer_to_handler,   \
			  ret_from_mcheck_exc)

#define EXC_XFER_TEMPLATE(hdlr, trap, msr, copyee, tfer, ret)	\
	li	r10,trap;					\
	stw	r10,TRAP(r11);					\
	lis	r10,msr@h;					\
	ori	r10,r10,msr@l;					\
	copyee(r10, r9);					\
	bl	tfer;		 				\
	.long	hdlr;						\
	.long	ret

#define COPY_EE(d, s)		rlwimi d,s,0,16,16
#define NOCOPY(d, s)

#define EXC_XFER_STD(n, hdlr)		\
	EXC_XFER_TEMPLATE(hdlr, n, MSR_KERNEL, NOCOPY, transfer_to_handler_full, \
			  ret_from_except_full)

#define EXC_XFER_LITE(n, hdlr)		\
	EXC_XFER_TEMPLATE(hdlr, n+1, MSR_KERNEL, NOCOPY, transfer_to_handler, \
			  ret_from_except)

#define EXC_XFER_EE(n, hdlr)		\
	EXC_XFER_TEMPLATE(hdlr, n, MSR_KERNEL, COPY_EE, transfer_to_handler_full, \
			  ret_from_except_full)

#define EXC_XFER_EE_LITE(n, hdlr)	\
	EXC_XFER_TEMPLATE(hdlr, n+1, MSR_KERNEL, COPY_EE, transfer_to_handler, \
			  ret_from_except)


/* Check for a single step debug exception while in an exception
 * handler before state has been saved.  This is to catch the case
 * where an instruction that we are trying to single step causes
 * an exception (eg ITLB/DTLB miss) and thus the first instruction of
 * the exception handler generates a single step debug exception.
 *
 * If we get a debug trap on the first instruction of an exception handler,
 * we reset the MSR_DE in the _exception handler's_ MSR (the debug trap is
 * a critical exception, so we are using SPRN_CSRR1 to manipulate the MSR).
 * The exception handler was handling a non-critical interrupt, so it will
 * save (and later restore) the MSR via SPRN_CSRR1, which will still have
 * the MSR_DE bit set.
 */
#define DEBUG_EXCEPTION							      \
	START_EXCEPTION(Debug);						      \
	CRITICAL_EXCEPTION_PROLOG;					      \
									      \
	/*								      \
	 * If there is a single step or branch-taken exception in an	      \
	 * exception entry sequence, it was probably meant to apply to	      \
	 * the code where the exception occurred (since exception entry	      \
	 * doesn't turn off DE automatically).  We simulate the effect	      \
	 * of turning off DE on entry to an exception handler by turning      \
	 * off DE in the CSRR1 value and clearing the debug status.	      \
	 */								      \
	mfspr	r10,SPRN_DBSR;		/* check single-step/branch taken */  \
	andis.	r10,r10,DBSR_IC@h;					      \
	beq+	2f;							      \
									      \
	lis	r10,KERNELBASE@h;	/* check if exception in vectors */   \
	ori	r10,r10,KERNELBASE@l;					      \
	cmplw	r12,r10;						      \
	blt+	2f;			/* addr below exception vectors */    \
									      \
	lis	r10,Debug@h;						      \
	ori	r10,r10,Debug@l;					      \
	cmplw	r12,r10;						      \
	bgt+	2f;			/* addr above exception vectors */    \
									      \
	/* here it looks like we got an inappropriate debug exception. */     \
1:	rlwinm	r9,r9,0,~MSR_DE;	/* clear DE in the CSRR1 value */     \
	lis	r10,DBSR_IC@h;		/* clear the IC event */	      \
	mtspr	SPRN_DBSR,r10;						      \
	/* restore state and get out */					      \
	lwz	r10,_CCR(r11);						      \
	lwz	r0,GPR0(r11);						      \
	lwz	r1,GPR1(r11);						      \
	mtcrf	0x80,r10;						      \
	mtspr	CSRR0,r12;						      \
	mtspr	CSRR1,r9;						      \
	lwz	r9,GPR9(r11);						      \
	lwz	r12,GPR12(r11);						      \
	mtspr	SPRG2,r8;		/* SPRG2 only used in criticals */    \
	lis	r8,crit_save@ha;					      \
	lwz	r10,crit_r10@l(r8);					      \
	lwz	r11,crit_r11@l(r8);					      \
	mfspr	r8,SPRG2;						      \
									      \
	rfci;								      \
	b	.;							      \
									      \
	/* continue normal handling for a critical exception... */	      \
2:	mfspr	r4,SPRN_DBSR;						      \
	addi	r3,r1,STACK_FRAME_OVERHEAD;				      \
	EXC_XFER_TEMPLATE(DebugException, 0x2002, (MSR_KERNEL & ~(MSR_ME|MSR_DE|MSR_CE)), NOCOPY, crit_transfer_to_handler, ret_from_crit_exc)

#define INSTRUCTION_STORAGE_EXCEPTION					      \
	START_EXCEPTION(InstructionStorage)				      \
	NORMAL_EXCEPTION_PROLOG;					      \
	mfspr	r5,SPRN_ESR;		/* Grab the ESR and save it */	      \
	stw	r5,_ESR(r11);						      \
	mr      r4,r12;                 /* Pass SRR0 as arg2 */		      \
	li      r5,0;                   /* Pass zero as arg3 */		      \
	EXC_XFER_EE_LITE(0x0400, handle_page_fault)

#define ALIGNMENT_EXCEPTION						      \
	START_EXCEPTION(Alignment)					      \
	NORMAL_EXCEPTION_PROLOG;					      \
	mfspr   r4,SPRN_DEAR;           /* Grab the DEAR and save it */	      \
	stw     r4,_DEAR(r11);						      \
	addi    r3,r1,STACK_FRAME_OVERHEAD;				      \
	EXC_XFER_EE(0x0600, AlignmentException)

#define PROGRAM_EXCEPTION						      \
	START_EXCEPTION(Program)					      \
	NORMAL_EXCEPTION_PROLOG;					      \
	mfspr	r4,SPRN_ESR;		/* Grab the ESR and save it */	      \
	stw	r4,_ESR(r11);						      \
	addi	r3,r1,STACK_FRAME_OVERHEAD;				      \
	EXC_XFER_STD(0x0700, ProgramCheckException)

#define DECREMENTER_EXCEPTION						      \
	START_EXCEPTION(Decrementer)					      \
	NORMAL_EXCEPTION_PROLOG;					      \
	lis     r0,TSR_DIS@h;           /* Setup the DEC interrupt mask */    \
	mtspr   SPRN_TSR,r0;		/* Clear the DEC interrupt */	      \
	addi    r3,r1,STACK_FRAME_OVERHEAD;				      \
	EXC_XFER_LITE(0x0900, timer_interrupt)

#endif /* __HEAD_BOOKE_H__ */
