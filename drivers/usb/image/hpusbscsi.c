#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/smp_lock.h>
#include <linux/usb.h>
#include <asm/atomic.h>
#include <linux/blkdev.h>
#include "../../scsi/scsi.h"
#include <scsi/scsi_host.h>

#include "hpusbscsi.h"

#define DEBUG(x...) \
	printk( KERN_DEBUG x )

static char *states[]={"FREE", "BEGINNING", "WORKING", "ERROR", "WAIT", "PREMATURE"};

#define TRACE_STATE printk(KERN_DEBUG"hpusbscsi->state = %s at line %d\n", states[hpusbscsi->state], __LINE__)

static Scsi_Host_Template hpusbscsi_scsi_host_template = {
	.module			= THIS_MODULE,
	.name			= "hpusbscsi",
	.proc_name		= "hpusbscsi",
	.queuecommand		= hpusbscsi_scsi_queuecommand,
	.eh_abort_handler	= hpusbscsi_scsi_abort,
	.eh_host_reset_handler	= hpusbscsi_scsi_host_reset,
	.sg_tablesize		= SG_ALL,
	.can_queue		= 1,
	.this_id		= -1,
	.cmd_per_lun		= 1,
	.use_clustering		= 1,
	.emulated		= 1,
};

static int
hpusbscsi_usb_probe(struct usb_interface *intf,
		    const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_host_interface *altsetting =	intf->cur_altsetting;
	struct hpusbscsi *new;
	int error = -ENOMEM;
	int i;

	if (altsetting->desc.bNumEndpoints != 3) {
		printk (KERN_ERR "Wrong number of endpoints\n");
		return -ENODEV;
	}

	new = kmalloc(sizeof(struct hpusbscsi), GFP_KERNEL);
	if (!new)
		return -ENOMEM;
	memset(new, 0, sizeof(struct hpusbscsi));
	new->dataurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!new->dataurb)
		goto out_kfree;
	new->controlurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!new->controlurb)
		goto out_free_dataurb;

	new->dev = dev;
	init_waitqueue_head(&new->pending);
	init_waitqueue_head(&new->deathrow);

	error = -ENODEV;
	for (i = 0; i < altsetting->desc.bNumEndpoints; i++) {
		if ((altsetting->endpoint[i].desc.
		     bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
				USB_ENDPOINT_XFER_BULK) {
			if (altsetting->endpoint[i].desc.
			    bEndpointAddress & USB_DIR_IN) {
				new->ep_in =
					altsetting->endpoint[i].desc.
					bEndpointAddress &
					USB_ENDPOINT_NUMBER_MASK;
			} else {
				new->ep_out =
					altsetting->endpoint[i].desc.
					bEndpointAddress &
					USB_ENDPOINT_NUMBER_MASK;
			}
		} else {
			new->ep_int =
				altsetting->endpoint[i].desc.
				bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
			new->interrupt_interval= altsetting->endpoint[i].desc.
				bInterval;
		}
	}

	/* build and submit an interrupt URB for status byte handling */
 	usb_fill_int_urb(new->controlurb, new->dev,
			usb_rcvintpipe(new->dev, new->ep_int),
			&new->scsi_state_byte, 1,
			control_interrupt_callback,new,
			new->interrupt_interval);

	if (usb_submit_urb(new->controlurb, GFP_KERNEL) < 0)
		goto out_free_controlurb;

	/* In host->hostdata we store a pointer to desc */
	new->host = scsi_host_alloc(&hpusbscsi_scsi_host_template, sizeof(new));
	if (!new->host)
		goto out_kill_controlurb;

	new->host->hostdata[0] = (unsigned long)new;
	scsi_add_host(new->host, &intf->dev); /* XXX handle failure */
	scsi_scan_host(new->host);

	new->sense_command[0] = REQUEST_SENSE;
	new->sense_command[4] = HPUSBSCSI_SENSE_LENGTH;

	usb_set_intfdata(intf, new);
	return 0;

 out_kill_controlurb:
	usb_kill_urb(new->controlurb);
 out_free_controlurb:
	usb_free_urb(new->controlurb);
 out_free_dataurb:
	usb_free_urb(new->dataurb);
 out_kfree:
	kfree(new);
	return error;
}

static void
hpusbscsi_usb_disconnect(struct usb_interface *intf)
{
	struct hpusbscsi *desc = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);

	scsi_remove_host(desc->host);
	usb_kill_urb(desc->controlurb);
	scsi_host_put(desc->host);

	usb_free_urb(desc->controlurb);
	usb_free_urb(desc->dataurb);
	kfree(desc);
}

static struct usb_device_id hpusbscsi_usb_ids[] = {
	{USB_DEVICE (0x03f0, 0x0701)},	/* HP 53xx */
	{USB_DEVICE (0x03f0, 0x0801)},	/* HP 7400 */
	{USB_DEVICE (0x0638, 0x0268)},  /*iVina 1200U */
	{USB_DEVICE (0x0638, 0x026a)},  /*Scan Dual II */
	{USB_DEVICE (0x0638, 0x0A13)},  /*Avision AV600U */
	{USB_DEVICE (0x0638, 0x0A16)},  /*Avision DS610CU Scancopier */
	{USB_DEVICE (0x0638, 0x0A18)},  /*Avision AV600U Plus */
	{USB_DEVICE (0x0638, 0x0A23)},  /*Avision AV220 */
	{USB_DEVICE (0x0638, 0x0A24)},  /*Avision AV210 */
	{USB_DEVICE (0x0686, 0x4004)},  /*Minolta Elite II */
	{}			/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, hpusbscsi_usb_ids);
MODULE_LICENSE("GPL");


static struct usb_driver hpusbscsi_usb_driver = {
	.owner = THIS_MODULE,
	.name ="hpusbscsi",
	.probe =hpusbscsi_usb_probe,
	.disconnect =hpusbscsi_usb_disconnect,
	.id_table =hpusbscsi_usb_ids,
};

/* module initialisation */

static int __init
hpusbscsi_init (void)
{
	return usb_register(&hpusbscsi_usb_driver);
}

static void __exit
hpusbscsi_exit (void)
{
	usb_deregister(&hpusbscsi_usb_driver);
}

module_init (hpusbscsi_init);
module_exit (hpusbscsi_exit);

static int hpusbscsi_scsi_queuecommand (Scsi_Cmnd *srb, scsi_callback callback)
{
	struct hpusbscsi* hpusbscsi = (struct hpusbscsi*)(srb->device->host->hostdata[0]);
	usb_complete_t usb_callback;
	int res;

	/* we don't answer for anything but our single device on any faked host controller */
	if ( srb->device->lun || srb->device->id || srb->device->channel ) {
		if (callback) {
			srb->result = DID_BAD_TARGET;
			callback(srb);
		}
                	goto out;
	}

	/* Now we need to decide which callback to give to the urb we send the command with */

	if (!srb->bufflen) {
		if (srb->cmnd[0] == REQUEST_SENSE){
			hpusbscsi->current_data_pipe = usb_rcvbulkpipe(hpusbscsi->dev, hpusbscsi->ep_in);
			usb_callback = request_sense_callback;
		} else {
			usb_callback = simple_command_callback;
		}
	} else {
        	if (likely(srb->use_sg)) {
			usb_callback = scatter_gather_callback;
			hpusbscsi->fragment = 0;
		} else {
                	usb_callback = simple_payload_callback;
		}
		/* Now we find out which direction data is to be transferred in */
		hpusbscsi->current_data_pipe = DIRECTION_IS_IN(srb->cmnd[0]) ?
			usb_rcvbulkpipe(hpusbscsi->dev, hpusbscsi->ep_in)
		:
			usb_sndbulkpipe(hpusbscsi->dev, hpusbscsi->ep_out)
		;
	}


	TRACE_STATE;

        /* We zero the sense buffer to avoid confusing user space */
        memset(srb->sense_buffer, 0, SCSI_SENSE_BUFFERSIZE);

	hpusbscsi->state = HP_STATE_BEGINNING;
	TRACE_STATE;

	/* We prepare the urb for writing out the scsi command */
	usb_fill_bulk_urb(
		hpusbscsi->dataurb,
		hpusbscsi->dev,
		usb_sndbulkpipe(hpusbscsi->dev,hpusbscsi->ep_out),
		srb->cmnd,
		srb->cmd_len,
		usb_callback,
		hpusbscsi
	);
	hpusbscsi->scallback = callback;
	hpusbscsi->srb = srb;

	res = usb_submit_urb(hpusbscsi->dataurb, GFP_ATOMIC);
	if (unlikely(res)) {
		hpusbscsi->state = HP_STATE_FREE;
		TRACE_STATE;
		if (likely(callback != NULL)) {
			srb->result = DID_ERROR;
			callback(srb);
		}
	}

out:
	return 0;
}

static int hpusbscsi_scsi_host_reset (Scsi_Cmnd *srb)
{
	struct hpusbscsi* hpusbscsi = (struct hpusbscsi*)(srb->device->host->hostdata[0]);

	printk(KERN_DEBUG"SCSI reset requested.\n");
	//usb_reset_device(hpusbscsi->dev);
	//printk(KERN_DEBUG"SCSI reset completed.\n");
	hpusbscsi->state = HP_STATE_FREE;

	return 0;
}

static int hpusbscsi_scsi_abort (Scsi_Cmnd *srb)
{
	struct hpusbscsi* hpusbscsi = (struct hpusbscsi*)(srb->device->host->hostdata[0]);
	printk(KERN_DEBUG"Requested is canceled.\n");

	usb_kill_urb(hpusbscsi->dataurb);
	usb_kill_urb(hpusbscsi->controlurb);
	hpusbscsi->state = HP_STATE_FREE;

	return SCSI_ABORT_PENDING;
}

/* usb interrupt handlers - they are all running IN INTERRUPT ! */

static void handle_usb_error (struct hpusbscsi *hpusbscsi)
{
	if (likely(hpusbscsi->scallback != NULL)) {
		hpusbscsi->srb->result = DID_ERROR;
		hpusbscsi->scallback(hpusbscsi->srb);
	}
	hpusbscsi->state = HP_STATE_FREE;
}

static void  control_interrupt_callback (struct urb *u, struct pt_regs *regs)
{
	struct hpusbscsi * hpusbscsi = (struct hpusbscsi *)u->context;
	u8 scsi_state;

DEBUG("Getting status byte %d \n",hpusbscsi->scsi_state_byte);
	if(unlikely(u->status < 0)) {
                if (likely(hpusbscsi->state != HP_STATE_FREE))
                        handle_usb_error(hpusbscsi);
		if (u->status == -ECONNRESET || u->status == -ENOENT || u->status == -ESHUTDOWN)
			return;
		else
			goto resub;
	}

	scsi_state = hpusbscsi->scsi_state_byte;
	if (hpusbscsi->state != HP_STATE_ERROR) {
		hpusbscsi->srb->result &= SCSI_ERR_MASK;
		hpusbscsi->srb->result |= scsi_state;
	}

	if (scsi_state == CHECK_CONDITION << 1) {
		if (hpusbscsi->state == HP_STATE_WAIT) {
			issue_request_sense(hpusbscsi);
		} else {
			/* we request sense after an eventual data transfer */
			hpusbscsi->state = HP_STATE_ERROR;
		}
	}

	if (hpusbscsi->scallback != NULL && hpusbscsi->state == HP_STATE_WAIT && scsi_state != CHECK_CONDITION <<1 )
		/* we do a callback to the scsi layer if and only if all data has been transferred */
		hpusbscsi->scallback(hpusbscsi->srb);

	TRACE_STATE;
	switch (hpusbscsi->state) {
	case HP_STATE_WAIT:
		hpusbscsi->state = HP_STATE_FREE;
	TRACE_STATE;
		break;
	case HP_STATE_WORKING:
	case HP_STATE_BEGINNING:
		hpusbscsi->state = HP_STATE_PREMATURE;
	TRACE_STATE;
		break;
	case HP_STATE_ERROR:
		break;
	default:
		printk(KERN_ERR"hpusbscsi: Unexpected status report.\n");
	TRACE_STATE;
		hpusbscsi->state = HP_STATE_FREE;
	TRACE_STATE;
		break;
	}
resub:
	usb_submit_urb(u, GFP_ATOMIC);
}

static void simple_command_callback(struct urb *u, struct pt_regs *regs)
{
	struct hpusbscsi * hpusbscsi = (struct hpusbscsi *)u->context;
	if (unlikely(u->status<0)) {
		handle_usb_error(hpusbscsi);
		return;
        }
	TRACE_STATE;
	if (hpusbscsi->state != HP_STATE_PREMATURE) {
	        TRACE_STATE;
		hpusbscsi->state = HP_STATE_WAIT;
	} else {
		if (likely(hpusbscsi->scallback != NULL))
			hpusbscsi->scallback(hpusbscsi->srb);
		hpusbscsi->state = HP_STATE_FREE;
	TRACE_STATE;
	}
}

static void scatter_gather_callback(struct urb *u, struct pt_regs *regs)
{
	struct hpusbscsi * hpusbscsi = (struct hpusbscsi *)u->context;
        struct scatterlist *sg = hpusbscsi->srb->buffer;
        usb_complete_t callback;
        int res;

        DEBUG("Going through scatter/gather\n");
        if (unlikely(u->status < 0)) {
                handle_usb_error(hpusbscsi);
                return;
        }

        if (hpusbscsi->fragment + 1 != hpusbscsi->srb->use_sg)
                callback = scatter_gather_callback;
        else
                callback = simple_done;

	TRACE_STATE;
        if (hpusbscsi->state != HP_STATE_PREMATURE)
		hpusbscsi->state = HP_STATE_WORKING;
	TRACE_STATE;

        usb_fill_bulk_urb(
                u,
                hpusbscsi->dev,
                hpusbscsi->current_data_pipe,
                page_address(sg[hpusbscsi->fragment].page) +
		sg[hpusbscsi->fragment].offset,
                sg[hpusbscsi->fragment++].length,
                callback,
                hpusbscsi
        );

        res = usb_submit_urb(u, GFP_ATOMIC);
        if (unlikely(res))
                handle_usb_error(hpusbscsi);
	TRACE_STATE;
}

static void simple_done (struct urb *u, struct pt_regs *regs)
{
	struct hpusbscsi * hpusbscsi = (struct hpusbscsi *)u->context;

        if (unlikely(u->status < 0)) {
                handle_usb_error(hpusbscsi);
                return;
        }
        DEBUG("Data transfer done\n");
	TRACE_STATE;
	if (hpusbscsi->state != HP_STATE_PREMATURE) {
		if (unlikely(u->status < 0)) {
			handle_usb_error(hpusbscsi);
		} else {
			if (hpusbscsi->state != HP_STATE_ERROR) {
				hpusbscsi->state = HP_STATE_WAIT;
			} else {
				issue_request_sense(hpusbscsi);
			}
		}
	} else {
		if (likely(hpusbscsi->scallback != NULL))
			hpusbscsi->scallback(hpusbscsi->srb);
		hpusbscsi->state = HP_STATE_FREE;
	}
}

static void simple_payload_callback (struct urb *u, struct pt_regs *regs)
{
	struct hpusbscsi * hpusbscsi = (struct hpusbscsi *)u->context;
	int res;

	if (unlikely(u->status<0)) {
                handle_usb_error(hpusbscsi);
		return;
        }

	usb_fill_bulk_urb(
		u,
		hpusbscsi->dev,
		hpusbscsi->current_data_pipe,
		hpusbscsi->srb->buffer,
		hpusbscsi->srb->bufflen,
		simple_done,
		hpusbscsi
	);

	res = usb_submit_urb(u, GFP_ATOMIC);
	if (unlikely(res)) {
                handle_usb_error(hpusbscsi);
		return;
        }
	TRACE_STATE;
	if (hpusbscsi->state != HP_STATE_PREMATURE) {
		hpusbscsi->state = HP_STATE_WORKING;
	TRACE_STATE;
	} 
}

static void request_sense_callback (struct urb *u, struct pt_regs *regs)
{
	struct hpusbscsi * hpusbscsi = (struct hpusbscsi *)u->context;

	if (u->status<0) {
                handle_usb_error(hpusbscsi);
		return;
        }

	usb_fill_bulk_urb(
		u,
		hpusbscsi->dev,
		hpusbscsi->current_data_pipe,
		hpusbscsi->srb->sense_buffer,
		SCSI_SENSE_BUFFERSIZE,
		simple_done,
		hpusbscsi
	);

	if (0 > usb_submit_urb(u, GFP_ATOMIC)) {
		handle_usb_error(hpusbscsi);
		return;
	}
	if (hpusbscsi->state != HP_STATE_PREMATURE && hpusbscsi->state != HP_STATE_ERROR)
		hpusbscsi->state = HP_STATE_WORKING;
}

static void issue_request_sense (struct hpusbscsi *hpusbscsi)
{
	usb_fill_bulk_urb(
		hpusbscsi->dataurb,
		hpusbscsi->dev,
		usb_sndbulkpipe(hpusbscsi->dev, hpusbscsi->ep_out),
		&hpusbscsi->sense_command,
		SENSE_COMMAND_SIZE,
		request_sense_callback,
		hpusbscsi
	);

	hpusbscsi->current_data_pipe = usb_rcvbulkpipe(hpusbscsi->dev, hpusbscsi->ep_in);

	if (0 > usb_submit_urb(hpusbscsi->dataurb, GFP_ATOMIC)) {
		handle_usb_error(hpusbscsi);
	}
}


