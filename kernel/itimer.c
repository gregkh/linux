/*
 * linux/kernel/itimer.c
 *
 * Copyright (C) 1992 Darren Senn
 */

/* These are all the functions necessary to implement itimers */

#include <linux/mm.h>
#include <linux/smp_lock.h>
#include <linux/interrupt.h>
#include <linux/syscalls.h>
#include <linux/time.h>

#include <asm/uaccess.h>

int do_getitimer(int which, struct itimerval *value)
{
	register unsigned long val;

	switch (which) {
	case ITIMER_REAL:
		val = 0;
		/* 
		 * FIXME! This needs to be atomic, in case the kernel timer happens!
		 */
		if (timer_pending(&current->real_timer)) {
			val = current->real_timer.expires - jiffies;

			/* look out for negative/zero itimer.. */
			if ((long) val <= 0)
				val = 1;
		}
		jiffies_to_timeval(val, &value->it_value);
		jiffies_to_timeval(current->it_real_incr, &value->it_interval);
		break;
	case ITIMER_VIRTUAL:
		cputime_to_timeval(current->it_virt_value, &value->it_value);
		cputime_to_timeval(current->it_virt_incr, &value->it_interval);
		break;
	case ITIMER_PROF:
		cputime_to_timeval(current->it_prof_value, &value->it_value);
		cputime_to_timeval(current->it_prof_incr, &value->it_interval);
		break;
	default:
		return(-EINVAL);
	}
	return 0;
}

/* SMP: Only we modify our itimer values. */
asmlinkage long sys_getitimer(int which, struct itimerval __user *value)
{
	int error = -EFAULT;
	struct itimerval get_buffer;

	if (value) {
		error = do_getitimer(which, &get_buffer);
		if (!error &&
		    copy_to_user(value, &get_buffer, sizeof(get_buffer)))
			error = -EFAULT;
	}
	return error;
}

void it_real_fn(unsigned long __data)
{
	struct task_struct * p = (struct task_struct *) __data;
	unsigned long interval;

	send_group_sig_info(SIGALRM, SEND_SIG_PRIV, p);
	interval = p->it_real_incr;
	if (interval) {
		if (interval > (unsigned long) LONG_MAX)
			interval = LONG_MAX;
		p->real_timer.expires = jiffies + interval;
		add_timer(&p->real_timer);
	}
}

int do_setitimer(int which, struct itimerval *value, struct itimerval *ovalue)
{
	unsigned long expire;
	cputime_t cputime;
	int k;

	if (ovalue && (k = do_getitimer(which, ovalue)) < 0)
		return k;
	switch (which) {
		case ITIMER_REAL:
			del_timer_sync(&current->real_timer);
			expire = timeval_to_jiffies(&value->it_value);
			current->it_real_value = expire;
			current->it_real_incr =
				timeval_to_jiffies(&value->it_interval);
			if (!expire)
				break;
			if (expire > (unsigned long) LONG_MAX)
				expire = LONG_MAX;
			current->real_timer.expires = jiffies + expire;
			add_timer(&current->real_timer);
			break;
		case ITIMER_VIRTUAL:
			cputime = timeval_to_cputime(&value->it_value);
			if (cputime_gt(cputime, cputime_zero))
				cputime = cputime_add(cputime,
						      jiffies_to_cputime(1));
			current->it_virt_value = cputime;
			cputime = timeval_to_cputime(&value->it_interval);
			current->it_virt_incr = cputime;
			break;
		case ITIMER_PROF:
			cputime = timeval_to_cputime(&value->it_value);
			if (cputime_gt(cputime, cputime_zero))
				cputime = cputime_add(cputime,
						      jiffies_to_cputime(1));
			current->it_prof_value = cputime;
			cputime = timeval_to_cputime(&value->it_interval);
			current->it_prof_incr = cputime;
			break;
		default:
			return -EINVAL;
	}
	return 0;
}

/* SMP: Again, only we play with our itimers, and signals are SMP safe
 *      now so that is not an issue at all anymore.
 */
asmlinkage long sys_setitimer(int which,
			      struct itimerval __user *value,
			      struct itimerval __user *ovalue)
{
	struct itimerval set_buffer, get_buffer;
	int error;

	if (value) {
		if(copy_from_user(&set_buffer, value, sizeof(set_buffer)))
			return -EFAULT;
	} else
		memset((char *) &set_buffer, 0, sizeof(set_buffer));

	error = do_setitimer(which, &set_buffer, ovalue ? &get_buffer : NULL);
	if (error || !ovalue)
		return error;

	if (copy_to_user(ovalue, &get_buffer, sizeof(get_buffer)))
		return -EFAULT; 
	return 0;
}
