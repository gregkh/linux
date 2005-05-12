/* rwsem.c: Don't inline expand these suckers all over the place.
 *
 * Written by David S. Miller (davem@redhat.com), 2001.
 * Derived from asm-i386/rwsem.h
 */

#include <linux/kernel.h>
#include <linux/rwsem.h>
#include <linux/init.h>
#include <linux/module.h>

extern struct rw_semaphore *FASTCALL(rwsem_down_read_failed(struct rw_semaphore *sem));
extern struct rw_semaphore *FASTCALL(rwsem_down_write_failed(struct rw_semaphore *sem));
extern struct rw_semaphore *FASTCALL(rwsem_wake(struct rw_semaphore *));
extern struct rw_semaphore *FASTCALL(rwsem_downgrade_wake(struct rw_semaphore *));

void __sched __down_read(struct rw_semaphore *sem)
{
	__asm__ __volatile__(
		"! beginning __down_read\n"
		"1:\tlduw	[%0], %%g5\n\t"
		"add		%%g5, 1, %%g7\n\t"
		"cas		[%0], %%g5, %%g7\n\t"
		"cmp		%%g5, %%g7\n\t"
		"bne,pn		%%icc, 1b\n\t"
		" add		%%g7, 1, %%g7\n\t"
		"cmp		%%g7, 0\n\t"
		"bl,pn		%%icc, 3f\n\t"
		" membar	#StoreLoad | #StoreStore\n"
		"2:\n\t"
		".subsection	2\n"
		"3:\tmov	%0, %%g5\n\t"
		"save		%%sp, -160, %%sp\n\t"
		"mov		%%g1, %%l1\n\t"
		"mov		%%g2, %%l2\n\t"
		"mov		%%g3, %%l3\n\t"
		"call		%1\n\t"
		" mov		%%g5, %%o0\n\t"
		"mov		%%l1, %%g1\n\t"
		"mov		%%l2, %%g2\n\t"
		"ba,pt		%%xcc, 2b\n\t"
		" restore	%%l3, %%g0, %%g3\n\t"
		".previous\n\t"
		"! ending __down_read"
		: : "r" (sem), "i" (rwsem_down_read_failed)
		: "g5", "g7", "memory", "cc");
}
EXPORT_SYMBOL(__down_read);

int __down_read_trylock(struct rw_semaphore *sem)
{
	int result;

	__asm__ __volatile__(
		"! beginning __down_read_trylock\n"
		"1:\tlduw	[%1], %%g5\n\t"
		"add		%%g5, 1, %%g7\n\t"
		"cmp		%%g7, 0\n\t"
		"bl,pn		%%icc, 2f\n\t"
		" mov		0, %0\n\t"
		"cas		[%1], %%g5, %%g7\n\t"
		"cmp		%%g5, %%g7\n\t"
		"bne,pn		%%icc, 1b\n\t"
		" mov		1, %0\n\t"
		"membar		#StoreLoad | #StoreStore\n"
		"2:\n\t"
		"! ending __down_read_trylock"
		: "=&r" (result)
                : "r" (sem)
		: "g5", "g7", "memory", "cc");

	return result;
}
EXPORT_SYMBOL(__down_read_trylock);

void __sched __down_write(struct rw_semaphore *sem)
{
	__asm__ __volatile__(
		"! beginning __down_write\n\t"
		"sethi		%%hi(%2), %%g1\n\t"
		"or		%%g1, %%lo(%2), %%g1\n"
		"1:\tlduw	[%0], %%g5\n\t"
		"add		%%g5, %%g1, %%g7\n\t"
		"cas		[%0], %%g5, %%g7\n\t"
		"cmp		%%g5, %%g7\n\t"
		"bne,pn		%%icc, 1b\n\t"
		" cmp		%%g7, 0\n\t"
		"bne,pn		%%icc, 3f\n\t"
		" membar	#StoreLoad | #StoreStore\n"
		"2:\n\t"
		".subsection	2\n"
		"3:\tmov	%0, %%g5\n\t"
		"save		%%sp, -160, %%sp\n\t"
		"mov		%%g2, %%l2\n\t"
		"mov		%%g3, %%l3\n\t"
		"call		%1\n\t"
		" mov		%%g5, %%o0\n\t"
		"mov		%%l2, %%g2\n\t"
		"ba,pt		%%xcc, 2b\n\t"
		" restore	%%l3, %%g0, %%g3\n\t"
		".previous\n\t"
		"! ending __down_write"
		: : "r" (sem), "i" (rwsem_down_write_failed),
		    "i" (RWSEM_ACTIVE_WRITE_BIAS)
		: "g1", "g5", "g7", "memory", "cc");
}
EXPORT_SYMBOL(__down_write);

int __down_write_trylock(struct rw_semaphore *sem)
{
	int result;

	__asm__ __volatile__(
		"! beginning __down_write_trylock\n\t"
		"sethi		%%hi(%2), %%g1\n\t"
		"or		%%g1, %%lo(%2), %%g1\n"
		"1:\tlduw	[%1], %%g5\n\t"
		"cmp		%%g5, 0\n\t"
		"bne,pn		%%icc, 2f\n\t"
		" mov		0, %0\n\t"
		"add		%%g5, %%g1, %%g7\n\t"
		"cas		[%1], %%g5, %%g7\n\t"
		"cmp		%%g5, %%g7\n\t"
		"bne,pn		%%icc, 1b\n\t"
		" mov		1, %0\n\t"
		"membar		#StoreLoad | #StoreStore\n"
		"2:\n\t"
		"! ending __down_write_trylock"
		: "=&r" (result)
		: "r" (sem), "i" (RWSEM_ACTIVE_WRITE_BIAS)
		: "g1", "g5", "g7", "memory", "cc");

	return result;
}
EXPORT_SYMBOL(__down_write_trylock);

void __up_read(struct rw_semaphore *sem)
{
	__asm__ __volatile__(
		"! beginning __up_read\n\t"
		"1:\tlduw	[%0], %%g5\n\t"
		"sub		%%g5, 1, %%g7\n\t"
		"cas		[%0], %%g5, %%g7\n\t"
		"cmp		%%g5, %%g7\n\t"
		"bne,pn		%%icc, 1b\n\t"
		" cmp		%%g7, 0\n\t"
		"bl,pn		%%icc, 3f\n\t"
		" membar	#StoreLoad | #StoreStore\n"
		"2:\n\t"
		".subsection	2\n"
		"3:\tsethi	%%hi(%2), %%g1\n\t"
		"sub		%%g7, 1, %%g7\n\t"
		"or		%%g1, %%lo(%2), %%g1\n\t"
		"andcc		%%g7, %%g1, %%g0\n\t"
		"bne,pn		%%icc, 2b\n\t"
		" mov		%0, %%g5\n\t"
		"save		%%sp, -160, %%sp\n\t"
		"mov		%%g2, %%l2\n\t"
		"mov		%%g3, %%l3\n\t"
		"call		%1\n\t"
		" mov		%%g5, %%o0\n\t"
		"mov		%%l2, %%g2\n\t"
		"ba,pt		%%xcc, 2b\n\t"
		" restore	%%l3, %%g0, %%g3\n\t"
		".previous\n\t"
		"! ending __up_read"
		: : "r" (sem), "i" (rwsem_wake),
		    "i" (RWSEM_ACTIVE_MASK)
		: "g1", "g5", "g7", "memory", "cc");
}
EXPORT_SYMBOL(__up_read);

void __up_write(struct rw_semaphore *sem)
{
	__asm__ __volatile__(
		"! beginning __up_write\n\t"
		"sethi		%%hi(%2), %%g1\n\t"
		"or		%%g1, %%lo(%2), %%g1\n"
		"1:\tlduw	[%0], %%g5\n\t"
		"sub		%%g5, %%g1, %%g7\n\t"
		"cas		[%0], %%g5, %%g7\n\t"
		"cmp		%%g5, %%g7\n\t"
		"bne,pn		%%icc, 1b\n\t"
		" sub		%%g7, %%g1, %%g7\n\t"
		"cmp		%%g7, 0\n\t"
		"bl,pn		%%icc, 3f\n\t"
		" membar	#StoreLoad | #StoreStore\n"
		"2:\n\t"
		".subsection 2\n"
		"3:\tmov	%0, %%g5\n\t"
		"save		%%sp, -160, %%sp\n\t"
		"mov		%%g2, %%l2\n\t"
		"mov		%%g3, %%l3\n\t"
		"call		%1\n\t"
		" mov		%%g5, %%o0\n\t"
		"mov		%%l2, %%g2\n\t"
		"ba,pt		%%xcc, 2b\n\t"
		" restore	%%l3, %%g0, %%g3\n\t"
		".previous\n\t"
		"! ending __up_write"
		: : "r" (sem), "i" (rwsem_wake),
		    "i" (RWSEM_ACTIVE_WRITE_BIAS)
		: "g1", "g5", "g7", "memory", "cc");
}
EXPORT_SYMBOL(__up_write);

void __downgrade_write(struct rw_semaphore *sem)
{
	__asm__ __volatile__(
		"! beginning __downgrade_write\n\t"
		"sethi		%%hi(%2), %%g1\n\t"
		"or		%%g1, %%lo(%2), %%g1\n"
		"1:\tlduw	[%0], %%g5\n\t"
		"sub		%%g5, %%g1, %%g7\n\t"
		"cas		[%0], %%g5, %%g7\n\t"
		"cmp		%%g5, %%g7\n\t"
		"bne,pn		%%icc, 1b\n\t"
		" sub		%%g7, %%g1, %%g7\n\t"
		"cmp		%%g7, 0\n\t"
		"bl,pn		%%icc, 3f\n\t"
		" membar	#StoreLoad | #StoreStore\n"
		"2:\n\t"
		".subsection 2\n"
		"3:\tmov	%0, %%g5\n\t"
		"save		%%sp, -160, %%sp\n\t"
		"mov		%%g2, %%l2\n\t"
		"mov		%%g3, %%l3\n\t"
		"call		%1\n\t"
		" mov		%%g5, %%o0\n\t"
		"mov		%%l2, %%g2\n\t"
		"ba,pt		%%xcc, 2b\n\t"
		" restore	%%l3, %%g0, %%g3\n\t"
		".previous\n\t"
		"! ending __up_write"
		: : "r" (sem), "i" (rwsem_downgrade_wake),
		    "i" (RWSEM_WAITING_BIAS)
		: "g1", "g5", "g7", "memory", "cc");
}
EXPORT_SYMBOL(__downgrade_write);
