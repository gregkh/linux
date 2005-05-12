/*
 * Generic gameport layer
 *
 * Copyright (c) 1999-2002 Vojtech Pavlik
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <asm/io.h>
#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/gameport.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/delay.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@ucw.cz>");
MODULE_DESCRIPTION("Generic gameport layer");
MODULE_LICENSE("GPL");

EXPORT_SYMBOL(gameport_register_port);
EXPORT_SYMBOL(gameport_unregister_port);
EXPORT_SYMBOL(gameport_register_device);
EXPORT_SYMBOL(gameport_unregister_device);
EXPORT_SYMBOL(gameport_open);
EXPORT_SYMBOL(gameport_close);
EXPORT_SYMBOL(gameport_rescan);
EXPORT_SYMBOL(gameport_cooked_read);

static LIST_HEAD(gameport_list);
static LIST_HEAD(gameport_dev_list);

#ifdef __i386__

#define DELTA(x,y)      ((y)-(x)+((y)<(x)?1193182/HZ:0))
#define GET_TIME(x)     do { x = get_time_pit(); } while (0)

static unsigned int get_time_pit(void)
{
	extern spinlock_t i8253_lock;
	unsigned long flags;
	unsigned int count;

	spin_lock_irqsave(&i8253_lock, flags);
	outb_p(0x00, 0x43);
	count = inb_p(0x40);
	count |= inb_p(0x40) << 8;
	spin_unlock_irqrestore(&i8253_lock, flags);

	return count;
}

#endif

/*
 * gameport_measure_speed() measures the gameport i/o speed.
 */

static int gameport_measure_speed(struct gameport *gameport)
{
#ifdef __i386__

	unsigned int i, t, t1, t2, t3, tx;
	unsigned long flags;

	if (gameport_open(gameport, NULL, GAMEPORT_MODE_RAW))
		return 0;

	tx = 1 << 30;

	for(i = 0; i < 50; i++) {
		local_irq_save(flags);
		GET_TIME(t1);
		for(t = 0; t < 50; t++) gameport_read(gameport);
		GET_TIME(t2);
		GET_TIME(t3);
		local_irq_restore(flags);
		udelay(i * 10);
		if ((t = DELTA(t2,t1) - DELTA(t3,t2)) < tx) tx = t;
	}

	gameport_close(gameport);
	return 59659 / (tx < 1 ? 1 : tx);

#else

	unsigned int j, t = 0;

	j = jiffies; while (j == jiffies);
	j = jiffies; while (j == jiffies) { t++; gameport_read(gameport); }

	gameport_close(gameport);
	return t * HZ / 1000;

#endif
}

static void gameport_find_dev(struct gameport *gameport)
{
        struct gameport_dev *dev;

        list_for_each_entry(dev, &gameport_dev_list, node) {
		if (gameport->dev)
			break;
		if (dev->connect)
                	dev->connect(gameport, dev);
        }
}

void gameport_rescan(struct gameport *gameport)
{
	gameport_close(gameport);
	gameport_find_dev(gameport);
}

void gameport_register_port(struct gameport *gameport)
{
	list_add_tail(&gameport->node, &gameport_list);
	gameport->speed = gameport_measure_speed(gameport);
	gameport_find_dev(gameport);
}

void gameport_unregister_port(struct gameport *gameport)
{
	list_del_init(&gameport->node);
	if (gameport->dev && gameport->dev->disconnect)
		gameport->dev->disconnect(gameport);
}

void gameport_register_device(struct gameport_dev *dev)
{
	struct gameport *gameport;

	list_add_tail(&dev->node, &gameport_dev_list);
	list_for_each_entry(gameport, &gameport_list, node)
		if (!gameport->dev && dev->connect)
			dev->connect(gameport, dev);
}

void gameport_unregister_device(struct gameport_dev *dev)
{
	struct gameport *gameport;

	list_del_init(&dev->node);
	list_for_each_entry(gameport, &gameport_list, node) {
		if (gameport->dev == dev && dev->disconnect)
			dev->disconnect(gameport);
		gameport_find_dev(gameport);
	}
}

int gameport_open(struct gameport *gameport, struct gameport_dev *dev, int mode)
{
	if (gameport->open) {
		if (gameport->open(gameport, mode))
			return -1;
	} else {
		if (mode != GAMEPORT_MODE_RAW)
			return -1;
	}

	if (gameport->dev)
		return -1;

	gameport->dev = dev;

	return 0;
}

void gameport_close(struct gameport *gameport)
{
	gameport->dev = NULL;
	if (gameport->close)
		gameport->close(gameport);
}
