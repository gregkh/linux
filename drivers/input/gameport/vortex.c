/*
 * $Id: vortex.c,v 1.5 2002/07/01 15:39:30 vojtech Exp $
 *
 *  Copyright (c) 2000-2001 Vojtech Pavlik
 *
 *  Based on the work of:
 *	Raymond Ingles
 */

/*
 * Trident 4DWave and Aureal Vortex gameport driver for Linux
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
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/gameport.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@ucw.cz>");
MODULE_DESCRIPTION("Aureal Vortex and Vortex2 gameport driver");
MODULE_LICENSE("GPL");

#define VORTEX_GCR		0x0c	/* Gameport control register */
#define VORTEX_LEG		0x08	/* Legacy port location */
#define VORTEX_AXD		0x10	/* Axes start */
#define VORTEX_DATA_WAIT	20	/* 20 ms */

struct vortex {
	struct gameport gameport;
	struct pci_dev *dev;
        unsigned char __iomem *base;
        unsigned char __iomem *io;
	char phys[32];
};

static unsigned char vortex_read(struct gameport *gameport)
{
	struct vortex *vortex = gameport->driver;
	return readb(vortex->io + VORTEX_LEG);
}

static void vortex_trigger(struct gameport *gameport)
{
	struct vortex *vortex = gameport->driver;
	writeb(0xff, vortex->io + VORTEX_LEG);
}

static int vortex_cooked_read(struct gameport *gameport, int *axes, int *buttons)
{
	struct vortex *vortex = gameport->driver;
	int i;

	*buttons = (~readb(vortex->base + VORTEX_LEG) >> 4) & 0xf;

	for (i = 0; i < 4; i++) {
		axes[i] = readw(vortex->io + VORTEX_AXD + i * sizeof(u32));
		if (axes[i] == 0x1fff) axes[i] = -1;
	}

        return 0;
}

static int vortex_open(struct gameport *gameport, int mode)
{
	struct vortex *vortex = gameport->driver;

	switch (mode) {
		case GAMEPORT_MODE_COOKED:
			writeb(0x40, vortex->io + VORTEX_GCR);
			msleep(VORTEX_DATA_WAIT);
			return 0;
		case GAMEPORT_MODE_RAW:
			writeb(0x00, vortex->io + VORTEX_GCR);
			return 0;
		default:
			return -1;
	}

	return 0;
}

static int __devinit vortex_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct vortex *vortex;
	int i;

	if (!(vortex = kmalloc(sizeof(struct vortex), GFP_KERNEL)))
		return -1;
        memset(vortex, 0, sizeof(struct vortex));

	vortex->dev = dev;
	sprintf(vortex->phys, "pci%s/gameport0", pci_name(dev));

	pci_set_drvdata(dev, vortex);

	vortex->gameport.driver = vortex;
	vortex->gameport.fuzz = 64;

	vortex->gameport.read = vortex_read;
	vortex->gameport.trigger = vortex_trigger;
	vortex->gameport.cooked_read = vortex_cooked_read;
	vortex->gameport.open = vortex_open;

	vortex->gameport.name = pci_name(dev);
	vortex->gameport.phys = vortex->phys;
	vortex->gameport.id.bustype = BUS_PCI;
	vortex->gameport.id.vendor = dev->vendor;
	vortex->gameport.id.product = dev->device;

	for (i = 0; i < 6; i++)
		if (~pci_resource_flags(dev, i) & IORESOURCE_IO)
			break;

	pci_enable_device(dev);

	vortex->base = ioremap(pci_resource_start(vortex->dev, i),
				pci_resource_len(vortex->dev, i));
	vortex->io = vortex->base + id->driver_data;

	gameport_register_port(&vortex->gameport);

	printk(KERN_INFO "gameport at pci%s speed %d kHz\n",
		pci_name(dev), vortex->gameport.speed);

	return 0;
}

static void __devexit vortex_remove(struct pci_dev *dev)
{
	struct vortex *vortex = pci_get_drvdata(dev);
	gameport_unregister_port(&vortex->gameport);
	iounmap(vortex->base);
	kfree(vortex);
}

static struct pci_device_id vortex_id_table[] =
{{ 0x12eb, 0x0001, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0x11000 },
 { 0x12eb, 0x0002, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0x28800 },
 { 0 }};

static struct pci_driver vortex_driver = {
	.name =		"vortex_gameport",
	.id_table =	vortex_id_table,
	.probe =	vortex_probe,
	.remove =	__devexit_p(vortex_remove),
};

int __init vortex_init(void)
{
	return pci_module_init(&vortex_driver);
}

void __exit vortex_exit(void)
{
	pci_unregister_driver(&vortex_driver);
}

module_init(vortex_init);
module_exit(vortex_exit);
