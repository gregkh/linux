#ifndef _linux_POSIX_TIMERS_H
#define _linux_POSIX_TIMERS_H

#include <linux/spinlock.h>
#include <linux/list.h>

/* POSIX.1b interval timer structure. */
struct k_itimer {
	struct list_head list;		/* free/ allocate list */
	spinlock_t it_lock;
	clockid_t it_clock;		/* which timer type */
	timer_t it_id;			/* timer id */
	int it_overrun;			/* overrun on pending signal  */
	int it_overrun_last;		/* overrun on last delivered signal */
	int it_requeue_pending;         /* waiting to requeue this timer */
	int it_sigev_notify;		/* notify word of sigevent struct */
	int it_sigev_signo;		/* signo word of sigevent struct */
	sigval_t it_sigev_value;	/* value word of sigevent struct */
	unsigned long it_incr;		/* interval specified in jiffies */
	struct task_struct *it_process;	/* process to send signal to */
	struct timer_list it_timer;
	struct sigqueue *sigq;		/* signal queue entry. */
	struct list_head abs_timer_entry; /* clock abs_timer_list */
	struct timespec wall_to_prev;   /* wall_to_monotonic used when set */
};

struct k_clock_abs {
	struct list_head list;
	spinlock_t lock;
};
struct k_clock {
	int res;		/* in nano seconds */
	struct k_clock_abs *abs_struct;
	int (*clock_set) (struct timespec * tp);
	int (*clock_get) (struct timespec * tp);
	int (*timer_create) (struct k_itimer *timer);
	int (*nsleep) (int which_clock, int flags,
		       struct timespec * t);
	int (*timer_set) (struct k_itimer * timr, int flags,
			  struct itimerspec * new_setting,
			  struct itimerspec * old_setting);
	int (*timer_del) (struct k_itimer * timr);
	void (*timer_get) (struct k_itimer * timr,
			   struct itimerspec * cur_setting);
};

void register_posix_clock(int clock_id, struct k_clock *new_clock);

/* Error handlers for timer_create, nanosleep and settime */
int do_posix_clock_notimer_create(struct k_itimer *timer);
int do_posix_clock_nonanosleep(int which_clock, int flags, struct timespec * t);
int do_posix_clock_nosettime(struct timespec *tp);

/* function to call to trigger timer event */
int posix_timer_event(struct k_itimer *timr, int si_private);

struct now_struct {
	unsigned long jiffies;
};

#define posix_get_now(now) (now)->jiffies = jiffies;
#define posix_time_before(timer, now) \
                      time_before((timer)->expires, (now)->jiffies)

#define posix_bump_timer(timr, now)					\
         do {								\
              long delta, orun;						\
	      delta = now.jiffies - (timr)->it_timer.expires;		\
              if (delta >= 0) {						\
	           orun = 1 + (delta / (timr)->it_incr);		\
	          (timr)->it_timer.expires += orun * (timr)->it_incr;	\
                  (timr)->it_overrun += orun;				\
              }								\
            }while (0)
#endif

