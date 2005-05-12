/*
 *   32bit -> 64bit ioctl wrapper for sequencer API
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
#include <sound/timer.h>
#include <asm/uaccess.h>
#include <sound/asequencer.h>
#include "ioctl32.h"

struct sndrv_seq_port_info32 {
	struct sndrv_seq_addr addr;	/* client/port numbers */
	char name[64];			/* port name */

	u32 capability;	/* port capability bits */
	u32 type;		/* port type bits */
	s32 midi_channels;		/* channels per MIDI port */
	s32 midi_voices;		/* voices per MIDI port */
	s32 synth_voices;		/* voices per SYNTH port */

	s32 read_use;			/* R/O: subscribers for output (from this port) */
	s32 write_use;			/* R/O: subscribers for input (to this port) */

	u32 kernel;			/* reserved for kernel use (must be NULL) */
	u32 flags;		/* misc. conditioning */
	unsigned char time_queue;	/* queue # for timestamping */
	char reserved[59];		/* for future use */
};

#define CVT_sndrv_seq_port_info()\
{\
	COPY(addr);\
	COPY_ARRAY(name);\
	COPY(capability);\
	COPY(type);\
	COPY(midi_channels);\
	COPY(midi_voices);\
	COPY(synth_voices);\
	COPY(read_use);\
	COPY(write_use);\
	COPY(flags);\
	COPY(time_queue);\
}

DEFINE_ALSA_IOCTL(seq_port_info);
DEFINE_ALSA_IOCTL_ENTRY(create_port, seq_port_info, SNDRV_SEQ_IOCTL_CREATE_PORT);
DEFINE_ALSA_IOCTL_ENTRY(delete_port, seq_port_info, SNDRV_SEQ_IOCTL_DELETE_PORT);
DEFINE_ALSA_IOCTL_ENTRY(get_port_info, seq_port_info, SNDRV_SEQ_IOCTL_GET_PORT_INFO);
DEFINE_ALSA_IOCTL_ENTRY(set_port_info, seq_port_info, SNDRV_SEQ_IOCTL_SET_PORT_INFO);
DEFINE_ALSA_IOCTL_ENTRY(query_next_port, seq_port_info, SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT);

/*
 */
#define AP(x) snd_ioctl32_##x

enum {
  SNDRV_SEQ_IOCTL_CREATE_PORT32 = _IOWR('S', 0x20, struct sndrv_seq_port_info32),
  SNDRV_SEQ_IOCTL_DELETE_PORT32 = _IOW ('S', 0x21, struct sndrv_seq_port_info32),
  SNDRV_SEQ_IOCTL_GET_PORT_INFO32 = _IOWR('S', 0x22, struct sndrv_seq_port_info32),
  SNDRV_SEQ_IOCTL_SET_PORT_INFO32 = _IOW ('S', 0x23, struct sndrv_seq_port_info32),
  SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT32 = _IOWR('S', 0x52, struct sndrv_seq_port_info32),
};

struct ioctl32_mapper seq_mappers[] = {
	MAP_COMPAT(SNDRV_SEQ_IOCTL_PVERSION),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_CLIENT_ID),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_SYSTEM_INFO),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_GET_CLIENT_INFO),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_SET_CLIENT_INFO),
	{ SNDRV_SEQ_IOCTL_CREATE_PORT32, AP(create_port) },
	{ SNDRV_SEQ_IOCTL_DELETE_PORT32, AP(delete_port) },
	{ SNDRV_SEQ_IOCTL_GET_PORT_INFO32, AP(get_port_info) },
	{ SNDRV_SEQ_IOCTL_SET_PORT_INFO32, AP(set_port_info) },
	MAP_COMPAT(SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_UNSUBSCRIBE_PORT),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_CREATE_QUEUE),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_DELETE_QUEUE),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_GET_QUEUE_INFO),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_SET_QUEUE_INFO),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_GET_NAMED_QUEUE),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_GET_QUEUE_STATUS),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_GET_QUEUE_TEMPO),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_SET_QUEUE_TEMPO),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_GET_QUEUE_TIMER),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_SET_QUEUE_TIMER),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_GET_QUEUE_CLIENT),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_SET_QUEUE_CLIENT),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_GET_CLIENT_POOL),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_SET_CLIENT_POOL),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_REMOVE_EVENTS),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_QUERY_SUBS),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_GET_SUBSCRIPTION),
	MAP_COMPAT(SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT),
	{ SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT32, AP(query_next_port) },
	MAP_COMPAT(SNDRV_SEQ_IOCTL_RUNNING_MODE),
	{ 0 },
};
