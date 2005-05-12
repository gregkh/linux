/*
 * $Id: joydump.c,v 1.1 2002/01/23 06:56:16 jsimmons Exp $
 *
 *  Copyright (c) 1996-2001 Vojtech Pavlik
 */

/*
 * This is just a very simple driver that can dump the data
 * out of the joystick port into the syslog ...
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so either by
 * e-mail - mail your message to <vojtech@ucw.cz>, or by paper mail:
 * Vojtech Pavlik, Simunkova 1594, Prague 8, 182 00 Czech Republic
 */

#include <linux/module.h>
#include <linux/gameport.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/init.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@ucw.cz>");
MODULE_DESCRIPTION("Gameport data dumper module");
MODULE_LICENSE("GPL");

#define BUF_SIZE 256

struct joydump {
	unsigned int time;
	unsigned char data;
};

static void __devinit joydump_connect(struct gameport *gameport, struct gameport_dev *dev)
{
	struct joydump buf[BUF_SIZE];
	int axes[4], buttons;
	int i, j, t, timeout;
	unsigned long flags;
	unsigned char u;

	printk(KERN_INFO "joydump: ,------------------- START ------------------.\n");
	printk(KERN_INFO "joydump: | Dumping gameport%s.\n", gameport->phys);
	printk(KERN_INFO "joydump: | Speed: %4d kHz.                            |\n", gameport->speed);

	if (gameport_open(gameport, dev, GAMEPORT_MODE_RAW)) {

		printk(KERN_INFO "joydump: | Raw mode not available - trying cooked.    |\n");

		if (gameport_open(gameport, dev, GAMEPORT_MODE_COOKED)) {

			printk(KERN_INFO "joydump: | Cooked not available either. Failing.      |\n");
			printk(KERN_INFO "joydump: `-------------------- END -------------------'\n");
			return;
		}

		gameport_cooked_read(gameport, axes, &buttons);

		for (i = 0; i < 4; i++)
			printk(KERN_INFO "joydump: | Axis %d: %4d.                              |\n", i, axes[i]);
		printk(KERN_INFO "joydump: | Buttons %02x.                                |\n", buttons);
		printk(KERN_INFO "joydump: `-------------------- END -------------------'\n");
	}

	timeout = gameport_time(gameport, 10000); /* 10 ms */
	t = 0;
	i = 1;

	local_irq_save(flags);

	u = gameport_read(gameport);

	buf[0].data = u;
	buf[0].time = t;

	gameport_trigger(gameport);

	while (i < BUF_SIZE && t < timeout) {

		buf[i].data = gameport_read(gameport);

		if (buf[i].data ^ u) {
			u = buf[i].data;
			buf[i].time = t;
			i++;
		}
		t++;
	}

	local_irq_restore(flags);

/*
 * Dump data.
 */

	t = i;

	printk(KERN_INFO "joydump: >------------------- DATA -------------------<\n");
	printk(KERN_INFO "joydump: | index: %3d delta: %3d.%02d us data: ", 0, 0, 0);
	for (j = 7; j >= 0; j--)
		printk("%d",(buf[0].data >> j) & 1);
	printk(" |\n");
	for (i = 1; i < t; i++) {
		printk(KERN_INFO "joydump: | index: %3d delta: %3d us data: ",
			i, buf[i].time - buf[i-1].time);
		for (j = 7; j >= 0; j--)
			printk("%d",(buf[i].data >> j) & 1);
		printk("    |\n");
	}

	printk(KERN_INFO "joydump: `-------------------- END -------------------'\n");
}

static void __devexit joydump_disconnect(struct gameport *gameport)
{
	gameport_close(gameport);
}

static struct gameport_dev joydump_dev = {
	.connect =	joydump_connect,
	.disconnect =	joydump_disconnect,
};

static int __init joydump_init(void)
{
	gameport_register_device(&joydump_dev);
	return 0;
}

static void __exit joydump_exit(void)
{
	gameport_unregister_device(&joydump_dev);
}

module_init(joydump_init);
module_exit(joydump_exit);
