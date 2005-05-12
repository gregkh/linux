/*
 * dvb-dibusb-pid.c is part of the driver for mobile USB Budget DVB-T devices 
 * based on reference design made by DiBcom (http://www.dibcom.fr/)
 *
 * Copyright (C) 2004-5 Patrick Boettcher (patrick.boettcher@desy.de)
 *
 * see dvb-dibusb-core.c for more copyright details.
 *
 * This file contains functions for initializing and handling the internal
 * pid-list. This pid-list mirrors the information currently stored in the
 * devices pid-list.
 */
#include "dvb-dibusb.h"

int dibusb_pid_list_init(struct usb_dibusb *dib)
{
	int i;
	dib->pid_list = kmalloc(sizeof(struct dibusb_pid) * dib->dibdev->dev_cl->demod->pid_filter_count,GFP_KERNEL);
	if (dib->pid_list == NULL)
		return -ENOMEM;

	deb_xfer("initializing %d pids for the pid_list.\n",dib->dibdev->dev_cl->demod->pid_filter_count);
	
	dib->pid_list_lock = SPIN_LOCK_UNLOCKED;
	memset(dib->pid_list,0,dib->dibdev->dev_cl->demod->pid_filter_count*(sizeof(struct dibusb_pid)));
	for (i=0; i < dib->dibdev->dev_cl->demod->pid_filter_count; i++) {
		dib->pid_list[i].index = i;
		dib->pid_list[i].pid = 0;
		dib->pid_list[i].active = 0;
	}

	dib->init_state |= DIBUSB_STATE_PIDLIST;
	return 0;
}

void dibusb_pid_list_exit(struct usb_dibusb *dib)
{
	if (dib->init_state & DIBUSB_STATE_PIDLIST)
		kfree(dib->pid_list);
	dib->init_state &= ~DIBUSB_STATE_PIDLIST;
}

/* fetch a pid from pid_list and set it on or off */
int dibusb_ctrl_pid(struct usb_dibusb *dib, struct dvb_demux_feed *dvbdmxfeed , int onoff)
{
	int i,ret = -1;
	unsigned long flags;
	u16 pid = dvbdmxfeed->pid;

	if (onoff) {
		spin_lock_irqsave(&dib->pid_list_lock,flags);
		for (i=0; i < dib->dibdev->dev_cl->demod->pid_filter_count; i++)
			if (!dib->pid_list[i].active) {
				dib->pid_list[i].pid = pid;
				dib->pid_list[i].active = 1;
				ret = i;
				break;
			}
		dvbdmxfeed->priv = &dib->pid_list[ret];
		spin_unlock_irqrestore(&dib->pid_list_lock,flags);
		
		if (dib->xfer_ops.pid_ctrl != NULL) 
			dib->xfer_ops.pid_ctrl(dib->fe,dib->pid_list[ret].index,dib->pid_list[ret].pid,1);
	} else {
		struct dibusb_pid *dpid = dvbdmxfeed->priv;
		
		if (dib->xfer_ops.pid_ctrl != NULL) 
			dib->xfer_ops.pid_ctrl(dib->fe,dpid->index,0,0);
		
		dpid->pid = 0;
		dpid->active = 0;
		ret = dpid->index;
	}
	
	/* a free pid from the list */
	deb_info("setting pid: %5d %04x at index %d '%s'\n",pid,pid,ret,onoff ? "on" : "off");

	return ret;
}

