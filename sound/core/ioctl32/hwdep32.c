/*
 *   32bit -> 64bit ioctl wrapper for hwdep API
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
#include <sound/core.h>
#include <sound/hwdep.h>
#include <asm/uaccess.h>
#include "ioctl32.h"

struct sndrv_hwdep_dsp_image32 {
	u32 index;
	unsigned char name[64];
	u32 image;	/* pointer */
	u32 length;
	u32 driver_data;
} /* don't set packed attribute here */;

static inline int _snd_ioctl32_hwdep_dsp_image(unsigned int fd, unsigned int cmd, unsigned long arg, struct file *file, unsigned int native_ctl)
{
	struct sndrv_hwdep_dsp_image __user *data, *dst;
	struct sndrv_hwdep_dsp_image32 __user *data32, *src;
	compat_caddr_t ptr;

	data32 = compat_ptr(arg);
	data = compat_alloc_user_space(sizeof(*data));

	/* index and name */
	if (copy_in_user(data, data32, 4 + 64))
		return -EFAULT;
	if (__get_user(ptr, &data32->image) ||
	    __put_user(compat_ptr(ptr), &data->image))
		return -EFAULT;
	src = data32;
	dst = data;
	COPY_CVT(length);
	COPY_CVT(driver_data);
	return file->f_op->ioctl(file->f_dentry->d_inode, file, native_ctl, (unsigned long)data);
}

DEFINE_ALSA_IOCTL_ENTRY(hwdep_dsp_image, hwdep_dsp_image, SNDRV_HWDEP_IOCTL_DSP_LOAD);

#define AP(x) snd_ioctl32_##x

enum {
	SNDRV_HWDEP_IOCTL_DSP_LOAD32   = _IOW('H', 0x03, struct sndrv_hwdep_dsp_image32)
};

struct ioctl32_mapper hwdep_mappers[] = {
	MAP_COMPAT(SNDRV_HWDEP_IOCTL_PVERSION),
	MAP_COMPAT(SNDRV_HWDEP_IOCTL_INFO),
	MAP_COMPAT(SNDRV_HWDEP_IOCTL_DSP_STATUS),
	{ SNDRV_HWDEP_IOCTL_DSP_LOAD32, AP(hwdep_dsp_image) },
	{ 0 },
};
