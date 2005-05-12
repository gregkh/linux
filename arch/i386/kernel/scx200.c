/* linux/arch/i386/kernel/scx200.c 

   Copyright (c) 2001,2002 Christer Weinigel <wingel@nano-system.com>

   National Semiconductor SCx200 support. */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>

#include <linux/scx200.h>

#define NAME "scx200"

MODULE_AUTHOR("Christer Weinigel <wingel@nano-system.com>");
MODULE_DESCRIPTION("NatSemi SCx200 Driver");
MODULE_LICENSE("GPL");

unsigned scx200_gpio_base = 0;
long scx200_gpio_shadow[2];

static struct pci_device_id scx200_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_NS, PCI_DEVICE_ID_NS_SCx200_BRIDGE) },
	{ PCI_DEVICE(PCI_VENDOR_ID_NS, PCI_DEVICE_ID_NS_SC1100_BRIDGE) },
	{ },
};
MODULE_DEVICE_TABLE(pci,scx200_tbl);

static int __devinit scx200_probe(struct pci_dev *, const struct pci_device_id *);

static struct pci_driver scx200_pci_driver = {
	.name = "scx200",
	.id_table = scx200_tbl,
	.probe = scx200_probe,
};

DEFINE_SPINLOCK(scx200_gpio_lock);
static DEFINE_SPINLOCK(scx200_gpio_config_lock);

static int __devinit scx200_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int bank;
	unsigned base;

	base = pci_resource_start(pdev, 0);
	printk(KERN_INFO NAME ": GPIO base 0x%x\n", base);

	if (request_region(base, SCx200_GPIO_SIZE, "NatSemi SCx200 GPIO") == 0) {
		printk(KERN_ERR NAME ": can't allocate I/O for GPIOs\n");
		return -EBUSY;
	}

	scx200_gpio_base = base;

	/* read the current values driven on the GPIO signals */
	for (bank = 0; bank < 2; ++bank)
		scx200_gpio_shadow[bank] = inl(scx200_gpio_base + 0x10 * bank);

	return 0;

}

u32 scx200_gpio_configure(int index, u32 mask, u32 bits)
{
	u32 config, new_config;
	unsigned long flags;

	spin_lock_irqsave(&scx200_gpio_config_lock, flags);

	outl(index, scx200_gpio_base + 0x20);
	config = inl(scx200_gpio_base + 0x24);

	new_config = (config & mask) | bits;
	outl(new_config, scx200_gpio_base + 0x24);

	spin_unlock_irqrestore(&scx200_gpio_config_lock, flags);

	return config;
}

void scx200_gpio_dump(unsigned index)
{
	u32 config = scx200_gpio_configure(index, ~0, 0);
	printk(KERN_DEBUG "GPIO%02u: 0x%08lx", index, (unsigned long)config);
	
	if (config & 1) 
		printk(" OE"); /* output enabled */
	else
		printk(" TS"); /* tristate */
	if (config & 2) 
		printk(" PP"); /* push pull */
	else
		printk(" OD"); /* open drain */
	if (config & 4) 
		printk(" PUE"); /* pull up enabled */
	else
		printk(" PUD"); /* pull up disabled */
	if (config & 8) 
		printk(" LOCKED"); /* locked */
	if (config & 16) 
		printk(" LEVEL"); /* level input */
	else
		printk(" EDGE"); /* edge input */
	if (config & 32) 
		printk(" HI"); /* trigger on rising edge */
	else
		printk(" LO"); /* trigger on falling edge */
	if (config & 64) 
		printk(" DEBOUNCE"); /* debounce */
	printk("\n");
}

int __init scx200_init(void)
{
	printk(KERN_INFO NAME ": NatSemi SCx200 Driver\n");

	return pci_module_init(&scx200_pci_driver);
}

void __exit scx200_cleanup(void)
{
	pci_unregister_driver(&scx200_pci_driver);
	release_region(scx200_gpio_base, SCx200_GPIO_SIZE);
}

module_init(scx200_init);
module_exit(scx200_cleanup);

EXPORT_SYMBOL(scx200_gpio_base);
EXPORT_SYMBOL(scx200_gpio_shadow);
EXPORT_SYMBOL(scx200_gpio_lock);
EXPORT_SYMBOL(scx200_gpio_configure);
EXPORT_SYMBOL(scx200_gpio_dump);

/*
    Local variables:
        compile-command: "make -k -C ../../.. SUBDIRS=arch/i386/kernel modules"
        c-basic-offset: 8
    End:
*/
