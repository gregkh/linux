/*
 *   32bit -> 64bit ioctl wrapper for raw MIDI API
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
#include <sound/rawmidi.h>
#include <asm/uaccess.h>
#include "ioctl32.h"

struct sndrv_rawmidi_params32 {
	s32 stream;
	u32 buffer_size;
	u32 avail_min;
	unsigned int no_active_sensing; /* avoid bit-field */
	unsigned char reserved[16];
} __attribute__((packed));

#define CVT_sndrv_rawmidi_params()\
{\
	COPY(stream);\
	COPY_CVT(buffer_size);\
	COPY_CVT(avail_min);\
	if (copy_in_user(((size_t __user *)&dst->avail_min + 1),\
			 ((size_t __user *)&src->avail_min + 1), 4)) \
		return -EFAULT;\
}

struct sndrv_rawmidi_status32 {
	s32 stream;
	struct compat_timespec tstamp;
	u32 avail;
	u32 xruns;
	unsigned char reserved[16];
} __attribute__((packed));

#define CVT_sndrv_rawmidi_status()\
{\
	COPY(stream);\
	COPY_CVT(tstamp.tv_sec);\
	COPY_CVT(tstamp.tv_nsec);\
	COPY_CVT(avail);\
	COPY_CVT(xruns);\
}

DEFINE_ALSA_IOCTL(rawmidi_params);
DEFINE_ALSA_IOCTL(rawmidi_status);

DEFINE_ALSA_IOCTL_ENTRY(rawmidi_params, rawmidi_params, SNDRV_RAWMIDI_IOCTL_PARAMS);
DEFINE_ALSA_IOCTL_ENTRY(rawmidi_status, rawmidi_status, SNDRV_RAWMIDI_IOCTL_STATUS);

#define AP(x) snd_ioctl32_##x

enum {
	SNDRV_RAWMIDI_IOCTL_PARAMS32 = _IOWR('W', 0x10, struct sndrv_rawmidi_params32),
	SNDRV_RAWMIDI_IOCTL_STATUS32 = _IOWR('W', 0x20, struct sndrv_rawmidi_status32),
};

struct ioctl32_mapper rawmidi_mappers[] = {
	MAP_COMPAT(SNDRV_RAWMIDI_IOCTL_PVERSION),
	MAP_COMPAT(SNDRV_RAWMIDI_IOCTL_INFO),
	{ SNDRV_RAWMIDI_IOCTL_PARAMS32, AP(rawmidi_params) },
	{ SNDRV_RAWMIDI_IOCTL_STATUS32, AP(rawmidi_status) },
	MAP_COMPAT(SNDRV_RAWMIDI_IOCTL_DROP),
	MAP_COMPAT(SNDRV_RAWMIDI_IOCTL_DRAIN),

	MAP_COMPAT(SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE),
	MAP_COMPAT(SNDRV_CTL_IOCTL_RAWMIDI_INFO),
	MAP_COMPAT(SNDRV_CTL_IOCTL_RAWMIDI_PREFER_SUBDEVICE),

	{ 0 },
};
