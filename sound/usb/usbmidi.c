/*
 * usbmidi.c - ALSA USB MIDI driver
 *
 * Copyright (c) 2002-2004 Clemens Ladisch
 * All rights reserved.
 *
 * Based on the OSS usb-midi driver by NAGANO Daisuke,
 *          NetBSD's umidi driver by Takuya SHIOZAKI,
 *          the "USB Device Class Definition for MIDI Devices" by Roland
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed and/or modified under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sound/driver.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <sound/core.h>
#include <sound/minors.h>
#include <sound/rawmidi.h>
#include "usbaudio.h"

MODULE_AUTHOR("Clemens Ladisch <clemens@ladisch.de>");
MODULE_DESCRIPTION("USB Audio/MIDI helper module");
MODULE_LICENSE("Dual BSD/GPL");


struct usb_ms_header_descriptor {
	__u8  bLength;
	__u8  bDescriptorType;
	__u8  bDescriptorSubtype;
	__u8  bcdMSC[2];
	__le16 wTotalLength;
} __attribute__ ((packed));

struct usb_ms_endpoint_descriptor {
	__u8  bLength;
	__u8  bDescriptorType;
	__u8  bDescriptorSubtype;
	__u8  bNumEmbMIDIJack;
	__u8  baAssocJackID[0];
} __attribute__ ((packed));

typedef struct snd_usb_midi snd_usb_midi_t;
typedef struct snd_usb_midi_endpoint snd_usb_midi_endpoint_t;
typedef struct snd_usb_midi_out_endpoint snd_usb_midi_out_endpoint_t;
typedef struct snd_usb_midi_in_endpoint snd_usb_midi_in_endpoint_t;
typedef struct usbmidi_out_port usbmidi_out_port_t;
typedef struct usbmidi_in_port usbmidi_in_port_t;

struct snd_usb_midi {
	snd_usb_audio_t *chip;
	struct usb_interface *iface;
	const snd_usb_audio_quirk_t *quirk;
	snd_rawmidi_t* rmidi;
	struct list_head list;

	struct snd_usb_midi_endpoint {
		snd_usb_midi_out_endpoint_t *out;
		snd_usb_midi_in_endpoint_t *in;
	} endpoints[MIDI_MAX_ENDPOINTS];
};

struct snd_usb_midi_out_endpoint {
	snd_usb_midi_t* umidi;
	struct urb* urb;
	int max_transfer;		/* size of urb buffer */
	struct tasklet_struct tasklet;

	spinlock_t buffer_lock;

	struct usbmidi_out_port {
		snd_usb_midi_out_endpoint_t* ep;
		snd_rawmidi_substream_t* substream;
		int active;
		uint8_t cable;		/* cable number << 4 */
		uint8_t state;
#define STATE_UNKNOWN	0
#define STATE_1PARAM	1
#define STATE_2PARAM_1	2
#define STATE_2PARAM_2	3
#define STATE_SYSEX_0	4
#define STATE_SYSEX_1	5
#define STATE_SYSEX_2	6
		uint8_t data[2];
	} ports[0x10];
};

struct snd_usb_midi_in_endpoint {
	snd_usb_midi_t* umidi;
	struct urb* urb;
	struct usbmidi_in_port {
		snd_rawmidi_substream_t* substream;
	} ports[0x10];
};

static void snd_usbmidi_do_output(snd_usb_midi_out_endpoint_t* ep);

static const uint8_t snd_usbmidi_cin_length[] = {
	0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1
};

/*
 * Submits the URB, with error handling.
 */
static int snd_usbmidi_submit_urb(struct urb* urb, int flags)
{
	int err = usb_submit_urb(urb, flags);
	if (err < 0 && err != -ENODEV)
		snd_printk(KERN_ERR "usb_submit_urb: %d\n", err);
	return err;
}

/*
 * Error handling for URB completion functions.
 */
static int snd_usbmidi_urb_error(int status)
{
	if (status == -ENOENT)
		return status; /* killed */
	if (status == -EILSEQ ||
	    status == -ECONNRESET ||
	    status == -ETIMEDOUT)
		return -ENODEV; /* device removed/shutdown */
	snd_printk(KERN_ERR "urb status %d\n", status);
	return 0; /* continue */
}

/*
 * Receives a USB MIDI packet.
 */
static void snd_usbmidi_input_packet(snd_usb_midi_in_endpoint_t* ep,
				     uint8_t packet[4])
{
	int cable = packet[0] >> 4;
	usbmidi_in_port_t* port = &ep->ports[cable];

	if (!port->substream) {
		snd_printd("unexpected port %d!\n", cable);
		return;
	}
	if (!port->substream->runtime ||
	    !port->substream->runtime->trigger)
		return;
	snd_rawmidi_receive(port->substream, &packet[1],
			    snd_usbmidi_cin_length[packet[0] & 0x0f]);
}

/*
 * Processes the data read from the device.
 */
static void snd_usbmidi_in_urb_complete(struct urb* urb, struct pt_regs *regs)
{
	snd_usb_midi_in_endpoint_t* ep = urb->context;

	if (urb->status == 0) {
		uint8_t* buffer = (uint8_t*)ep->urb->transfer_buffer;
		int i;

		for (i = 0; i + 4 <= urb->actual_length; i += 4)
			if (buffer[i] != 0)
				snd_usbmidi_input_packet(ep, &buffer[i]);
	} else {
		if (snd_usbmidi_urb_error(urb->status) < 0)
			return;
	}

	if (usb_pipe_needs_resubmit(urb->pipe)) {
		urb->dev = ep->umidi->chip->dev;
		snd_usbmidi_submit_urb(urb, GFP_ATOMIC);
	}
}

/*
 * Converts the data read from a Midiman device to standard USB MIDI packets.
 */
static void snd_usbmidi_in_midiman_complete(struct urb* urb, struct pt_regs *regs)
{
	if (urb->status == 0) {
		uint8_t* buffer = (uint8_t*)urb->transfer_buffer;
		int i;

		for (i = 0; i + 4 <= urb->actual_length; i += 4) {
			if (buffer[i + 3] != 0) {
				/*
				 * snd_usbmidi_input_packet() doesn't check the
				 * contents of the message, so we simply use
				 * some random CIN with the desired length.
				 */
				static const uint8_t cin[4] = {
					0x0, 0xf, 0x2, 0x3
				};
				uint8_t ctl = buffer[i + 3];
				buffer[i + 3] = buffer[i + 2];
				buffer[i + 2] = buffer[i + 1];
				buffer[i + 1] = buffer[i + 0];
				buffer[i + 0] = (ctl & 0xf0) | cin[ctl & 3];
			} else {
				buffer[i + 0] = 0;
			}
		}
	}
	snd_usbmidi_in_urb_complete(urb, regs);
}

static void snd_usbmidi_out_urb_complete(struct urb* urb, struct pt_regs *regs)
{
	snd_usb_midi_out_endpoint_t* ep = urb->context;

	if (urb->status < 0) {
		if (snd_usbmidi_urb_error(urb->status) < 0)
			return;
	}
	snd_usbmidi_do_output(ep);
}

/*
 * Converts standard USB MIDI packets to what Midman devices expect.
 */
static void snd_usbmidi_convert_to_midiman(struct urb* urb)
{
	uint8_t* buffer = (uint8_t*)urb->transfer_buffer;
	int i;

	for (i = 0; i + 4 <= urb->transfer_buffer_length; i += 4) {
		uint8_t cin = buffer[i];
		buffer[i + 0] = buffer[i + 1];
		buffer[i + 1] = buffer[i + 2];
		buffer[i + 2] = buffer[i + 3];
		buffer[i + 3] = (cin & 0xf0) | snd_usbmidi_cin_length[cin & 0x0f];
	}
}

/*
 * Adds one USB MIDI packet to the output buffer.
 */
static inline void output_packet(struct urb* urb,
	       			 uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3)
{

	uint8_t* buf = (uint8_t*)urb->transfer_buffer + urb->transfer_buffer_length;
	buf[0] = p0;
	buf[1] = p1;
	buf[2] = p2;
	buf[3] = p3;
	urb->transfer_buffer_length += 4;
}

/*
 * Converts MIDI commands to USB MIDI packets.
 */
static void snd_usbmidi_transmit_byte(usbmidi_out_port_t* port,
				      uint8_t b, struct urb* urb)
{
	uint8_t p0 = port->cable;

	if (b >= 0xf8) {
		output_packet(urb, p0 | 0x0f, b, 0, 0);
	} else if (b >= 0xf0) {
		switch (b) {
		case 0xf0:
			port->data[0] = b;
			port->state = STATE_SYSEX_1;
			break;
		case 0xf1:
		case 0xf3:
			port->data[0] = b;
			port->state = STATE_1PARAM;
			break;
		case 0xf2:
			port->data[0] = b;
			port->state = STATE_2PARAM_1;
			break;
		case 0xf4:
		case 0xf5:
			port->state = STATE_UNKNOWN;
			break;
		case 0xf6:
			output_packet(urb, p0 | 0x05, 0xf6, 0, 0);
			port->state = STATE_UNKNOWN;
			break;
		case 0xf7:
			switch (port->state) {
			case STATE_SYSEX_0:
				output_packet(urb, p0 | 0x05, 0xf7, 0, 0);
				break;
			case STATE_SYSEX_1:
				output_packet(urb, p0 | 0x06, port->data[0], 0xf7, 0);
				break;
			case STATE_SYSEX_2:
				output_packet(urb, p0 | 0x07, port->data[0], port->data[1], 0xf7);
				break;
			}
			port->state = STATE_UNKNOWN;
			break;
		}
	} else if (b >= 0x80) {
		port->data[0] = b;
		if (b >= 0xc0 && b <= 0xdf)
			port->state = STATE_1PARAM;
		else
			port->state = STATE_2PARAM_1;
	} else { /* b < 0x80 */
		switch (port->state) {
		case STATE_1PARAM:
			if (port->data[0] < 0xf0) {
				p0 |= port->data[0] >> 4;
			} else {
				p0 |= 0x02;
				port->state = STATE_UNKNOWN;
			}
			output_packet(urb, p0, port->data[0], b, 0);
			break;
		case STATE_2PARAM_1:
			port->data[1] = b;
			port->state = STATE_2PARAM_2;
			break;
		case STATE_2PARAM_2:
			if (port->data[0] < 0xf0) {
				p0 |= port->data[0] >> 4;
				port->state = STATE_2PARAM_1;
			} else {
				p0 |= 0x03;
				port->state = STATE_UNKNOWN;
			}
			output_packet(urb, p0, port->data[0], port->data[1], b);
			break;
		case STATE_SYSEX_0:
			port->data[0] = b;
			port->state = STATE_SYSEX_1;
			break;
		case STATE_SYSEX_1:
			port->data[1] = b;
			port->state = STATE_SYSEX_2;
			break;
		case STATE_SYSEX_2:
			output_packet(urb, p0 | 0x04, port->data[0], port->data[1], b);
			port->state = STATE_SYSEX_0;
			break;
		}
	}
}

/*
 * Moves data from one substream buffer to the URB transfer buffer.
 */
static void snd_usbmidi_transmit(snd_usb_midi_out_endpoint_t* ep, int port_idx)
{
	struct urb* urb = ep->urb;
	usbmidi_out_port_t* port = &ep->ports[port_idx];

	while (urb->transfer_buffer_length < ep->max_transfer) {
		uint8_t b;
		if (snd_rawmidi_transmit_peek(port->substream, &b, 1) != 1) {
			port->active = 0;
			break;
		}
		snd_usbmidi_transmit_byte(port, b, urb);
		snd_rawmidi_transmit_ack(port->substream, 1);
	}
}

/*
 * This is called when some data should be transferred to the device
 * (from one or more substreams).
 */
static void snd_usbmidi_do_output(snd_usb_midi_out_endpoint_t* ep)
{
	int p;
	struct urb* urb = ep->urb;
	unsigned long flags;

	spin_lock_irqsave(&ep->buffer_lock, flags);
	if (urb->status == -EINPROGRESS || ep->umidi->chip->shutdown) {
		spin_unlock_irqrestore(&ep->buffer_lock, flags);
		return;
	}

	urb->transfer_buffer_length = 0;
	for (p= 0; p < 0x10; ++p)
		if (ep->ports[p].active)
			snd_usbmidi_transmit(ep, p);

	if (urb->transfer_buffer_length > 0) {
		if (ep->umidi->quirk && ep->umidi->quirk->type == QUIRK_MIDI_MIDIMAN)
			snd_usbmidi_convert_to_midiman(urb);

		urb->dev = ep->umidi->chip->dev;
		snd_usbmidi_submit_urb(urb, GFP_ATOMIC);
	}
	spin_unlock_irqrestore(&ep->buffer_lock, flags);
}

static void snd_usbmidi_out_tasklet(unsigned long data)
{
	snd_usb_midi_out_endpoint_t* ep = (snd_usb_midi_out_endpoint_t *) data;

	snd_usbmidi_do_output(ep);
}

static int snd_usbmidi_output_open(snd_rawmidi_substream_t* substream)
{
	snd_usb_midi_t* umidi = substream->rmidi->private_data;
	usbmidi_out_port_t* port = NULL;
	int i, j;

	for (i = 0; i < MIDI_MAX_ENDPOINTS; ++i)
		if (umidi->endpoints[i].out)
			for (j = 0; j < 0x10; ++j)
				if (umidi->endpoints[i].out->ports[j].substream == substream) {
					port = &umidi->endpoints[i].out->ports[j];
					break;
				}
	if (!port) {
		snd_BUG();
		return -ENXIO;
	}
	substream->runtime->private_data = port;
	port->state = STATE_UNKNOWN;
	return 0;
}

static int snd_usbmidi_output_close(snd_rawmidi_substream_t* substream)
{
	return 0;
}

static void snd_usbmidi_output_trigger(snd_rawmidi_substream_t* substream, int up)
{
	usbmidi_out_port_t* port = (usbmidi_out_port_t*)substream->runtime->private_data;

	port->active = up;
	if (up) {
		if (port->ep->umidi->chip->shutdown) {
			/* gobble up remaining bytes to prevent wait in
			 * snd_rawmidi_drain_output */
			while (!snd_rawmidi_transmit_empty(substream))
				snd_rawmidi_transmit_ack(substream, 1);
			return;
		}
		tasklet_hi_schedule(&port->ep->tasklet);
	}
}

static int snd_usbmidi_input_open(snd_rawmidi_substream_t* substream)
{
	return 0;
}

static int snd_usbmidi_input_close(snd_rawmidi_substream_t* substream)
{
	return 0;
}

static void snd_usbmidi_input_trigger(snd_rawmidi_substream_t* substream, int up)
{
}

static snd_rawmidi_ops_t snd_usbmidi_output_ops = {
	.open = snd_usbmidi_output_open,
	.close = snd_usbmidi_output_close,
	.trigger = snd_usbmidi_output_trigger,
};

static snd_rawmidi_ops_t snd_usbmidi_input_ops = {
	.open = snd_usbmidi_input_open,
	.close = snd_usbmidi_input_close,
	.trigger = snd_usbmidi_input_trigger
};

/*
 * Frees an input endpoint.
 * May be called when ep hasn't been initialized completely.
 */
static void snd_usbmidi_in_endpoint_delete(snd_usb_midi_in_endpoint_t* ep)
{
	if (ep->urb) {
		kfree(ep->urb->transfer_buffer);
		usb_free_urb(ep->urb);
	}
	kfree(ep);
}

/*
 * For Roland devices, use the alternate setting which uses interrupt
 * transfers for input.
 */
static struct usb_endpoint_descriptor* snd_usbmidi_get_int_epd(snd_usb_midi_t* umidi)
{
	struct usb_interface* intf;
	struct usb_host_interface *hostif;
	struct usb_interface_descriptor* intfd;

	if (le16_to_cpu(umidi->chip->dev->descriptor.idVendor) != 0x0582)
		return NULL;
	intf = umidi->iface;
	if (!intf || intf->num_altsetting != 2)
		return NULL;

	hostif = &intf->altsetting[0];
	intfd = get_iface_desc(hostif);
	if (intfd->bNumEndpoints != 2 ||
	    (get_endpoint(hostif, 0)->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_BULK ||
	    (get_endpoint(hostif, 1)->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_BULK)
		return NULL;

	hostif = &intf->altsetting[1];
	intfd = get_iface_desc(hostif);
	if (intfd->bNumEndpoints != 2 ||
	    (get_endpoint(hostif, 0)->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_BULK ||
	    (get_endpoint(hostif, 1)->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_INT)
		return NULL;

	snd_printdd(KERN_INFO "switching to altsetting %d with int ep\n",
		    intfd->bAlternateSetting);
	usb_set_interface(umidi->chip->dev, intfd->bInterfaceNumber,
			  intfd->bAlternateSetting);
	return get_endpoint(hostif, 1);
}

static struct usb_endpoint_descriptor* snd_usbmidi_get_midiman_int_epd(snd_usb_midi_t* umidi)
{
	struct usb_interface* intf = umidi->iface;
	struct usb_host_interface *hostif;
	struct usb_interface_descriptor *intfd;
	if (!intf)
		return NULL;
	hostif = &intf->altsetting[0];
	intfd = get_iface_desc(hostif);
	if (intfd->bNumEndpoints < 1)
		return NULL;
	return get_endpoint(hostif, 0);
}

/*
 * Creates an input endpoint.
 */
static int snd_usbmidi_in_endpoint_create(snd_usb_midi_t* umidi,
					  snd_usb_midi_endpoint_info_t* ep_info,
					  snd_usb_midi_endpoint_t* rep)
{
	snd_usb_midi_in_endpoint_t* ep;
	struct usb_endpoint_descriptor* int_epd;
	void* buffer;
	unsigned int pipe;
	int length;

	rep->in = NULL;
	ep = kcalloc(1, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;
	ep->umidi = umidi;

	if (umidi->quirk && umidi->quirk->type == QUIRK_MIDI_MIDIMAN)
		int_epd = snd_usbmidi_get_midiman_int_epd(umidi);
	else
		int_epd = snd_usbmidi_get_int_epd(umidi);

	ep->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ep->urb) {
		snd_usbmidi_in_endpoint_delete(ep);
		return -ENOMEM;
	}
	if (int_epd)
		pipe = usb_rcvintpipe(umidi->chip->dev, ep_info->in_ep);
	else
		pipe = usb_rcvbulkpipe(umidi->chip->dev, ep_info->in_ep);
	length = usb_maxpacket(umidi->chip->dev, pipe, 0);
	buffer = kmalloc(length, GFP_KERNEL);
	if (!buffer) {
		snd_usbmidi_in_endpoint_delete(ep);
		return -ENOMEM;
	}
	if (int_epd)
		usb_fill_int_urb(ep->urb, umidi->chip->dev, pipe, buffer, length,
				 snd_usb_complete_callback(snd_usbmidi_in_urb_complete),
				 ep, int_epd->bInterval);
	else
		usb_fill_bulk_urb(ep->urb, umidi->chip->dev, pipe, buffer, length,
				  snd_usb_complete_callback(snd_usbmidi_in_urb_complete),
				  ep);

	rep->in = ep;
	return 0;
}

static int snd_usbmidi_count_bits(uint16_t x)
{
	int i, bits = 0;

	for (i = 0; i < 16; ++i)
		bits += (x & (1 << i)) != 0;
	return bits;
}

/*
 * Frees an output endpoint.
 * May be called when ep hasn't been initialized completely.
 */
static void snd_usbmidi_out_endpoint_delete(snd_usb_midi_out_endpoint_t* ep)
{
	if (ep->tasklet.func)
		tasklet_kill(&ep->tasklet);
	if (ep->urb) {
		kfree(ep->urb->transfer_buffer);
		usb_free_urb(ep->urb);
	}
	kfree(ep);
}

/*
 * Creates an output endpoint, and initializes output ports.
 */
static int snd_usbmidi_out_endpoint_create(snd_usb_midi_t* umidi,
					   snd_usb_midi_endpoint_info_t* ep_info,
			 		   snd_usb_midi_endpoint_t* rep)
{
	snd_usb_midi_out_endpoint_t* ep;
	int i;
	unsigned int pipe;
	void* buffer;

	rep->out = NULL;
	ep = kcalloc(1, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;
	ep->umidi = umidi;

	ep->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ep->urb) {
		snd_usbmidi_out_endpoint_delete(ep);
		return -ENOMEM;
	}
	pipe = usb_sndbulkpipe(umidi->chip->dev, ep_info->out_ep);
	ep->max_transfer = usb_maxpacket(umidi->chip->dev, pipe, 1) & ~3;
	buffer = kmalloc(ep->max_transfer, GFP_KERNEL);
	if (!buffer) {
		snd_usbmidi_out_endpoint_delete(ep);
		return -ENOMEM;
	}
	usb_fill_bulk_urb(ep->urb, umidi->chip->dev, pipe, buffer,
			  ep->max_transfer,
			  snd_usb_complete_callback(snd_usbmidi_out_urb_complete), ep);

	spin_lock_init(&ep->buffer_lock);
	tasklet_init(&ep->tasklet, snd_usbmidi_out_tasklet, (unsigned long)ep);

	for (i = 0; i < 0x10; ++i)
		if (ep_info->out_cables & (1 << i)) {
			ep->ports[i].ep = ep;
			ep->ports[i].cable = i << 4;
		}

	rep->out = ep;
	return 0;
}

/*
 * Frees everything.
 */
static void snd_usbmidi_free(snd_usb_midi_t* umidi)
{
	int i;

	for (i = 0; i < MIDI_MAX_ENDPOINTS; ++i) {
		snd_usb_midi_endpoint_t* ep = &umidi->endpoints[i];
		if (ep->out)
			snd_usbmidi_out_endpoint_delete(ep->out);
		if (ep->in)
			snd_usbmidi_in_endpoint_delete(ep->in);
	}
	kfree(umidi);
}

/*
 * Unlinks all URBs (must be done before the usb_device is deleted).
 */
void snd_usbmidi_disconnect(struct list_head* p, struct usb_driver *driver)
{
	snd_usb_midi_t* umidi;
	int i;

	umidi = list_entry(p, snd_usb_midi_t, list);
	for (i = 0; i < MIDI_MAX_ENDPOINTS; ++i) {
		snd_usb_midi_endpoint_t* ep = &umidi->endpoints[i];
		if (ep->out && ep->out->urb)
			usb_kill_urb(ep->out->urb);
		if (ep->in && ep->in->urb)
			usb_kill_urb(ep->in->urb);
	}
}

static void snd_usbmidi_rawmidi_free(snd_rawmidi_t* rmidi)
{
	snd_usb_midi_t* umidi = rmidi->private_data;
	snd_usbmidi_free(umidi);
}

static snd_rawmidi_substream_t* snd_usbmidi_find_substream(snd_usb_midi_t* umidi,
							   int stream, int number)
{
	struct list_head* list;

	list_for_each(list, &umidi->rmidi->streams[stream].substreams) {
		snd_rawmidi_substream_t* substream = list_entry(list, snd_rawmidi_substream_t, list);
		if (substream->number == number)
			return substream;
	}
	return NULL;
}

/*
 * This list specifies names for ports that do not fit into the standard
 * "(product) MIDI (n)" schema because they aren't external MIDI ports,
 * such as internal control or synthesizer ports.
 */
static struct {
	__u16 vendor;
	__u16 product;
	int port;
	const char *name_format;
} snd_usbmidi_port_names[] = {
	/* Roland UA-100 */
	{0x0582, 0x0000, 2, "%s Control"},
	/* Roland SC-8850 */
	{0x0582, 0x0003, 0, "%s Part A"},
	{0x0582, 0x0003, 1, "%s Part B"},
	{0x0582, 0x0003, 2, "%s Part C"},
	{0x0582, 0x0003, 3, "%s Part D"},
	{0x0582, 0x0003, 4, "%s MIDI 1"},
	{0x0582, 0x0003, 5, "%s MIDI 2"},
	/* Roland U-8 */
	{0x0582, 0x0004, 0, "%s MIDI"},
	{0x0582, 0x0004, 1, "%s Control"},
	/* Roland SC-8820 */
	{0x0582, 0x0007, 0, "%s Part A"},
	{0x0582, 0x0007, 1, "%s Part B"},
	{0x0582, 0x0007, 2, "%s MIDI"},
	/* Roland SK-500 */
	{0x0582, 0x000b, 0, "%s Part A"},
	{0x0582, 0x000b, 1, "%s Part B"},
	{0x0582, 0x000b, 2, "%s MIDI"},
	/* Roland SC-D70 */
	{0x0582, 0x000c, 0, "%s Part A"},
	{0x0582, 0x000c, 1, "%s Part B"},
	{0x0582, 0x000c, 2, "%s MIDI"},
	/* Edirol UM-880 */
	{0x0582, 0x0014, 8, "%s Control"},
	/* Edirol SD-90 */
	{0x0582, 0x0016, 0, "%s Part A"},
	{0x0582, 0x0016, 1, "%s Part B"},
	{0x0582, 0x0016, 2, "%s MIDI 1"},
	{0x0582, 0x0016, 3, "%s MIDI 2"},
	/* Edirol UM-550 */
	{0x0582, 0x0023, 5, "%s Control"},
	/* Edirol SD-20 */
	{0x0582, 0x0027, 0, "%s Part A"},
	{0x0582, 0x0027, 1, "%s Part B"},
	{0x0582, 0x0027, 2, "%s MIDI"},
	/* Edirol SD-80 */
	{0x0582, 0x0029, 0, "%s Part A"},
	{0x0582, 0x0029, 1, "%s Part B"},
	{0x0582, 0x0029, 2, "%s MIDI 1"},
	{0x0582, 0x0029, 3, "%s MIDI 2"},
	/* Edirol UA-700 */
	{0x0582, 0x002b, 0, "%s MIDI"},
	{0x0582, 0x002b, 1, "%s Control"},
	/* Roland VariOS */
	{0x0582, 0x002f, 0, "%s MIDI"},
	{0x0582, 0x002f, 1, "%s External MIDI"},
	{0x0582, 0x002f, 2, "%s Sync"},
	/* Edirol PCR */
	{0x0582, 0x0033, 0, "%s MIDI"},
	{0x0582, 0x0033, 1, "%s 1"},
	{0x0582, 0x0033, 2, "%s 2"},
	/* BOSS GS-10 */
	{0x0582, 0x003b, 0, "%s MIDI"},
	{0x0582, 0x003b, 1, "%s Control"},
	/* Edirol UA-1000 */
	{0x0582, 0x0044, 0, "%s MIDI"},
	{0x0582, 0x0044, 1, "%s Control"},
	/* Edirol UR-80 */
	{0x0582, 0x0048, 0, "%s MIDI"},
	{0x0582, 0x0048, 1, "%s 1"},
	{0x0582, 0x0048, 2, "%s 2"},
	/* Edirol PCR-A */
	{0x0582, 0x004d, 0, "%s MIDI"},
	{0x0582, 0x004d, 1, "%s 1"},
	{0x0582, 0x004d, 2, "%s 2"},
	/* M-Audio MidiSport 8x8 */
	{0x0763, 0x1031, 8, "%s Control"},
	{0x0763, 0x1033, 8, "%s Control"},
};

static void snd_usbmidi_init_substream(snd_usb_midi_t* umidi,
				       int stream, int number,
				       snd_rawmidi_substream_t** rsubstream)
{
	int i;
	__u16 vendor, product;
	const char *name_format;

	snd_rawmidi_substream_t* substream = snd_usbmidi_find_substream(umidi, stream, number);
	if (!substream) {
		snd_printd(KERN_ERR "substream %d:%d not found\n", stream, number);
		return;
	}

	/* TODO: read port name from jack descriptor */
	name_format = "%s MIDI %d";
	vendor = le16_to_cpu(umidi->chip->dev->descriptor.idVendor);
	product = le16_to_cpu(umidi->chip->dev->descriptor.idProduct);
	for (i = 0; i < ARRAY_SIZE(snd_usbmidi_port_names); ++i) {
		if (snd_usbmidi_port_names[i].vendor == vendor &&
		    snd_usbmidi_port_names[i].product == product &&
		    snd_usbmidi_port_names[i].port == number) {
			name_format = snd_usbmidi_port_names[i].name_format;
			break;
		}
	}
	snprintf(substream->name, sizeof(substream->name),
		 name_format, umidi->chip->card->shortname, number + 1);

	*rsubstream = substream;
}

/*
 * Creates the endpoints and their ports.
 */
static int snd_usbmidi_create_endpoints(snd_usb_midi_t* umidi,
					snd_usb_midi_endpoint_info_t* endpoints)
{
	int i, j, err;
	int out_ports = 0, in_ports = 0;

	for (i = 0; i < MIDI_MAX_ENDPOINTS; ++i) {
		if (endpoints[i].out_cables) {
			err = snd_usbmidi_out_endpoint_create(umidi, &endpoints[i],
							      &umidi->endpoints[i]);
			if (err < 0)
				return err;
		}
		if (endpoints[i].in_cables) {
			err = snd_usbmidi_in_endpoint_create(umidi, &endpoints[i],
							     &umidi->endpoints[i]);
			if (err < 0)
				return err;
		}

		for (j = 0; j < 0x10; ++j) {
			if (endpoints[i].out_cables & (1 << j)) {
				snd_usbmidi_init_substream(umidi, SNDRV_RAWMIDI_STREAM_OUTPUT, out_ports,
							   &umidi->endpoints[i].out->ports[j].substream);
				++out_ports;
			}
			if (endpoints[i].in_cables & (1 << j)) {
				snd_usbmidi_init_substream(umidi, SNDRV_RAWMIDI_STREAM_INPUT, in_ports,
							   &umidi->endpoints[i].in->ports[j].substream);
				++in_ports;
			}
		}
	}
	snd_printdd(KERN_INFO "created %d output and %d input ports\n",
		    out_ports, in_ports);
	return 0;
}

/*
 * Returns MIDIStreaming device capabilities.
 */
static int snd_usbmidi_get_ms_info(snd_usb_midi_t* umidi,
			   	   snd_usb_midi_endpoint_info_t* endpoints)
{
	struct usb_interface* intf;
	struct usb_host_interface *hostif;
	struct usb_interface_descriptor* intfd;
	struct usb_ms_header_descriptor* ms_header;
	struct usb_host_endpoint *hostep;
	struct usb_endpoint_descriptor* ep;
	struct usb_ms_endpoint_descriptor* ms_ep;
	int i, epidx;

	intf = umidi->iface;
	if (!intf)
		return -ENXIO;
	hostif = &intf->altsetting[0];
	intfd = get_iface_desc(hostif);
	ms_header = (struct usb_ms_header_descriptor*)hostif->extra;
	if (hostif->extralen >= 7 &&
	    ms_header->bLength >= 7 &&
	    ms_header->bDescriptorType == USB_DT_CS_INTERFACE &&
	    ms_header->bDescriptorSubtype == HEADER)
		snd_printdd(KERN_INFO "MIDIStreaming version %02x.%02x\n",
			    ms_header->bcdMSC[1], ms_header->bcdMSC[0]);
	else
		snd_printk(KERN_WARNING "MIDIStreaming interface descriptor not found\n");

	epidx = 0;
	for (i = 0; i < intfd->bNumEndpoints; ++i) {
		hostep = &hostif->endpoint[i];
		ep = get_ep_desc(hostep);
		if ((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_BULK)
			continue;
		ms_ep = (struct usb_ms_endpoint_descriptor*)hostep->extra;
		if (hostep->extralen < 4 ||
		    ms_ep->bLength < 4 ||
		    ms_ep->bDescriptorType != USB_DT_CS_ENDPOINT ||
		    ms_ep->bDescriptorSubtype != MS_GENERAL)
			continue;
		if ((ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT) {
			if (endpoints[epidx].out_ep) {
				if (++epidx >= MIDI_MAX_ENDPOINTS) {
					snd_printk(KERN_WARNING "too many endpoints\n");
					break;
				}
			}
			endpoints[epidx].out_ep = ep->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
			endpoints[epidx].out_cables = (1 << ms_ep->bNumEmbMIDIJack) - 1;
			snd_printdd(KERN_INFO "EP %02X: %d jack(s)\n",
				    ep->bEndpointAddress, ms_ep->bNumEmbMIDIJack);
		} else {
			if (endpoints[epidx].in_ep) {
				if (++epidx >= MIDI_MAX_ENDPOINTS) {
					snd_printk(KERN_WARNING "too many endpoints\n");
					break;
				}
			}
			endpoints[epidx].in_ep = ep->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
			endpoints[epidx].in_cables = (1 << ms_ep->bNumEmbMIDIJack) - 1;
			snd_printdd(KERN_INFO "EP %02X: %d jack(s)\n",
				    ep->bEndpointAddress, ms_ep->bNumEmbMIDIJack);
		}
	}
	return 0;
}

/*
 * If the endpoints aren't specified, use the first bulk endpoints in the
 * first alternate setting of the interface.
 */
static int snd_usbmidi_detect_endpoint(snd_usb_midi_t* umidi,
				       snd_usb_midi_endpoint_info_t* endpoint)
{
	struct usb_interface* intf;
	struct usb_host_interface *hostif;
	struct usb_interface_descriptor* intfd;
	struct usb_endpoint_descriptor* epd;
	int i;

	intf = umidi->iface;
	if (!intf || intf->num_altsetting < 1)
		return -ENOENT;
	hostif = intf->altsetting;
	intfd = get_iface_desc(hostif);
	if (intfd->bNumEndpoints < 1)
		return -ENOENT;

	for (i = 0; i < intfd->bNumEndpoints; ++i) {
		epd = get_endpoint(hostif, i);
		if ((epd->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_BULK)
			continue;
		if (!endpoint->out_ep && endpoint->out_cables &&
		    (epd->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT)
			endpoint->out_ep = epd->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
		if (!endpoint->in_ep && endpoint->in_cables &&
		    (epd->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN)
			endpoint->in_ep = epd->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
	}
	return 0;
}

/*
 * Detects the endpoints and ports of Yamaha devices.
 */
static int snd_usbmidi_detect_yamaha(snd_usb_midi_t* umidi,
				     snd_usb_midi_endpoint_info_t* endpoint)
{
	struct usb_interface* intf;
	struct usb_host_interface *hostif;
	struct usb_interface_descriptor* intfd;
	uint8_t* cs_desc;

	intf = umidi->iface;
	if (!intf)
		return -ENOENT;
	hostif = intf->altsetting;
	intfd = get_iface_desc(hostif);
	if (intfd->bNumEndpoints < 1)
		return -ENOENT;

	/*
	 * For each port there is one MIDI_IN/OUT_JACK descriptor, not
	 * necessarily with any useful contents.  So simply count 'em.
	 */
	for (cs_desc = hostif->extra;
	     cs_desc < hostif->extra + hostif->extralen && cs_desc[0] >= 2;
	     cs_desc += cs_desc[0]) {
		if (cs_desc[1] == CS_AUDIO_INTERFACE) {
			if (cs_desc[2] == MIDI_IN_JACK)
				endpoint->in_cables = (endpoint->in_cables << 1) | 1;
			else if (cs_desc[2] == MIDI_OUT_JACK)
				endpoint->out_cables = (endpoint->out_cables << 1) | 1;
		}
	}
	if (!endpoint->in_cables && !endpoint->out_cables)
		return -ENOENT;

	return snd_usbmidi_detect_endpoint(umidi, endpoint);
}

/*
 * Creates the endpoints and their ports for Midiman devices.
 */
static int snd_usbmidi_create_endpoints_midiman(snd_usb_midi_t* umidi,
						snd_usb_midi_endpoint_info_t* endpoint)
{
	snd_usb_midi_endpoint_info_t ep_info;
	struct usb_interface* intf;
	struct usb_host_interface *hostif;
	struct usb_interface_descriptor* intfd;
	struct usb_endpoint_descriptor* epd;
	int cable, err;

	intf = umidi->iface;
	if (!intf)
		return -ENOENT;
	hostif = intf->altsetting;
	intfd = get_iface_desc(hostif);
	/*
	 * The various MidiSport devices have more or less random endpoint
	 * numbers, so we have to identify the endpoints by their index in
	 * the descriptor array, like the driver for that other OS does.
	 *
	 * There is one interrupt input endpoint for all input ports, one
	 * bulk output endpoint for even-numbered ports, and one for odd-
	 * numbered ports.  Both bulk output endpoints have corresponding
	 * input bulk endpoints (at indices 1 and 3) which aren't used.
	 */
	if (intfd->bNumEndpoints < (endpoint->out_cables > 0x0001 ? 5 : 3)) {
		snd_printdd(KERN_ERR "not enough endpoints\n");
		return -ENOENT;
	}

	epd = get_endpoint(hostif, 0);
	if ((epd->bEndpointAddress & USB_ENDPOINT_DIR_MASK) != USB_DIR_IN ||
	    (epd->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_INT) {
		snd_printdd(KERN_ERR "endpoint[0] isn't interrupt\n");
		return -ENXIO;
	}
	epd = get_endpoint(hostif, 2);
	if ((epd->bEndpointAddress & USB_ENDPOINT_DIR_MASK) != USB_DIR_OUT ||
	    (epd->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_BULK) {
		snd_printdd(KERN_ERR "endpoint[2] isn't bulk output\n");
		return -ENXIO;
	}
	if (endpoint->out_cables > 0x0001) {
		epd = get_endpoint(hostif, 4);
		if ((epd->bEndpointAddress & USB_ENDPOINT_DIR_MASK) != USB_DIR_OUT ||
		    (epd->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_BULK) {
			snd_printdd(KERN_ERR "endpoint[4] isn't bulk output\n");
			return -ENXIO;
		}
	}

	ep_info.out_ep = get_endpoint(hostif, 2)->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
	ep_info.out_cables = endpoint->out_cables & 0x5555;
	err = snd_usbmidi_out_endpoint_create(umidi, &ep_info, &umidi->endpoints[0]);
	if (err < 0)
		return err;

	ep_info.in_ep = get_endpoint(hostif, 0)->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
	ep_info.in_cables = endpoint->in_cables;
	err = snd_usbmidi_in_endpoint_create(umidi, &ep_info, &umidi->endpoints[0]);
	if (err < 0)
		return err;
	umidi->endpoints[0].in->urb->complete = snd_usb_complete_callback(snd_usbmidi_in_midiman_complete);

	if (endpoint->out_cables > 0x0001) {
		ep_info.out_ep = get_endpoint(hostif, 4)->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
		ep_info.out_cables = endpoint->out_cables & 0xaaaa;
		err = snd_usbmidi_out_endpoint_create(umidi, &ep_info, &umidi->endpoints[1]);
		if (err < 0)
			return err;
	}

	for (cable = 0; cable < 0x10; ++cable) {
		if (endpoint->out_cables & (1 << cable))
			snd_usbmidi_init_substream(umidi, SNDRV_RAWMIDI_STREAM_OUTPUT, cable,
						   &umidi->endpoints[cable & 1].out->ports[cable].substream);
		if (endpoint->in_cables & (1 << cable))
			snd_usbmidi_init_substream(umidi, SNDRV_RAWMIDI_STREAM_INPUT, cable,
						   &umidi->endpoints[0].in->ports[cable].substream);
	}
	return 0;
}

static int snd_usbmidi_create_rawmidi(snd_usb_midi_t* umidi,
				      int out_ports, int in_ports)
{
	snd_rawmidi_t* rmidi;
	int err;

	err = snd_rawmidi_new(umidi->chip->card, "USB MIDI",
			      umidi->chip->next_midi_device++,
			      out_ports, in_ports, &rmidi);
	if (err < 0)
		return err;
	strcpy(rmidi->name, umidi->chip->card->shortname);
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT |
			    SNDRV_RAWMIDI_INFO_INPUT |
			    SNDRV_RAWMIDI_INFO_DUPLEX;
	rmidi->private_data = umidi;
	rmidi->private_free = snd_usbmidi_rawmidi_free;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &snd_usbmidi_output_ops);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &snd_usbmidi_input_ops);

	umidi->rmidi = rmidi;
	return 0;
}

/*
 * Temporarily stop input.
 */
void snd_usbmidi_input_stop(struct list_head* p)
{
	snd_usb_midi_t* umidi;
	int i;

	umidi = list_entry(p, snd_usb_midi_t, list);
	for (i = 0; i < MIDI_MAX_ENDPOINTS; ++i) {
		snd_usb_midi_endpoint_t* ep = &umidi->endpoints[i];
		if (ep->in)
			usb_kill_urb(ep->in->urb);
	}
}

static void snd_usbmidi_input_start_ep(snd_usb_midi_in_endpoint_t* ep)
{
	if (ep) {
		struct urb* urb = ep->urb;
		urb->dev = ep->umidi->chip->dev;
		snd_usbmidi_submit_urb(urb, GFP_KERNEL);
	}
}

/*
 * Resume input after a call to snd_usbmidi_input_stop().
 */
void snd_usbmidi_input_start(struct list_head* p)
{
	snd_usb_midi_t* umidi;
	int i;

	umidi = list_entry(p, snd_usb_midi_t, list);
	for (i = 0; i < MIDI_MAX_ENDPOINTS; ++i)
		snd_usbmidi_input_start_ep(umidi->endpoints[i].in);
}

/*
 * Creates and registers everything needed for a MIDI streaming interface.
 */
int snd_usb_create_midi_interface(snd_usb_audio_t* chip,
				  struct usb_interface* iface,
				  const snd_usb_audio_quirk_t* quirk)
{
	snd_usb_midi_t* umidi;
	snd_usb_midi_endpoint_info_t endpoints[MIDI_MAX_ENDPOINTS];
	int out_ports, in_ports;
	int i, err;

	umidi = kcalloc(1, sizeof(*umidi), GFP_KERNEL);
	if (!umidi)
		return -ENOMEM;
	umidi->chip = chip;
	umidi->iface = iface;
	umidi->quirk = quirk;

	/* detect the endpoint(s) to use */
	memset(endpoints, 0, sizeof(endpoints));
	if (!quirk) {
		err = snd_usbmidi_get_ms_info(umidi, endpoints);
	} else {
		switch (quirk->type) {
		case QUIRK_MIDI_FIXED_ENDPOINT:
			memcpy(&endpoints[0], quirk->data,
			       sizeof(snd_usb_midi_endpoint_info_t));
			err = snd_usbmidi_detect_endpoint(umidi, &endpoints[0]);
			break;
		case QUIRK_MIDI_YAMAHA:
			err = snd_usbmidi_detect_yamaha(umidi, &endpoints[0]);
			break;
		case QUIRK_MIDI_MIDIMAN:
			memcpy(&endpoints[0], quirk->data,
			       sizeof(snd_usb_midi_endpoint_info_t));
			err = 0;
			break;
		default:
			snd_printd(KERN_ERR "invalid quirk type %d\n", quirk->type);
			err = -ENXIO;
			break;
		}
	}
	if (err < 0) {
		kfree(umidi);
		return err;
	}

	/* create rawmidi device */
	out_ports = 0;
	in_ports = 0;
	for (i = 0; i < MIDI_MAX_ENDPOINTS; ++i) {
		out_ports += snd_usbmidi_count_bits(endpoints[i].out_cables);
		in_ports += snd_usbmidi_count_bits(endpoints[i].in_cables);
	}
	err = snd_usbmidi_create_rawmidi(umidi, out_ports, in_ports);
	if (err < 0) {
		kfree(umidi);
		return err;
	}

	/* create endpoint/port structures */
	if (quirk && quirk->type == QUIRK_MIDI_MIDIMAN)
		err = snd_usbmidi_create_endpoints_midiman(umidi, &endpoints[0]);
	else
		err = snd_usbmidi_create_endpoints(umidi, endpoints);
	if (err < 0) {
		snd_usbmidi_free(umidi);
		return err;
	}

	list_add(&umidi->list, &umidi->chip->midi_list);

	for (i = 0; i < MIDI_MAX_ENDPOINTS; ++i)
		snd_usbmidi_input_start_ep(umidi->endpoints[i].in);
	return 0;
}

EXPORT_SYMBOL(snd_usb_create_midi_interface);
EXPORT_SYMBOL(snd_usbmidi_input_stop);
EXPORT_SYMBOL(snd_usbmidi_input_start);
EXPORT_SYMBOL(snd_usbmidi_disconnect);
