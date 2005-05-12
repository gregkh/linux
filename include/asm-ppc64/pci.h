#ifndef __PPC64_PCI_H
#define __PPC64_PCI_H
#ifdef __KERNEL__

/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/dma-mapping.h>
#include <asm/machdep.h>
#include <asm/scatterlist.h>
#include <asm/io.h>
#include <asm/prom.h>

#define PCIBIOS_MIN_IO		0x1000
#define PCIBIOS_MIN_MEM		0x10000000

struct pci_dev;

#ifdef CONFIG_PPC_ISERIES
#define pcibios_scan_all_fns(a, b)	0
#else
extern int pcibios_scan_all_fns(struct pci_bus *bus, int devfn);
#endif

static inline void pcibios_set_master(struct pci_dev *dev)
{
	/* No special bus mastering setup handling */
}

static inline void pcibios_penalize_isa_irq(int irq)
{
	/* We don't do dynamic PCI IRQ allocation */
}

#define HAVE_ARCH_PCI_GET_LEGACY_IDE_IRQ
static inline int pci_get_legacy_ide_irq(struct pci_dev *dev, int channel)
{
	if (ppc_md.pci_get_legacy_ide_irq)
		return ppc_md.pci_get_legacy_ide_irq(dev, channel);
	return channel ? 15 : 14;
}

#define HAVE_ARCH_PCI_MWI 1
static inline int pcibios_prep_mwi(struct pci_dev *dev)
{
	/*
	 * We would like to avoid touching the cacheline size or MWI bit
	 * but we cant do that with the current pcibios_prep_mwi 
	 * interface. pSeries firmware sets the cacheline size (which is not
	 * the cpu cacheline size in all cases) and hardware treats MWI 
	 * the same as memory write. So we dont touch the cacheline size
	 * here and allow the generic code to set the MWI bit.
	 */
	return 0;
}

extern unsigned int pcibios_assign_all_busses(void);

/*
 * PCI DMA operations are abstracted for G5 vs. i/pSeries
 */
struct pci_dma_ops {
	void *		(*pci_alloc_consistent)(struct pci_dev *hwdev, size_t size,
					dma_addr_t *dma_handle);
	void		(*pci_free_consistent)(struct pci_dev *hwdev, size_t size,
				       void *vaddr, dma_addr_t dma_handle);

	dma_addr_t	(*pci_map_single)(struct pci_dev *hwdev, void *ptr,
					  size_t size, enum dma_data_direction direction);
	void		(*pci_unmap_single)(struct pci_dev *hwdev, dma_addr_t dma_addr,
					    size_t size, enum dma_data_direction direction);
	int		(*pci_map_sg)(struct pci_dev *hwdev, struct scatterlist *sg,
				      int nents, enum dma_data_direction direction);
	void		(*pci_unmap_sg)(struct pci_dev *hwdev, struct scatterlist *sg,
					int nents, enum dma_data_direction direction);
	int		(*pci_dma_supported)(struct pci_dev *hwdev, u64 mask);
	int		(*pci_dac_dma_supported)(struct pci_dev *hwdev, u64 mask);
};

extern struct pci_dma_ops pci_dma_ops;

static inline void *pci_alloc_consistent(struct pci_dev *hwdev, size_t size,
					 dma_addr_t *dma_handle)
{
	return pci_dma_ops.pci_alloc_consistent(hwdev, size, dma_handle);
}

static inline void pci_free_consistent(struct pci_dev *hwdev, size_t size,
				       void *vaddr, dma_addr_t dma_handle)
{
	pci_dma_ops.pci_free_consistent(hwdev, size, vaddr, dma_handle);
}

static inline dma_addr_t pci_map_single(struct pci_dev *hwdev, void *ptr,
					size_t size, int direction)
{
	return pci_dma_ops.pci_map_single(hwdev, ptr, size,
			(enum dma_data_direction)direction);
}

static inline void pci_unmap_single(struct pci_dev *hwdev, dma_addr_t dma_addr,
				    size_t size, int direction)
{
	pci_dma_ops.pci_unmap_single(hwdev, dma_addr, size,
			(enum dma_data_direction)direction);
}

static inline int pci_map_sg(struct pci_dev *hwdev, struct scatterlist *sg,
			     int nents, int direction)
{
	return pci_dma_ops.pci_map_sg(hwdev, sg, nents,
			(enum dma_data_direction)direction);
}

static inline void pci_unmap_sg(struct pci_dev *hwdev, struct scatterlist *sg,
				int nents, int direction)
{
	pci_dma_ops.pci_unmap_sg(hwdev, sg, nents,
			(enum dma_data_direction)direction);
}

static inline void pci_dma_sync_single_for_cpu(struct pci_dev *hwdev,
					       dma_addr_t dma_handle,
					       size_t size, int direction)
{
	BUG_ON(direction == PCI_DMA_NONE);
	/* nothing to do */
}

static inline void pci_dma_sync_single_for_device(struct pci_dev *hwdev,
						  dma_addr_t dma_handle,
						  size_t size, int direction)
{
	BUG_ON(direction == PCI_DMA_NONE);
	/* nothing to do */
}

static inline void pci_dma_sync_sg_for_cpu(struct pci_dev *hwdev,
					   struct scatterlist *sg,
					   int nelems, int direction)
{
	BUG_ON(direction == PCI_DMA_NONE);
	/* nothing to do */
}

static inline void pci_dma_sync_sg_for_device(struct pci_dev *hwdev,
					      struct scatterlist *sg,
					      int nelems, int direction)
{
	BUG_ON(direction == PCI_DMA_NONE);
	/* nothing to do */
}

/* Return whether the given PCI device DMA address mask can
 * be supported properly.  For example, if your device can
 * only drive the low 24-bits during PCI bus mastering, then
 * you would pass 0x00ffffff as the mask to this function.
 * We default to supporting only 32 bits DMA unless we have
 * an explicit override of this function in pci_dma_ops for
 * the platform
 */
static inline int pci_dma_supported(struct pci_dev *hwdev, u64 mask)
{
	if (pci_dma_ops.pci_dma_supported)
		return pci_dma_ops.pci_dma_supported(hwdev, mask);
	return (mask < 0x100000000ull);
}

/* For DAC DMA, we currently don't support it by default, but
 * we let the platform override this
 */
static inline int pci_dac_dma_supported(struct pci_dev *hwdev,u64 mask)
{
	if (pci_dma_ops.pci_dac_dma_supported)
		return pci_dma_ops.pci_dac_dma_supported(hwdev, mask);
	return 0;
}

static inline int pci_dma_mapping_error(dma_addr_t dma_addr)
{
	return dma_mapping_error(dma_addr);
}

extern int pci_domain_nr(struct pci_bus *bus);

/* Set the name of the bus as it appears in /proc/bus/pci */
extern int pci_name_bus(char *name, struct pci_bus *bus);

struct vm_area_struct;
/* Map a range of PCI memory or I/O space for a device into user space */
int pci_mmap_page_range(struct pci_dev *pdev, struct vm_area_struct *vma,
			enum pci_mmap_state mmap_state, int write_combine);

/* Tell drivers/pci/proc.c that we have pci_mmap_page_range() */
#define HAVE_PCI_MMAP	1

#define pci_map_page(dev, page, off, size, dir) \
		pci_map_single(dev, (page_address(page) + (off)), size, dir)
#define pci_unmap_page(dev,addr,sz,dir) pci_unmap_single(dev,addr,sz,dir)

/* pci_unmap_{single,page} is not a nop, thus... */
#define DECLARE_PCI_UNMAP_ADDR(ADDR_NAME)	\
	dma_addr_t ADDR_NAME;
#define DECLARE_PCI_UNMAP_LEN(LEN_NAME)		\
	__u32 LEN_NAME;
#define pci_unmap_addr(PTR, ADDR_NAME)			\
	((PTR)->ADDR_NAME)
#define pci_unmap_addr_set(PTR, ADDR_NAME, VAL)		\
	(((PTR)->ADDR_NAME) = (VAL))
#define pci_unmap_len(PTR, LEN_NAME)			\
	((PTR)->LEN_NAME)
#define pci_unmap_len_set(PTR, LEN_NAME, VAL)		\
	(((PTR)->LEN_NAME) = (VAL))

/* The PCI address space does equal the physical memory
 * address space.  The networking and block device layers use
 * this boolean for bounce buffer decisions.
 */
#define PCI_DMA_BUS_IS_PHYS	(0)
	
extern void
pcibios_resource_to_bus(struct pci_dev *dev, struct pci_bus_region *region,
			struct resource *res);

extern int
unmap_bus_range(struct pci_bus *bus);

extern int
remap_bus_range(struct pci_bus *bus);

extern void
pcibios_fixup_device_resources(struct pci_dev *dev, struct pci_bus *bus);

extern struct pci_controller *init_phb_dynamic(struct device_node *dn);

extern int pci_read_irq_line(struct pci_dev *dev);

extern void pcibios_add_platform_entries(struct pci_dev *dev);

#endif	/* __KERNEL__ */

#endif /* __PPC64_PCI_H */
