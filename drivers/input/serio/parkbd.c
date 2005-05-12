/*
 * $Id: parkbd.c,v 1.10 2002/03/13 10:09:20 vojtech Exp $
 *
 *  Copyright (c) 1999-2001 Vojtech Pavlik
 */

/*
 *  Parallel port to Keyboard port adapter driver for Linux
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
#include <linux/parport.h>
#include <linux/init.h>
#include <linux/serio.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@ucw.cz>");
MODULE_DESCRIPTION("Parallel port to Keyboard port adapter driver");
MODULE_LICENSE("GPL");

static unsigned int parkbd_pp_no;
module_param_named(port, parkbd_pp_no, int, 0);
MODULE_PARM_DESC(port, "Parallel port the adapter is connected to (default is 0)");

static unsigned int parkbd_mode = SERIO_8042;
module_param_named(mode, parkbd_mode, uint, 0);
MODULE_PARM_DESC(mode, "Mode of operation: XT = 0/AT = 1 (default)");

#define PARKBD_CLOCK	0x01	/* Strobe & Ack */
#define PARKBD_DATA	0x02	/* AutoFd & Busy */

static int parkbd_buffer;
static int parkbd_counter;
static unsigned long parkbd_last;
static int parkbd_writing;
static unsigned long parkbd_start;

static struct pardevice *parkbd_dev;
static struct serio *parkbd_port;

static int parkbd_readlines(void)
{
	return (parport_read_status(parkbd_dev->port) >> 6) ^ 2;
}

static void parkbd_writelines(int data)
{
	parport_write_control(parkbd_dev->port, (~data & 3) | 0x10);
}

static int parkbd_write(struct serio *port, unsigned char c)
{
	unsigned char p;

	if (!parkbd_mode) return -1;

        p = c ^ (c >> 4);
	p = p ^ (p >> 2);
	p = p ^ (p >> 1);

	parkbd_counter = 0;
	parkbd_writing = 1;
	parkbd_buffer = c | (((int) (~p & 1)) << 8) | 0x600;

	parkbd_writelines(2);

	return 0;
}

static void parkbd_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{

	if (parkbd_writing) {

		if (parkbd_counter && ((parkbd_counter == 11) || time_after(jiffies, parkbd_last + HZ/100))) {
			parkbd_counter = 0;
			parkbd_buffer = 0;
			parkbd_writing = 0;
			parkbd_writelines(3);
			return;
		}

		parkbd_writelines(((parkbd_buffer >> parkbd_counter++) & 1) | 2);

		if (parkbd_counter == 11) {
			parkbd_counter = 0;
			parkbd_buffer = 0;
			parkbd_writing = 0;
			parkbd_writelines(3);
		}

	} else {

		if ((parkbd_counter == parkbd_mode + 10) || time_after(jiffies, parkbd_last + HZ/100)) {
			parkbd_counter = 0;
			parkbd_buffer = 0;
		}

		parkbd_buffer |= (parkbd_readlines() >> 1) << parkbd_counter++;

		if (parkbd_counter == parkbd_mode + 10)
			serio_interrupt(parkbd_port, (parkbd_buffer >> (2 - parkbd_mode)) & 0xff, 0, regs);
	}

	parkbd_last = jiffies;
}

static int parkbd_getport(void)
{
	struct parport *pp;

	pp = parport_find_number(parkbd_pp_no);

	if (pp == NULL) {
		printk(KERN_ERR "parkbd: no such parport\n");
		return -ENODEV;
	}

	parkbd_dev = parport_register_device(pp, "parkbd", NULL, NULL, parkbd_interrupt, PARPORT_DEV_EXCL, NULL);
	parport_put_port(pp);

	if (!parkbd_dev)
		return -ENODEV;

	if (parport_claim(parkbd_dev)) {
		parport_unregister_device(parkbd_dev);
		return -EBUSY;
	}

	parkbd_start = jiffies;

	return 0;
}

static struct serio * __init parkbd_allocate_serio(void)
{
	struct serio *serio;

	serio = kmalloc(sizeof(struct serio), GFP_KERNEL);
	if (serio) {
		memset(serio, 0, sizeof(struct serio));
		serio->type = parkbd_mode;
		serio->write = parkbd_write,
		strlcpy(serio->name, "PARKBD AT/XT keyboard adapter", sizeof(serio->name));
		snprintf(serio->phys, sizeof(serio->phys), "%s/serio0", parkbd_dev->port->name);
	}

	return serio;
}

int __init parkbd_init(void)
{
	int err;

	err = parkbd_getport();
	if (err)
		return err;

	parkbd_port = parkbd_allocate_serio();
	if (!parkbd_port) {
		parport_release(parkbd_dev);
		return -ENOMEM;
	}

	parkbd_writelines(3);

	serio_register_port(parkbd_port);

	printk(KERN_INFO "serio: PARKBD %s adapter on %s\n",
                        parkbd_mode ? "AT" : "XT", parkbd_dev->port->name);

	return 0;
}

void __exit parkbd_exit(void)
{
	parport_release(parkbd_dev);
	serio_unregister_port(parkbd_port);
	parport_unregister_device(parkbd_dev);
}

module_init(parkbd_init);
module_exit(parkbd_exit);
