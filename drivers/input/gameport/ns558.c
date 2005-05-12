/*
 * $Id: ns558.c,v 1.43 2002/01/24 19:23:21 vojtech Exp $
 *
 *  Copyright (c) 1999-2001 Vojtech Pavlik
 *  Copyright (c) 1999 Brian Gerst
 */

/*
 * NS558 based standard IBM game port driver for Linux
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

#include <asm/io.h>

#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/config.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/gameport.h>
#include <linux/slab.h>
#include <linux/pnp.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@ucw.cz>");
MODULE_DESCRIPTION("Classic gameport (ISA/PnP) driver");
MODULE_LICENSE("GPL");

#define NS558_ISA	1
#define NS558_PNP	2

static int ns558_isa_portlist[] = { 0x201, 0x200, 0x202, 0x203, 0x204, 0x205, 0x207, 0x209,
				    0x20b, 0x20c, 0x20e, 0x20f, 0x211, 0x219, 0x101, 0 };

struct ns558 {
	int type;
	int size;
	struct pnp_dev *dev;
	struct list_head node;
	struct gameport gameport;
	char phys[32];
	char name[32];
};

static LIST_HEAD(ns558_list);

/*
 * ns558_isa_probe() tries to find an isa gameport at the
 * specified address, and also checks for mirrors.
 * A joystick must be attached for this to work.
 */

static void ns558_isa_probe(int io)
{
	int i, j, b;
	unsigned char c, u, v;
	struct ns558 *port;

/*
 * No one should be using this address.
 */

	if (!request_region(io, 1, "ns558-isa"))
		return;

/*
 * We must not be able to write arbitrary values to the port.
 * The lower two axis bits must be 1 after a write.
 */

	c = inb(io);
	outb(~c & ~3, io);
	if (~(u = v = inb(io)) & 3) {
		outb(c, io);
		i = 0;
		goto out;
	}
/*
 * After a trigger, there must be at least some bits changing.
 */

	for (i = 0; i < 1000; i++) v &= inb(io);

	if (u == v) {
		outb(c, io);
		i = 0;
		goto out;
	}
	msleep(3);
/*
 * After some time (4ms) the axes shouldn't change anymore.
 */

	u = inb(io);
	for (i = 0; i < 1000; i++)
		if ((u ^ inb(io)) & 0xf) {
			outb(c, io);
			i = 0;
			goto out;
		}
/*
 * And now find the number of mirrors of the port.
 */

	for (i = 1; i < 5; i++) {

		release_region(io & (-1 << (i-1)), (1 << (i-1)));

		if (!request_region(io & (-1 << i), (1 << i), "ns558-isa"))	/* Don't disturb anyone */
			break;

		outb(0xff, io & (-1 << i));
		for (j = b = 0; j < 1000; j++)
			if (inb(io & (-1 << i)) != inb((io & (-1 << i)) + (1 << i) - 1)) b++;
		msleep(3);

		if (b > 300) {					/* We allow 30% difference */
			release_region(io & (-1 << i), (1 << i));
			break;
		}
	}

	i--;

	if (i != 4) {
		if (!request_region(io & (-1 << i), (1 << i), "ns558-isa"))
			return;
	}

	if (!(port = kmalloc(sizeof(struct ns558), GFP_KERNEL))) {
		printk(KERN_ERR "ns558: Memory allocation failed.\n");
		goto out;
	}
	memset(port, 0, sizeof(struct ns558));

	port->type = NS558_ISA;
	port->size = (1 << i);
	port->gameport.io = io;
	port->gameport.phys = port->phys;
	port->gameport.name = port->name;
	port->gameport.id.bustype = BUS_ISA;

	sprintf(port->phys, "isa%04x/gameport0", io & (-1 << i));
	sprintf(port->name, "NS558 ISA");

	gameport_register_port(&port->gameport);

	printk(KERN_INFO "gameport: NS558 ISA at %#x", port->gameport.io);
	if (port->size > 1) printk(" size %d", port->size);
	printk(" speed %d kHz\n", port->gameport.speed);

	list_add(&port->node, &ns558_list);
	return;
out:
	release_region(io & (-1 << i), (1 << i));
}

#ifdef CONFIG_PNP

static struct pnp_device_id pnp_devids[] = {
	{ .id = "@P@0001", .driver_data = 0 }, /* ALS 100 */
	{ .id = "@P@0020", .driver_data = 0 }, /* ALS 200 */
	{ .id = "@P@1001", .driver_data = 0 }, /* ALS 100+ */
	{ .id = "@P@2001", .driver_data = 0 }, /* ALS 120 */
	{ .id = "ASB16fd", .driver_data = 0 }, /* AdLib NSC16 */
	{ .id = "AZT3001", .driver_data = 0 }, /* AZT1008 */
	{ .id = "CDC0001", .driver_data = 0 }, /* Opl3-SAx */
	{ .id = "CSC0001", .driver_data = 0 }, /* CS4232 */
	{ .id = "CSC000f", .driver_data = 0 }, /* CS4236 */
	{ .id = "CSC0101", .driver_data = 0 }, /* CS4327 */
	{ .id = "CTL7001", .driver_data = 0 }, /* SB16 */
	{ .id = "CTL7002", .driver_data = 0 }, /* AWE64 */
	{ .id = "CTL7005", .driver_data = 0 }, /* Vibra16 */
	{ .id = "ENS2020", .driver_data = 0 }, /* SoundscapeVIVO */
	{ .id = "ESS0001", .driver_data = 0 }, /* ES1869 */
	{ .id = "ESS0005", .driver_data = 0 }, /* ES1878 */
	{ .id = "ESS6880", .driver_data = 0 }, /* ES688 */
	{ .id = "IBM0012", .driver_data = 0 }, /* CS4232 */
	{ .id = "OPT0001", .driver_data = 0 }, /* OPTi Audio16 */
	{ .id = "YMH0006", .driver_data = 0 }, /* Opl3-SA */
	{ .id = "YMH0022", .driver_data = 0 }, /* Opl3-SAx */
	{ .id = "PNPb02f", .driver_data = 0 }, /* Generic */
	{ .id = "", },
};

MODULE_DEVICE_TABLE(pnp, pnp_devids);

static int ns558_pnp_probe(struct pnp_dev *dev, const struct pnp_device_id *did)
{
	int ioport, iolen;
	struct ns558 *port;

	if (!pnp_port_valid(dev, 0)) {
		printk(KERN_WARNING "ns558: No i/o ports on a gameport? Weird\n");
		return -ENODEV;
	}

	ioport = pnp_port_start(dev,0);
	iolen = pnp_port_len(dev,0);

	if (!request_region(ioport, iolen, "ns558-pnp"))
		return -EBUSY;

	if (!(port = kmalloc(sizeof(struct ns558), GFP_KERNEL))) {
		printk(KERN_ERR "ns558: Memory allocation failed.\n");
		return -ENOMEM;
	}
	memset(port, 0, sizeof(struct ns558));

	port->type = NS558_PNP;
	port->size = iolen;
	port->dev = dev;

	port->gameport.io = ioport;
	port->gameport.phys = port->phys;
	port->gameport.name = port->name;
	port->gameport.id.bustype = BUS_ISAPNP;
	port->gameport.id.version = 0x100;

	sprintf(port->phys, "pnp%s/gameport0", dev->dev.bus_id);
	sprintf(port->name, "%s", "NS558 PnP Gameport");

	gameport_register_port(&port->gameport);

	printk(KERN_INFO "gameport: NS558 PnP at pnp%s io %#x",
		dev->dev.bus_id, port->gameport.io);
	if (iolen > 1) printk(" size %d", iolen);
	printk(" speed %d kHz\n", port->gameport.speed);

	list_add_tail(&port->node, &ns558_list);
	return 0;
}

static struct pnp_driver ns558_pnp_driver = {
	.name		= "ns558",
	.id_table	= pnp_devids,
	.probe		= ns558_pnp_probe,
};

#else

static struct pnp_driver ns558_pnp_driver;

#endif

int __init ns558_init(void)
{
	int i = 0;

/*
 * Probe for ISA ports.
 */

	while (ns558_isa_portlist[i])
		ns558_isa_probe(ns558_isa_portlist[i++]);

	pnp_register_driver(&ns558_pnp_driver);
	return list_empty(&ns558_list) ? -ENODEV : 0;
}

void __exit ns558_exit(void)
{
	struct ns558 *port;

	list_for_each_entry(port, &ns558_list, node) {
		gameport_unregister_port(&port->gameport);
		switch (port->type) {

#ifdef CONFIG_PNP
			case NS558_PNP:
				/* fall through */
#endif
			case NS558_ISA:
				release_region(port->gameport.io & ~(port->size - 1), port->size);
				kfree(port);
				break;

			default:
				break;
		}
	}
	pnp_unregister_driver(&ns558_pnp_driver);
}

module_init(ns558_init);
module_exit(ns558_exit);
