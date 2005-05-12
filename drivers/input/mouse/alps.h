/*
 * ALPS touchpad PS/2 mouse driver
 *
 * Copyright (c) 2003 Peter Osterlund <petero2@telia.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#ifndef _ALPS_H
#define _ALPS_H

int alps_detect(struct psmouse *psmouse, int set_properties);
int alps_init(struct psmouse *psmouse);

#endif
