/*
 * Universal Host Controller Interface driver for USB.
 *
 * Maintainer: Alan Stern <stern@rowland.harvard.edu>
 *
 * (C) Copyright 1999 Linus Torvalds
 * (C) Copyright 1999-2002 Johannes Erdfelt, johannes@erdfelt.com
 * (C) Copyright 1999 Randy Dunlap
 * (C) Copyright 1999 Georg Acher, acher@in.tum.de
 * (C) Copyright 1999 Deti Fliegl, deti@fliegl.de
 * (C) Copyright 1999 Thomas Sailer, sailer@ife.ee.ethz.ch
 * (C) Copyright 1999 Roman Weissgaerber, weissg@vienna.at
 * (C) Copyright 2000 Yggdrasil Computing, Inc. (port of new PCI interface
 *               support from usb-ohci.c by Adam Richter, adam@yggdrasil.com).
 * (C) Copyright 1999 Gregory P. Smith (from usb-ohci.c)
 * (C) Copyright 2004 Alan Stern, stern@rowland.harvard.edu
 *
 * Intel documents this fairly well, and as far as I know there
 * are no royalties or anything like that, but even so there are
 * people who decided that they want to do the same thing in a
 * completely different way.
 *
 * WARNING! The USB documentation is downright evil. Most of it
 * is just crap, written by a committee. You're better off ignoring
 * most of it, the important stuff is:
 *  - the low-level protocol (fairly simple but lots of small details)
 *  - working around the horridness of the rest
 */

#include <linux/config.h>
#ifdef CONFIG_USB_DEBUG
#define DEBUG
#else
#undef DEBUG
#endif
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/pm.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>
#include <linux/usb.h>
#include <linux/bitops.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/system.h>

#include "../core/hcd.h"
#include "uhci-hcd.h"

/*
 * Version Information
 */
#define DRIVER_VERSION "v2.2"
#define DRIVER_AUTHOR "Linus 'Frodo Rabbit' Torvalds, Johannes Erdfelt, \
Randy Dunlap, Georg Acher, Deti Fliegl, Thomas Sailer, Roman Weissgaerber, \
Alan Stern"
#define DRIVER_DESC "USB Universal Host Controller Interface driver"

/*
 * debug = 0, no debugging messages
 * debug = 1, dump failed URB's except for stalls
 * debug = 2, dump all failed URB's (including stalls)
 *            show all queues in /debug/uhci/[pci_addr]
 * debug = 3, show all TD's in URB's when dumping
 */
#ifdef DEBUG
static int debug = 1;
#else
static int debug = 0;
#endif
module_param(debug, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug level");
static char *errbuf;
#define ERRBUF_LEN    (32 * 1024)

#include "uhci-hub.c"
#include "uhci-debug.c"

static kmem_cache_t *uhci_up_cachep;	/* urb_priv */

static unsigned int uhci_get_current_frame_number(struct uhci_hcd *uhci);
static int uhci_urb_dequeue(struct usb_hcd *hcd, struct urb *urb);
static void uhci_unlink_generic(struct uhci_hcd *uhci, struct urb *urb);
static void uhci_remove_pending_urbps(struct uhci_hcd *uhci);
static void uhci_finish_completion(struct usb_hcd *hcd, struct pt_regs *regs);
static void uhci_free_pending_qhs(struct uhci_hcd *uhci);
static void uhci_free_pending_tds(struct uhci_hcd *uhci);

static void hc_state_transitions(struct uhci_hcd *uhci);

/* If a transfer is still active after this much time, turn off FSBR */
#define IDLE_TIMEOUT	msecs_to_jiffies(50)
#define FSBR_DELAY	msecs_to_jiffies(50)

/* When we timeout an idle transfer for FSBR, we'll switch it over to */
/* depth first traversal. We'll do it in groups of this number of TD's */
/* to make sure it doesn't hog all of the bandwidth */
#define DEPTH_INTERVAL 5

/*
 * Technically, updating td->status here is a race, but it's not really a
 * problem. The worst that can happen is that we set the IOC bit again
 * generating a spurious interrupt. We could fix this by creating another
 * QH and leaving the IOC bit always set, but then we would have to play
 * games with the FSBR code to make sure we get the correct order in all
 * the cases. I don't think it's worth the effort
 */
static inline void uhci_set_next_interrupt(struct uhci_hcd *uhci)
{
	uhci->term_td->status |= cpu_to_le32(TD_CTRL_IOC); 
}

static inline void uhci_clear_next_interrupt(struct uhci_hcd *uhci)
{
	uhci->term_td->status &= ~cpu_to_le32(TD_CTRL_IOC);
}

static inline void uhci_moveto_complete(struct uhci_hcd *uhci, 
					struct urb_priv *urbp)
{
	list_move_tail(&urbp->urb_list, &uhci->complete_list);
}

static struct uhci_td *uhci_alloc_td(struct uhci_hcd *uhci, struct usb_device *dev)
{
	dma_addr_t dma_handle;
	struct uhci_td *td;

	td = dma_pool_alloc(uhci->td_pool, GFP_ATOMIC, &dma_handle);
	if (!td)
		return NULL;

	td->dma_handle = dma_handle;

	td->link = UHCI_PTR_TERM;
	td->buffer = 0;

	td->frame = -1;
	td->dev = dev;

	INIT_LIST_HEAD(&td->list);
	INIT_LIST_HEAD(&td->remove_list);
	INIT_LIST_HEAD(&td->fl_list);

	usb_get_dev(dev);

	return td;
}

static inline void uhci_fill_td(struct uhci_td *td, u32 status,
		u32 token, u32 buffer)
{
	td->status = cpu_to_le32(status);
	td->token = cpu_to_le32(token);
	td->buffer = cpu_to_le32(buffer);
}

/*
 * We insert Isochronous URB's directly into the frame list at the beginning
 */
static void uhci_insert_td_frame_list(struct uhci_hcd *uhci, struct uhci_td *td, unsigned framenum)
{
	framenum &= (UHCI_NUMFRAMES - 1);

	td->frame = framenum;

	/* Is there a TD already mapped there? */
	if (uhci->fl->frame_cpu[framenum]) {
		struct uhci_td *ftd, *ltd;

		ftd = uhci->fl->frame_cpu[framenum];
		ltd = list_entry(ftd->fl_list.prev, struct uhci_td, fl_list);

		list_add_tail(&td->fl_list, &ftd->fl_list);

		td->link = ltd->link;
		wmb();
		ltd->link = cpu_to_le32(td->dma_handle);
	} else {
		td->link = uhci->fl->frame[framenum];
		wmb();
		uhci->fl->frame[framenum] = cpu_to_le32(td->dma_handle);
		uhci->fl->frame_cpu[framenum] = td;
	}
}

static void uhci_remove_td(struct uhci_hcd *uhci, struct uhci_td *td)
{
	/* If it's not inserted, don't remove it */
	if (td->frame == -1 && list_empty(&td->fl_list))
		return;

	if (td->frame != -1 && uhci->fl->frame_cpu[td->frame] == td) {
		if (list_empty(&td->fl_list)) {
			uhci->fl->frame[td->frame] = td->link;
			uhci->fl->frame_cpu[td->frame] = NULL;
		} else {
			struct uhci_td *ntd;

			ntd = list_entry(td->fl_list.next, struct uhci_td, fl_list);
			uhci->fl->frame[td->frame] = cpu_to_le32(ntd->dma_handle);
			uhci->fl->frame_cpu[td->frame] = ntd;
		}
	} else {
		struct uhci_td *ptd;

		ptd = list_entry(td->fl_list.prev, struct uhci_td, fl_list);
		ptd->link = td->link;
	}

	wmb();
	td->link = UHCI_PTR_TERM;

	list_del_init(&td->fl_list);
	td->frame = -1;
}

/*
 * Inserts a td list into qh.
 */
static void uhci_insert_tds_in_qh(struct uhci_qh *qh, struct urb *urb, __le32 breadth)
{
	struct urb_priv *urbp = (struct urb_priv *)urb->hcpriv;
	struct uhci_td *td;
	__le32 *plink;

	/* Ordering isn't important here yet since the QH hasn't been */
	/* inserted into the schedule yet */
	plink = &qh->element;
	list_for_each_entry(td, &urbp->td_list, list) {
		*plink = cpu_to_le32(td->dma_handle) | breadth;
		plink = &td->link;
	}
	*plink = UHCI_PTR_TERM;
}

static void uhci_free_td(struct uhci_hcd *uhci, struct uhci_td *td)
{
	if (!list_empty(&td->list))
		dev_warn(uhci_dev(uhci), "td %p still in list!\n", td);
	if (!list_empty(&td->remove_list))
		dev_warn(uhci_dev(uhci), "td %p still in remove_list!\n", td);
	if (!list_empty(&td->fl_list))
		dev_warn(uhci_dev(uhci), "td %p still in fl_list!\n", td);

	if (td->dev)
		usb_put_dev(td->dev);

	dma_pool_free(uhci->td_pool, td, td->dma_handle);
}

static struct uhci_qh *uhci_alloc_qh(struct uhci_hcd *uhci, struct usb_device *dev)
{
	dma_addr_t dma_handle;
	struct uhci_qh *qh;

	qh = dma_pool_alloc(uhci->qh_pool, GFP_ATOMIC, &dma_handle);
	if (!qh)
		return NULL;

	qh->dma_handle = dma_handle;

	qh->element = UHCI_PTR_TERM;
	qh->link = UHCI_PTR_TERM;

	qh->dev = dev;
	qh->urbp = NULL;

	INIT_LIST_HEAD(&qh->list);
	INIT_LIST_HEAD(&qh->remove_list);

	usb_get_dev(dev);

	return qh;
}

static void uhci_free_qh(struct uhci_hcd *uhci, struct uhci_qh *qh)
{
	if (!list_empty(&qh->list))
		dev_warn(uhci_dev(uhci), "qh %p list not empty!\n", qh);
	if (!list_empty(&qh->remove_list))
		dev_warn(uhci_dev(uhci), "qh %p still in remove_list!\n", qh);

	if (qh->dev)
		usb_put_dev(qh->dev);

	dma_pool_free(uhci->qh_pool, qh, qh->dma_handle);
}

/*
 * Append this urb's qh after the last qh in skelqh->list
 *
 * Note that urb_priv.queue_list doesn't have a separate queue head;
 * it's a ring with every element "live".
 */
static void uhci_insert_qh(struct uhci_hcd *uhci, struct uhci_qh *skelqh, struct urb *urb)
{
	struct urb_priv *urbp = (struct urb_priv *)urb->hcpriv;
	struct urb_priv *turbp;
	struct uhci_qh *lqh;

	/* Grab the last QH */
	lqh = list_entry(skelqh->list.prev, struct uhci_qh, list);

	/* Point to the next skelqh */
	urbp->qh->link = lqh->link;
	wmb();				/* Ordering is important */

	/*
	 * Patch QHs for previous endpoint's queued URBs?  HC goes
	 * here next, not to the next skelqh it now points to.
	 *
	 *    lqh --> td ... --> qh ... --> td --> qh ... --> td
	 *     |                 |                 |
	 *     v                 v                 v
	 *     +<----------------+-----------------+
	 *     v
	 *    newqh --> td ... --> td
	 *     |
	 *     v
	 *    ...
	 *
	 * The HC could see (and use!) any of these as we write them.
	 */
	lqh->link = cpu_to_le32(urbp->qh->dma_handle) | UHCI_PTR_QH;
	if (lqh->urbp) {
		list_for_each_entry(turbp, &lqh->urbp->queue_list, queue_list)
			turbp->qh->link = lqh->link;
	}

	list_add_tail(&urbp->qh->list, &skelqh->list);
}

/*
 * Start removal of QH from schedule; it finishes next frame.
 * TDs should be unlinked before this is called.
 */
static void uhci_remove_qh(struct uhci_hcd *uhci, struct uhci_qh *qh)
{
	struct uhci_qh *pqh;
	__le32 newlink;
	unsigned int age;

	if (!qh)
		return;

	/*
	 * Only go through the hoops if it's actually linked in
	 */
	if (!list_empty(&qh->list)) {

		/* If our queue is nonempty, make the next URB the head */
		if (!list_empty(&qh->urbp->queue_list)) {
			struct urb_priv *nurbp;

			nurbp = list_entry(qh->urbp->queue_list.next,
					struct urb_priv, queue_list);
			nurbp->queued = 0;
			list_add(&nurbp->qh->list, &qh->list);
			newlink = cpu_to_le32(nurbp->qh->dma_handle) | UHCI_PTR_QH;
		} else
			newlink = qh->link;

		/* Fix up the previous QH's queue to link to either
		 * the new head of this queue or the start of the
		 * next endpoint's queue. */
		pqh = list_entry(qh->list.prev, struct uhci_qh, list);
		pqh->link = newlink;
		if (pqh->urbp) {
			struct urb_priv *turbp;

			list_for_each_entry(turbp, &pqh->urbp->queue_list,
					queue_list)
				turbp->qh->link = newlink;
		}
		wmb();

		/* Leave qh->link in case the HC is on the QH now, it will */
		/* continue the rest of the schedule */
		qh->element = UHCI_PTR_TERM;

		list_del_init(&qh->list);
	}

	list_del_init(&qh->urbp->queue_list);
	qh->urbp = NULL;

	age = uhci_get_current_frame_number(uhci);
	if (age != uhci->qh_remove_age) {
		uhci_free_pending_qhs(uhci);
		uhci->qh_remove_age = age;
	}

	/* Check to see if the remove list is empty. Set the IOC bit */
	/* to force an interrupt so we can remove the QH */
	if (list_empty(&uhci->qh_remove_list))
		uhci_set_next_interrupt(uhci);

	list_add(&qh->remove_list, &uhci->qh_remove_list);
}

static int uhci_fixup_toggle(struct urb *urb, unsigned int toggle)
{
	struct urb_priv *urbp = (struct urb_priv *)urb->hcpriv;
	struct uhci_td *td;

	list_for_each_entry(td, &urbp->td_list, list) {
		if (toggle)
			td->token |= cpu_to_le32(TD_TOKEN_TOGGLE);
		else
			td->token &= ~cpu_to_le32(TD_TOKEN_TOGGLE);

		toggle ^= 1;
	}

	return toggle;
}

/* This function will append one URB's QH to another URB's QH. This is for */
/* queuing interrupt, control or bulk transfers */
static void uhci_append_queued_urb(struct uhci_hcd *uhci, struct urb *eurb, struct urb *urb)
{
	struct urb_priv *eurbp, *urbp, *furbp, *lurbp;
	struct uhci_td *lltd;

	eurbp = eurb->hcpriv;
	urbp = urb->hcpriv;

	/* Find the first URB in the queue */
	furbp = eurbp;
	if (eurbp->queued) {
		list_for_each_entry(furbp, &eurbp->queue_list, queue_list)
			if (!furbp->queued)
				break;
	}

	lurbp = list_entry(furbp->queue_list.prev, struct urb_priv, queue_list);

	lltd = list_entry(lurbp->td_list.prev, struct uhci_td, list);

	/* Control transfers always start with toggle 0 */
	if (!usb_pipecontrol(urb->pipe))
		usb_settoggle(urb->dev, usb_pipeendpoint(urb->pipe),
				usb_pipeout(urb->pipe),
				uhci_fixup_toggle(urb,
					uhci_toggle(td_token(lltd)) ^ 1));

	/* All qh's in the queue need to link to the next queue */
	urbp->qh->link = eurbp->qh->link;

	wmb();			/* Make sure we flush everything */

	lltd->link = cpu_to_le32(urbp->qh->dma_handle) | UHCI_PTR_QH;

	list_add_tail(&urbp->queue_list, &furbp->queue_list);

	urbp->queued = 1;
}

static void uhci_delete_queued_urb(struct uhci_hcd *uhci, struct urb *urb)
{
	struct urb_priv *urbp, *nurbp, *purbp, *turbp;
	struct uhci_td *pltd;
	unsigned int toggle;

	urbp = urb->hcpriv;

	if (list_empty(&urbp->queue_list))
		return;

	nurbp = list_entry(urbp->queue_list.next, struct urb_priv, queue_list);

	/*
	 * Fix up the toggle for the following URBs in the queue.
	 * Only needed for bulk and interrupt: control and isochronous
	 * endpoints don't propagate toggles between messages.
	 */
	if (usb_pipebulk(urb->pipe) || usb_pipeint(urb->pipe)) {
		if (!urbp->queued)
			/* We just set the toggle in uhci_unlink_generic */
			toggle = usb_gettoggle(urb->dev,
					usb_pipeendpoint(urb->pipe),
					usb_pipeout(urb->pipe));
		else {
			/* If we're in the middle of the queue, grab the */
			/* toggle from the TD previous to us */
			purbp = list_entry(urbp->queue_list.prev,
					struct urb_priv, queue_list);
			pltd = list_entry(purbp->td_list.prev,
					struct uhci_td, list);
			toggle = uhci_toggle(td_token(pltd)) ^ 1;
		}

		list_for_each_entry(turbp, &urbp->queue_list, queue_list) {
			if (!turbp->queued)
				break;
			toggle = uhci_fixup_toggle(turbp->urb, toggle);
		}

		usb_settoggle(urb->dev, usb_pipeendpoint(urb->pipe),
				usb_pipeout(urb->pipe), toggle);
	}

	if (urbp->queued) {
		/* We're somewhere in the middle (or end).  The case where
		 * we're at the head is handled in uhci_remove_qh(). */
		purbp = list_entry(urbp->queue_list.prev, struct urb_priv,
				queue_list);

		pltd = list_entry(purbp->td_list.prev, struct uhci_td, list);
		if (nurbp->queued)
			pltd->link = cpu_to_le32(nurbp->qh->dma_handle) | UHCI_PTR_QH;
		else
			/* The next URB happens to be the beginning, so */
			/*  we're the last, end the chain */
			pltd->link = UHCI_PTR_TERM;
	}

	/* urbp->queue_list is handled in uhci_remove_qh() */
}

static struct urb_priv *uhci_alloc_urb_priv(struct uhci_hcd *uhci, struct urb *urb)
{
	struct urb_priv *urbp;

	urbp = kmem_cache_alloc(uhci_up_cachep, SLAB_ATOMIC);
	if (!urbp)
		return NULL;

	memset((void *)urbp, 0, sizeof(*urbp));

	urbp->inserttime = jiffies;
	urbp->fsbrtime = jiffies;
	urbp->urb = urb;
	
	INIT_LIST_HEAD(&urbp->td_list);
	INIT_LIST_HEAD(&urbp->queue_list);
	INIT_LIST_HEAD(&urbp->urb_list);

	list_add_tail(&urbp->urb_list, &uhci->urb_list);

	urb->hcpriv = urbp;

	return urbp;
}

static void uhci_add_td_to_urb(struct urb *urb, struct uhci_td *td)
{
	struct urb_priv *urbp = (struct urb_priv *)urb->hcpriv;

	td->urb = urb;

	list_add_tail(&td->list, &urbp->td_list);
}

static void uhci_remove_td_from_urb(struct uhci_td *td)
{
	if (list_empty(&td->list))
		return;

	list_del_init(&td->list);

	td->urb = NULL;
}

static void uhci_destroy_urb_priv(struct uhci_hcd *uhci, struct urb *urb)
{
	struct uhci_td *td, *tmp;
	struct urb_priv *urbp;
	unsigned int age;

	urbp = (struct urb_priv *)urb->hcpriv;
	if (!urbp)
		return;

	if (!list_empty(&urbp->urb_list))
		dev_warn(uhci_dev(uhci), "urb %p still on uhci->urb_list "
				"or uhci->remove_list!\n", urb);

	age = uhci_get_current_frame_number(uhci);
	if (age != uhci->td_remove_age) {
		uhci_free_pending_tds(uhci);
		uhci->td_remove_age = age;
	}

	/* Check to see if the remove list is empty. Set the IOC bit */
	/* to force an interrupt so we can remove the TD's*/
	if (list_empty(&uhci->td_remove_list))
		uhci_set_next_interrupt(uhci);

	list_for_each_entry_safe(td, tmp, &urbp->td_list, list) {
		uhci_remove_td_from_urb(td);
		uhci_remove_td(uhci, td);
		list_add(&td->remove_list, &uhci->td_remove_list);
	}

	urb->hcpriv = NULL;
	kmem_cache_free(uhci_up_cachep, urbp);
}

static void uhci_inc_fsbr(struct uhci_hcd *uhci, struct urb *urb)
{
	struct urb_priv *urbp = (struct urb_priv *)urb->hcpriv;

	if ((!(urb->transfer_flags & URB_NO_FSBR)) && !urbp->fsbr) {
		urbp->fsbr = 1;
		if (!uhci->fsbr++ && !uhci->fsbrtimeout)
			uhci->skel_term_qh->link = cpu_to_le32(uhci->skel_fs_control_qh->dma_handle) | UHCI_PTR_QH;
	}
}

static void uhci_dec_fsbr(struct uhci_hcd *uhci, struct urb *urb)
{
	struct urb_priv *urbp = (struct urb_priv *)urb->hcpriv;

	if ((!(urb->transfer_flags & URB_NO_FSBR)) && urbp->fsbr) {
		urbp->fsbr = 0;
		if (!--uhci->fsbr)
			uhci->fsbrtimeout = jiffies + FSBR_DELAY;
	}
}

/*
 * Map status to standard result codes
 *
 * <status> is (td_status(td) & 0xF60000), a.k.a.
 * uhci_status_bits(td_status(td)).
 * Note: <status> does not include the TD_CTRL_NAK bit.
 * <dir_out> is True for output TDs and False for input TDs.
 */
static int uhci_map_status(int status, int dir_out)
{
	if (!status)
		return 0;
	if (status & TD_CTRL_BITSTUFF)			/* Bitstuff error */
		return -EPROTO;
	if (status & TD_CTRL_CRCTIMEO) {		/* CRC/Timeout */
		if (dir_out)
			return -EPROTO;
		else
			return -EILSEQ;
	}
	if (status & TD_CTRL_BABBLE)			/* Babble */
		return -EOVERFLOW;
	if (status & TD_CTRL_DBUFERR)			/* Buffer error */
		return -ENOSR;
	if (status & TD_CTRL_STALLED)			/* Stalled */
		return -EPIPE;
	WARN_ON(status & TD_CTRL_ACTIVE);		/* Active */
	return 0;
}

/*
 * Control transfers
 */
static int uhci_submit_control(struct uhci_hcd *uhci, struct urb *urb, struct urb *eurb)
{
	struct urb_priv *urbp = (struct urb_priv *)urb->hcpriv;
	struct uhci_td *td;
	struct uhci_qh *qh, *skelqh;
	unsigned long destination, status;
	int maxsze = usb_maxpacket(urb->dev, urb->pipe, usb_pipeout(urb->pipe));
	int len = urb->transfer_buffer_length;
	dma_addr_t data = urb->transfer_dma;

	/* The "pipe" thing contains the destination in bits 8--18 */
	destination = (urb->pipe & PIPE_DEVEP_MASK) | USB_PID_SETUP;

	/* 3 errors */
	status = TD_CTRL_ACTIVE | uhci_maxerr(3);
	if (urb->dev->speed == USB_SPEED_LOW)
		status |= TD_CTRL_LS;

	/*
	 * Build the TD for the control request setup packet
	 */
	td = uhci_alloc_td(uhci, urb->dev);
	if (!td)
		return -ENOMEM;

	uhci_add_td_to_urb(urb, td);
	uhci_fill_td(td, status, destination | uhci_explen(7),
		urb->setup_dma);

	/*
	 * If direction is "send", change the packet ID from SETUP (0x2D)
	 * to OUT (0xE1).  Else change it from SETUP to IN (0x69) and
	 * set Short Packet Detect (SPD) for all data packets.
	 */
	if (usb_pipeout(urb->pipe))
		destination ^= (USB_PID_SETUP ^ USB_PID_OUT);
	else {
		destination ^= (USB_PID_SETUP ^ USB_PID_IN);
		status |= TD_CTRL_SPD;
	}

	/*
	 * Build the DATA TD's
	 */
	while (len > 0) {
		int pktsze = len;

		if (pktsze > maxsze)
			pktsze = maxsze;

		td = uhci_alloc_td(uhci, urb->dev);
		if (!td)
			return -ENOMEM;

		/* Alternate Data0/1 (start with Data1) */
		destination ^= TD_TOKEN_TOGGLE;
	
		uhci_add_td_to_urb(urb, td);
		uhci_fill_td(td, status, destination | uhci_explen(pktsze - 1),
			data);

		data += pktsze;
		len -= pktsze;
	}

	/*
	 * Build the final TD for control status 
	 */
	td = uhci_alloc_td(uhci, urb->dev);
	if (!td)
		return -ENOMEM;

	/*
	 * It's IN if the pipe is an output pipe or we're not expecting
	 * data back.
	 */
	destination &= ~TD_TOKEN_PID_MASK;
	if (usb_pipeout(urb->pipe) || !urb->transfer_buffer_length)
		destination |= USB_PID_IN;
	else
		destination |= USB_PID_OUT;

	destination |= TD_TOKEN_TOGGLE;		/* End in Data1 */

	status &= ~TD_CTRL_SPD;

	uhci_add_td_to_urb(urb, td);
	uhci_fill_td(td, status | TD_CTRL_IOC,
		destination | uhci_explen(UHCI_NULL_DATA_SIZE), 0);

	qh = uhci_alloc_qh(uhci, urb->dev);
	if (!qh)
		return -ENOMEM;

	urbp->qh = qh;
	qh->urbp = urbp;

	uhci_insert_tds_in_qh(qh, urb, UHCI_PTR_BREADTH);

	/* Low-speed transfers get a different queue, and won't hog the bus.
	 * Also, some devices enumerate better without FSBR; the easiest way
	 * to do that is to put URBs on the low-speed queue while the device
	 * is in the DEFAULT state. */
	if (urb->dev->speed == USB_SPEED_LOW ||
			urb->dev->state == USB_STATE_DEFAULT)
		skelqh = uhci->skel_ls_control_qh;
	else {
		skelqh = uhci->skel_fs_control_qh;
		uhci_inc_fsbr(uhci, urb);
	}

	if (eurb)
		uhci_append_queued_urb(uhci, eurb, urb);
	else
		uhci_insert_qh(uhci, skelqh, urb);

	return -EINPROGRESS;
}

/*
 * If control-IN transfer was short, the status packet wasn't sent.
 * This routine changes the element pointer in the QH to point at the
 * status TD.  It's safe to do this even while the QH is live, because
 * the hardware only updates the element pointer following a successful
 * transfer.  The inactive TD for the short packet won't cause an update,
 * so the pointer won't get overwritten.  The next time the controller
 * sees this QH, it will send the status packet.
 */
static int usb_control_retrigger_status(struct uhci_hcd *uhci, struct urb *urb)
{
	struct urb_priv *urbp = (struct urb_priv *)urb->hcpriv;
	struct uhci_td *td;

	urbp->short_control_packet = 1;

	td = list_entry(urbp->td_list.prev, struct uhci_td, list);
	urbp->qh->element = cpu_to_le32(td->dma_handle);

	return -EINPROGRESS;
}


static int uhci_result_control(struct uhci_hcd *uhci, struct urb *urb)
{
	struct list_head *tmp, *head;
	struct urb_priv *urbp = urb->hcpriv;
	struct uhci_td *td;
	unsigned int status;
	int ret = 0;

	if (list_empty(&urbp->td_list))
		return -EINVAL;

	head = &urbp->td_list;

	if (urbp->short_control_packet) {
		tmp = head->prev;
		goto status_stage;
	}

	tmp = head->next;
	td = list_entry(tmp, struct uhci_td, list);

	/* The first TD is the SETUP stage, check the status, but skip */
	/*  the count */
	status = uhci_status_bits(td_status(td));
	if (status & TD_CTRL_ACTIVE)
		return -EINPROGRESS;

	if (status)
		goto td_error;

	urb->actual_length = 0;

	/* The rest of the TD's (but the last) are data */
	tmp = tmp->next;
	while (tmp != head && tmp->next != head) {
		unsigned int ctrlstat;

		td = list_entry(tmp, struct uhci_td, list);
		tmp = tmp->next;

		ctrlstat = td_status(td);
		status = uhci_status_bits(ctrlstat);
		if (status & TD_CTRL_ACTIVE)
			return -EINPROGRESS;

		urb->actual_length += uhci_actual_length(ctrlstat);

		if (status)
			goto td_error;

		/* Check to see if we received a short packet */
		if (uhci_actual_length(ctrlstat) <
				uhci_expected_length(td_token(td))) {
			if (urb->transfer_flags & URB_SHORT_NOT_OK) {
				ret = -EREMOTEIO;
				goto err;
			}

			if (uhci_packetid(td_token(td)) == USB_PID_IN)
				return usb_control_retrigger_status(uhci, urb);
			else
				return 0;
		}
	}

status_stage:
	td = list_entry(tmp, struct uhci_td, list);

	/* Control status stage */
	status = td_status(td);

#ifdef I_HAVE_BUGGY_APC_BACKUPS
	/* APC BackUPS Pro kludge */
	/* It tries to send all of the descriptor instead of the amount */
	/*  we requested */
	if (status & TD_CTRL_IOC &&	/* IOC is masked out by uhci_status_bits */
	    status & TD_CTRL_ACTIVE &&
	    status & TD_CTRL_NAK)
		return 0;
#endif

	status = uhci_status_bits(status);
	if (status & TD_CTRL_ACTIVE)
		return -EINPROGRESS;

	if (status)
		goto td_error;

	return 0;

td_error:
	ret = uhci_map_status(status, uhci_packetout(td_token(td)));

err:
	if ((debug == 1 && ret != -EPIPE) || debug > 1) {
		/* Some debugging code */
		dev_dbg(uhci_dev(uhci), "%s: failed with status %x\n",
				__FUNCTION__, status);

		if (errbuf) {
			/* Print the chain for debugging purposes */
			uhci_show_qh(urbp->qh, errbuf, ERRBUF_LEN, 0);

			lprintk(errbuf);
		}
	}

	return ret;
}

/*
 * Common submit for bulk and interrupt
 */
static int uhci_submit_common(struct uhci_hcd *uhci, struct urb *urb, struct urb *eurb, struct uhci_qh *skelqh)
{
	struct uhci_td *td;
	struct uhci_qh *qh;
	unsigned long destination, status;
	int maxsze = usb_maxpacket(urb->dev, urb->pipe, usb_pipeout(urb->pipe));
	int len = urb->transfer_buffer_length;
	struct urb_priv *urbp = (struct urb_priv *)urb->hcpriv;
	dma_addr_t data = urb->transfer_dma;

	if (len < 0)
		return -EINVAL;

	/* The "pipe" thing contains the destination in bits 8--18 */
	destination = (urb->pipe & PIPE_DEVEP_MASK) | usb_packetid(urb->pipe);

	status = uhci_maxerr(3) | TD_CTRL_ACTIVE;
	if (urb->dev->speed == USB_SPEED_LOW)
		status |= TD_CTRL_LS;
	if (usb_pipein(urb->pipe))
		status |= TD_CTRL_SPD;

	/*
	 * Build the DATA TD's
	 */
	do {	/* Allow zero length packets */
		int pktsze = maxsze;

		if (pktsze >= len) {
			pktsze = len;
			if (!(urb->transfer_flags & URB_SHORT_NOT_OK))
				status &= ~TD_CTRL_SPD;
		}

		td = uhci_alloc_td(uhci, urb->dev);
		if (!td)
			return -ENOMEM;

		uhci_add_td_to_urb(urb, td);
		uhci_fill_td(td, status, destination | uhci_explen(pktsze - 1) |
			(usb_gettoggle(urb->dev, usb_pipeendpoint(urb->pipe),
			 usb_pipeout(urb->pipe)) << TD_TOKEN_TOGGLE_SHIFT),
			data);

		data += pktsze;
		len -= maxsze;

		usb_dotoggle(urb->dev, usb_pipeendpoint(urb->pipe),
			usb_pipeout(urb->pipe));
	} while (len > 0);

	/*
	 * URB_ZERO_PACKET means adding a 0-length packet, if direction
	 * is OUT and the transfer_length was an exact multiple of maxsze,
	 * hence (len = transfer_length - N * maxsze) == 0
	 * however, if transfer_length == 0, the zero packet was already
	 * prepared above.
	 */
	if (usb_pipeout(urb->pipe) && (urb->transfer_flags & URB_ZERO_PACKET) &&
	    !len && urb->transfer_buffer_length) {
		td = uhci_alloc_td(uhci, urb->dev);
		if (!td)
			return -ENOMEM;

		uhci_add_td_to_urb(urb, td);
		uhci_fill_td(td, status, destination | uhci_explen(UHCI_NULL_DATA_SIZE) |
			(usb_gettoggle(urb->dev, usb_pipeendpoint(urb->pipe),
			 usb_pipeout(urb->pipe)) << TD_TOKEN_TOGGLE_SHIFT),
			data);

		usb_dotoggle(urb->dev, usb_pipeendpoint(urb->pipe),
			usb_pipeout(urb->pipe));
	}

	/* Set the interrupt-on-completion flag on the last packet.
	 * A more-or-less typical 4 KB URB (= size of one memory page)
	 * will require about 3 ms to transfer; that's a little on the
	 * fast side but not enough to justify delaying an interrupt
	 * more than 2 or 3 URBs, so we will ignore the URB_NO_INTERRUPT
	 * flag setting. */
	td->status |= cpu_to_le32(TD_CTRL_IOC);

	qh = uhci_alloc_qh(uhci, urb->dev);
	if (!qh)
		return -ENOMEM;

	urbp->qh = qh;
	qh->urbp = urbp;

	/* Always breadth first */
	uhci_insert_tds_in_qh(qh, urb, UHCI_PTR_BREADTH);

	if (eurb)
		uhci_append_queued_urb(uhci, eurb, urb);
	else
		uhci_insert_qh(uhci, skelqh, urb);

	return -EINPROGRESS;
}

/*
 * Common result for bulk and interrupt
 */
static int uhci_result_common(struct uhci_hcd *uhci, struct urb *urb)
{
	struct urb_priv *urbp = urb->hcpriv;
	struct uhci_td *td;
	unsigned int status = 0;
	int ret = 0;

	urb->actual_length = 0;

	list_for_each_entry(td, &urbp->td_list, list) {
		unsigned int ctrlstat = td_status(td);

		status = uhci_status_bits(ctrlstat);
		if (status & TD_CTRL_ACTIVE)
			return -EINPROGRESS;

		urb->actual_length += uhci_actual_length(ctrlstat);

		if (status)
			goto td_error;

		if (uhci_actual_length(ctrlstat) <
				uhci_expected_length(td_token(td))) {
			if (urb->transfer_flags & URB_SHORT_NOT_OK) {
				ret = -EREMOTEIO;
				goto err;
			} else
				return 0;
		}
	}

	return 0;

td_error:
	ret = uhci_map_status(status, uhci_packetout(td_token(td)));

err:
	/* 
	 * Enable this chunk of code if you want to see some more debugging.
	 * But be careful, it has the tendancy to starve out khubd and prevent
	 * disconnects from happening successfully if you have a slow debug
	 * log interface (like a serial console.
	 */
#if 0
	if ((debug == 1 && ret != -EPIPE) || debug > 1) {
		/* Some debugging code */
		dev_dbg(uhci_dev(uhci), "%s: failed with status %x\n",
				__FUNCTION__, status);

		if (errbuf) {
			/* Print the chain for debugging purposes */
			uhci_show_qh(urbp->qh, errbuf, ERRBUF_LEN, 0);

			lprintk(errbuf);
		}
	}
#endif
	return ret;
}

static inline int uhci_submit_bulk(struct uhci_hcd *uhci, struct urb *urb, struct urb *eurb)
{
	int ret;

	/* Can't have low-speed bulk transfers */
	if (urb->dev->speed == USB_SPEED_LOW)
		return -EINVAL;

	ret = uhci_submit_common(uhci, urb, eurb, uhci->skel_bulk_qh);
	if (ret == -EINPROGRESS)
		uhci_inc_fsbr(uhci, urb);

	return ret;
}

static inline int uhci_submit_interrupt(struct uhci_hcd *uhci, struct urb *urb, struct urb *eurb)
{
	/* USB 1.1 interrupt transfers only involve one packet per interval;
	 * that's the uhci_submit_common() "breadth first" policy.  Drivers
	 * can submit urbs of any length, but longer ones might need many
	 * intervals to complete.
	 */
	return uhci_submit_common(uhci, urb, eurb, uhci->skelqh[__interval_to_skel(urb->interval)]);
}

/*
 * Isochronous transfers
 */
static int isochronous_find_limits(struct uhci_hcd *uhci, struct urb *urb, unsigned int *start, unsigned int *end)
{
	struct urb *last_urb = NULL;
	struct urb_priv *up;
	int ret = 0;

	list_for_each_entry(up, &uhci->urb_list, urb_list) {
		struct urb *u = up->urb;

		/* look for pending URB's with identical pipe handle */
		if ((urb->pipe == u->pipe) && (urb->dev == u->dev) &&
		    (u->status == -EINPROGRESS) && (u != urb)) {
			if (!last_urb)
				*start = u->start_frame;
			last_urb = u;
		}
	}

	if (last_urb) {
		*end = (last_urb->start_frame + last_urb->number_of_packets *
				last_urb->interval) & (UHCI_NUMFRAMES-1);
		ret = 0;
	} else
		ret = -1;	/* no previous urb found */

	return ret;
}

static int isochronous_find_start(struct uhci_hcd *uhci, struct urb *urb)
{
	int limits;
	unsigned int start = 0, end = 0;

	if (urb->number_of_packets > 900)	/* 900? Why? */
		return -EFBIG;

	limits = isochronous_find_limits(uhci, urb, &start, &end);

	if (urb->transfer_flags & URB_ISO_ASAP) {
		if (limits)
			urb->start_frame =
					(uhci_get_current_frame_number(uhci) +
						10) & (UHCI_NUMFRAMES - 1);
		else
			urb->start_frame = end;
	} else {
		urb->start_frame &= (UHCI_NUMFRAMES - 1);
		/* FIXME: Sanity check */
	}

	return 0;
}

/*
 * Isochronous transfers
 */
static int uhci_submit_isochronous(struct uhci_hcd *uhci, struct urb *urb)
{
	struct uhci_td *td;
	int i, ret, frame;
	int status, destination;

	status = TD_CTRL_ACTIVE | TD_CTRL_IOS;
	destination = (urb->pipe & PIPE_DEVEP_MASK) | usb_packetid(urb->pipe);

	ret = isochronous_find_start(uhci, urb);
	if (ret)
		return ret;

	frame = urb->start_frame;
	for (i = 0; i < urb->number_of_packets; i++, frame += urb->interval) {
		if (!urb->iso_frame_desc[i].length)
			continue;

		td = uhci_alloc_td(uhci, urb->dev);
		if (!td)
			return -ENOMEM;

		uhci_add_td_to_urb(urb, td);
		uhci_fill_td(td, status, destination | uhci_explen(urb->iso_frame_desc[i].length - 1),
			urb->transfer_dma + urb->iso_frame_desc[i].offset);

		if (i + 1 >= urb->number_of_packets)
			td->status |= cpu_to_le32(TD_CTRL_IOC);

		uhci_insert_td_frame_list(uhci, td, frame);
	}

	return -EINPROGRESS;
}

static int uhci_result_isochronous(struct uhci_hcd *uhci, struct urb *urb)
{
	struct uhci_td *td;
	struct urb_priv *urbp = (struct urb_priv *)urb->hcpriv;
	int status;
	int i, ret = 0;

	urb->actual_length = 0;

	i = 0;
	list_for_each_entry(td, &urbp->td_list, list) {
		int actlength;
		unsigned int ctrlstat = td_status(td);

		if (ctrlstat & TD_CTRL_ACTIVE)
			return -EINPROGRESS;

		actlength = uhci_actual_length(ctrlstat);
		urb->iso_frame_desc[i].actual_length = actlength;
		urb->actual_length += actlength;

		status = uhci_map_status(uhci_status_bits(ctrlstat),
				usb_pipeout(urb->pipe));
		urb->iso_frame_desc[i].status = status;
		if (status) {
			urb->error_count++;
			ret = status;
		}

		i++;
	}

	return ret;
}

static struct urb *uhci_find_urb_ep(struct uhci_hcd *uhci, struct urb *urb)
{
	struct urb_priv *up;

	/* We don't match Isoc transfers since they are special */
	if (usb_pipeisoc(urb->pipe))
		return NULL;

	list_for_each_entry(up, &uhci->urb_list, urb_list) {
		struct urb *u = up->urb;

		if (u->dev == urb->dev && u->status == -EINPROGRESS) {
			/* For control, ignore the direction */
			if (usb_pipecontrol(urb->pipe) &&
			    (u->pipe & ~USB_DIR_IN) == (urb->pipe & ~USB_DIR_IN))
				return u;
			else if (u->pipe == urb->pipe)
				return u;
		}
	}

	return NULL;
}

static int uhci_urb_enqueue(struct usb_hcd *hcd,
		struct usb_host_endpoint *ep,
		struct urb *urb, int mem_flags)
{
	int ret;
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);
	unsigned long flags;
	struct urb *eurb;
	int bustime;

	spin_lock_irqsave(&uhci->schedule_lock, flags);

	ret = urb->status;
	if (ret != -EINPROGRESS)		/* URB already unlinked! */
		goto out;

	eurb = uhci_find_urb_ep(uhci, urb);

	if (!uhci_alloc_urb_priv(uhci, urb)) {
		ret = -ENOMEM;
		goto out;
	}

	switch (usb_pipetype(urb->pipe)) {
	case PIPE_CONTROL:
		ret = uhci_submit_control(uhci, urb, eurb);
		break;
	case PIPE_INTERRUPT:
		if (!eurb) {
			bustime = usb_check_bandwidth(urb->dev, urb);
			if (bustime < 0)
				ret = bustime;
			else {
				ret = uhci_submit_interrupt(uhci, urb, eurb);
				if (ret == -EINPROGRESS)
					usb_claim_bandwidth(urb->dev, urb, bustime, 0);
			}
		} else {	/* inherit from parent */
			urb->bandwidth = eurb->bandwidth;
			ret = uhci_submit_interrupt(uhci, urb, eurb);
		}
		break;
	case PIPE_BULK:
		ret = uhci_submit_bulk(uhci, urb, eurb);
		break;
	case PIPE_ISOCHRONOUS:
		bustime = usb_check_bandwidth(urb->dev, urb);
		if (bustime < 0) {
			ret = bustime;
			break;
		}

		ret = uhci_submit_isochronous(uhci, urb);
		if (ret == -EINPROGRESS)
			usb_claim_bandwidth(urb->dev, urb, bustime, 1);
		break;
	}

	if (ret != -EINPROGRESS) {
		/* Submit failed, so delete it from the urb_list */
		struct urb_priv *urbp = urb->hcpriv;

		list_del_init(&urbp->urb_list);
		uhci_destroy_urb_priv(uhci, urb);
	} else
		ret = 0;

out:
	spin_unlock_irqrestore(&uhci->schedule_lock, flags);
	return ret;
}

/*
 * Return the result of a transfer
 */
static void uhci_transfer_result(struct uhci_hcd *uhci, struct urb *urb)
{
	int ret = -EINPROGRESS;
	struct urb_priv *urbp;

	spin_lock(&urb->lock);

	urbp = (struct urb_priv *)urb->hcpriv;

	if (urb->status != -EINPROGRESS)	/* URB already dequeued */
		goto out;

	switch (usb_pipetype(urb->pipe)) {
	case PIPE_CONTROL:
		ret = uhci_result_control(uhci, urb);
		break;
	case PIPE_BULK:
	case PIPE_INTERRUPT:
		ret = uhci_result_common(uhci, urb);
		break;
	case PIPE_ISOCHRONOUS:
		ret = uhci_result_isochronous(uhci, urb);
		break;
	}

	if (ret == -EINPROGRESS)
		goto out;
	urb->status = ret;

	switch (usb_pipetype(urb->pipe)) {
	case PIPE_CONTROL:
	case PIPE_BULK:
	case PIPE_ISOCHRONOUS:
		/* Release bandwidth for Interrupt or Isoc. transfers */
		if (urb->bandwidth)
			usb_release_bandwidth(urb->dev, urb, 1);
		uhci_unlink_generic(uhci, urb);
		break;
	case PIPE_INTERRUPT:
		/* Release bandwidth for Interrupt or Isoc. transfers */
		/* Make sure we don't release if we have a queued URB */
		if (list_empty(&urbp->queue_list) && urb->bandwidth)
			usb_release_bandwidth(urb->dev, urb, 0);
		else
			/* bandwidth was passed on to queued URB, */
			/* so don't let usb_unlink_urb() release it */
			urb->bandwidth = 0;
		uhci_unlink_generic(uhci, urb);
		break;
	default:
		dev_info(uhci_dev(uhci), "%s: unknown pipe type %d "
				"for urb %p\n",
				__FUNCTION__, usb_pipetype(urb->pipe), urb);
	}

	/* Move it from uhci->urb_list to uhci->complete_list */
	uhci_moveto_complete(uhci, urbp);

out:
	spin_unlock(&urb->lock);
}

static void uhci_unlink_generic(struct uhci_hcd *uhci, struct urb *urb)
{
	struct list_head *head;
	struct uhci_td *td;
	struct urb_priv *urbp = (struct urb_priv *)urb->hcpriv;
	int prevactive = 0;

	uhci_dec_fsbr(uhci, urb);	/* Safe since it checks */

	/*
	 * Now we need to find out what the last successful toggle was
	 * so we can update the local data toggle for the next transfer
	 *
	 * There are 2 ways the last successful completed TD is found:
	 *
	 * 1) The TD is NOT active and the actual length < expected length
	 * 2) The TD is NOT active and it's the last TD in the chain
	 *
	 * and a third way the first uncompleted TD is found:
	 *
	 * 3) The TD is active and the previous TD is NOT active
	 *
	 * Control and Isochronous ignore the toggle, so this is safe
	 * for all types
	 *
	 * FIXME: The toggle fixups won't be 100% reliable until we
	 * change over to using a single queue for each endpoint and
	 * stop the queue before unlinking.
	 */
	head = &urbp->td_list;
	list_for_each_entry(td, head, list) {
		unsigned int ctrlstat = td_status(td);

		if (!(ctrlstat & TD_CTRL_ACTIVE) &&
				(uhci_actual_length(ctrlstat) <
				 uhci_expected_length(td_token(td)) ||
				td->list.next == head))
			usb_settoggle(urb->dev, uhci_endpoint(td_token(td)),
				uhci_packetout(td_token(td)),
				uhci_toggle(td_token(td)) ^ 1);
		else if ((ctrlstat & TD_CTRL_ACTIVE) && !prevactive)
			usb_settoggle(urb->dev, uhci_endpoint(td_token(td)),
				uhci_packetout(td_token(td)),
				uhci_toggle(td_token(td)));

		prevactive = ctrlstat & TD_CTRL_ACTIVE;
	}

	uhci_delete_queued_urb(uhci, urb);

	/* The interrupt loop will reclaim the QH's */
	uhci_remove_qh(uhci, urbp->qh);
	urbp->qh = NULL;
}

static int uhci_urb_dequeue(struct usb_hcd *hcd, struct urb *urb)
{
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);
	unsigned long flags;
	struct urb_priv *urbp;
	unsigned int age;

	spin_lock_irqsave(&uhci->schedule_lock, flags);
	urbp = urb->hcpriv;
	if (!urbp)			/* URB was never linked! */
		goto done;
	list_del_init(&urbp->urb_list);

	uhci_unlink_generic(uhci, urb);

	age = uhci_get_current_frame_number(uhci);
	if (age != uhci->urb_remove_age) {
		uhci_remove_pending_urbps(uhci);
		uhci->urb_remove_age = age;
	}

	/* If we're the first, set the next interrupt bit */
	if (list_empty(&uhci->urb_remove_list))
		uhci_set_next_interrupt(uhci);
	list_add_tail(&urbp->urb_list, &uhci->urb_remove_list);

done:
	spin_unlock_irqrestore(&uhci->schedule_lock, flags);
	return 0;
}

static int uhci_fsbr_timeout(struct uhci_hcd *uhci, struct urb *urb)
{
	struct urb_priv *urbp = (struct urb_priv *)urb->hcpriv;
	struct list_head *head;
	struct uhci_td *td;
	int count = 0;

	uhci_dec_fsbr(uhci, urb);

	urbp->fsbr_timeout = 1;

	/*
	 * Ideally we would want to fix qh->element as well, but it's
	 * read/write by the HC, so that can introduce a race. It's not
	 * really worth the hassle
	 */

	head = &urbp->td_list;
	list_for_each_entry(td, head, list) {
		/*
		 * Make sure we don't do the last one (since it'll have the
		 * TERM bit set) as well as we skip every so many TD's to
		 * make sure it doesn't hog the bandwidth
		 */
		if (td->list.next != head && (count % DEPTH_INTERVAL) ==
				(DEPTH_INTERVAL - 1))
			td->link |= UHCI_PTR_DEPTH;

		count++;
	}

	return 0;
}

/*
 * uhci_get_current_frame_number()
 *
 * returns the current frame number for a USB bus/controller.
 */
static unsigned int uhci_get_current_frame_number(struct uhci_hcd *uhci)
{
	return inw(uhci->io_addr + USBFRNUM);
}

static int init_stall_timer(struct usb_hcd *hcd);

static void stall_callback(unsigned long ptr)
{
	struct usb_hcd *hcd = (struct usb_hcd *)ptr;
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);
	struct urb_priv *up;
	unsigned long flags;
	int called_uhci_finish_completion = 0;

	spin_lock_irqsave(&uhci->schedule_lock, flags);
	if (!list_empty(&uhci->urb_remove_list) &&
	    uhci_get_current_frame_number(uhci) != uhci->urb_remove_age) {
		uhci_remove_pending_urbps(uhci);
		uhci_finish_completion(hcd, NULL);
		called_uhci_finish_completion = 1;
	}

	list_for_each_entry(up, &uhci->urb_list, urb_list) {
		struct urb *u = up->urb;

		spin_lock(&u->lock);

		/* Check if the FSBR timed out */
		if (up->fsbr && !up->fsbr_timeout && time_after_eq(jiffies, up->fsbrtime + IDLE_TIMEOUT))
			uhci_fsbr_timeout(uhci, u);

		spin_unlock(&u->lock);
	}
	spin_unlock_irqrestore(&uhci->schedule_lock, flags);

	/* Wake up anyone waiting for an URB to complete */
	if (called_uhci_finish_completion)
		wake_up_all(&uhci->waitqh);

	/* Really disable FSBR */
	if (!uhci->fsbr && uhci->fsbrtimeout && time_after_eq(jiffies, uhci->fsbrtimeout)) {
		uhci->fsbrtimeout = 0;
		uhci->skel_term_qh->link = UHCI_PTR_TERM;
	}

	/* Poll for and perform state transitions */
	hc_state_transitions(uhci);
	if (unlikely(uhci->suspended_ports && uhci->state != UHCI_SUSPENDED))
		uhci_check_resume(uhci);

	init_stall_timer(hcd);
}

static int init_stall_timer(struct usb_hcd *hcd)
{
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);

	init_timer(&uhci->stall_timer);
	uhci->stall_timer.function = stall_callback;
	uhci->stall_timer.data = (unsigned long)hcd;
	uhci->stall_timer.expires = jiffies + msecs_to_jiffies(100);
	add_timer(&uhci->stall_timer);

	return 0;
}

static void uhci_free_pending_qhs(struct uhci_hcd *uhci)
{
	struct uhci_qh *qh, *tmp;

	list_for_each_entry_safe(qh, tmp, &uhci->qh_remove_list, remove_list) {
		list_del_init(&qh->remove_list);

		uhci_free_qh(uhci, qh);
	}
}

static void uhci_free_pending_tds(struct uhci_hcd *uhci)
{
	struct uhci_td *td, *tmp;

	list_for_each_entry_safe(td, tmp, &uhci->td_remove_list, remove_list) {
		list_del_init(&td->remove_list);

		uhci_free_td(uhci, td);
	}
}

static void
uhci_finish_urb(struct usb_hcd *hcd, struct urb *urb, struct pt_regs *regs)
__releases(uhci->schedule_lock)
__acquires(uhci->schedule_lock)
{
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);

	uhci_destroy_urb_priv(uhci, urb);

	spin_unlock(&uhci->schedule_lock);
	usb_hcd_giveback_urb(hcd, urb, regs);
	spin_lock(&uhci->schedule_lock);
}

static void uhci_finish_completion(struct usb_hcd *hcd, struct pt_regs *regs)
{
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);
	struct urb_priv *urbp, *tmp;

	list_for_each_entry_safe(urbp, tmp, &uhci->complete_list, urb_list) {
		struct urb *urb = urbp->urb;

		list_del_init(&urbp->urb_list);
		uhci_finish_urb(hcd, urb, regs);
	}
}

static void uhci_remove_pending_urbps(struct uhci_hcd *uhci)
{

	/* Splice the urb_remove_list onto the end of the complete_list */
	list_splice_init(&uhci->urb_remove_list, uhci->complete_list.prev);
}

static irqreturn_t uhci_irq(struct usb_hcd *hcd, struct pt_regs *regs)
{
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);
	unsigned long io_addr = uhci->io_addr;
	unsigned short status;
	struct urb_priv *urbp, *tmp;
	unsigned int age;

	/*
	 * Read the interrupt status, and write it back to clear the
	 * interrupt cause.  Contrary to the UHCI specification, the
	 * "HC Halted" status bit is persistent: it is RO, not R/WC.
	 */
	status = inw(io_addr + USBSTS);
	if (!(status & ~USBSTS_HCH))	/* shared interrupt, not mine */
		return IRQ_NONE;
	outw(status, io_addr + USBSTS);		/* Clear it */

	if (status & ~(USBSTS_USBINT | USBSTS_ERROR | USBSTS_RD)) {
		if (status & USBSTS_HSE)
			dev_err(uhci_dev(uhci), "host system error, "
					"PCI problems?\n");
		if (status & USBSTS_HCPE)
			dev_err(uhci_dev(uhci), "host controller process "
					"error, something bad happened!\n");
		if ((status & USBSTS_HCH) && uhci->state > 0) {
			dev_err(uhci_dev(uhci), "host controller halted, "
					"very bad!\n");
			/* FIXME: Reset the controller, fix the offending TD */
		}
	}

	if (status & USBSTS_RD)
		uhci->resume_detect = 1;

	spin_lock(&uhci->schedule_lock);

	age = uhci_get_current_frame_number(uhci);
	if (age != uhci->qh_remove_age)
		uhci_free_pending_qhs(uhci);
	if (age != uhci->td_remove_age)
		uhci_free_pending_tds(uhci);
	if (age != uhci->urb_remove_age)
		uhci_remove_pending_urbps(uhci);

	if (list_empty(&uhci->urb_remove_list) &&
	    list_empty(&uhci->td_remove_list) &&
	    list_empty(&uhci->qh_remove_list))
		uhci_clear_next_interrupt(uhci);
	else
		uhci_set_next_interrupt(uhci);

	/* Walk the list of pending URBs to see which ones completed
	 * (must be _safe because uhci_transfer_result() dequeues URBs) */
	list_for_each_entry_safe(urbp, tmp, &uhci->urb_list, urb_list) {
		struct urb *urb = urbp->urb;

		/* Checks the status and does all of the magic necessary */
		uhci_transfer_result(uhci, urb);
	}
	uhci_finish_completion(hcd, regs);

	spin_unlock(&uhci->schedule_lock);

	/* Wake up anyone waiting for an URB to complete */
	wake_up_all(&uhci->waitqh);

	return IRQ_HANDLED;
}

static void reset_hc(struct uhci_hcd *uhci)
{
	unsigned long io_addr = uhci->io_addr;

	/* Turn off PIRQ, SMI, and all interrupts.  This also turns off
	 * the BIOS's USB Legacy Support.
	 */
	pci_write_config_word(to_pci_dev(uhci_dev(uhci)), USBLEGSUP, 0);
	outw(0, uhci->io_addr + USBINTR);

	/* Global reset for 50ms */
	uhci->state = UHCI_RESET;
	outw(USBCMD_GRESET, io_addr + USBCMD);
	msleep(50);
	outw(0, io_addr + USBCMD);

	/* Another 10ms delay */
	msleep(10);
	uhci->resume_detect = 0;
}

static void suspend_hc(struct uhci_hcd *uhci)
{
	unsigned long io_addr = uhci->io_addr;

	dev_dbg(uhci_dev(uhci), "%s\n", __FUNCTION__);
	uhci->state = UHCI_SUSPENDED;
	uhci->resume_detect = 0;
	outw(USBCMD_EGSM, io_addr + USBCMD);
}

static void wakeup_hc(struct uhci_hcd *uhci)
{
	unsigned long io_addr = uhci->io_addr;

	switch (uhci->state) {
		case UHCI_SUSPENDED:		/* Start the resume */
			dev_dbg(uhci_dev(uhci), "%s\n", __FUNCTION__);

			/* Global resume for >= 20ms */
			outw(USBCMD_FGR | USBCMD_EGSM, io_addr + USBCMD);
			uhci->state = UHCI_RESUMING_1;
			uhci->state_end = jiffies + msecs_to_jiffies(20);
			break;

		case UHCI_RESUMING_1:		/* End global resume */
			uhci->state = UHCI_RESUMING_2;
			outw(0, io_addr + USBCMD);
			/* Falls through */

		case UHCI_RESUMING_2:		/* Wait for EOP to be sent */
			if (inw(io_addr + USBCMD) & USBCMD_FGR)
				break;

			/* Run for at least 1 second, and
			 * mark it configured with a 64-byte max packet */
			uhci->state = UHCI_RUNNING_GRACE;
			uhci->state_end = jiffies + HZ;
			outw(USBCMD_RS | USBCMD_CF | USBCMD_MAXP,
					io_addr + USBCMD);
			break;

		case UHCI_RUNNING_GRACE:	/* Now allowed to suspend */
			uhci->state = UHCI_RUNNING;
			break;

		default:
			break;
	}
}

static int ports_active(struct uhci_hcd *uhci)
{
	unsigned long io_addr = uhci->io_addr;
	int connection = 0;
	int i;

	for (i = 0; i < uhci->rh_numports; i++)
		connection |= (inw(io_addr + USBPORTSC1 + i * 2) & USBPORTSC_CCS);

	return connection;
}

static int suspend_allowed(struct uhci_hcd *uhci)
{
	unsigned long io_addr = uhci->io_addr;
	int i;

	if (to_pci_dev(uhci_dev(uhci))->vendor != PCI_VENDOR_ID_INTEL)
		return 1;

	/* Some of Intel's USB controllers have a bug that causes false
	 * resume indications if any port has an over current condition.
	 * To prevent problems, we will not allow a global suspend if
	 * any ports are OC.
	 *
	 * Some motherboards using Intel's chipsets (but not using all
	 * the USB ports) appear to hardwire the over current inputs active
	 * to disable the USB ports.
	 */

	/* check for over current condition on any port */
	for (i = 0; i < uhci->rh_numports; i++) {
		if (inw(io_addr + USBPORTSC1 + i * 2) & USBPORTSC_OC)
			return 0;
	}

	return 1;
}

static void hc_state_transitions(struct uhci_hcd *uhci)
{
	switch (uhci->state) {
		case UHCI_RUNNING:

			/* global suspend if nothing connected for 1 second */
			if (!ports_active(uhci) && suspend_allowed(uhci)) {
				uhci->state = UHCI_SUSPENDING_GRACE;
				uhci->state_end = jiffies + HZ;
			}
			break;

		case UHCI_SUSPENDING_GRACE:
			if (ports_active(uhci))
				uhci->state = UHCI_RUNNING;
			else if (time_after_eq(jiffies, uhci->state_end))
				suspend_hc(uhci);
			break;

		case UHCI_SUSPENDED:

			/* wakeup if requested by a device */
			if (uhci->resume_detect)
				wakeup_hc(uhci);
			break;

		case UHCI_RESUMING_1:
		case UHCI_RESUMING_2:
		case UHCI_RUNNING_GRACE:
			if (time_after_eq(jiffies, uhci->state_end))
				wakeup_hc(uhci);
			break;

		default:
			break;
	}
}

static int start_hc(struct uhci_hcd *uhci)
{
	unsigned long io_addr = uhci->io_addr;
	int timeout = 10;

	/*
	 * Reset the HC - this will force us to get a
	 * new notification of any already connected
	 * ports due to the virtual disconnect that it
	 * implies.
	 */
	outw(USBCMD_HCRESET, io_addr + USBCMD);
	while (inw(io_addr + USBCMD) & USBCMD_HCRESET) {
		if (--timeout < 0) {
			dev_err(uhci_dev(uhci), "USBCMD_HCRESET timed out!\n");
			return -ETIMEDOUT;
		}
		msleep(1);
	}

	/* Turn on PIRQ and all interrupts */
	pci_write_config_word(to_pci_dev(uhci_dev(uhci)), USBLEGSUP,
			USBLEGSUP_DEFAULT);
	outw(USBINTR_TIMEOUT | USBINTR_RESUME | USBINTR_IOC | USBINTR_SP,
		io_addr + USBINTR);

	/* Start at frame 0 */
	outw(0, io_addr + USBFRNUM);
	outl(uhci->fl->dma_handle, io_addr + USBFLBASEADD);

	/* Run and mark it configured with a 64-byte max packet */
	uhci->state = UHCI_RUNNING_GRACE;
	uhci->state_end = jiffies + HZ;
	outw(USBCMD_RS | USBCMD_CF | USBCMD_MAXP, io_addr + USBCMD);

        uhci_to_hcd(uhci)->state = USB_STATE_RUNNING;
	return 0;
}

/*
 * De-allocate all resources..
 */
static void release_uhci(struct uhci_hcd *uhci)
{
	int i;

	for (i = 0; i < UHCI_NUM_SKELQH; i++)
		if (uhci->skelqh[i]) {
			uhci_free_qh(uhci, uhci->skelqh[i]);
			uhci->skelqh[i] = NULL;
		}

	if (uhci->term_td) {
		uhci_free_td(uhci, uhci->term_td);
		uhci->term_td = NULL;
	}

	if (uhci->qh_pool) {
		dma_pool_destroy(uhci->qh_pool);
		uhci->qh_pool = NULL;
	}

	if (uhci->td_pool) {
		dma_pool_destroy(uhci->td_pool);
		uhci->td_pool = NULL;
	}

	if (uhci->fl) {
		dma_free_coherent(uhci_dev(uhci), sizeof(*uhci->fl),
				uhci->fl, uhci->fl->dma_handle);
		uhci->fl = NULL;
	}

	if (uhci->dentry) {
		debugfs_remove(uhci->dentry);
		uhci->dentry = NULL;
	}
}

static int uhci_reset(struct usb_hcd *hcd)
{
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);

	uhci->io_addr = (unsigned long) hcd->regs;

	/* Kick BIOS off this hardware and reset, so we won't get
	 * interrupts from any previous setup.
	 */
	reset_hc(uhci);
	return 0;
}

/*
 * Allocate a frame list, and then setup the skeleton
 *
 * The hardware doesn't really know any difference
 * in the queues, but the order does matter for the
 * protocols higher up. The order is:
 *
 *  - any isochronous events handled before any
 *    of the queues. We don't do that here, because
 *    we'll create the actual TD entries on demand.
 *  - The first queue is the interrupt queue.
 *  - The second queue is the control queue, split into low- and full-speed
 *  - The third queue is bulk queue.
 *  - The fourth queue is the bandwidth reclamation queue, which loops back
 *    to the full-speed control queue.
 */
static int uhci_start(struct usb_hcd *hcd)
{
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);
	int retval = -EBUSY;
	int i, port;
	unsigned io_size;
	dma_addr_t dma_handle;
	struct usb_device *udev;
	struct dentry *dentry;

	io_size = pci_resource_len(to_pci_dev(uhci_dev(uhci)), hcd->region);

	dentry = debugfs_create_file(hcd->self.bus_name, S_IFREG|S_IRUGO|S_IWUSR, uhci_debugfs_root, uhci, &uhci_debug_operations);
	if (!dentry) {
		dev_err(uhci_dev(uhci), "couldn't create uhci debugfs entry\n");
		retval = -ENOMEM;
		goto err_create_debug_entry;
	}
	uhci->dentry = dentry;

	uhci->fsbr = 0;
	uhci->fsbrtimeout = 0;

	spin_lock_init(&uhci->schedule_lock);
	INIT_LIST_HEAD(&uhci->qh_remove_list);

	INIT_LIST_HEAD(&uhci->td_remove_list);

	INIT_LIST_HEAD(&uhci->urb_remove_list);

	INIT_LIST_HEAD(&uhci->urb_list);

	INIT_LIST_HEAD(&uhci->complete_list);

	init_waitqueue_head(&uhci->waitqh);

	uhci->fl = dma_alloc_coherent(uhci_dev(uhci), sizeof(*uhci->fl),
			&dma_handle, 0);
	if (!uhci->fl) {
		dev_err(uhci_dev(uhci), "unable to allocate "
				"consistent memory for frame list\n");
		goto err_alloc_fl;
	}

	memset((void *)uhci->fl, 0, sizeof(*uhci->fl));

	uhci->fl->dma_handle = dma_handle;

	uhci->td_pool = dma_pool_create("uhci_td", uhci_dev(uhci),
			sizeof(struct uhci_td), 16, 0);
	if (!uhci->td_pool) {
		dev_err(uhci_dev(uhci), "unable to create td dma_pool\n");
		goto err_create_td_pool;
	}

	uhci->qh_pool = dma_pool_create("uhci_qh", uhci_dev(uhci),
			sizeof(struct uhci_qh), 16, 0);
	if (!uhci->qh_pool) {
		dev_err(uhci_dev(uhci), "unable to create qh dma_pool\n");
		goto err_create_qh_pool;
	}

	/* Initialize the root hub */

	/* UHCI specs says devices must have 2 ports, but goes on to say */
	/*  they may have more but give no way to determine how many they */
	/*  have. However, according to the UHCI spec, Bit 7 is always set */
	/*  to 1. So we try to use this to our advantage */
	for (port = 0; port < (io_size - 0x10) / 2; port++) {
		unsigned int portstatus;

		portstatus = inw(uhci->io_addr + 0x10 + (port * 2));
		if (!(portstatus & 0x0080))
			break;
	}
	if (debug)
		dev_info(uhci_dev(uhci), "detected %d ports\n", port);

	/* This is experimental so anything less than 2 or greater than 8 is */
	/*  something weird and we'll ignore it */
	if (port < 2 || port > UHCI_RH_MAXCHILD) {
		dev_info(uhci_dev(uhci), "port count misdetected? "
				"forcing to 2 ports\n");
		port = 2;
	}

	uhci->rh_numports = port;

	udev = usb_alloc_dev(NULL, &hcd->self, 0);
	if (!udev) {
		dev_err(uhci_dev(uhci), "unable to allocate root hub\n");
		goto err_alloc_root_hub;
	}

	uhci->term_td = uhci_alloc_td(uhci, udev);
	if (!uhci->term_td) {
		dev_err(uhci_dev(uhci), "unable to allocate terminating TD\n");
		goto err_alloc_term_td;
	}

	for (i = 0; i < UHCI_NUM_SKELQH; i++) {
		uhci->skelqh[i] = uhci_alloc_qh(uhci, udev);
		if (!uhci->skelqh[i]) {
			dev_err(uhci_dev(uhci), "unable to allocate QH\n");
			goto err_alloc_skelqh;
		}
	}

	/*
	 * 8 Interrupt queues; link all higher int queues to int1,
	 * then link int1 to control and control to bulk
	 */
	uhci->skel_int128_qh->link =
			uhci->skel_int64_qh->link =
			uhci->skel_int32_qh->link =
			uhci->skel_int16_qh->link =
			uhci->skel_int8_qh->link =
			uhci->skel_int4_qh->link =
			uhci->skel_int2_qh->link =
			cpu_to_le32(uhci->skel_int1_qh->dma_handle) | UHCI_PTR_QH;
	uhci->skel_int1_qh->link = cpu_to_le32(uhci->skel_ls_control_qh->dma_handle) | UHCI_PTR_QH;

	uhci->skel_ls_control_qh->link = cpu_to_le32(uhci->skel_fs_control_qh->dma_handle) | UHCI_PTR_QH;
	uhci->skel_fs_control_qh->link = cpu_to_le32(uhci->skel_bulk_qh->dma_handle) | UHCI_PTR_QH;
	uhci->skel_bulk_qh->link = cpu_to_le32(uhci->skel_term_qh->dma_handle) | UHCI_PTR_QH;

	/* This dummy TD is to work around a bug in Intel PIIX controllers */
	uhci_fill_td(uhci->term_td, 0, (UHCI_NULL_DATA_SIZE << 21) |
		(0x7f << TD_TOKEN_DEVADDR_SHIFT) | USB_PID_IN, 0);
	uhci->term_td->link = cpu_to_le32(uhci->term_td->dma_handle);

	uhci->skel_term_qh->link = UHCI_PTR_TERM;
	uhci->skel_term_qh->element = cpu_to_le32(uhci->term_td->dma_handle);

	/*
	 * Fill the frame list: make all entries point to the proper
	 * interrupt queue.
	 *
	 * The interrupt queues will be interleaved as evenly as possible.
	 * There's not much to be done about period-1 interrupts; they have
	 * to occur in every frame.  But we can schedule period-2 interrupts
	 * in odd-numbered frames, period-4 interrupts in frames congruent
	 * to 2 (mod 4), and so on.  This way each frame only has two
	 * interrupt QHs, which will help spread out bandwidth utilization.
	 */
	for (i = 0; i < UHCI_NUMFRAMES; i++) {
		int irq;

		/*
		 * ffs (Find First bit Set) does exactly what we need:
		 * 1,3,5,...  => ffs = 0 => use skel_int2_qh = skelqh[6],
		 * 2,6,10,... => ffs = 1 => use skel_int4_qh = skelqh[5], etc.
		 * ffs > 6 => not on any high-period queue, so use
		 *	skel_int1_qh = skelqh[7].
		 * Add UHCI_NUMFRAMES to insure at least one bit is set.
		 */
		irq = 6 - (int) __ffs(i + UHCI_NUMFRAMES);
		if (irq < 0)
			irq = 7;

		/* Only place we don't use the frame list routines */
		uhci->fl->frame[i] = UHCI_PTR_QH |
				cpu_to_le32(uhci->skelqh[irq]->dma_handle);
	}

	/*
	 * Some architectures require a full mb() to enforce completion of
	 * the memory writes above before the I/O transfers in start_hc().
	 */
	mb();
	if ((retval = start_hc(uhci)) != 0)
		goto err_alloc_skelqh;

	init_stall_timer(hcd);

	udev->speed = USB_SPEED_FULL;

	if (hcd_register_root(udev, hcd) != 0) {
		dev_err(uhci_dev(uhci), "unable to start root hub\n");
		retval = -ENOMEM;
		goto err_start_root_hub;
	}

	return 0;

/*
 * error exits:
 */
err_start_root_hub:
	reset_hc(uhci);

	del_timer_sync(&uhci->stall_timer);

err_alloc_skelqh:
	for (i = 0; i < UHCI_NUM_SKELQH; i++)
		if (uhci->skelqh[i]) {
			uhci_free_qh(uhci, uhci->skelqh[i]);
			uhci->skelqh[i] = NULL;
		}

	uhci_free_td(uhci, uhci->term_td);
	uhci->term_td = NULL;

err_alloc_term_td:
	usb_put_dev(udev);

err_alloc_root_hub:
	dma_pool_destroy(uhci->qh_pool);
	uhci->qh_pool = NULL;

err_create_qh_pool:
	dma_pool_destroy(uhci->td_pool);
	uhci->td_pool = NULL;

err_create_td_pool:
	dma_free_coherent(uhci_dev(uhci), sizeof(*uhci->fl),
			uhci->fl, uhci->fl->dma_handle);
	uhci->fl = NULL;

err_alloc_fl:
	debugfs_remove(uhci->dentry);
	uhci->dentry = NULL;

err_create_debug_entry:
	return retval;
}

static void uhci_stop(struct usb_hcd *hcd)
{
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);

	del_timer_sync(&uhci->stall_timer);

	/*
	 * At this point, we're guaranteed that no new connects can be made
	 * to this bus since there are no more parents
	 */

	reset_hc(uhci);

	spin_lock_irq(&uhci->schedule_lock);
	uhci_free_pending_qhs(uhci);
	uhci_free_pending_tds(uhci);
	uhci_remove_pending_urbps(uhci);
	uhci_finish_completion(hcd, NULL);

	uhci_free_pending_qhs(uhci);
	uhci_free_pending_tds(uhci);
	spin_unlock_irq(&uhci->schedule_lock);

	/* Wake up anyone waiting for an URB to complete */
	wake_up_all(&uhci->waitqh);
	
	release_uhci(uhci);
}

#ifdef CONFIG_PM
static int uhci_suspend(struct usb_hcd *hcd, u32 state)
{
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);

	/* Don't try to suspend broken motherboards, reset instead */
	if (suspend_allowed(uhci)) {
		suspend_hc(uhci);
		uhci->saved_framenumber =
				inw(uhci->io_addr + USBFRNUM) & 0x3ff;
	} else
		reset_hc(uhci);
	return 0;
}

static int uhci_resume(struct usb_hcd *hcd)
{
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);
	int rc;

	pci_set_master(to_pci_dev(uhci_dev(uhci)));

	if (uhci->state == UHCI_SUSPENDED) {

		/*
		 * Some systems don't maintain the UHCI register values
		 * during a PM suspend/resume cycle, so reinitialize
		 * the Frame Number, Framelist Base Address, Interrupt
		 * Enable, and Legacy Support registers.
		 */
		pci_write_config_word(to_pci_dev(uhci_dev(uhci)), USBLEGSUP,
				0);
		outw(uhci->saved_framenumber, uhci->io_addr + USBFRNUM);
		outl(uhci->fl->dma_handle, uhci->io_addr + USBFLBASEADD);
		outw(USBINTR_TIMEOUT | USBINTR_RESUME | USBINTR_IOC |
				USBINTR_SP, uhci->io_addr + USBINTR);
		uhci->resume_detect = 1;
		pci_write_config_word(to_pci_dev(uhci_dev(uhci)), USBLEGSUP,
				USBLEGSUP_DEFAULT);
	} else {
		reset_hc(uhci);
		if ((rc = start_hc(uhci)) != 0)
			return rc;
	}
	hcd->state = USB_STATE_RUNNING;
	return 0;
}
#endif

/* Wait until all the URBs for a particular device/endpoint are gone */
static void uhci_hcd_endpoint_disable(struct usb_hcd *hcd,
		struct usb_host_endpoint *ep)
{
	struct uhci_hcd *uhci = hcd_to_uhci(hcd);

	wait_event_interruptible(uhci->waitqh, list_empty(&ep->urb_list));
}

static int uhci_hcd_get_frame_number(struct usb_hcd *hcd)
{
	return uhci_get_current_frame_number(hcd_to_uhci(hcd));
}

static const char hcd_name[] = "uhci_hcd";

static const struct hc_driver uhci_driver = {
	.description =		hcd_name,
	.product_desc =		"UHCI Host Controller",
	.hcd_priv_size =	sizeof(struct uhci_hcd),

	/* Generic hardware linkage */
	.irq =			uhci_irq,
	.flags =		HCD_USB11,

	/* Basic lifecycle operations */
	.reset =		uhci_reset,
	.start =		uhci_start,
#ifdef CONFIG_PM
	.suspend =		uhci_suspend,
	.resume =		uhci_resume,
#endif
	.stop =			uhci_stop,

	.urb_enqueue =		uhci_urb_enqueue,
	.urb_dequeue =		uhci_urb_dequeue,

	.endpoint_disable =	uhci_hcd_endpoint_disable,
	.get_frame_number =	uhci_hcd_get_frame_number,

	.hub_status_data =	uhci_hub_status_data,
	.hub_control =		uhci_hub_control,
};

static const struct pci_device_id uhci_pci_ids[] = { {
	/* handle any USB UHCI controller */
	PCI_DEVICE_CLASS(((PCI_CLASS_SERIAL_USB << 8) | 0x00), ~0),
	.driver_data =	(unsigned long) &uhci_driver,
	}, { /* end: all zeroes */ }
};

MODULE_DEVICE_TABLE(pci, uhci_pci_ids);

static struct pci_driver uhci_pci_driver = {
	.name =		(char *)hcd_name,
	.id_table =	uhci_pci_ids,

	.probe =	usb_hcd_pci_probe,
	.remove =	usb_hcd_pci_remove,

#ifdef	CONFIG_PM
	.suspend =	usb_hcd_pci_suspend,
	.resume =	usb_hcd_pci_resume,
#endif	/* PM */
};
 
static int __init uhci_hcd_init(void)
{
	int retval = -ENOMEM;

	printk(KERN_INFO DRIVER_DESC " " DRIVER_VERSION "\n");

	if (usb_disabled())
		return -ENODEV;

	if (debug) {
		errbuf = kmalloc(ERRBUF_LEN, GFP_KERNEL);
		if (!errbuf)
			goto errbuf_failed;
	}

	uhci_debugfs_root = debugfs_create_dir("uhci", NULL);
	if (!uhci_debugfs_root)
		goto debug_failed;

	uhci_up_cachep = kmem_cache_create("uhci_urb_priv",
		sizeof(struct urb_priv), 0, 0, NULL, NULL);
	if (!uhci_up_cachep)
		goto up_failed;

	retval = pci_register_driver(&uhci_pci_driver);
	if (retval)
		goto init_failed;

	return 0;

init_failed:
	if (kmem_cache_destroy(uhci_up_cachep))
		warn("not all urb_priv's were freed!");

up_failed:
	debugfs_remove(uhci_debugfs_root);

debug_failed:
	if (errbuf)
		kfree(errbuf);

errbuf_failed:

	return retval;
}

static void __exit uhci_hcd_cleanup(void) 
{
	pci_unregister_driver(&uhci_pci_driver);
	
	if (kmem_cache_destroy(uhci_up_cachep))
		warn("not all urb_priv's were freed!");

	debugfs_remove(uhci_debugfs_root);

	if (errbuf)
		kfree(errbuf);
}

module_init(uhci_hcd_init);
module_exit(uhci_hcd_cleanup);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
