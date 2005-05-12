/*
 *   32bit -> 64bit ioctl wrapper for control API
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
#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/minors.h>
#include <asm/uaccess.h>
#include "ioctl32.h"


/*
 * register/unregister mappers
 * exported for other modules
 */

MODULE_AUTHOR("Takashi Iwai <tiwai@suse.de>");
MODULE_DESCRIPTION("ioctl32 wrapper for ALSA");
MODULE_LICENSE("GPL");

int register_ioctl32_conversion(unsigned int cmd, int (*handler)(unsigned int, unsigned int, unsigned long, struct file *));
int unregister_ioctl32_conversion(unsigned int cmd);


int snd_ioctl32_register(struct ioctl32_mapper *mappers)
{
	int err;
	struct ioctl32_mapper *m;

	for (m = mappers; m->cmd; m++) {
		err = register_ioctl32_conversion(m->cmd, m->handler);
		if (err >= 0)
			m->registered++;
	}
	return 0;
}

void snd_ioctl32_unregister(struct ioctl32_mapper *mappers)
{
	struct ioctl32_mapper *m;

	for (m = mappers; m->cmd; m++) {
		if (m->registered) {
			unregister_ioctl32_conversion(m->cmd);
			m->registered = 0;
		}
	}
}


/*
 * compatible wrapper
 */
int snd_ioctl32_compat(unsigned int fd, unsigned int cmd, unsigned long arg, struct file *filp)
{
	if (! filp->f_op || ! filp->f_op->ioctl)
		return -ENOTTY;
	return filp->f_op->ioctl(filp->f_dentry->d_inode, filp, cmd, arg);
}


/*
 * Controls
 */

struct sndrv_ctl_elem_list32 {
	u32 offset;
	u32 space;
	u32 used;
	u32 count;
	u32 pids;
	unsigned char reserved[50];
} /* don't set packed attribute here */;

static inline int _snd_ioctl32_ctl_elem_list(unsigned int fd, unsigned int cmd, unsigned long arg, struct file *file, unsigned int native_ctl)
{
	struct sndrv_ctl_elem_list32 __user *data32;
	struct sndrv_ctl_elem_list __user *data;
	compat_caddr_t ptr;
	int err;

	data32 = compat_ptr(arg);
	data = compat_alloc_user_space(sizeof(*data));

	/* offset, space, used, count */
	if (copy_in_user(data, data32, 4 * sizeof(u32)))
		return -EFAULT;
	/* pids */
	if (__get_user(ptr, &data32->pids) ||
	    __put_user(compat_ptr(ptr), &data->pids))
		return -EFAULT;
	err = file->f_op->ioctl(file->f_dentry->d_inode, file, native_ctl, (unsigned long)data);
	if (err < 0)
		return err;
	/* copy the result */
	if (copy_in_user(data32, data, 4 * sizeof(u32)))
		return -EFAULT;
	return 0;
}

DEFINE_ALSA_IOCTL_ENTRY(ctl_elem_list, ctl_elem_list, SNDRV_CTL_IOCTL_ELEM_LIST);

/*
 * control element info
 * it uses union, so the things are not easy..
 */

struct sndrv_ctl_elem_info32 {
	struct sndrv_ctl_elem_id id; // the size of struct is same
	s32 type;
	u32 access;
	u32 count;
	s32 owner;
	union {
		struct {
			s32 min;
			s32 max;
			s32 step;
		} integer;
		struct {
			u64 min;
			u64 max;
			u64 step;
		} integer64;
		struct {
			u32 items;
			u32 item;
			char name[64];
		} enumerated;
		unsigned char reserved[128];
	} value;
	unsigned char reserved[64];
} __attribute__((packed));

static inline int _snd_ioctl32_ctl_elem_info(unsigned int fd, unsigned int cmd, unsigned long arg, struct file *file, unsigned int native_ctl)
{
	struct sndrv_ctl_elem_info __user *data, *src;
	struct sndrv_ctl_elem_info32 __user *data32, *dst;
	unsigned int type;
	int err;

	data32 = compat_ptr(arg);
	data = compat_alloc_user_space(sizeof(*data));

	/* copy id */
	if (copy_in_user(&data->id, &data32->id, sizeof(data->id)))
		return -EFAULT;
	/* we need to copy the item index.
	 * hope this doesn't break anything..
	 */
	if (copy_in_user(&data->value.enumerated.item,
			 &data32->value.enumerated.item,
			 sizeof(data->value.enumerated.item)))
		return -EFAULT;
	err = file->f_op->ioctl(file->f_dentry->d_inode, file, native_ctl, (unsigned long)data);
	if (err < 0)
		return err;
	/* restore info to 32bit */
	/* for COPY_CVT macro */
	src = data;
	dst = data32;
	/* id, type, access, count */
	if (copy_in_user(&data32->id, &data->id, sizeof(data->id)) ||
	    copy_in_user(&data32->type, &data->type, 3 * sizeof(u32)))
		return -EFAULT;
	COPY_CVT(owner);
	__get_user(type, &data->type);
	switch (type) {
	case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
	case SNDRV_CTL_ELEM_TYPE_INTEGER:
		COPY_CVT(value.integer.min);
		COPY_CVT(value.integer.max);
		COPY_CVT(value.integer.step);
		break;
	case SNDRV_CTL_ELEM_TYPE_INTEGER64:
		if (copy_in_user(&data32->value.integer64,
				 &data->value.integer64,
				 sizeof(data->value.integer64)))
			return -EFAULT;
		break;
	case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
		if (copy_in_user(&data32->value.enumerated,
				 &data->value.enumerated,
				 sizeof(data->value.enumerated)))
			return -EFAULT;
		break;
	default:
		break;
	}
	return 0;
}

DEFINE_ALSA_IOCTL_ENTRY(ctl_elem_info, ctl_elem_info, SNDRV_CTL_IOCTL_ELEM_INFO);

struct sndrv_ctl_elem_value32 {
	struct sndrv_ctl_elem_id id;
	unsigned int indirect;	/* bit-field causes misalignment */
        union {
		s32 integer[128];	/* integer and boolean need conversion */
#ifndef CONFIG_X86_64
		s64 integer64[64];	/* for alignment */
#endif
		unsigned char data[512];	/* others should be compatible */
        } value;
        unsigned char reserved[128];	/* not used */
};


/* hmm, it's so hard to retrieve the value type from the control id.. */
static int get_ctl_type(snd_card_t *card, snd_ctl_elem_id_t *id)
{
	snd_kcontrol_t *kctl;
	snd_ctl_elem_info_t info;
	int err;

	down_read(&card->controls_rwsem);
	kctl = snd_ctl_find_id(card, id);
	if (! kctl) {
		up_read(&card->controls_rwsem);
		return -ENXIO;
	}
	info.id = *id;
	err = kctl->info(kctl, &info);
	up_read(&card->controls_rwsem);
	if (err >= 0)
		err = info.type;
	return err;
}

extern int snd_major;

static inline int _snd_ioctl32_ctl_elem_value(unsigned int fd, unsigned int cmd, unsigned long arg, struct file *file, unsigned int native_ctl)
{
	struct sndrv_ctl_elem_value *data;
	struct sndrv_ctl_elem_value32 __user *data32;
	snd_ctl_file_t *ctl;
	int err, i, indirect;
	int type;

	/* sanity check */
	if (imajor(file->f_dentry->d_inode) != snd_major ||
	    SNDRV_MINOR_DEVICE(iminor(file->f_dentry->d_inode)) != SNDRV_MINOR_CONTROL)
		return -ENOTTY;

	if ((ctl = file->private_data) == NULL)
		return -ENOTTY;

	data32 = compat_ptr(arg);
	data = kcalloc(1, sizeof(*data), GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;

	if (copy_from_user(&data->id, &data32->id, sizeof(data->id))) {
		err = -EFAULT;
		goto __end;
	}
	if (__get_user(indirect, &data32->indirect)) {
		err = -EFAULT;
		goto __end;
	}
	/* FIXME: indirect access is not supported */
	if (indirect) {
		err = -EINVAL;
		goto __end;
	}
	type = get_ctl_type(ctl->card, &data->id);
	if (type < 0) {
		err = type;
		goto __end;
	}

	switch (type) {
	case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
	case SNDRV_CTL_ELEM_TYPE_INTEGER:
		for (i = 0; i < 128; i++) {
			int val;
			if (__get_user(val, &data32->value.integer[i])) {
				err = -EFAULT;
				goto __end;
			}
			data->value.integer.value[i] = val;
		}
		break;
	case SNDRV_CTL_ELEM_TYPE_INTEGER64:
	case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
	case SNDRV_CTL_ELEM_TYPE_BYTES:
	case SNDRV_CTL_ELEM_TYPE_IEC958:
		if (__copy_from_user(data->value.bytes.data,
				     data32->value.data,
				     sizeof(data32->value.data))) {
			err = -EFAULT;
			goto __end;
		}
		break;
	default:
		printk(KERN_ERR "snd_ioctl32_ctl_elem_value: unknown type %d\n", type);
		err = -EINVAL;
		goto __end;
	}

	if (native_ctl == SNDRV_CTL_IOCTL_ELEM_READ)
		err = snd_ctl_elem_read(ctl->card, data);
	else
		err = snd_ctl_elem_write(ctl->card, ctl, data);
	if (err < 0)
		goto __end;
	/* restore info to 32bit */
	switch (type) {
	case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
	case SNDRV_CTL_ELEM_TYPE_INTEGER:
		for (i = 0; i < 128; i++) {
			int val;
			val = data->value.integer.value[i];
			if (__put_user(val, &data32->value.integer[i])) {
				err = -EFAULT;
				goto __end;
			}
		}
		break;
	default:
		if (__copy_to_user(data32->value.data,
				   data->value.bytes.data,
				   sizeof(data32->value.data))) {
			err = -EFAULT;
			goto __end;
		}
		break;
		break;
	}
	err = 0;
      __end:
	kfree(data);
	return err;
}

DEFINE_ALSA_IOCTL_ENTRY(ctl_elem_read, ctl_elem_value, SNDRV_CTL_IOCTL_ELEM_READ);
DEFINE_ALSA_IOCTL_ENTRY(ctl_elem_write, ctl_elem_value, SNDRV_CTL_IOCTL_ELEM_WRITE);

/*
 */

#define AP(x) snd_ioctl32_##x

enum {
	SNDRV_CTL_IOCTL_ELEM_LIST32 = _IOWR('U', 0x10, struct sndrv_ctl_elem_list32),
	SNDRV_CTL_IOCTL_ELEM_INFO32 = _IOWR('U', 0x11, struct sndrv_ctl_elem_info32),
	SNDRV_CTL_IOCTL_ELEM_READ32 = _IOWR('U', 0x12, struct sndrv_ctl_elem_value32),
	SNDRV_CTL_IOCTL_ELEM_WRITE32 = _IOWR('U', 0x13, struct sndrv_ctl_elem_value32),
};

static struct ioctl32_mapper control_mappers[] = {
	/* controls (without rawmidi, hwdep, timer releated ones) */
	MAP_COMPAT(SNDRV_CTL_IOCTL_PVERSION),
	MAP_COMPAT(SNDRV_CTL_IOCTL_CARD_INFO),
	{ SNDRV_CTL_IOCTL_ELEM_LIST32, AP(ctl_elem_list) },
	{ SNDRV_CTL_IOCTL_ELEM_INFO32, AP(ctl_elem_info) },
	{ SNDRV_CTL_IOCTL_ELEM_READ32, AP(ctl_elem_read) },
	{ SNDRV_CTL_IOCTL_ELEM_WRITE32, AP(ctl_elem_write) },
	MAP_COMPAT(SNDRV_CTL_IOCTL_ELEM_LOCK),
	MAP_COMPAT(SNDRV_CTL_IOCTL_ELEM_UNLOCK),
	MAP_COMPAT(SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS),
	MAP_COMPAT(SNDRV_CTL_IOCTL_HWDEP_INFO),
	MAP_COMPAT(SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE),
	MAP_COMPAT(SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE),
	MAP_COMPAT(SNDRV_CTL_IOCTL_PCM_INFO),
	MAP_COMPAT(SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE),
	MAP_COMPAT(SNDRV_CTL_IOCTL_POWER),
	MAP_COMPAT(SNDRV_CTL_IOCTL_POWER_STATE),
	{ 0 }
};


/*
 */

extern struct ioctl32_mapper pcm_mappers[];
extern struct ioctl32_mapper rawmidi_mappers[];
extern struct ioctl32_mapper timer_mappers[];
extern struct ioctl32_mapper hwdep_mappers[];
#if defined(CONFIG_SND_SEQUENCER) || (defined(MODULE) && defined(CONFIG_SND_SEQUENCER_MODULE))
extern struct ioctl32_mapper seq_mappers[];
#endif

static void snd_ioctl32_done(void)
{
#if defined(CONFIG_SND_SEQUENCER) || (defined(MODULE) && defined(CONFIG_SND_SEQUENCER_MODULE))
	snd_ioctl32_unregister(seq_mappers);
#endif
	snd_ioctl32_unregister(hwdep_mappers);
	snd_ioctl32_unregister(timer_mappers);
	snd_ioctl32_unregister(rawmidi_mappers);
	snd_ioctl32_unregister(pcm_mappers);
	snd_ioctl32_unregister(control_mappers);
}

static int __init snd_ioctl32_init(void)
{
	snd_ioctl32_register(control_mappers);
	snd_ioctl32_register(pcm_mappers);
	snd_ioctl32_register(rawmidi_mappers);
	snd_ioctl32_register(timer_mappers);
	snd_ioctl32_register(hwdep_mappers);
#if defined(CONFIG_SND_SEQUENCER) || (defined(MODULE) && defined(CONFIG_SND_SEQUENCER_MODULE))
	snd_ioctl32_register(seq_mappers);
#endif
	return 0;
}

module_init(snd_ioctl32_init)
module_exit(snd_ioctl32_done)
