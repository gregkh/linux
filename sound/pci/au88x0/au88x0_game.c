/*
 * $Id: au88x0_game.c,v 1.9 2003/09/22 03:51:28 mjander Exp $
 *
 *  Manuel Jander.
 *
 *  Based on the work of:
 *  Vojtech Pavlik
 *  Raymond Ingles
 *
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
 * e-mail - mail your message to <vojtech@suse.cz>, or by paper mail:
 * Vojtech Pavlik, Ucitelska 1576, Prague 8, 182 00 Czech Republic
 *
 * Based 90% on Vojtech Pavlik pcigame driver.
 * Merged and modified by Manuel Jander, for the OpenVortex
 * driver. (email: mjander@embedded.cl).
 */

#include <sound/driver.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <sound/core.h>
#include "au88x0.h"
#include <linux/gameport.h>

#if defined(CONFIG_GAMEPORT) || (defined(MODULE) && defined(CONFIG_GAMEPORT_MODULE))

#define VORTEX_GAME_DWAIT	20	/* 20 ms */

static unsigned char vortex_game_read(struct gameport *gameport)
{
	vortex_t *vortex = gameport->driver;
	return hwread(vortex->mmio, VORTEX_GAME_LEGACY);
}

static void vortex_game_trigger(struct gameport *gameport)
{
	vortex_t *vortex = gameport->driver;
	hwwrite(vortex->mmio, VORTEX_GAME_LEGACY, 0xff);
}

static int
vortex_game_cooked_read(struct gameport *gameport, int *axes, int *buttons)
{
	vortex_t *vortex = gameport->driver;
	int i;

	*buttons = (~hwread(vortex->mmio, VORTEX_GAME_LEGACY) >> 4) & 0xf;

	for (i = 0; i < 4; i++) {
		axes[i] =
		    hwread(vortex->mmio, VORTEX_GAME_AXIS + (i * AXIS_SIZE));
		if (axes[i] == AXIS_RANGE)
			axes[i] = -1;
	}
	return 0;
}

static int vortex_game_open(struct gameport *gameport, int mode)
{
	vortex_t *vortex = gameport->driver;

	switch (mode) {
	case GAMEPORT_MODE_COOKED:
		hwwrite(vortex->mmio, VORTEX_CTRL2,
			hwread(vortex->mmio,
			       VORTEX_CTRL2) | CTRL2_GAME_ADCMODE);
		msleep(VORTEX_GAME_DWAIT);
		return 0;
	case GAMEPORT_MODE_RAW:
		hwwrite(vortex->mmio, VORTEX_CTRL2,
			hwread(vortex->mmio,
			       VORTEX_CTRL2) & ~CTRL2_GAME_ADCMODE);
		return 0;
	default:
		return -1;
	}

	return 0;
}

static int vortex_gameport_register(vortex_t * vortex)
{
	if ((vortex->gameport = kcalloc(1, sizeof(struct gameport), GFP_KERNEL)) == NULL) {
		return -1;
	};
	
	vortex->gameport->driver = vortex;
	vortex->gameport->fuzz = 64;

	vortex->gameport->read = vortex_game_read;
	vortex->gameport->trigger = vortex_game_trigger;
	vortex->gameport->cooked_read = vortex_game_cooked_read;
	vortex->gameport->open = vortex_game_open;

	gameport_register_port((struct gameport *)vortex->gameport);

/*	printk(KERN_INFO "gameport%d: %s at speed %d kHz\n",
		vortex->gameport->number, vortex->pci_dev->name, vortex->gameport->speed);
*/
	return 0;
}

static int vortex_gameport_unregister(vortex_t * vortex)
{
	if (vortex->gameport != NULL) {
		gameport_unregister_port(vortex->gameport);
		kfree(vortex->gameport);
	}
	return 0;
}

#else

static inline int vortex_gameport_register(vortex_t * vortex) { return 0; }
static inline int vortex_gameport_unregister(vortex_t * vortex) { return 0; }

#endif
