/*
 * ALPS touchpad PS/2 mouse driver
 *
 * Copyright (c) 2003 Neil Brown <neilb@cse.unsw.edu.au>
 * Copyright (c) 2003 Peter Osterlund <petero2@telia.com>
 * Copyright (c) 2004 Dmitry Torokhov <dtor@mail.ru>
 *
 * ALPS detection, tap switching and status querying info is taken from
 * tpconfig utility (by C. Scott Ananian and Bruce Kall).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/input.h>
#include <linux/serio.h>
#include <linux/libps2.h>

#include "psmouse.h"
#include "alps.h"

#undef DEBUG
#ifdef DEBUG
#define dbg(format, arg...) printk(KERN_INFO "alps.c: " format "\n", ## arg)
#else
#define dbg(format, arg...) do {} while (0)
#endif

#define ALPS_MODEL_GLIDEPOINT	1
#define ALPS_MODEL_DUALPOINT	2

struct alps_model_info {
	unsigned char signature[3];
	unsigned char model;
} alps_model_data[] = {
/*	{ { 0x33, 0x02, 0x0a },	ALPS_MODEL_GLIDEPOINT },	*/
	{ { 0x53, 0x02, 0x0a },	ALPS_MODEL_GLIDEPOINT },
	{ { 0x53, 0x02, 0x14 },	ALPS_MODEL_GLIDEPOINT },
	{ { 0x63, 0x02, 0x0a },	ALPS_MODEL_GLIDEPOINT },
	{ { 0x63, 0x02, 0x14 },	ALPS_MODEL_GLIDEPOINT },
	{ { 0x73, 0x02, 0x0a },	ALPS_MODEL_GLIDEPOINT },
	{ { 0x73, 0x02, 0x14 },	ALPS_MODEL_GLIDEPOINT },
	{ { 0x63, 0x02, 0x28 },	ALPS_MODEL_GLIDEPOINT },
/*	{ { 0x63, 0x02, 0x3c },	ALPS_MODEL_GLIDEPOINT },	*/
/*	{ { 0x63, 0x02, 0x50 },	ALPS_MODEL_GLIDEPOINT },	*/
	{ { 0x63, 0x02, 0x64 },	ALPS_MODEL_GLIDEPOINT },
	{ { 0x20, 0x02, 0x0e },	ALPS_MODEL_DUALPOINT },
	{ { 0x22, 0x02, 0x0a },	ALPS_MODEL_DUALPOINT },
	{ { 0x22, 0x02, 0x14 }, ALPS_MODEL_DUALPOINT },
	{ { 0x63, 0x03, 0xc8 },	ALPS_MODEL_DUALPOINT },
};

/*
 * ALPS abolute Mode
 * byte 0:  1    1    1    1    1  mid0 rig0 lef0
 * byte 1:  0   x6   x5   x4   x3   x2   x1   x0
 * byte 2:  0   x10  x9   x8   x7  up1  fin  ges
 * byte 3:  0   y9   y8   y7    1  mid1 rig1 lef1
 * byte 4:  0   y6   y5   y4   y3   y2   y1   y0
 * byte 5:  0   z6   z5   z4   z3   z2   z1   z0
 *
 * On a dualpoint, {mid,rig,lef}0 are the stick, 1 are the pad.
 * We just 'or' them together for now.
 *
 * We used to send 'ges'tures as BTN_TOUCH but this made it impossible
 * to disable tap events in the synaptics driver since the driver
 * was unable to distinguish a gesture tap from an actual button click.
 * A tap gesture now creates an emulated touch that the synaptics
 * driver can interpret as a tap event, if MaxTapTime=0 and
 * MaxTapMove=0 then the driver will ignore taps.
 *
 * The touchpad on an 'Acer Aspire' has 4 buttons:
 *   left,right,up,down.
 * This device always sets {mid,rig,lef}0 to 1 and
 * reflects left,right,down,up in lef1,rig1,mid1,up1.
 */

static void alps_process_packet(struct psmouse *psmouse, struct pt_regs *regs)
{
	unsigned char *packet = psmouse->packet;
	struct input_dev *dev = &psmouse->dev;
	int x, y, z;
	int left = 0, right = 0, middle = 0;

	input_regs(dev, regs);

	if ((packet[0] & 0xc8) == 0x08) {   /* 3-byte PS/2 packet */
		x = packet[1];
		if (packet[0] & 0x10)
			x = x - 256;
		y = packet[2];
		if (packet[0] & 0x20)
			y = y - 256;
		left  = (packet[0]     ) & 1;
		right = (packet[0] >> 1) & 1;

		input_report_rel(dev, REL_X, x);
		input_report_rel(dev, REL_Y, -y);
		input_report_key(dev, BTN_A, left);
		input_report_key(dev, BTN_B, right);
		input_sync(dev);
		return;
	}

	x = (packet[1] & 0x7f) | ((packet[2] & 0x78)<<(7-3));
	y = (packet[4] & 0x7f) | ((packet[3] & 0x70)<<(7-4));
	z = packet[5];

	if (z == 127) {	/* DualPoint stick is relative, not absolute */
		if (x > 383)
			x = x - 768;
		if (y > 255)
			y = y - 512;
		left  = packet[3] & 1;
		right = (packet[3] >> 1) & 1;

		input_report_rel(dev, REL_X, x);
		input_report_rel(dev, REL_Y, -y);
		input_report_key(dev, BTN_LEFT, left);
		input_report_key(dev, BTN_RIGHT, right);
		input_sync(dev);
		return;
	}

	if (z > 30) input_report_key(dev, BTN_TOUCH, 1);
	if (z < 25) input_report_key(dev, BTN_TOUCH, 0);

	if (z > 0) {
		input_report_abs(dev, ABS_X, x);
		input_report_abs(dev, ABS_Y, y);
	}
	input_report_abs(dev, ABS_PRESSURE, z);
	input_report_key(dev, BTN_TOOL_FINGER, z > 0);

	left  |= (packet[2]     ) & 1;
	left  |= (packet[3]     ) & 1;
	right |= (packet[3] >> 1) & 1;
	if (packet[0] == 0xff) {
		int back    = (packet[3] >> 2) & 1;
		int forward = (packet[2] >> 2) & 1;
		if (back && forward) {
			middle = 1;
			back = 0;
			forward = 0;
		}
		input_report_key(dev, BTN_BACK,    back);
		input_report_key(dev, BTN_FORWARD, forward);
	} else {
		left   |= (packet[0]     ) & 1;
		right  |= (packet[0] >> 1) & 1;
		middle |= (packet[0] >> 2) & 1;
		middle |= (packet[3] >> 2) & 1;
	}

	input_report_key(dev, BTN_LEFT, left);
	input_report_key(dev, BTN_RIGHT, right);
	input_report_key(dev, BTN_MIDDLE, middle);

	input_sync(dev);
}

static psmouse_ret_t alps_process_byte(struct psmouse *psmouse, struct pt_regs *regs)
{
	if ((psmouse->packet[0] & 0xc8) == 0x08) { /* PS/2 packet */
		if (psmouse->pktcnt == 3) {
			alps_process_packet(psmouse, regs);
			return PSMOUSE_FULL_PACKET;
		}
		return PSMOUSE_GOOD_DATA;
	}

	/* ALPS absolute mode packets start with 0b11111mrl */
	if ((psmouse->packet[0] & 0xf8) != 0xf8)
		return PSMOUSE_BAD_DATA;

	/* Bytes 2 - 6 should have 0 in the highest bit */
	if (psmouse->pktcnt >= 2 && psmouse->pktcnt <= 6 &&
	    (psmouse->packet[psmouse->pktcnt-1] & 0x80))
		return PSMOUSE_BAD_DATA;

	if (psmouse->pktcnt == 6) {
		alps_process_packet(psmouse, regs);
		return PSMOUSE_FULL_PACKET;
	}

	return PSMOUSE_GOOD_DATA;
}

int alps_get_model(struct psmouse *psmouse)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	unsigned char param[4];
	int i;

	/*
	 * First try "E6 report".
	 * ALPS should return 0x00,0x00,0x0a or 0x00,0x00,0x64
	 */
	param[0] = 0;
	if (ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES) ||
	    ps2_command(ps2dev,  NULL, PSMOUSE_CMD_SETSCALE11) ||
	    ps2_command(ps2dev,  NULL, PSMOUSE_CMD_SETSCALE11) ||
	    ps2_command(ps2dev,  NULL, PSMOUSE_CMD_SETSCALE11))
		return -1;

	param[0] = param[1] = param[2] = 0xff;
	if (ps2_command(ps2dev, param, PSMOUSE_CMD_GETINFO))
		return -1;

	dbg("E6 report: %2.2x %2.2x %2.2x", param[0], param[1], param[2]);

	if (param[0] != 0x00 || param[1] != 0x00 || (param[2] != 0x0a && param[2] != 0x64))
		return -1;

	/* Now try "E7 report". ALPS should return 0x33 in byte 1 */
	param[0] = 0;
	if (ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES) ||
	    ps2_command(ps2dev,  NULL, PSMOUSE_CMD_SETSCALE21) ||
	    ps2_command(ps2dev,  NULL, PSMOUSE_CMD_SETSCALE21) ||
	    ps2_command(ps2dev,  NULL, PSMOUSE_CMD_SETSCALE21))
		return -1;

	param[0] = param[1] = param[2] = 0xff;
	if (ps2_command(ps2dev, param, PSMOUSE_CMD_GETINFO))
		return -1;

	dbg("E7 report: %2.2x %2.2x %2.2x", param[0], param[1], param[2]);

	for (i = 0; i < ARRAY_SIZE(alps_model_data); i++)
		if (!memcmp(param, alps_model_data[i].signature, sizeof(alps_model_data[i].signature)))
			return alps_model_data[i].model;

	return -1;
}

/*
 * For DualPoint devices select the device that should respond to
 * subsequent commands. It looks like glidepad is behind stickpointer,
 * I'd thought it would be other way around...
 */
static int alps_passthrough_mode(struct psmouse *psmouse, int enable)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	unsigned char param[3];
	int cmd = enable ? PSMOUSE_CMD_SETSCALE21 : PSMOUSE_CMD_SETSCALE11;

	if (ps2_command(ps2dev, NULL, cmd) ||
	    ps2_command(ps2dev, NULL, cmd) ||
	    ps2_command(ps2dev, NULL, cmd) ||
	    ps2_command(ps2dev, NULL, PSMOUSE_CMD_DISABLE))
		return -1;

	/* we may get 3 more bytes, just ignore them */
	ps2_command(ps2dev, param, 0x0300);

	return 0;
}

static int alps_absolute_mode(struct psmouse *psmouse)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;

	/* Try ALPS magic knock - 4 disable before enable */
	if (ps2_command(ps2dev, NULL, PSMOUSE_CMD_DISABLE) ||
	    ps2_command(ps2dev, NULL, PSMOUSE_CMD_DISABLE) ||
	    ps2_command(ps2dev, NULL, PSMOUSE_CMD_DISABLE) ||
	    ps2_command(ps2dev, NULL, PSMOUSE_CMD_DISABLE) ||
	    ps2_command(ps2dev, NULL, PSMOUSE_CMD_ENABLE))
		return -1;

	/*
	 * Switch mouse to poll (remote) mode so motion data will not
	 * get in our way
	 */
	return ps2_command(&psmouse->ps2dev, NULL, PSMOUSE_CMD_SETPOLL);
}

static int alps_get_status(struct psmouse *psmouse, char *param)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;

	/* Get status: 0xF5 0xF5 0xF5 0xE9 */
	if (ps2_command(ps2dev, NULL, PSMOUSE_CMD_DISABLE) ||
	    ps2_command(ps2dev, NULL, PSMOUSE_CMD_DISABLE) ||
	    ps2_command(ps2dev, NULL, PSMOUSE_CMD_DISABLE) ||
	    ps2_command(ps2dev, param, PSMOUSE_CMD_GETINFO))
		return -1;

	dbg("Status: %2.2x %2.2x %2.2x", param[0], param[1], param[2]);

	return 0;
}

/*
 * Turn touchpad tapping on or off. The sequences are:
 * 0xE9 0xF5 0xF5 0xF3 0x0A to enable,
 * 0xE9 0xF5 0xF5 0xE8 0x00 to disable.
 * My guess that 0xE9 (GetInfo) is here as a sync point.
 * For models that also have stickpointer (DualPoints) its tapping
 * is controlled separately (0xE6 0xE6 0xE6 0xF3 0x14|0x0A) but
 * we don't fiddle with it.
 */
static int alps_tap_mode(struct psmouse *psmouse, int enable)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	int cmd = enable ? PSMOUSE_CMD_SETRATE : PSMOUSE_CMD_SETRES;
	unsigned char tap_arg = enable ? 0x0A : 0x00;
	unsigned char param[4];

	if (ps2_command(ps2dev, param, PSMOUSE_CMD_GETINFO) ||
	    ps2_command(ps2dev, NULL, PSMOUSE_CMD_DISABLE) ||
	    ps2_command(ps2dev, NULL, PSMOUSE_CMD_DISABLE) ||
	    ps2_command(ps2dev, &tap_arg, cmd))
		return -1;

	if (alps_get_status(psmouse, param))
		return -1;

	return 0;
}

static int alps_reconnect(struct psmouse *psmouse)
{
	int model;
	unsigned char param[4];

	if ((model = alps_get_model(psmouse)) < 0)
		return -1;

	if (model == ALPS_MODEL_DUALPOINT && alps_passthrough_mode(psmouse, 1))
		return -1;

	if (alps_get_status(psmouse, param))
		return -1;

	if (param[0] & 0x04)
		alps_tap_mode(psmouse, 0);

	if (alps_absolute_mode(psmouse)) {
		printk(KERN_ERR "alps.c: Failed to enable absolute mode\n");
		return -1;
	}

	if (model == ALPS_MODEL_DUALPOINT && alps_passthrough_mode(psmouse, 0))
		return -1;

	return 0;
}

static void alps_disconnect(struct psmouse *psmouse)
{
	psmouse_reset(psmouse);
}

int alps_init(struct psmouse *psmouse)
{
	unsigned char param[4];
	int model;

	if ((model = alps_get_model(psmouse)) < 0)
		return -1;

	printk(KERN_INFO "ALPS Touchpad (%s) detected\n",
		model == ALPS_MODEL_GLIDEPOINT ? "Glidepoint" : "Dualpoint");

	if (model == ALPS_MODEL_DUALPOINT && alps_passthrough_mode(psmouse, 1))
		return -1;

	if (alps_get_status(psmouse, param)) {
		printk(KERN_ERR "alps.c: touchpad status report request failed\n");
		return -1;
	}

	if (param[0] & 0x04) {
		printk(KERN_INFO "  Disabling hardware tapping\n");
		if (alps_tap_mode(psmouse, 0))
			printk(KERN_WARNING "alps.c: Failed to disable hardware tapping\n");
	}

	if (alps_absolute_mode(psmouse)) {
		printk(KERN_ERR "alps.c: Failed to enable absolute mode\n");
		return -1;
	}

	if (model == ALPS_MODEL_DUALPOINT && alps_passthrough_mode(psmouse, 0))
		return -1;

	psmouse->dev.evbit[LONG(EV_REL)] |= BIT(EV_REL);
	psmouse->dev.relbit[LONG(REL_X)] |= BIT(REL_X);
	psmouse->dev.relbit[LONG(REL_Y)] |= BIT(REL_Y);
	psmouse->dev.keybit[LONG(BTN_A)] |= BIT(BTN_A);
	psmouse->dev.keybit[LONG(BTN_B)] |= BIT(BTN_B);

	psmouse->dev.evbit[LONG(EV_ABS)] |= BIT(EV_ABS);
	input_set_abs_params(&psmouse->dev, ABS_X, 0, 1023, 0, 0);
	input_set_abs_params(&psmouse->dev, ABS_Y, 0, 1023, 0, 0);
	input_set_abs_params(&psmouse->dev, ABS_PRESSURE, 0, 127, 0, 0);

	psmouse->dev.keybit[LONG(BTN_TOUCH)] |= BIT(BTN_TOUCH);
	psmouse->dev.keybit[LONG(BTN_TOOL_FINGER)] |= BIT(BTN_TOOL_FINGER);
	psmouse->dev.keybit[LONG(BTN_FORWARD)] |= BIT(BTN_FORWARD);
	psmouse->dev.keybit[LONG(BTN_BACK)] |= BIT(BTN_BACK);

	psmouse->protocol_handler = alps_process_byte;
	psmouse->disconnect = alps_disconnect;
	psmouse->reconnect = alps_reconnect;
	psmouse->pktsize = 6;

	return 0;
}

int alps_detect(struct psmouse *psmouse, int set_properties)
{
	if (alps_get_model(psmouse) < 0)
		return -1;

	if (set_properties) {
		psmouse->vendor = "ALPS";
		psmouse->name = "TouchPad";
	}
	return 0;
}

