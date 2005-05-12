/*
 *   32bit -> 64bit ioctl helpers
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
 *
 * This file registers the converters from 32-bit ioctls to 64-bit ones.
 * The converter assumes that a 32-bit user-pointer can be casted by compat_ptr(x)
 * macro to a valid 64-bit pointer which is accessible via copy_from/to_user.
 *
 */

#ifndef __ALSA_IOCTL32_H
#define __ALSA_IOCTL32_H

#include <linux/compat.h>

#define COPY(x) \
	do { \
		if (copy_in_user(&dst->x, &src->x, sizeof(dst->x))) \
			return -EFAULT; \
	} while (0)

#define COPY_ARRAY(x) \
	do { \
		if (copy_in_user(dst->x, src->x, sizeof(dst->x))) \
			return -EFAULT; \
	} while (0)

#define COPY_CVT(x) \
	do { \
		__typeof__(src->x) __val_tmp; \
		if (get_user(__val_tmp, &src->x) || \
		    put_user(__val_tmp, &dst->x))\
			return -EFAULT; \
	} while (0)

#define convert_from_32(type, dstp, srcp)\
{\
	struct sndrv_##type __user *dst = dstp;\
	struct sndrv_##type##32 __user *src = srcp;\
	CVT_##sndrv_##type();\
}

#define convert_to_32(type, dstp, srcp)\
{\
	struct sndrv_##type __user *src = srcp;\
	struct sndrv_##type##32 __user *dst = dstp;\
	CVT_##sndrv_##type();\
}


#define DEFINE_ALSA_IOCTL(type) \
static inline int _snd_ioctl32_##type(unsigned int fd, unsigned int cmd, unsigned long arg, struct file *file, unsigned int native_ctl)\
{\
	struct sndrv_##type##32 __user *data32;\
	struct sndrv_##type __user *data;\
	int err;\
	data32 = compat_ptr(arg);\
	data = compat_alloc_user_space(sizeof(*data));\
	convert_from_32(type, data, data32);\
	err = file->f_op->ioctl(file->f_dentry->d_inode, file, native_ctl, (unsigned long)data);\
	if (err < 0) \
		return err;\
	if (native_ctl & (_IOC_READ << _IOC_DIRSHIFT)) {\
		convert_to_32(type, data32, data);\
	}\
	return 0;\
}

#define DEFINE_ALSA_IOCTL_ENTRY(name,type,native_ctl) \
static int snd_ioctl32_##name(unsigned int fd, unsigned int cmd, unsigned long arg, struct file *file) {\
	return _snd_ioctl32_##type(fd, cmd, arg, file, native_ctl);\
}

#define MAP_COMPAT(ctl) { ctl, snd_ioctl32_compat }

struct ioctl32_mapper {
	unsigned int cmd;
	int (*handler)(unsigned int, unsigned int, unsigned long, struct file * filp);
	int registered;
};

int snd_ioctl32_compat(unsigned int, unsigned int, unsigned long, struct file *);

int snd_ioctl32_register(struct ioctl32_mapper *mappers);
void snd_ioctl32_unregister(struct ioctl32_mapper *mappers);

#endif /* __ALSA_IOCTL32_H */
