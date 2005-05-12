/*
 *   32bit -> 64bit ioctl wrapper for PCM API
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
#include <linux/slab.h>
#include <linux/compat.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/minors.h>
#include "ioctl32.h"


/* wrapper for sndrv_pcm_[us]frames */
struct sndrv_pcm_sframes_str {
	sndrv_pcm_sframes_t val;
};
struct sndrv_pcm_sframes_str32 {
	s32 val;
};
struct sndrv_pcm_uframes_str {
	sndrv_pcm_uframes_t val;
};
struct sndrv_pcm_uframes_str32 {
	u32 val;
};

#define CVT_sndrv_pcm_sframes_str() { COPY_CVT(val); }
#define CVT_sndrv_pcm_uframes_str() { COPY_CVT(val); }


struct sndrv_pcm_hw_params32 {
	u32 flags;
	struct sndrv_mask masks[SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK + 1]; /* this must be identical */
	struct sndrv_mask mres[5];	/* reserved masks */
	struct sndrv_interval intervals[SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1];
	struct sndrv_interval ires[9];	/* reserved intervals */
	u32 rmask;
	u32 cmask;
	u32 info;
	u32 msbits;
	u32 rate_num;
	u32 rate_den;
	u32 fifo_size;
	unsigned char reserved[64];
} __attribute__((packed));

struct sndrv_pcm_sw_params32 {
	s32 tstamp_mode;
	u32 period_step;
	u32 sleep_min;
	u32 avail_min;
	u32 xfer_align;
	u32 start_threshold;
	u32 stop_threshold;
	u32 silence_threshold;
	u32 silence_size;
	u32 boundary;
	unsigned char reserved[64];
} __attribute__((packed));

#define CVT_sndrv_pcm_sw_params()\
{\
	COPY(tstamp_mode);\
	COPY(period_step);\
	COPY(sleep_min);\
	COPY_CVT(avail_min);\
	COPY_CVT(xfer_align);\
	COPY_CVT(start_threshold);\
	COPY_CVT(stop_threshold);\
	COPY_CVT(silence_threshold);\
	COPY_CVT(silence_size);\
	COPY_CVT(boundary);\
}

struct sndrv_pcm_channel_info32 {
	u32 channel;
	u32 offset;
	u32 first;
	u32 step;
} __attribute__((packed));

#define CVT_sndrv_pcm_channel_info()\
{\
	COPY(channel);\
	COPY_CVT(offset);\
	COPY(first);\
	COPY(step);\
}

struct sndrv_pcm_status32 {
	s32 state;
	struct compat_timespec trigger_tstamp;
	struct compat_timespec tstamp;
	u32 appl_ptr;
	u32 hw_ptr;
	s32 delay;
	u32 avail;
	u32 avail_max;
	u32 overrange;
	s32 suspended_state;
	unsigned char reserved[60];
} __attribute__((packed));

#define CVT_sndrv_pcm_status()\
{\
	COPY(state);\
	COPY_CVT(trigger_tstamp.tv_sec);\
	COPY_CVT(trigger_tstamp.tv_nsec);\
	COPY_CVT(tstamp.tv_sec);\
	COPY_CVT(tstamp.tv_nsec);\
	COPY_CVT(appl_ptr);\
	COPY_CVT(hw_ptr);\
	COPY_CVT(delay);\
	COPY_CVT(avail);\
	COPY_CVT(avail_max);\
	COPY_CVT(overrange);\
	COPY(suspended_state);\
}

DEFINE_ALSA_IOCTL(pcm_uframes_str);
DEFINE_ALSA_IOCTL(pcm_sframes_str);
DEFINE_ALSA_IOCTL(pcm_sw_params);
DEFINE_ALSA_IOCTL(pcm_channel_info);
DEFINE_ALSA_IOCTL(pcm_status);

/* sanity device check */
extern int snd_major;
static int sanity_check_pcm(struct file *file)
{
	unsigned short minor;
	if (imajor(file->f_dentry->d_inode) != snd_major)
		return -ENOTTY;
	minor = iminor(file->f_dentry->d_inode);
	if (minor >= 256 || 
	    minor % SNDRV_MINOR_DEVICES < SNDRV_MINOR_PCM_PLAYBACK)
		return -ENOTTY;
	return 0;
}

/* recalcuate the boundary within 32bit */
static void recalculate_boundary(snd_pcm_runtime_t *runtime)
{
	if (! runtime->buffer_size)
		return;
	runtime->boundary = runtime->buffer_size;
	while (runtime->boundary * 2 <= 0x7fffffffUL - runtime->buffer_size)
		runtime->boundary *= 2;
}

/* both for HW_PARAMS and HW_REFINE */
static int _snd_ioctl32_pcm_hw_params(unsigned int fd, unsigned int cmd, unsigned long arg, struct file *file, unsigned int native_ctl)
{
	struct sndrv_pcm_hw_params32 __user *data32;
	struct sndrv_pcm_hw_params *data;
	snd_pcm_file_t *pcm_file;
	snd_pcm_substream_t *substream;
	snd_pcm_runtime_t *runtime;
	int err;

	if (sanity_check_pcm(file))
		return -ENOTTY;
	if (! (pcm_file = file->private_data))
		return -ENOTTY;
	if (! (substream = pcm_file->substream))
		return -ENOTTY;
	if (! (runtime = substream->runtime))
		return -ENOTTY;

	data32 = compat_ptr(arg);
	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;
	if (copy_from_user(data, data32, sizeof(*data32))) {
		err = -EFAULT;
		goto error;
	}
	if (native_ctl == SNDRV_PCM_IOCTL_HW_REFINE)
		err = snd_pcm_hw_refine(substream, data);
	else
		err = snd_pcm_hw_params(substream, data);
	if (err < 0)
		goto error;
	if (copy_to_user(data32, data, sizeof(*data32)) ||
	    __put_user((u32)data->fifo_size, &data32->fifo_size)) {
		err = -EFAULT;
		goto error;
	}

	if (native_ctl == SNDRV_PCM_IOCTL_HW_PARAMS)
		recalculate_boundary(runtime);
 error:
	kfree(data);
	return err;
}


/*
 */
struct sndrv_xferi32 {
	s32 result;
	u32 buf;
	u32 frames;
} __attribute__((packed));

static int _snd_ioctl32_xferi(unsigned int fd, unsigned int cmd, unsigned long arg, struct file *file, unsigned int native_ctl)
{
	struct sndrv_xferi32 data32;
	struct sndrv_xferi __user *data;
	snd_pcm_sframes_t result;
	int err;

	if (copy_from_user(&data32, (void __user *)arg, sizeof(data32)))
		return -EFAULT;
	data = compat_alloc_user_space(sizeof(*data));
	if (put_user((snd_pcm_sframes_t)data32.result, &data->result) ||
	    __put_user(compat_ptr(data32.buf), &data->buf) ||
	    __put_user((snd_pcm_uframes_t)data32.frames, &data->frames))
		return -EFAULT;
	err = file->f_op->ioctl(file->f_dentry->d_inode, file, native_ctl, (unsigned long)data);
	if (err < 0)
		return err;
	/* copy the result */
	if (__get_user(result, &data->result))
		return -EFAULT;
	data32.result = result;
	if (copy_to_user((void __user *)arg, &data32, sizeof(data32)))
		return -EFAULT;
	return 0;
}


/* snd_xfern needs remapping of bufs */
struct sndrv_xfern32 {
	s32 result;
	u32 bufs;  /* this is void **; */
	u32 frames;
} __attribute__((packed));

/*
 * xfern ioctl nees to copy (up to) 128 pointers on stack.
 * although we may pass the copied pointers through f_op->ioctl, but the ioctl
 * handler there expands again the same 128 pointers on stack, so it is better
 * to handle the function (calling pcm_readv/writev) directly in this handler.
 */
static int _snd_ioctl32_xfern(unsigned int fd, unsigned int cmd, unsigned long arg, struct file *file, unsigned int native_ctl)
{
	snd_pcm_file_t *pcm_file;
	snd_pcm_substream_t *substream;
	struct sndrv_xfern32 __user *srcptr = compat_ptr(arg);
	struct sndrv_xfern32 data32;
	void __user **bufs;
	int err = 0, ch, i;
	u32 __user *bufptr;

	if (sanity_check_pcm(file))
		return -ENOTTY;
	if (! (pcm_file = file->private_data))
		return -ENOTTY;
	if (! (substream = pcm_file->substream))
		return -ENOTTY;
	if (! substream->runtime)
		return -ENOTTY;

	/* check validty of the command */
	switch (native_ctl) {
	case SNDRV_PCM_IOCTL_WRITEN_FRAMES:
		if (substream->stream  != SNDRV_PCM_STREAM_PLAYBACK)
			return -EINVAL;
		if (substream->runtime->status->state == SNDRV_PCM_STATE_OPEN)
			return -EBADFD;
		break;
	case SNDRV_PCM_IOCTL_READN_FRAMES:
		if (substream->stream  != SNDRV_PCM_STREAM_CAPTURE)
			return -EINVAL;
		break;
	}
	if ((ch = substream->runtime->channels) > 128)
		return -EINVAL;
	if (copy_from_user(&data32, (void __user *)arg, sizeof(data32)))
		return -EFAULT;
	bufptr = compat_ptr(data32.bufs);
	bufs = kmalloc(sizeof(void __user *) * ch, GFP_KERNEL);
	if (bufs == NULL)
		return -ENOMEM;
	for (i = 0; i < ch; i++) {
		u32 ptr;
		if (get_user(ptr, bufptr)) {
			kfree(bufs);
			return -EFAULT;
		}
		bufs[ch] = compat_ptr(ptr);
		bufptr++;
	}
	switch (native_ctl) {
	case SNDRV_PCM_IOCTL_WRITEN_FRAMES:
		err = snd_pcm_lib_writev(substream, bufs, data32.frames);
		break;
	case SNDRV_PCM_IOCTL_READN_FRAMES:
		err = snd_pcm_lib_readv(substream, bufs, data32.frames);
		break;
	}
	if (err >= 0) {
		if (put_user(err, &srcptr->result))
			err = -EFAULT;
	}
	kfree(bufs);
	return err;
}


struct sndrv_pcm_mmap_status32 {
	s32 state;
	s32 pad1;
	u32 hw_ptr;
	struct compat_timespec tstamp;
	s32 suspended_state;
} __attribute__((packed));

struct sndrv_pcm_mmap_control32 {
	u32 appl_ptr;
	u32 avail_min;
} __attribute__((packed));

struct sndrv_pcm_sync_ptr32 {
	u32 flags;
	union {
		struct sndrv_pcm_mmap_status32 status;
		unsigned char reserved[64];
	} s;
	union {
		struct sndrv_pcm_mmap_control32 control;
		unsigned char reserved[64];
	} c;
} __attribute__((packed));

#define CVT_sndrv_pcm_sync_ptr()\
{\
	COPY(flags);\
	COPY(s.status.state);\
	COPY(s.status.pad1);\
	COPY_CVT(s.status.hw_ptr);\
	COPY_CVT(s.status.tstamp.tv_sec);\
	COPY_CVT(s.status.tstamp.tv_nsec);\
	COPY(s.status.suspended_state);\
	COPY_CVT(c.control.appl_ptr);\
	COPY_CVT(c.control.avail_min);\
}

DEFINE_ALSA_IOCTL(pcm_sync_ptr);

/*
 */

DEFINE_ALSA_IOCTL_ENTRY(pcm_hw_refine, pcm_hw_params, SNDRV_PCM_IOCTL_HW_REFINE);
DEFINE_ALSA_IOCTL_ENTRY(pcm_hw_params, pcm_hw_params, SNDRV_PCM_IOCTL_HW_PARAMS);
DEFINE_ALSA_IOCTL_ENTRY(pcm_sw_params, pcm_sw_params, SNDRV_PCM_IOCTL_SW_PARAMS);
DEFINE_ALSA_IOCTL_ENTRY(pcm_status, pcm_status, SNDRV_PCM_IOCTL_STATUS);
DEFINE_ALSA_IOCTL_ENTRY(pcm_delay, pcm_sframes_str, SNDRV_PCM_IOCTL_DELAY);
DEFINE_ALSA_IOCTL_ENTRY(pcm_channel_info, pcm_channel_info, SNDRV_PCM_IOCTL_CHANNEL_INFO);
DEFINE_ALSA_IOCTL_ENTRY(pcm_rewind, pcm_uframes_str, SNDRV_PCM_IOCTL_REWIND);
DEFINE_ALSA_IOCTL_ENTRY(pcm_forward, pcm_uframes_str, SNDRV_PCM_IOCTL_FORWARD);
DEFINE_ALSA_IOCTL_ENTRY(pcm_readi, xferi, SNDRV_PCM_IOCTL_READI_FRAMES);
DEFINE_ALSA_IOCTL_ENTRY(pcm_writei, xferi, SNDRV_PCM_IOCTL_WRITEI_FRAMES);
DEFINE_ALSA_IOCTL_ENTRY(pcm_readn, xfern, SNDRV_PCM_IOCTL_READN_FRAMES);
DEFINE_ALSA_IOCTL_ENTRY(pcm_writen, xfern, SNDRV_PCM_IOCTL_WRITEN_FRAMES);
DEFINE_ALSA_IOCTL_ENTRY(pcm_sync_ptr, pcm_sync_ptr, SNDRV_PCM_IOCTL_SYNC_PTR);


/*
 * When PCM is used on 32bit mode, we need to disable
 * mmap of PCM status/control records because of the size
 * incompatibility.
 * 
 * Since INFO ioctl is always called at first, we mark the
 * mmap-disabling in this ioctl wrapper.
 */
static int snd_pcm_info_ioctl32(unsigned int fd, unsigned int cmd, unsigned long arg, struct file *filp)
{
	snd_pcm_file_t *pcm_file;
	snd_pcm_substream_t *substream;
	if (! filp->f_op || ! filp->f_op->ioctl)
		return -ENOTTY;
	pcm_file = filp->private_data;
	if (! pcm_file)
		return -ENOTTY;
	substream = pcm_file->substream;
	if (! substream)
		return -ENOTTY;
	substream->no_mmap_ctrl = 1;
	return filp->f_op->ioctl(filp->f_dentry->d_inode, filp, cmd, arg);
}

/*
 */
#define AP(x) snd_ioctl32_##x

enum {
	SNDRV_PCM_IOCTL_HW_REFINE32 = _IOWR('A', 0x10, struct sndrv_pcm_hw_params32),
	SNDRV_PCM_IOCTL_HW_PARAMS32 = _IOWR('A', 0x11, struct sndrv_pcm_hw_params32),
	SNDRV_PCM_IOCTL_SW_PARAMS32 = _IOWR('A', 0x13, struct sndrv_pcm_sw_params32),
	SNDRV_PCM_IOCTL_STATUS32 = _IOR('A', 0x20, struct sndrv_pcm_status32),
	SNDRV_PCM_IOCTL_DELAY32 = _IOR('A', 0x21, s32),
	SNDRV_PCM_IOCTL_CHANNEL_INFO32 = _IOR('A', 0x32, struct sndrv_pcm_channel_info32),
	SNDRV_PCM_IOCTL_REWIND32 = _IOW('A', 0x46, u32),
	SNDRV_PCM_IOCTL_FORWARD32 = _IOW('A', 0x49, u32),
	SNDRV_PCM_IOCTL_WRITEI_FRAMES32 = _IOW('A', 0x50, struct sndrv_xferi32),
	SNDRV_PCM_IOCTL_READI_FRAMES32 = _IOR('A', 0x51, struct sndrv_xferi32),
	SNDRV_PCM_IOCTL_WRITEN_FRAMES32 = _IOW('A', 0x52, struct sndrv_xfern32),
	SNDRV_PCM_IOCTL_READN_FRAMES32 = _IOR('A', 0x53, struct sndrv_xfern32),
	SNDRV_PCM_IOCTL_SYNC_PTR32 = _IOWR('A', 0x23, struct sndrv_pcm_sync_ptr32),

};

struct ioctl32_mapper pcm_mappers[] = {
	MAP_COMPAT(SNDRV_PCM_IOCTL_PVERSION),
	/* MAP_COMPAT(SNDRV_PCM_IOCTL_INFO), */
	{ SNDRV_PCM_IOCTL_INFO, snd_pcm_info_ioctl32 },
	MAP_COMPAT(SNDRV_PCM_IOCTL_TSTAMP),
	{ SNDRV_PCM_IOCTL_HW_REFINE32, AP(pcm_hw_refine) },
	{ SNDRV_PCM_IOCTL_HW_PARAMS32, AP(pcm_hw_params) },
	MAP_COMPAT(SNDRV_PCM_IOCTL_HW_FREE),
	{ SNDRV_PCM_IOCTL_SW_PARAMS32, AP(pcm_sw_params) },
	{ SNDRV_PCM_IOCTL_STATUS32, AP(pcm_status) },
	{ SNDRV_PCM_IOCTL_DELAY32, AP(pcm_delay) },
	MAP_COMPAT(SNDRV_PCM_IOCTL_HWSYNC),
	{ SNDRV_PCM_IOCTL_SYNC_PTR32, AP(pcm_sync_ptr) },
	{ SNDRV_PCM_IOCTL_CHANNEL_INFO32, AP(pcm_channel_info) },
	MAP_COMPAT(SNDRV_PCM_IOCTL_PREPARE),
	MAP_COMPAT(SNDRV_PCM_IOCTL_RESET),
	MAP_COMPAT(SNDRV_PCM_IOCTL_START),
	MAP_COMPAT(SNDRV_PCM_IOCTL_DROP),
	MAP_COMPAT(SNDRV_PCM_IOCTL_DRAIN),
	MAP_COMPAT(SNDRV_PCM_IOCTL_PAUSE),
	{ SNDRV_PCM_IOCTL_REWIND32, AP(pcm_rewind) },
	MAP_COMPAT(SNDRV_PCM_IOCTL_RESUME),
	MAP_COMPAT(SNDRV_PCM_IOCTL_XRUN),
	{ SNDRV_PCM_IOCTL_FORWARD32, AP(pcm_forward) },
	{ SNDRV_PCM_IOCTL_WRITEI_FRAMES32, AP(pcm_writei) },
	{ SNDRV_PCM_IOCTL_READI_FRAMES32, AP(pcm_readi) },
	{ SNDRV_PCM_IOCTL_WRITEN_FRAMES32, AP(pcm_writen) },
	{ SNDRV_PCM_IOCTL_READN_FRAMES32, AP(pcm_readn) },
	MAP_COMPAT(SNDRV_PCM_IOCTL_LINK),
	MAP_COMPAT(SNDRV_PCM_IOCTL_UNLINK),

	{ 0 },
};
