/*
 * $Id: saa7134-dvb.c,v 1.4 2004/11/07 14:44:59 kraxel Exp $
 *
 * (c) 2004 Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/suspend.h>

#include "saa7134-reg.h"
#include "saa7134.h"

MODULE_AUTHOR("Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]");
MODULE_LICENSE("GPL");

/* ------------------------------------------------------------------ */

static int dvb_init(struct saa7134_dev *dev)
{
	printk("%s: %s\n",dev->name,__FUNCTION__);

	/* init struct videobuf_dvb */
	dev->dvb.name = dev->name;
	videobuf_queue_init(&dev->dvb.dvbq, &saa7134_ts_qops,
			    dev->pci, &dev->slock,
			    V4L2_BUF_TYPE_VIDEO_CAPTURE,
			    V4L2_FIELD_TOP,
			    sizeof(struct saa7134_buf),
			    dev);

	/* TODO: init frontend */
	if (NULL == dev->dvb.frontend)
		return -1;

	/* register everything else */
	return videobuf_dvb_register(&dev->dvb);
}

static int dvb_fini(struct saa7134_dev *dev)
{
	printk("%s: %s\n",dev->name,__FUNCTION__);
	videobuf_dvb_unregister(&dev->dvb);
	return 0;
}

static struct saa7134_mpeg_ops dvb_ops = {
	.type          = SAA7134_MPEG_DVB,
	.init          = dvb_init,
	.fini          = dvb_fini,
};

static int __init dvb_register(void)
{
	return saa7134_ts_register(&dvb_ops);
}

static void __exit dvb_unregister(void)
{
	saa7134_ts_unregister(&dvb_ops);
}

module_init(dvb_register);
module_exit(dvb_unregister);

/* ------------------------------------------------------------------ */
/*
 * Local variables:
 * c-basic-offset: 8
 * compile-command: "make DVB=1"
 * End:
 */
