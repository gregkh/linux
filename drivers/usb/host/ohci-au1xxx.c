/*
 * OHCI HCD (Host Controller Driver) for USB.
 *
 * (C) Copyright 1999 Roman Weissgaerber <weissg@vienna.at>
 * (C) Copyright 2000-2002 David Brownell <dbrownell@users.sourceforge.net>
 * (C) Copyright 2002 Hewlett-Packard Company
 *
 * Bus Glue for AMD Alchemy Au1xxx
 *
 * Written by Christopher Hoover <ch@hpl.hp.com>
 * Based on fragments of previous driver by Rusell King et al.
 *
 * Modified for LH7A404 from ohci-sa1111.c
 *  by Durgesh Pattamatta <pattamattad@sharpsec.com>
 * Modified for AMD Alchemy Au1xxx
 *  by Matt Porter <mporter@kernel.crashing.org>
 *
 * This file is licenced under the GPL.
 */

#include <asm/mach-au1x00/au1000.h>

#define USBH_ENABLE_BE (1<<0)
#define USBH_ENABLE_C  (1<<1)
#define USBH_ENABLE_E  (1<<2)
#define USBH_ENABLE_CE (1<<3)
#define USBH_ENABLE_RD (1<<4)

#ifdef __LITTLE_ENDIAN
#define USBH_ENABLE_INIT (USBH_ENABLE_CE | USBH_ENABLE_E | USBH_ENABLE_C)
#elif __BIG_ENDIAN
#define USBH_ENABLE_INIT (USBH_ENABLE_CE | USBH_ENABLE_E | USBH_ENABLE_C | USBH_ENABLE_BE)
#else
#error not byte order defined
#endif

extern int usb_disabled(void);

/*-------------------------------------------------------------------------*/

static void au1xxx_start_hc(struct platform_device *dev)
{
	printk(KERN_DEBUG __FILE__
		": starting Au1xxx OHCI USB Controller\n");

	/* enable host controller */
	au_writel(USBH_ENABLE_CE, USB_HOST_CONFIG);
	udelay(1000);
	au_writel(USBH_ENABLE_INIT, USB_HOST_CONFIG);
	udelay(1000);

	/* wait for reset complete (read register twice; see au1500 errata) */
	while (au_readl(USB_HOST_CONFIG),
		!(au_readl(USB_HOST_CONFIG) & USBH_ENABLE_RD))
		udelay(1000);

	printk(KERN_DEBUG __FILE__
	": Clock to USB host has been enabled \n");
}

static void au1xxx_stop_hc(struct platform_device *dev)
{
	printk(KERN_DEBUG __FILE__
	       ": stopping Au1xxx OHCI USB Controller\n");

	/* Disable clock */
	au_writel(readl((void *)USB_HOST_CONFIG) & ~USBH_ENABLE_CE, USB_HOST_CONFIG);
}


/*-------------------------------------------------------------------------*/


static irqreturn_t usb_hcd_au1xxx_hcim_irq (int irq, void *__hcd,
					     struct pt_regs * r)
{
	struct usb_hcd *hcd = __hcd;

	return usb_hcd_irq(irq, hcd, r);
}

/*-------------------------------------------------------------------------*/

void usb_hcd_au1xxx_remove (struct usb_hcd *, struct platform_device *);

/* configure so an HC device and id are always provided */
/* always called with process context; sleeping is OK */


/**
 * usb_hcd_au1xxx_probe - initialize Au1xxx-based HCDs
 * Context: !in_interrupt()
 *
 * Allocates basic resources for this USB host controller, and
 * then invokes the start() method for the HCD associated with it
 * through the hotplug entry's driver_data.
 *
 */
int usb_hcd_au1xxx_probe (const struct hc_driver *driver,
			  struct usb_hcd **hcd_out,
			  struct platform_device *dev)
{
	int retval;
	struct usb_hcd *hcd = 0;

	unsigned int *addr = NULL;

	if (!request_mem_region(dev->resource[0].start,
				dev->resource[0].end
				- dev->resource[0].start + 1, hcd_name)) {
		pr_debug("request_mem_region failed");
		return -EBUSY;
	}

	au1xxx_start_hc(dev);

	addr = ioremap(dev->resource[0].start,
		       dev->resource[0].end
		       - dev->resource[0].start + 1);
	if (!addr) {
		pr_debug("ioremap failed");
		retval = -ENOMEM;
		goto err1;
	}

	if(dev->resource[1].flags != IORESOURCE_IRQ) {
		pr_debug ("resource[1] is not IORESOURCE_IRQ");
		retval = -ENOMEM;
		goto err1;
	}

	hcd = usb_create_hcd(driver);
	if (hcd == NULL) {
		pr_debug ("usb_create_hcd failed");
		retval = -ENOMEM;
		goto err1;
	}
	ohci_hcd_init(hcd_to_ohci(hcd));

	hcd->irq = dev->resource[1].start;
	hcd->regs = addr;
	hcd->self.controller = &dev->dev;

	retval = hcd_buffer_create (hcd);
	if (retval != 0) {
		pr_debug ("pool alloc fail");
		goto err2;
	}

	retval = request_irq (hcd->irq, usb_hcd_au1xxx_hcim_irq, SA_INTERRUPT,
			      hcd->driver->description, hcd);
	if (retval != 0) {
		pr_debug("request_irq failed");
		retval = -EBUSY;
		goto err3;
	}

	pr_debug ("%s (Au1xxx) at 0x%p, irq %d",
	     hcd->driver->description, hcd->regs, hcd->irq);

	hcd->self.bus_name = "au1xxx";

	usb_register_bus (&hcd->self);

	if ((retval = driver->start (hcd)) < 0)
	{
		usb_hcd_au1xxx_remove(hcd, dev);
		printk("bad driver->start\n");
		return retval;
	}

	*hcd_out = hcd;
	return 0;

 err3:
	hcd_buffer_destroy (hcd);
 err2:
	usb_put_hcd(hcd);
 err1:
	au1xxx_stop_hc(dev);
	release_mem_region(dev->resource[0].start,
				dev->resource[0].end
			   - dev->resource[0].start + 1);
	return retval;
}


/* may be called without controller electrically present */
/* may be called with controller, bus, and devices active */

/**
 * usb_hcd_au1xxx_remove - shutdown processing for Au1xxx-based HCDs
 * @dev: USB Host Controller being removed
 * Context: !in_interrupt()
 *
 * Reverses the effect of usb_hcd_au1xxx_probe(), first invoking
 * the HCD's stop() method.  It is always called from a thread
 * context, normally "rmmod", "apmd", or something similar.
 *
 */
void usb_hcd_au1xxx_remove (struct usb_hcd *hcd, struct platform_device *dev)
{
	pr_debug ("remove: %s, state %x", hcd->self.bus_name, hcd->state);

	if (in_interrupt ())
		BUG ();

	hcd->state = USB_STATE_QUIESCING;

	pr_debug ("%s: roothub graceful disconnect", hcd->self.bus_name);
	usb_disconnect (&hcd->self.root_hub);

	hcd->driver->stop (hcd);
	hcd->state = USB_STATE_HALT;

	free_irq (hcd->irq, hcd);
	hcd_buffer_destroy (hcd);

	usb_deregister_bus (&hcd->self);

	au1xxx_stop_hc(dev);
	release_mem_region(dev->resource[0].start,
			   dev->resource[0].end
			   - dev->resource[0].start + 1);
}

/*-------------------------------------------------------------------------*/

static int __devinit
ohci_au1xxx_start (struct usb_hcd *hcd)
{
	struct ohci_hcd	*ohci = hcd_to_ohci (hcd);
	int		ret;

	ohci_dbg (ohci, "ohci_au1xxx_start, ohci:%p", ohci);

	if ((ret = ohci_init (ohci)) < 0)
		return ret;

	if ((ret = ohci_run (ohci)) < 0) {
		err ("can't start %s", hcd->self.bus_name);
		ohci_stop (hcd);
		return ret;
	}

	return 0;
}

/*-------------------------------------------------------------------------*/

static const struct hc_driver ohci_au1xxx_hc_driver = {
	.description =		hcd_name,
	.product_desc =		"Au1xxx OHCI",
	.hcd_priv_size =	sizeof(struct ohci_hcd),

	/*
	 * generic hardware linkage
	 */
	.irq =			ohci_irq,
	.flags =		HCD_USB11,

	/*
	 * basic lifecycle operations
	 */
	.start =		ohci_au1xxx_start,
#ifdef	CONFIG_PM
	/* suspend:		ohci_au1xxx_suspend,  -- tbd */
	/* resume:		ohci_au1xxx_resume,   -- tbd */
#endif /*CONFIG_PM*/
	.stop =			ohci_stop,

	/*
	 * managing i/o requests and associated device resources
	 */
	.urb_enqueue =		ohci_urb_enqueue,
	.urb_dequeue =		ohci_urb_dequeue,
	.endpoint_disable =	ohci_endpoint_disable,

	/*
	 * scheduling support
	 */
	.get_frame_number =	ohci_get_frame,

	/*
	 * root hub support
	 */
	.hub_status_data =	ohci_hub_status_data,
	.hub_control =		ohci_hub_control,
};

/*-------------------------------------------------------------------------*/

static int ohci_hcd_au1xxx_drv_probe(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct usb_hcd *hcd = NULL;
	int ret;

	pr_debug ("In ohci_hcd_au1xxx_drv_probe");

	if (usb_disabled())
		return -ENODEV;

	ret = usb_hcd_au1xxx_probe(&ohci_au1xxx_hc_driver, &hcd, pdev);

	if (ret == 0)
		dev_set_drvdata(dev, hcd);

	return ret;
}

static int ohci_hcd_au1xxx_drv_remove(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct usb_hcd *hcd = dev_get_drvdata(dev);

	usb_hcd_au1xxx_remove(hcd, pdev);
	dev_set_drvdata(dev, NULL);
	return 0;
}
	/*TBD*/
/*static int ohci_hcd_au1xxx_drv_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct usb_hcd *hcd = dev_get_drvdata(dev);

	return 0;
}
static int ohci_hcd_au1xxx_drv_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct usb_hcd *hcd = dev_get_drvdata(dev);

	return 0;
}
*/

static struct device_driver ohci_hcd_au1xxx_driver = {
	.name		= "au1xxx-ohci",
	.bus		= &platform_bus_type,
	.probe		= ohci_hcd_au1xxx_drv_probe,
	.remove		= ohci_hcd_au1xxx_drv_remove,
	/*.suspend	= ohci_hcd_au1xxx_drv_suspend, */
	/*.resume	= ohci_hcd_au1xxx_drv_resume, */
};

static int __init ohci_hcd_au1xxx_init (void)
{
	pr_debug (DRIVER_INFO " (Au1xxx)");
	pr_debug ("block sizes: ed %d td %d\n",
		sizeof (struct ed), sizeof (struct td));

	return driver_register(&ohci_hcd_au1xxx_driver);
}

static void __exit ohci_hcd_au1xxx_cleanup (void)
{
	driver_unregister(&ohci_hcd_au1xxx_driver);
}

module_init (ohci_hcd_au1xxx_init);
module_exit (ohci_hcd_au1xxx_cleanup);
