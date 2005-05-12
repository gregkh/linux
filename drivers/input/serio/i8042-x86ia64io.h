#ifndef _I8042_X86IA64IO_H
#define _I8042_X86IA64IO_H

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

/*
 * Names.
 */

#define I8042_KBD_PHYS_DESC "isa0060/serio0"
#define I8042_AUX_PHYS_DESC "isa0060/serio1"
#define I8042_MUX_PHYS_DESC "isa0060/serio%d"

/*
 * IRQs.
 */

#if defined(__ia64__)
# define I8042_MAP_IRQ(x)	isa_irq_to_vector((x))
#else
# define I8042_MAP_IRQ(x)	(x)
#endif

#define I8042_KBD_IRQ	i8042_kbd_irq
#define I8042_AUX_IRQ	i8042_aux_irq

static int i8042_kbd_irq;
static int i8042_aux_irq;

/*
 * Register numbers.
 */

#define I8042_COMMAND_REG	i8042_command_reg
#define I8042_STATUS_REG	i8042_command_reg
#define I8042_DATA_REG		i8042_data_reg

static int i8042_command_reg = 0x64;
static int i8042_data_reg = 0x60;


static inline int i8042_read_data(void)
{
	return inb(I8042_DATA_REG);
}

static inline int i8042_read_status(void)
{
	return inb(I8042_STATUS_REG);
}

static inline void i8042_write_data(int val)
{
	outb(val, I8042_DATA_REG);
}

static inline void i8042_write_command(int val)
{
	outb(val, I8042_COMMAND_REG);
}

#if defined(__i386__)

#include <linux/dmi.h>

static struct dmi_system_id __initdata i8042_dmi_table[] = {
	{
		.ident = "Compaq Proliant 8500",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Compaq"),
			DMI_MATCH(DMI_PRODUCT_NAME , "ProLiant"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "8500"),
		},
	},
	{
		.ident = "Compaq Proliant DL760",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Compaq"),
			DMI_MATCH(DMI_PRODUCT_NAME , "ProLiant"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "DL760"),
		},
	},
	{ }
};
#endif

#ifdef CONFIG_ACPI
#include <linux/acpi.h>
#include <acpi/acpi_bus.h>

struct i8042_acpi_resources {
	unsigned int port1;
	unsigned int port2;
	unsigned int irq;
};

static int i8042_acpi_kbd_registered;
static int i8042_acpi_aux_registered;

static acpi_status i8042_acpi_parse_resource(struct acpi_resource *res, void *data)
{
	struct i8042_acpi_resources *i8042_res = data;
	struct acpi_resource_io *io;
	struct acpi_resource_fixed_io *fixed_io;
	struct acpi_resource_irq *irq;
	struct acpi_resource_ext_irq *ext_irq;

	switch (res->id) {
		case ACPI_RSTYPE_IO:
			io = &res->data.io;
			if (io->range_length) {
				if (!i8042_res->port1)
					i8042_res->port1 = io->min_base_address;
				else
					i8042_res->port2 = io->min_base_address;
			}
			break;

		case ACPI_RSTYPE_FIXED_IO:
			fixed_io = &res->data.fixed_io;
			if (fixed_io->range_length) {
				if (!i8042_res->port1)
					i8042_res->port1 = fixed_io->base_address;
				else
					i8042_res->port2 = fixed_io->base_address;
			}
			break;

		case ACPI_RSTYPE_IRQ:
			irq = &res->data.irq;
			if (irq->number_of_interrupts > 0)
				i8042_res->irq =
					acpi_register_gsi(irq->interrupts[0],
							  irq->edge_level,
							  irq->active_high_low);
			break;

		case ACPI_RSTYPE_EXT_IRQ:
			ext_irq = &res->data.extended_irq;
			if (ext_irq->number_of_interrupts > 0)
				i8042_res->irq =
					acpi_register_gsi(ext_irq->interrupts[0],
							  ext_irq->edge_level,
							  ext_irq->active_high_low);
			break;
	}
	return AE_OK;
}

static int i8042_acpi_kbd_add(struct acpi_device *device)
{
	struct i8042_acpi_resources kbd_res;
	acpi_status status;

	memset(&kbd_res, 0, sizeof(kbd_res));
	status = acpi_walk_resources(device->handle, METHOD_NAME__CRS,
				     i8042_acpi_parse_resource, &kbd_res);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	if (kbd_res.port1)
		i8042_data_reg = kbd_res.port1;
	else
		printk(KERN_WARNING "ACPI: [%s] has no data port; default is 0x%x\n",
			acpi_device_bid(device), i8042_data_reg);

	if (kbd_res.port2)
		i8042_command_reg = kbd_res.port2;
	else
		printk(KERN_WARNING "ACPI: [%s] has no command port; default is 0x%x\n",
			acpi_device_bid(device), i8042_command_reg);

	if (kbd_res.irq)
		i8042_kbd_irq = kbd_res.irq;
	else
		printk(KERN_WARNING "ACPI: [%s] has no IRQ; default is %d\n",
			acpi_device_bid(device), i8042_kbd_irq);

	strncpy(acpi_device_name(device), "PS/2 Keyboard Controller",
		sizeof(acpi_device_name(device)));
	printk("ACPI: %s [%s] at I/O 0x%x, 0x%x, irq %d\n",
		acpi_device_name(device), acpi_device_bid(device),
		i8042_data_reg, i8042_command_reg, i8042_kbd_irq);

	return 0;
}

static int i8042_acpi_aux_add(struct acpi_device *device)
{
	struct i8042_acpi_resources aux_res;
	acpi_status status;

	memset(&aux_res, 0, sizeof(aux_res));
	status = acpi_walk_resources(device->handle, METHOD_NAME__CRS,
				     i8042_acpi_parse_resource, &aux_res);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	if (aux_res.irq)
		i8042_aux_irq = aux_res.irq;
	else
		printk(KERN_WARNING "ACPI: [%s] has no IRQ; default is %d\n",
			acpi_device_bid(device), i8042_aux_irq);

	strncpy(acpi_device_name(device), "PS/2 Mouse Controller",
		sizeof(acpi_device_name(device)));
	printk("ACPI: %s [%s] at irq %d\n",
		acpi_device_name(device), acpi_device_bid(device), i8042_aux_irq);

	return 0;
}

static struct acpi_driver i8042_acpi_kbd_driver = {
	.name		= "i8042",
	.ids		= "PNP0303,PNP030B",
	.ops		= {
		.add		= i8042_acpi_kbd_add,
	},
};

static struct acpi_driver i8042_acpi_aux_driver = {
	.name		= "i8042",
	.ids		= "PNP0F03,PNP0F0B,PNP0F0E,PNP0F12,PNP0F13,SYN0801",
	.ops		= {
		.add		= i8042_acpi_aux_add,
	},
};

static int i8042_acpi_init(void)
{
	int result;

	if (acpi_disabled || i8042_noacpi) {
		printk("i8042: ACPI detection disabled\n");
		return 0;
	}

	result = acpi_bus_register_driver(&i8042_acpi_kbd_driver);
	if (result < 0)
		return result;

	if (result == 0) {
		acpi_bus_unregister_driver(&i8042_acpi_kbd_driver);
		return -ENODEV;
	}
	i8042_acpi_kbd_registered = 1;

	result = acpi_bus_register_driver(&i8042_acpi_aux_driver);
	if (result >= 0)
		i8042_acpi_aux_registered = 1;
	if (result == 0)
		i8042_noaux = 1;

	return 0;
}

static void i8042_acpi_exit(void)
{
	if (i8042_acpi_kbd_registered)
		acpi_bus_unregister_driver(&i8042_acpi_kbd_driver);

	if (i8042_acpi_aux_registered)
		acpi_bus_unregister_driver(&i8042_acpi_aux_driver);
}
#endif

static inline int i8042_platform_init(void)
{
/*
 * On ix86 platforms touching the i8042 data register region can do really
 * bad things. Because of this the region is always reserved on ix86 boxes.
 *
 *	if (!request_region(I8042_DATA_REG, 16, "i8042"))
 *		return -1;
 */

	i8042_kbd_irq = I8042_MAP_IRQ(1);
	i8042_aux_irq = I8042_MAP_IRQ(12);

#ifdef CONFIG_ACPI
	if (i8042_acpi_init())
		return -1;
#endif

#if defined(__ia64__)
        i8042_reset = 1;
#endif

#if defined(__i386__)
	if (dmi_check_system(i8042_dmi_table))
		i8042_noloop = 1;
#endif

	return 0;
}

static inline void i8042_platform_exit(void)
{
#ifdef CONFIG_ACPI
	i8042_acpi_exit();
#endif
}

#endif /* _I8042_X86IA64IO_H */
