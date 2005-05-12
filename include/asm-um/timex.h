#ifndef __UM_TIMEX_H
#define __UM_TIMEX_H

typedef unsigned long cycles_t;

#define cacheflush_time (0)

static inline cycles_t get_cycles (void)
{
	return 0;
}

#define CLOCK_TICK_RATE (HZ)

#endif
