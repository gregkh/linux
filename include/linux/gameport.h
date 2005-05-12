#ifndef _GAMEPORT_H
#define _GAMEPORT_H

/*
 *  Copyright (c) 1999-2002 Vojtech Pavlik
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <asm/io.h>
#include <linux/input.h>
#include <linux/list.h>

struct gameport;

struct gameport {

	void *private;	/* Private pointer for joystick drivers */
	void *driver;	/* Private pointer for gameport drivers */
	char *name;
	char *phys;

	struct input_id id;

	int io;
	int speed;
	int fuzz;

	void (*trigger)(struct gameport *);
	unsigned char (*read)(struct gameport *);
	int (*cooked_read)(struct gameport *, int *, int *);
	int (*calibrate)(struct gameport *, int *, int *);
	int (*open)(struct gameport *, int);
	void (*close)(struct gameport *);

	struct gameport_dev *dev;

	struct list_head node;
};

struct gameport_dev {

	void *private;
	char *name;

	void (*connect)(struct gameport *, struct gameport_dev *dev);
	void (*disconnect)(struct gameport *);

	struct list_head node;
};

int gameport_open(struct gameport *gameport, struct gameport_dev *dev, int mode);
void gameport_close(struct gameport *gameport);
void gameport_rescan(struct gameport *gameport);

#if defined(CONFIG_GAMEPORT) || defined(CONFIG_GAMEPORT_MODULE)
void gameport_register_port(struct gameport *gameport);
void gameport_unregister_port(struct gameport *gameport);
#else
static inline void gameport_register_port(struct gameport *gameport) { return; }
static inline void gameport_unregister_port(struct gameport *gameport) { return; }
#endif

void gameport_register_device(struct gameport_dev *dev);
void gameport_unregister_device(struct gameport_dev *dev);

#define GAMEPORT_MODE_DISABLED		0
#define GAMEPORT_MODE_RAW		1
#define GAMEPORT_MODE_COOKED		2

#define GAMEPORT_ID_VENDOR_ANALOG	0x0001
#define GAMEPORT_ID_VENDOR_MADCATZ	0x0002
#define GAMEPORT_ID_VENDOR_LOGITECH	0x0003
#define GAMEPORT_ID_VENDOR_CREATIVE	0x0004
#define GAMEPORT_ID_VENDOR_GENIUS	0x0005
#define GAMEPORT_ID_VENDOR_INTERACT	0x0006
#define GAMEPORT_ID_VENDOR_MICROSOFT	0x0007
#define GAMEPORT_ID_VENDOR_THRUSTMASTER	0x0008
#define GAMEPORT_ID_VENDOR_GRAVIS	0x0009
#define GAMEPORT_ID_VENDOR_GUILLEMOT	0x000a

static __inline__ void gameport_trigger(struct gameport *gameport)
{
	if (gameport->trigger)
		gameport->trigger(gameport);
	else
		outb(0xff, gameport->io);
}

static __inline__ unsigned char gameport_read(struct gameport *gameport)
{
	if (gameport->read)
		return gameport->read(gameport);
	else
		return inb(gameport->io);
}

static __inline__ int gameport_cooked_read(struct gameport *gameport, int *axes, int *buttons)
{
	if (gameport->cooked_read)
		return gameport->cooked_read(gameport, axes, buttons);
	else
		return -1;
}

static __inline__ int gameport_calibrate(struct gameport *gameport, int *axes, int *max)
{
	if (gameport->calibrate)
		return gameport->calibrate(gameport, axes, max);
	else
		return -1;
}

static __inline__ int gameport_time(struct gameport *gameport, int time)
{
	return (time * gameport->speed) / 1000;
}

#endif
