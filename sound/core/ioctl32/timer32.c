/*
 *   32bit -> 64bit ioctl wrapper for timer API
 *   Copyright (c) by Takashi Iwai <tiwai@suse.de>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <sound/driver.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/compat.h>
#include <sound/core.h>
#include <sound/timer.h>
#include <asm/uaccess.h>
#include "ioctl32.h"

struct sndrv_timer_info32 {
	u32 flags;
	s32 card;
	unsigned char id[64];
	unsigned char name[80];
	u32 reserved0;
	u32 resolution;
	unsigned char reserved[64];
};

#define CVT_sndrv_timer_info()\
{\
	COPY(flags);\
	COPY(card);\
	COPY_ARRAY(id);\
	COPY_ARRAY(name);\
	COPY_CVT(resolution);\
}

struct sndrv_timer_status32 {
	struct compat_timespec tstamp;
	u32 resolution;
	u32 lost;
	u32 overrun;
	u32 queue;
	unsigned char reserved[64];
};

#define CVT_sndrv_timer_status()\
{\
	COPY_CVT(tstamp.tv_sec);\
	COPY_CVT(tstamp.tv_nsec);\
	COPY(resolution);\
	COPY(lost);\
	COPY(overrun);\
	COPY(queue);\
}

DEFINE_ALSA_IOCTL(timer_info);
DEFINE_ALSA_IOCTL(timer_status);

DEFINE_ALSA_IOCTL_ENTRY(timer_info, timer_info, SNDRV_TIMER_IOCTL_INFO);
DEFINE_ALSA_IOCTL_ENTRY(timer_status, timer_status, SNDRV_TIMER_IOCTL_STATUS);

/*
 */

#define AP(x) snd_ioctl32_##x

enum {
	SNDRV_TIMER_IOCTL_INFO32 = _IOR('T', 0x11, struct sndrv_timer_info32),
	SNDRV_TIMER_IOCTL_STATUS32 = _IOW('T', 0x14, struct sndrv_timer_status32),
};

struct ioctl32_mapper timer_mappers[] = {
	MAP_COMPAT(SNDRV_TIMER_IOCTL_PVERSION),
	MAP_COMPAT(SNDRV_TIMER_IOCTL_NEXT_DEVICE),
	MAP_COMPAT(SNDRV_TIMER_IOCTL_SELECT),
	{ SNDRV_TIMER_IOCTL_INFO32, AP(timer_info) },
	MAP_COMPAT(SNDRV_TIMER_IOCTL_PARAMS),
	{ SNDRV_TIMER_IOCTL_STATUS32, AP(timer_status) },
#if 0
	/* ** FIXME **
	 * The following four entries are disabled because they conflict
	 * with the TCOC* definitions.
	 * Unfortunately, the current ioctl32 wrapper uses a single
	 * hash table for all devices.  Once when the wrapper is fixed
	 * with the table based on devices, they'll be back again.
	 */
	MAP_COMPAT(SNDRV_TIMER_IOCTL_START),
	MAP_COMPAT(SNDRV_TIMER_IOCTL_STOP),
	MAP_COMPAT(SNDRV_TIMER_IOCTL_CONTINUE),
	MAP_COMPAT(SNDRV_TIMER_IOCTL_PAUSE),
#endif
	{ 0 },
};
