/*
 * Intel MCH AGPGART routines.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/agp_backend.h>
#include "agp.h"


#define AGP_DCACHE_MEMORY	1
#define AGP_PHYS_MEMORY		2

static struct gatt_mask intel_i810_masks[] =
{
	{.mask = I810_PTE_VALID, .type = 0},
	{.mask = (I810_PTE_VALID | I810_PTE_LOCAL), .type = AGP_DCACHE_MEMORY},
	{.mask = I810_PTE_VALID, .type = 0}
};

static void intel_i810_tlbflush(struct agp_memory *mem)
{
	return;
}

static void intel_i810_agp_enable(u32 mode)
{
	return;
}


/*
 * The i810/i830 requires a physical address to program its mouse
 * pointer into hardware.
 * However the Xserver still writes to it through the agp aperture.
 */
static struct agp_memory *alloc_agpphysmem_i8xx(size_t pg_count, int type)
{
	struct agp_memory *new;
	void *addr;

	if (pg_count != 1)
		return NULL;

	addr = agp_bridge->driver->agp_alloc_page();
	if (addr == NULL)
		return NULL;

	new = agp_create_memory(1);
	if (new == NULL)
		return NULL;

	new->memory[0] = virt_to_phys(addr);
	new->page_count = 1;
	new->num_scratch_pages = 1;
	new->type = AGP_PHYS_MEMORY;
	new->physical = new->memory[0];
	return new;
}

static void intel_i810_free_by_type(struct agp_memory *curr)
{
	agp_free_key(curr->key);
	if(curr->type == AGP_PHYS_MEMORY) {
		agp_bridge->driver->agp_destroy_page(phys_to_virt(curr->memory[0]));
		vfree(curr->memory);
	}
	kfree(curr);
}

static unsigned long intel_i810_mask_memory(unsigned long addr, int type)
{
	/* Type checking must be done elsewhere */
	return addr | agp_bridge->driver->masks[type].mask;
}

static struct aper_size_info_fixed intel_i830_sizes[] =
{
	{128, 32768, 5},
	/* The 64M mode still requires a 128k gatt */
	{64, 16384, 5}
};

static struct _intel_i830_private {
	struct pci_dev *i830_dev;		/* device one */
	volatile u8 __iomem *registers;
	int gtt_entries;
} intel_i830_private;

static void intel_i830_init_gtt_entries(void)
{
	u16 gmch_ctrl;
	int gtt_entries;
	u8 rdct;
	int local = 0;
	static const int ddt[4] = { 0, 16, 32, 64 };

	pci_read_config_word(agp_bridge->dev,I830_GMCH_CTRL,&gmch_ctrl);

	if (agp_bridge->dev->device == PCI_DEVICE_ID_INTEL_82830_HB ||
	    agp_bridge->dev->device == PCI_DEVICE_ID_INTEL_82845G_HB) {
		switch (gmch_ctrl & I830_GMCH_GMS_MASK) {
		case I830_GMCH_GMS_STOLEN_512:
			gtt_entries = KB(512) - KB(132);
			break;
		case I830_GMCH_GMS_STOLEN_1024:
			gtt_entries = MB(1) - KB(132);
			break;
		case I830_GMCH_GMS_STOLEN_8192:
			gtt_entries = MB(8) - KB(132);
			break;
		case I830_GMCH_GMS_LOCAL:
			rdct = readb(intel_i830_private.registers+I830_RDRAM_CHANNEL_TYPE);
			gtt_entries = (I830_RDRAM_ND(rdct) + 1) *
					MB(ddt[I830_RDRAM_DDT(rdct)]);
			local = 1;
			break;
		default:
			gtt_entries = 0;
			break;
		}
	} else {
		switch (gmch_ctrl & I830_GMCH_GMS_MASK) {
		case I855_GMCH_GMS_STOLEN_1M:
			gtt_entries = MB(1) - KB(132);
			break;
		case I855_GMCH_GMS_STOLEN_4M:
			gtt_entries = MB(4) - KB(132);
			break;
		case I855_GMCH_GMS_STOLEN_8M:
			gtt_entries = MB(8) - KB(132);
			break;
		case I855_GMCH_GMS_STOLEN_16M:
			gtt_entries = MB(16) - KB(132);
			break;
		case I855_GMCH_GMS_STOLEN_32M:
			gtt_entries = MB(32) - KB(132);
			break;
		default:
			gtt_entries = 0;
			break;
		}
	}
	if (gtt_entries > 0)
		printk(KERN_INFO PFX "Detected %dK %s memory.\n",
		       gtt_entries / KB(1), local ? "local" : "stolen");
	else
		printk(KERN_INFO PFX
		       "No pre-allocated video memory detected.\n");
	gtt_entries /= KB(4);

	intel_i830_private.gtt_entries = gtt_entries;
}

/* The intel i830 automatically initializes the agp aperture during POST.
 * Use the memory already set aside for in the GTT.
 */
static int intel_i830_create_gatt_table(void)
{
	int page_order;
	struct aper_size_info_fixed *size;
	int num_entries;
	u32 temp;

	size = agp_bridge->current_size;
	page_order = size->page_order;
	num_entries = size->num_entries;
	agp_bridge->gatt_table_real = NULL;

	pci_read_config_dword(intel_i830_private.i830_dev,I810_MMADDR,&temp);
	temp &= 0xfff80000;

	intel_i830_private.registers = (volatile u8 __iomem*) ioremap(temp,128 * 4096);
	if (!intel_i830_private.registers)
		return -ENOMEM;

	temp = readl(intel_i830_private.registers+I810_PGETBL_CTL) & 0xfffff000;
	global_cache_flush();	/* FIXME: ?? */

	/* we have to call this as early as possible after the MMIO base address is known */
	intel_i830_init_gtt_entries();

	agp_bridge->gatt_table = NULL;

	agp_bridge->gatt_bus_addr = temp;

	return 0;
}

/* Return the gatt table to a sane state. Use the top of stolen
 * memory for the GTT.
 */
static int intel_i830_free_gatt_table(void)
{
	return 0;
}

static int intel_i830_fetch_size(void)
{
	u16 gmch_ctrl;
	struct aper_size_info_fixed *values;

	values = A_SIZE_FIX(agp_bridge->driver->aperture_sizes);

	if (agp_bridge->dev->device != PCI_DEVICE_ID_INTEL_82830_HB &&
	    agp_bridge->dev->device != PCI_DEVICE_ID_INTEL_82845G_HB) {
		/* 855GM/852GM/865G has 128MB aperture size */
		agp_bridge->previous_size = agp_bridge->current_size = (void *) values;
		agp_bridge->aperture_size_idx = 0;
		return values[0].size;
	}

	pci_read_config_word(agp_bridge->dev,I830_GMCH_CTRL,&gmch_ctrl);

	if ((gmch_ctrl & I830_GMCH_MEM_MASK) == I830_GMCH_MEM_128M) {
		agp_bridge->previous_size = agp_bridge->current_size = (void *) values;
		agp_bridge->aperture_size_idx = 0;
		return values[0].size;
	} else {
		agp_bridge->previous_size = agp_bridge->current_size = (void *) values;
		agp_bridge->aperture_size_idx = 1;
		return values[1].size;
	}

	return 0;
}

static int intel_i830_configure(void)
{
	struct aper_size_info_fixed *current_size;
	u32 temp;
	u16 gmch_ctrl;
	int i;

	current_size = A_SIZE_FIX(agp_bridge->current_size);

	pci_read_config_dword(intel_i830_private.i830_dev,I810_GMADDR,&temp);
	agp_bridge->gart_bus_addr = (temp & PCI_BASE_ADDRESS_MEM_MASK);

	pci_read_config_word(agp_bridge->dev,I830_GMCH_CTRL,&gmch_ctrl);
	gmch_ctrl |= I830_GMCH_ENABLED;
	pci_write_config_word(agp_bridge->dev,I830_GMCH_CTRL,gmch_ctrl);

	writel(agp_bridge->gatt_bus_addr|I810_PGETBL_ENABLED, intel_i830_private.registers+I810_PGETBL_CTL);
	readl(intel_i830_private.registers+I810_PGETBL_CTL);	/* PCI Posting. */

	if (agp_bridge->driver->needs_scratch_page) {
		for (i = intel_i830_private.gtt_entries; i < current_size->num_entries; i++) {
			writel(agp_bridge->scratch_page, intel_i830_private.registers+I810_PTE_BASE+(i*4));
			readl(intel_i830_private.registers+I810_PTE_BASE+(i*4));	/* PCI Posting. */
		}
	}
	global_cache_flush();
	return 0;
}

static void intel_i830_cleanup(void)
{
	iounmap((void __iomem *) intel_i830_private.registers);
}

static int intel_i830_insert_entries(struct agp_memory *mem,off_t pg_start,
				int type)
{
	int i,j,num_entries;
	void *temp;

	temp = agp_bridge->current_size;
	num_entries = A_SIZE_FIX(temp)->num_entries;

	if (pg_start < intel_i830_private.gtt_entries) {
		printk (KERN_DEBUG PFX "pg_start == 0x%.8lx,intel_i830_private.gtt_entries == 0x%.8x\n",
				pg_start,intel_i830_private.gtt_entries);

		printk (KERN_INFO PFX "Trying to insert into local/stolen memory\n");
		return -EINVAL;
	}

	if ((pg_start + mem->page_count) > num_entries)
		return -EINVAL;

	/* The i830 can't check the GTT for entries since its read only,
	 * depend on the caller to make the correct offset decisions.
	 */

	if ((type != 0 && type != AGP_PHYS_MEMORY) ||
		(mem->type != 0 && mem->type != AGP_PHYS_MEMORY))
		return -EINVAL;

	global_cache_flush();	/* FIXME: ?? */

	for (i = 0, j = pg_start; i < mem->page_count; i++, j++) {
		writel(agp_bridge->driver->mask_memory(mem->memory[i], mem->type),
				intel_i830_private.registers+I810_PTE_BASE+(j*4));
		readl(intel_i830_private.registers+I810_PTE_BASE+(j*4));	/* PCI Posting. */
	}

	global_cache_flush();

	agp_bridge->driver->tlb_flush(mem);

	return 0;
}

static int intel_i830_remove_entries(struct agp_memory *mem,off_t pg_start,
				int type)
{
	int i;

	global_cache_flush();

	if (pg_start < intel_i830_private.gtt_entries) {
		printk (KERN_INFO PFX "Trying to disable local/stolen memory\n");
		return -EINVAL;
	}

	for (i = pg_start; i < (mem->page_count + pg_start); i++) {
		writel(agp_bridge->scratch_page, intel_i830_private.registers+I810_PTE_BASE+(i*4));
		readl(intel_i830_private.registers+I810_PTE_BASE+(i*4));	/* PCI Posting. */
	}

	global_cache_flush();
	agp_bridge->driver->tlb_flush(mem);
	return 0;
}

static struct agp_memory *intel_i830_alloc_by_type(size_t pg_count,int type)
{
	if (type == AGP_PHYS_MEMORY)
		return alloc_agpphysmem_i8xx(pg_count, type);

	/* always return NULL for other allocation types for now */
	return NULL;
}

static int intel_8xx_fetch_size(void)
{
	u8 temp;
	int i;
	struct aper_size_info_8 *values;

	pci_read_config_byte(agp_bridge->dev, INTEL_APSIZE, &temp);

	values = A_SIZE_8(agp_bridge->driver->aperture_sizes);

	for (i = 0; i < agp_bridge->driver->num_aperture_sizes; i++) {
		if (temp == values[i].size_value) {
			agp_bridge->previous_size =
				agp_bridge->current_size = (void *) (values + i);
			agp_bridge->aperture_size_idx = i;
			return values[i].size;
		}
	}
	return 0;
}

static void intel_8xx_tlbflush(struct agp_memory *mem)
{
	u32 temp;
	pci_read_config_dword(agp_bridge->dev, INTEL_AGPCTRL, &temp);
	pci_write_config_dword(agp_bridge->dev, INTEL_AGPCTRL, temp & ~(1 << 7));
	pci_read_config_dword(agp_bridge->dev, INTEL_AGPCTRL, &temp);
	pci_write_config_dword(agp_bridge->dev, INTEL_AGPCTRL, temp | (1 << 7));
}

static void intel_8xx_cleanup(void)
{
	u16 temp;
	struct aper_size_info_8 *previous_size;

	previous_size = A_SIZE_8(agp_bridge->previous_size);
	pci_read_config_word(agp_bridge->dev, INTEL_NBXCFG, &temp);
	pci_write_config_word(agp_bridge->dev, INTEL_NBXCFG, temp & ~(1 << 9));
	pci_write_config_byte(agp_bridge->dev, INTEL_APSIZE, previous_size->size_value);
}

static int intel_845_configure(void)
{
	u32 temp;
	u8 temp2;
	struct aper_size_info_8 *current_size;

	current_size = A_SIZE_8(agp_bridge->current_size);

	/* aperture size */
	pci_write_config_byte(agp_bridge->dev, INTEL_APSIZE, current_size->size_value); 

	/* address to map to */
	pci_read_config_dword(agp_bridge->dev, AGP_APBASE, &temp);
	agp_bridge->gart_bus_addr = (temp & PCI_BASE_ADDRESS_MEM_MASK);

	/* attbase - aperture base */
	pci_write_config_dword(agp_bridge->dev, INTEL_ATTBASE, agp_bridge->gatt_bus_addr); 

	/* agpctrl */
	pci_write_config_dword(agp_bridge->dev, INTEL_AGPCTRL, 0x0000); 

	/* agpm */
	pci_read_config_byte(agp_bridge->dev, INTEL_I845_AGPM, &temp2);
	pci_write_config_byte(agp_bridge->dev, INTEL_I845_AGPM, temp2 | (1 << 1));
	/* clear any possible error conditions */
	pci_write_config_word(agp_bridge->dev, INTEL_I845_ERRSTS, 0x001c); 
	return 0;
}


/* Setup function */
static struct gatt_mask intel_generic_masks[] =
{
	{.mask = 0x00000017, .type = 0}
};

static struct aper_size_info_8 intel_8xx_sizes[7] =
{
	{256, 65536, 6, 0},
	{128, 32768, 5, 32},
	{64, 16384, 4, 48},
	{32, 8192, 3, 56},
	{16, 4096, 2, 60},
	{8, 2048, 1, 62},
	{4, 1024, 0, 63}
};

static struct agp_bridge_driver intel_830_driver = {
	.owner			= THIS_MODULE,
	.aperture_sizes		= intel_i830_sizes,
	.size_type		= FIXED_APER_SIZE,
	.num_aperture_sizes 	= 2,
	.needs_scratch_page	= TRUE,
	.configure		= intel_i830_configure,
	.fetch_size		= intel_i830_fetch_size,
	.cleanup		= intel_i830_cleanup,
	.tlb_flush		= intel_i810_tlbflush,
	.mask_memory		= intel_i810_mask_memory,
	.masks			= intel_i810_masks,
	.agp_enable		= intel_i810_agp_enable,
	.cache_flush		= global_cache_flush,
	.create_gatt_table	= intel_i830_create_gatt_table,
	.free_gatt_table	= intel_i830_free_gatt_table,
	.insert_memory		= intel_i830_insert_entries,
	.remove_memory		= intel_i830_remove_entries,
	.alloc_by_type		= intel_i830_alloc_by_type,
	.free_by_type		= intel_i810_free_by_type,
	.agp_alloc_page		= agp_generic_alloc_page,
	.agp_destroy_page	= agp_generic_destroy_page,
};

static struct agp_bridge_driver intel_845_driver = {
	.owner			= THIS_MODULE,
	.aperture_sizes		= intel_8xx_sizes,
	.size_type		= U8_APER_SIZE,
	.num_aperture_sizes	= 7,
	.configure		= intel_845_configure,
	.fetch_size		= intel_8xx_fetch_size,
	.cleanup		= intel_8xx_cleanup,
	.tlb_flush		= intel_8xx_tlbflush,
	.mask_memory		= agp_generic_mask_memory,
	.masks			= intel_generic_masks,
	.agp_enable		= agp_generic_enable,
	.cache_flush		= global_cache_flush,
	.create_gatt_table	= agp_generic_create_gatt_table,
	.free_gatt_table	= agp_generic_free_gatt_table,
	.insert_memory		= agp_generic_insert_memory,
	.remove_memory		= agp_generic_remove_memory,
	.alloc_by_type		= agp_generic_alloc_by_type,
	.free_by_type		= agp_generic_free_by_type,
	.agp_alloc_page		= agp_generic_alloc_page,
	.agp_destroy_page	= agp_generic_destroy_page,
};


static int find_i830(u16 device)
{
	struct pci_dev *i830_dev;

	i830_dev = pci_get_device(PCI_VENDOR_ID_INTEL, device, NULL);
	if (i830_dev && PCI_FUNC(i830_dev->devfn) != 0) {
		i830_dev = pci_get_device(PCI_VENDOR_ID_INTEL,
				device, i830_dev);
	}

	if (!i830_dev)
		return 0;

	intel_i830_private.i830_dev = i830_dev;
	return 1;
}

static int __devinit agp_intelmch_probe(struct pci_dev *pdev,
				     const struct pci_device_id *ent)
{
	struct agp_bridge_data *bridge;
	struct resource *r;
	char *name = "(unknown)";
	u8 cap_ptr = 0;

	cap_ptr = pci_find_capability(pdev, PCI_CAP_ID_AGP);
	if (!cap_ptr) 
		return -ENODEV;

	bridge = agp_alloc_bridge();
	if (!bridge)
		return -ENOMEM;

	switch (pdev->device) {
	case PCI_DEVICE_ID_INTEL_82865_HB:
		if (find_i830(PCI_DEVICE_ID_INTEL_82865_IG)) {
			bridge->driver = &intel_830_driver;
		} else {
			bridge->driver = &intel_845_driver;
		}
		name = "865";
		break;
	case PCI_DEVICE_ID_INTEL_82875_HB:
		bridge->driver = &intel_845_driver;
		name = "i875";
		break;

	default:
		printk(KERN_ERR PFX "Unsupported Intel chipset (device id: %04x)\n",
			    pdev->device);
		return -ENODEV;
	};

	bridge->dev = pdev;
	bridge->capndx = cap_ptr;

	if (bridge->driver == &intel_830_driver)
		bridge->dev_private_data = &intel_i830_private;

	printk(KERN_INFO PFX "Detected an Intel %s Chipset.\n", name);

	/*
	* The following fixes the case where the BIOS has "forgotten" to
	* provide an address range for the GART.
	* 20030610 - hamish@zot.org
	*/
	r = &pdev->resource[0];
	if (!r->start && r->end) {
		if(pci_assign_resource(pdev, 0)) {
			printk(KERN_ERR PFX "could not assign resource 0\n");
			return -ENODEV;
		}
	}

	/*
	* If the device has not been properly setup, the following will catch
	* the problem and should stop the system from crashing.
	* 20030610 - hamish@zot.org
	*/
	if (pci_enable_device(pdev)) {
		printk(KERN_ERR PFX "Unable to Enable PCI device\n");
		return -ENODEV;
	}

	/* Fill in the mode register */
	if (cap_ptr) {
		pci_read_config_dword(pdev,
				bridge->capndx+PCI_AGP_STATUS,
				&bridge->mode);
	}

	pci_set_drvdata(pdev, bridge);
	return agp_add_bridge(bridge);
}

static void __devexit agp_intelmch_remove(struct pci_dev *pdev)
{
	struct agp_bridge_data *bridge = pci_get_drvdata(pdev);

	agp_remove_bridge(bridge);
	if (intel_i830_private.i830_dev)
		pci_dev_put(intel_i830_private.i830_dev);
	agp_put_bridge(bridge);
}

static int agp_intelmch_resume(struct pci_dev *pdev)
{
	struct agp_bridge_data *bridge = pci_get_drvdata(pdev);

	pci_restore_state(pdev);

	if (bridge->driver == &intel_845_driver)
		intel_845_configure();

	return 0;
}

static struct pci_device_id agp_intelmch_pci_table[] = {
	{
	.class		= (PCI_CLASS_BRIDGE_HOST << 8),
	.class_mask	= ~0,
	.vendor		= PCI_VENDOR_ID_INTEL,
	.device		= PCI_DEVICE_ID_INTEL_82865_HB,
	.subvendor	= PCI_ANY_ID,
	.subdevice	= PCI_ANY_ID,
	},
	{
	.class		= (PCI_CLASS_BRIDGE_HOST << 8),
	.class_mask	= ~0,
	.vendor		= PCI_VENDOR_ID_INTEL,
	.device		= PCI_DEVICE_ID_INTEL_82875_HB,
	.subvendor	= PCI_ANY_ID,
	.subdevice	= PCI_ANY_ID,
	},
	{ }
};

MODULE_DEVICE_TABLE(pci, agp_intelmch_pci_table);

static struct pci_driver agp_intelmch_pci_driver = {
	.name		= "agpgart-intel-mch",
	.id_table	= agp_intelmch_pci_table,
	.probe		= agp_intelmch_probe,
	.remove		= agp_intelmch_remove,
	.resume		= agp_intelmch_resume,
};

/* intel_agp_init() must not be declared static for explicit
   early initialization to work (ie i810fb) */
int __init agp_intelmch_init(void)
{
	static int agp_initialised=0;

	if (agp_initialised == 1)
		return 0;
	agp_initialised=1;

	return pci_module_init(&agp_intelmch_pci_driver);
}

static void __exit agp_intelmch_cleanup(void)
{
	pci_unregister_driver(&agp_intelmch_pci_driver);
}

module_init(agp_intelmch_init);
module_exit(agp_intelmch_cleanup);

MODULE_AUTHOR("Dave Jones <davej@codemonkey.org.uk>");
MODULE_LICENSE("GPL");

