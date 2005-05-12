/*
 * OHCI HCD (Host Controller Driver) for USB.
 *
 * (C) Copyright 1999 Roman Weissgaerber <weissg@vienna.at>
 * (C) Copyright 2000-2004 David Brownell <dbrownell@users.sourceforge.net>
 * (C) Copyright 2002 Hewlett-Packard Company
 * 
 * OMAP Bus Glue
 *
 * Written by Christopher Hoover <ch@hpl.hp.com>
 * Based on fragments of previous driver by Russell King et al.
 *
 * Modified for OMAP from ohci-sa1111.c by Tony Lindgren <tony@atomide.com>
 * Based on the 2.4 OMAP OHCI driver originally done by MontaVista Software Inc.
 *
 * This file is licenced under the GPL.
 */

#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/mach-types.h>

#include <asm/arch/hardware.h>
#include <asm/arch/mux.h>
#include <asm/arch/irqs.h>
#include <asm/arch/gpio.h>
#include <asm/arch/fpga.h>
#include <asm/arch/usb.h>

#include "ohci-omap.h"

#ifndef CONFIG_ARCH_OMAP
#error "This file is OMAP bus glue.  CONFIG_OMAP must be defined."
#endif

extern int usb_disabled(void);
extern int ocpi_enable(void);

/*
 * OHCI clock initialization for OMAP-1510 and 1610
 */
static int omap_ohci_clock_power(int on)
{
	if (on) {
		/* for 1510, 48MHz DPLL is set up in usb init */

		if (cpu_is_omap16xx()) {
			/* Enable OHCI */
			omap_writel(omap_readl(ULPD_SOFT_REQ) | SOFT_USB_OTG_REQ,
				ULPD_SOFT_REQ);

			/* USB host clock request if not using OTG */
			omap_writel(omap_readl(ULPD_SOFT_REQ) | SOFT_USB_REQ,
				ULPD_SOFT_REQ);

			omap_writel(omap_readl(ULPD_STATUS_REQ) | USB_HOST_DPLL_REQ,
			     ULPD_STATUS_REQ);
		}

		/* Enable 48MHz clock to USB */
		omap_writel(omap_readl(ULPD_CLOCK_CTRL) | USB_MCLK_EN,
		       ULPD_CLOCK_CTRL);

		omap_writel(omap_readl(ARM_IDLECT2) | (1 << EN_LBFREECK) | (1 << EN_LBCK),
		       ARM_IDLECT2);

		omap_writel(omap_readl(MOD_CONF_CTRL_0) | USB_HOST_HHC_UHOST_EN,
		       MOD_CONF_CTRL_0);
	} else {
		/* Disable 48MHz clock to USB */
		omap_writel(omap_readl(ULPD_CLOCK_CTRL) & ~USB_MCLK_EN,
		       ULPD_CLOCK_CTRL);

		/* FIXME: The DPLL stays on for now */
	}

	return 0;
}

/*
 * Hardware specific transceiver power on/off
 */
static int omap_ohci_transceiver_power(int on)
{
	if (on) {
		if (machine_is_omap_innovator() && cpu_is_omap1510())
			fpga_write(fpga_read(INNOVATOR_FPGA_CAM_USB_CONTROL)
				| ((1 << 5/*usb1*/) | (1 << 3/*usb2*/)), 
			       INNOVATOR_FPGA_CAM_USB_CONTROL);
		else if (machine_is_omap_osk()) {
			/* FIXME: GPIO1 -> 1 on the TPS65010 I2C chip */
		}
	} else {
		if (machine_is_omap_innovator() && cpu_is_omap1510())
			fpga_write(fpga_read(INNOVATOR_FPGA_CAM_USB_CONTROL)
				& ~((1 << 5/*usb1*/) | (1 << 3/*usb2*/)), 
			       INNOVATOR_FPGA_CAM_USB_CONTROL);
		else if (machine_is_omap_osk()) {
			/* FIXME: GPIO1 -> 0 on the TPS65010 I2C chip */
		}
	}

	return 0;
}

/*
 * OMAP-1510 specific Local Bus clock on/off
 */
static int omap_1510_local_bus_power(int on)
{
	if (on) {
		omap_writel((1 << 1) | (1 << 0), OMAP1510_LB_MMU_CTL);
		udelay(200);
	} else {
		omap_writel(0, OMAP1510_LB_MMU_CTL);
	}

	return 0;
}

/*
 * OMAP-1510 specific Local Bus initialization
 * NOTE: This assumes 32MB memory size in OMAP1510LB_MEMSIZE.
 *       See also arch/mach-omap/memory.h for __virt_to_dma() and 
 *       __dma_to_virt() which need to match with the physical 
 *       Local Bus address below.
 */
static int omap_1510_local_bus_init(void)
{
	unsigned int tlb;
	unsigned long lbaddr, physaddr;

	omap_writel((omap_readl(OMAP1510_LB_CLOCK_DIV) & 0xfffffff8) | 0x4, 
	       OMAP1510_LB_CLOCK_DIV);

	/* Configure the Local Bus MMU table */
	for (tlb = 0; tlb < OMAP1510_LB_MEMSIZE; tlb++) {
		lbaddr = tlb * 0x00100000 + OMAP1510_LB_OFFSET;
		physaddr = tlb * 0x00100000 + PHYS_OFFSET;
		omap_writel((lbaddr & 0x0fffffff) >> 22, OMAP1510_LB_MMU_CAM_H);
		omap_writel(((lbaddr & 0x003ffc00) >> 6) | 0xc, 
		       OMAP1510_LB_MMU_CAM_L);
		omap_writel(physaddr >> 16, OMAP1510_LB_MMU_RAM_H);
		omap_writel((physaddr & 0x0000fc00) | 0x300, OMAP1510_LB_MMU_RAM_L);
		omap_writel(tlb << 4, OMAP1510_LB_MMU_LCK);
		omap_writel(0x1, OMAP1510_LB_MMU_LD_TLB);
	}

	/* Enable the walking table */
	omap_writel(omap_readl(OMAP1510_LB_MMU_CTL) | (1 << 3), OMAP1510_LB_MMU_CTL);
	udelay(200);

	return 0;
}

#ifdef	CONFIG_USB_OTG

static void start_hnp(struct ohci_hcd *ohci)
{
	const unsigned	port = ohci_to_hcd(ohci)->self.otg_port - 1;
	unsigned long	flags;

	otg_start_hnp(ohci->transceiver);

	local_irq_save(flags);
	ohci->transceiver->state = OTG_STATE_A_SUSPEND;
	writel (RH_PS_PSS, &ohci->regs->roothub.portstatus [port]);
	OTG_CTRL_REG &= ~OTG_A_BUSREQ;
	local_irq_restore(flags);
}

#endif

/*-------------------------------------------------------------------------*/

static int omap_start_hc(struct ohci_hcd *ohci, struct platform_device *pdev)
{
	struct omap_usb_config	*config = pdev->dev.platform_data;
	int			need_transceiver = (config->otg != 0);

	dev_dbg(&pdev->dev, "starting USB Controller\n");

	if (config->otg) {
		ohci_to_hcd(ohci)->self.otg_port = config->otg;
		/* default/minimum OTG power budget:  8 mA */
		ohci->power_budget = 8;
	}

	/* boards can use OTG transceivers in non-OTG modes */
	need_transceiver = need_transceiver
			|| machine_is_omap_h2();

	if (cpu_is_omap16xx())
		ocpi_enable();

#ifdef	CONFIG_ARCH_OMAP_OTG
	if (need_transceiver) {
		ohci->transceiver = otg_get_transceiver();
		if (ohci->transceiver) {
			int	status = otg_set_host(ohci->transceiver,
						&ohci_to_hcd(ohci)->self);
			dev_dbg(&pdev->dev, "init %s transceiver, status %d\n",
					ohci->transceiver->label, status);
			if (status) {
				if (ohci->transceiver)
					put_device(ohci->transceiver->dev);
				return status;
			}
		} else {
			dev_err(&pdev->dev, "can't find transceiver\n");
			return -ENODEV;
		}
	}
#endif

	if (machine_is_omap_osk()) {
		omap_request_gpio(9);
		omap_set_gpio_direction(9, 1);
		omap_set_gpio_dataout(9, 1);
	}

	omap_ohci_clock_power(1);

	omap_ohci_transceiver_power(1);

	if (cpu_is_omap1510()) {
		omap_1510_local_bus_power(1);
		omap_1510_local_bus_init();
	}

	/* board init will have already handled HMC and mux setup.
	 * any external transceiver should already be initialized
	 * too, so all configured ports use the right signaling now.
	 */

	return 0;
}

static void omap_stop_hc(struct platform_device *pdev)
{
	dev_dbg(&pdev->dev, "stopping USB Controller\n");

	/*
	 * FIXME: Put the USB host controller into reset.
	 */

	/*
	 * FIXME: Stop the USB clock.
	 */
	//omap_disable_device(dev);

}


/*-------------------------------------------------------------------------*/

void usb_hcd_omap_remove (struct usb_hcd *, struct platform_device *);

/* configure so an HC device and id are always provided */
/* always called with process context; sleeping is OK */


/**
 * usb_hcd_omap_probe - initialize OMAP-based HCDs
 * Context: !in_interrupt()
 *
 * Allocates basic resources for this USB host controller, and
 * then invokes the start() method for the HCD associated with it
 * through the hotplug entry's driver_data.
 */
int usb_hcd_omap_probe (const struct hc_driver *driver,
			  struct platform_device *pdev)
{
	int retval;
	struct usb_hcd *hcd = 0;
	struct ohci_hcd *ohci;

	if (pdev->num_resources != 2) {
		printk(KERN_ERR "hcd probe: invalid num_resources: %i\n", 
		       pdev->num_resources);
		return -ENODEV;
	}

	if (pdev->resource[0].flags != IORESOURCE_MEM 
	    || pdev->resource[1].flags != IORESOURCE_IRQ) {
		printk(KERN_ERR "hcd probe: invalid resource type\n");
		return -ENODEV;
	}

	if (!request_mem_region(pdev->resource[0].start, 
				pdev->resource[0].end - pdev->resource[0].start + 1, hcd_name)) {
		dev_dbg(&pdev->dev, "request_mem_region failed\n");
		return -EBUSY;
	}

	hcd = usb_create_hcd (driver);
	if (hcd == NULL){
		dev_dbg(&pdev->dev, "hcd_alloc failed\n");
		retval = -ENOMEM;
		goto err1;
	}
	dev_set_drvdata(&pdev->dev, hcd);
	ohci = hcd_to_ohci(hcd);
	ohci_hcd_init(ohci);

	hcd->irq = pdev->resource[1].start;
	hcd->regs = (void *)pdev->resource[0].start;
	hcd->self.controller = &pdev->dev;

	retval = omap_start_hc(ohci, pdev);
	if (retval < 0)
		goto err2;

	retval = hcd_buffer_create (hcd);
	if (retval != 0) {
		dev_dbg(&pdev->dev, "pool alloc fail\n");
		goto err2;
	}

	retval = request_irq (hcd->irq, usb_hcd_irq, 
			      SA_INTERRUPT, hcd->driver->description, hcd);
	if (retval != 0) {
		dev_dbg(&pdev->dev, "request_irq failed\n");
		retval = -EBUSY;
		goto err3;
	}

	dev_info(&pdev->dev, "at 0x%p, irq %d\n", hcd->regs, hcd->irq);

	hcd->self.bus_name = pdev->dev.bus_id;
	usb_register_bus (&hcd->self);

	if ((retval = driver->start (hcd)) < 0) 
	{
		usb_hcd_omap_remove(hcd, pdev);
		return retval;
	}

	return 0;

 err3:
	hcd_buffer_destroy (hcd);
 err2:
	dev_set_drvdata(&pdev->dev, NULL);
	usb_put_hcd(hcd);
 err1:
	omap_stop_hc(pdev);

	release_mem_region(pdev->resource[0].start, 
			   pdev->resource[0].end - pdev->resource[0].start + 1);

	return retval;
}


/* may be called without controller electrically present */
/* may be called with controller, bus, and devices active */

/**
 * usb_hcd_omap_remove - shutdown processing for OMAP-based HCDs
 * @dev: USB Host Controller being removed
 * Context: !in_interrupt()
 *
 * Reverses the effect of usb_hcd_omap_probe(), first invoking
 * the HCD's stop() method.  It is always called from a thread
 * context, normally "rmmod", "apmd", or something similar.
 *
 */
void usb_hcd_omap_remove (struct usb_hcd *hcd, struct platform_device *pdev)
{
	dev_info(&pdev->dev, "remove: state %x\n", hcd->state);

	if (in_interrupt ())
		BUG ();

	hcd->state = USB_STATE_QUIESCING;

	dev_dbg(&pdev->dev, "roothub graceful disconnect\n");
	usb_disconnect (&hcd->self.root_hub);

	hcd->driver->stop (hcd);
	hcd_buffer_destroy (hcd);
	hcd->state = USB_STATE_HALT;

	if (machine_is_omap_osk())
		omap_free_gpio(9);

	free_irq (hcd->irq, hcd);

	usb_deregister_bus (&hcd->self);

	omap_stop_hc(pdev);

	release_mem_region(pdev->resource[0].start, 
			   pdev->resource[0].end - pdev->resource[0].start + 1);
}

/*-------------------------------------------------------------------------*/

static int __devinit
ohci_omap_start (struct usb_hcd *hcd)
{
	struct omap_usb_config *config;
	struct ohci_hcd	*ohci = hcd_to_ohci (hcd);
	int		ret;

	if ((ret = ohci_init(ohci)) < 0)
		return ret;

	config = hcd->self.controller->platform_data;
	if (config->otg || config->rwc)
		writel(OHCI_CTRL_RWC, &ohci->regs->control);

	if ((ret = ohci_run (ohci)) < 0) {
		err ("can't start %s", hcd->self.bus_name);
		ohci_stop (hcd);
		return ret;
	}
	return 0;
}

/*-------------------------------------------------------------------------*/

static const struct hc_driver ohci_omap_hc_driver = {
	.description =		hcd_name,
	.product_desc =		"OMAP OHCI",
	.hcd_priv_size =	sizeof(struct ohci_hcd),

	/*
	 * generic hardware linkage
	 */
	.irq =			ohci_irq,
	.flags =		HCD_USB11,

	/*
	 * basic lifecycle operations
	 */
	.start =		ohci_omap_start,
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
#ifdef	CONFIG_USB_SUSPEND
	.hub_suspend =		ohci_hub_suspend,
	.hub_resume =		ohci_hub_resume,
#endif
	.start_port_reset =	ohci_start_port_reset,
};

/*-------------------------------------------------------------------------*/

static int ohci_hcd_omap_drv_probe(struct device *dev)
{
	return usb_hcd_omap_probe(&ohci_omap_hc_driver,
				to_platform_device(dev));
}

static int ohci_hcd_omap_drv_remove(struct device *dev)
{
	struct platform_device	*pdev = to_platform_device(dev);
	struct usb_hcd		*hcd = dev_get_drvdata(dev);
	struct ohci_hcd		*ohci = hcd_to_ohci (hcd);

	usb_hcd_omap_remove(hcd, pdev);
	if (ohci->transceiver) {
		(void) otg_set_host(ohci->transceiver, 0);
		put_device(ohci->transceiver->dev);
	}
	dev_set_drvdata(dev, NULL);

	return 0;
}

/*-------------------------------------------------------------------------*/

#if	defined(CONFIG_USB_SUSPEND) || defined(CONFIG_PM)

/* states match PCI usage, always suspending the root hub except that
 * 4 ~= D3cold (ACPI D3) with clock off (resume sees reset).
 */

static int ohci_omap_suspend(struct device *dev, u32 state, u32 level)
{
	struct ohci_hcd	*ohci = hcd_to_ohci(dev_get_drvdata(dev));
	int		status = -EINVAL;

	if (state <= dev->power.power_state)
		return 0;

	dev_dbg(dev, "suspend to %d\n", state);
	down(&ohci_to_hcd(ohci)->self.root_hub->serialize);
	status = ohci_hub_suspend(ohci_to_hcd(ohci));
	if (status == 0) {
		if (state >= 4) {
			/* power off + reset */
			OTG_SYSCON_2_REG &= ~UHOST_EN;
			ohci_to_hcd(ohci)->self.root_hub->state =
					USB_STATE_SUSPENDED;
			state = 4;
		}
		ohci_to_hcd(ohci)->state = HCD_STATE_SUSPENDED;
		dev->power.power_state = state;
	}
	up(&ohci_to_hcd(ohci)->self.root_hub->serialize);
	return status;
}

static int ohci_omap_resume(struct device *dev, u32 level)
{
	struct ohci_hcd	*ohci = hcd_to_ohci(dev_get_drvdata(dev));
	int		status = 0;

	switch (dev->power.power_state) {
	case 0:
		break;
	case 4:
		if (time_before(jiffies, ohci->next_statechange))
			msleep(5);
		ohci->next_statechange = jiffies;
		OTG_SYSCON_2_REG |= UHOST_EN;
		/* FALLTHROUGH */
	default:
		dev_dbg(dev, "resume from %d\n", dev->power.power_state);
#ifdef	CONFIG_USB_SUSPEND
		/* get extra cleanup even if remote wakeup isn't in use */
		status = usb_resume_device(ohci_to_hcd(ohci)->self.root_hub);
#else
		down(&ohci_to_hcd(ohci)->self.root_hub->serialize);
		status = ohci_hub_resume(ohci_to_hcd(ohci));
		up(&ohci_to_hcd(ohci)->self.root_hub->serialize);
#endif
		if (status == 0)
			dev->power.power_state = 0;
		break;
	}
	return status;
}

#endif

/*-------------------------------------------------------------------------*/

/*
 * Driver definition to register with the OMAP bus
 */
static struct device_driver ohci_hcd_omap_driver = {
	.name		= "ohci",
	.bus		= &platform_bus_type,
	.probe		= ohci_hcd_omap_drv_probe,
	.remove		= ohci_hcd_omap_drv_remove,
#if	defined(CONFIG_USB_SUSPEND) || defined(CONFIG_PM)
	.suspend	= ohci_omap_suspend,
	.resume		= ohci_omap_resume,
#endif
};

static int __init ohci_hcd_omap_init (void)
{
	printk (KERN_DEBUG "%s: " DRIVER_INFO " (OMAP)\n", hcd_name);
	if (usb_disabled())
		return -ENODEV;

	pr_debug("%s: block sizes: ed %Zd td %Zd\n", hcd_name,
		sizeof (struct ed), sizeof (struct td));

	return driver_register(&ohci_hcd_omap_driver);
}

static void __exit ohci_hcd_omap_cleanup (void)
{
	driver_unregister(&ohci_hcd_omap_driver);
}

module_init (ohci_hcd_omap_init);
module_exit (ohci_hcd_omap_cleanup);
