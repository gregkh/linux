/*
 * UniNorth AGPGART routines.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/agp_backend.h>
#include <asm/uninorth.h>
#include <asm/pci-bridge.h>
#include "agp.h"

static int uninorth_fetch_size(void)
{
	int i;
	u32 temp;
	struct aper_size_info_32 *values;

	pci_read_config_dword(agp_bridge->dev, UNI_N_CFG_GART_BASE, &temp);
	temp &= ~(0xfffff000);
	values = A_SIZE_32(agp_bridge->driver->aperture_sizes);

	for (i = 0; i < agp_bridge->driver->num_aperture_sizes; i++) {
		if (temp == values[i].size_value) {
			agp_bridge->previous_size =
			    agp_bridge->current_size = (void *) (values + i);
			agp_bridge->aperture_size_idx = i;
			return values[i].size;
		}
	}

	agp_bridge->previous_size =
	    agp_bridge->current_size = (void *) (values + 1);
	agp_bridge->aperture_size_idx = 1;
	return values[1].size;

	return 0;
}

static void uninorth_tlbflush(struct agp_memory *mem)
{
	pci_write_config_dword(agp_bridge->dev, UNI_N_CFG_GART_CTRL,
			UNI_N_CFG_GART_ENABLE | UNI_N_CFG_GART_INVAL);
	pci_write_config_dword(agp_bridge->dev, UNI_N_CFG_GART_CTRL,
			UNI_N_CFG_GART_ENABLE);
	pci_write_config_dword(agp_bridge->dev, UNI_N_CFG_GART_CTRL,
			UNI_N_CFG_GART_ENABLE | UNI_N_CFG_GART_2xRESET);
	pci_write_config_dword(agp_bridge->dev, UNI_N_CFG_GART_CTRL,
			UNI_N_CFG_GART_ENABLE);
}

static void uninorth_cleanup(void)
{
	pci_write_config_dword(agp_bridge->dev, UNI_N_CFG_GART_CTRL,
			UNI_N_CFG_GART_ENABLE | UNI_N_CFG_GART_INVAL);
	pci_write_config_dword(agp_bridge->dev, UNI_N_CFG_GART_CTRL,
			0);
	pci_write_config_dword(agp_bridge->dev, UNI_N_CFG_GART_CTRL,
			UNI_N_CFG_GART_2xRESET);
	pci_write_config_dword(agp_bridge->dev, UNI_N_CFG_GART_CTRL,
			0);
}

static int uninorth_configure(void)
{
	struct aper_size_info_32 *current_size;
	
	current_size = A_SIZE_32(agp_bridge->current_size);

	printk(KERN_INFO PFX "configuring for size idx: %d\n",
	       current_size->size_value);
	
	/* aperture size and gatt addr */
	pci_write_config_dword(agp_bridge->dev,
		UNI_N_CFG_GART_BASE,
		(agp_bridge->gatt_bus_addr & 0xfffff000)
			| current_size->size_value);

	/* HACK ALERT
	 * UniNorth seem to be buggy enough not to handle properly when
	 * the AGP aperture isn't mapped at bus physical address 0
	 */
	agp_bridge->gart_bus_addr = 0;
	pci_write_config_dword(agp_bridge->dev,
		UNI_N_CFG_AGP_BASE, agp_bridge->gart_bus_addr);
	
	return 0;
}

static int uninorth_insert_memory(struct agp_memory *mem, off_t pg_start,
				int type)
{
	int i, j, num_entries;
	void *temp;

	temp = agp_bridge->current_size;
	num_entries = A_SIZE_32(temp)->num_entries;

	if (type != 0 || mem->type != 0)
		/* We know nothing of memory types */
		return -EINVAL;
	if ((pg_start + mem->page_count) > num_entries)
		return -EINVAL;

	j = pg_start;

	while (j < (pg_start + mem->page_count)) {
		if (!PGE_EMPTY(agp_bridge, agp_bridge->gatt_table[j]))
			return -EBUSY;
		j++;
	}

	for (i = 0, j = pg_start; i < mem->page_count; i++, j++) {
		agp_bridge->gatt_table[j] = cpu_to_le32((mem->memory[i] & 0xfffff000) | 0x00000001UL);
		flush_dcache_range((unsigned long)__va(mem->memory[i]),
				   (unsigned long)__va(mem->memory[i])+0x1000);
	}
	(void)in_le32((volatile u32*)&agp_bridge->gatt_table[pg_start]);
	mb();
	flush_dcache_range((unsigned long)&agp_bridge->gatt_table[pg_start], 
		(unsigned long)&agp_bridge->gatt_table[pg_start + mem->page_count]);

	uninorth_tlbflush(mem);
	return 0;
}

static void uninorth_agp_enable(u32 mode)
{
	u32 command, scratch;
	int timeout;

	pci_read_config_dword(agp_bridge->dev,
			      agp_bridge->capndx + PCI_AGP_STATUS,
			      &command);

	command = agp_collect_device_status(mode, command);
	command |= 0x100;

	uninorth_tlbflush(NULL);

	timeout = 0;
	do {
		pci_write_config_dword(agp_bridge->dev,
				       agp_bridge->capndx + PCI_AGP_COMMAND,
				       command);
		pci_read_config_dword(agp_bridge->dev,
				       agp_bridge->capndx + PCI_AGP_COMMAND,
				       &scratch);
	} while ((scratch & 0x100) == 0 && ++timeout < 1000);
	if ((scratch & 0x100) == 0)
		printk(KERN_ERR PFX "failed to write UniNorth AGP command reg\n");

	agp_device_command(command, 0);

	uninorth_tlbflush(NULL);
}

static int uninorth_create_gatt_table(void)
{
	char *table;
	char *table_end;
	int size;
	int page_order;
	int num_entries;
	int i;
	void *temp;
	struct page *page;

	/* We can't handle 2 level gatt's */
	if (agp_bridge->driver->size_type == LVL2_APER_SIZE)
		return -EINVAL;

	table = NULL;
	i = agp_bridge->aperture_size_idx;
	temp = agp_bridge->current_size;
	size = page_order = num_entries = 0;

	do {
		size = A_SIZE_32(temp)->size;
		page_order = A_SIZE_32(temp)->page_order;
		num_entries = A_SIZE_32(temp)->num_entries;

		table = (char *) __get_free_pages(GFP_KERNEL, page_order);

		if (table == NULL) {
			i++;
			agp_bridge->current_size = A_IDX32(agp_bridge);
		} else {
			agp_bridge->aperture_size_idx = i;
		}
	} while (!table && (i < agp_bridge->driver->num_aperture_sizes));

	if (table == NULL)
		return -ENOMEM;

	table_end = table + ((PAGE_SIZE * (1 << page_order)) - 1);

	for (page = virt_to_page(table); page <= virt_to_page(table_end); page++)
		SetPageReserved(page);

	agp_bridge->gatt_table_real = (u32 *) table;
	agp_bridge->gatt_table = (u32 *)table;
	agp_bridge->gatt_bus_addr = virt_to_phys(table);

	for (i = 0; i < num_entries; i++) {
		agp_bridge->gatt_table[i] =
		    (unsigned long) agp_bridge->scratch_page;
	}

	flush_dcache_range((unsigned long)table, (unsigned long)table_end);

	return 0;
}

static int uninorth_free_gatt_table(void)
{
	int page_order;
	char *table, *table_end;
	void *temp;
	struct page *page;

	temp = agp_bridge->current_size;
	page_order = A_SIZE_32(temp)->page_order;

	/* Do not worry about freeing memory, because if this is
	 * called, then all agp memory is deallocated and removed
	 * from the table.
	 */

	table = (char *) agp_bridge->gatt_table_real;
	table_end = table + ((PAGE_SIZE * (1 << page_order)) - 1);

	for (page = virt_to_page(table); page <= virt_to_page(table_end); page++)
		ClearPageReserved(page);

	free_pages((unsigned long) agp_bridge->gatt_table_real, page_order);

	return 0;
}

void null_cache_flush(void)
{
	mb();
}

/* Setup function */

static struct aper_size_info_32 uninorth_sizes[7] =
{
#if 0 /* Not sure uninorth supports that high aperture sizes */
	{256, 65536, 6, 64},
	{128, 32768, 5, 32},
	{64, 16384, 4, 16},
#endif	
	{32, 8192, 3, 8},
	{16, 4096, 2, 4},
	{8, 2048, 1, 2},
	{4, 1024, 0, 1}
};

struct agp_bridge_driver uninorth_agp_driver = {
	.owner			= THIS_MODULE,
	.aperture_sizes		= (void *)uninorth_sizes,
	.size_type		= U32_APER_SIZE,
	.num_aperture_sizes	= 4,
	.configure		= uninorth_configure,
	.fetch_size		= uninorth_fetch_size,
	.cleanup		= uninorth_cleanup,
	.tlb_flush		= uninorth_tlbflush,
	.mask_memory		= agp_generic_mask_memory,
	.masks			= NULL,
	.cache_flush		= null_cache_flush,
	.agp_enable		= uninorth_agp_enable,
	.create_gatt_table	= uninorth_create_gatt_table,
	.free_gatt_table	= uninorth_free_gatt_table,
	.insert_memory		= uninorth_insert_memory,
	.remove_memory		= agp_generic_remove_memory,
	.alloc_by_type		= agp_generic_alloc_by_type,
	.free_by_type		= agp_generic_free_by_type,
	.agp_alloc_page		= agp_generic_alloc_page,
	.agp_destroy_page	= agp_generic_destroy_page,
	.cant_use_aperture	= 1,
};

static struct agp_device_ids uninorth_agp_device_ids[] __devinitdata = {
	{
		.device_id	= PCI_DEVICE_ID_APPLE_UNI_N_AGP,
		.chipset_name	= "UniNorth",
	},
	{
		.device_id	= PCI_DEVICE_ID_APPLE_UNI_N_AGP_P,
		.chipset_name	= "UniNorth/Pangea",
	},
	{
		.device_id	= PCI_DEVICE_ID_APPLE_UNI_N_AGP15,
		.chipset_name	= "UniNorth 1.5",
	},
	{
		.device_id	= PCI_DEVICE_ID_APPLE_UNI_N_AGP2,
		.chipset_name	= "UniNorth 2",
	},
};

static int __devinit agp_uninorth_probe(struct pci_dev *pdev,
					const struct pci_device_id *ent)
{
	struct agp_device_ids *devs = uninorth_agp_device_ids;
	struct agp_bridge_data *bridge;
	u8 cap_ptr;
	int j;

	cap_ptr = pci_find_capability(pdev, PCI_CAP_ID_AGP);
	if (cap_ptr == 0)
		return -ENODEV;

	/* probe for known chipsets */
	for (j = 0; devs[j].chipset_name != NULL; ++j) {
		if (pdev->device == devs[j].device_id) {
			printk(KERN_INFO PFX "Detected Apple %s chipset\n",
			       devs[j].chipset_name);
			goto found;
		}
	}

	printk(KERN_ERR PFX "Unsupported Apple chipset (device id: %04x).\n",
		pdev->device);
	return -ENODEV;

 found:
	bridge = agp_alloc_bridge();
	if (!bridge)
		return -ENOMEM;

	bridge->driver = &uninorth_agp_driver;
	bridge->dev = pdev;
	bridge->capndx = cap_ptr;

	/* Fill in the mode register */
	pci_read_config_dword(pdev, cap_ptr+PCI_AGP_STATUS, &bridge->mode);

	pci_set_drvdata(pdev, bridge);
	return agp_add_bridge(bridge);
}

static void __devexit agp_uninorth_remove(struct pci_dev *pdev)
{
	struct agp_bridge_data *bridge = pci_get_drvdata(pdev);

	agp_remove_bridge(bridge);
	agp_put_bridge(bridge);
}

static struct pci_device_id agp_uninorth_pci_table[] = {
	{
	.class		= (PCI_CLASS_BRIDGE_HOST << 8),
	.class_mask	= ~0,
	.vendor		= PCI_VENDOR_ID_APPLE,
	.device		= PCI_ANY_ID,
	.subvendor	= PCI_ANY_ID,
	.subdevice	= PCI_ANY_ID,
	},
	{ }
};

MODULE_DEVICE_TABLE(pci, agp_uninorth_pci_table);

static struct pci_driver agp_uninorth_pci_driver = {
	.name		= "agpgart-uninorth",
	.id_table	= agp_uninorth_pci_table,
	.probe		= agp_uninorth_probe,
	.remove		= agp_uninorth_remove,
};

static int __init agp_uninorth_init(void)
{
	if (agp_off)
		return -EINVAL;
	return pci_module_init(&agp_uninorth_pci_driver);
}

static void __exit agp_uninorth_cleanup(void)
{
	pci_unregister_driver(&agp_uninorth_pci_driver);
}

module_init(agp_uninorth_init);
module_exit(agp_uninorth_cleanup);

MODULE_AUTHOR("Ben Herrenschmidt & Paul Mackerras");
MODULE_LICENSE("GPL");
