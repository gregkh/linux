/*
 * Device driver for the SYMBIOS/LSILOGIC 53C8XX and 53C1010 family 
 * of PCI-SCSI IO processors.
 *
 * Copyright (C) 1999-2001  Gerard Roudier <groudier@free.fr>
 *
 * This driver is derived from the Linux sym53c8xx driver.
 * Copyright (C) 1998-2000  Gerard Roudier
 *
 * The sym53c8xx driver is derived from the ncr53c8xx driver that had been 
 * a port of the FreeBSD ncr driver to Linux-1.2.13.
 *
 * The original ncr driver has been written for 386bsd and FreeBSD by
 *         Wolfgang Stanglmeier        <wolf@cologne.de>
 *         Stefan Esser                <se@mi.Uni-Koeln.de>
 * Copyright (C) 1994  Wolfgang Stanglmeier
 *
 * Other major contributions:
 *
 * NVRAM detection and reading.
 * Copyright (C) 1997 Richard Waltham <dormouse@farsrobt.demon.co.uk>
 *
 *-----------------------------------------------------------------------------
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef SYM_GLUE_H
#define SYM_GLUE_H

#include <linux/config.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/types.h>

#include <asm/io.h>
#ifdef __sparc__
#  include <asm/irq.h>
#endif

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

#include "sym_conf.h"
#include "sym_defs.h"
#include "sym_misc.h"

/*
 * Configuration addendum for Linux.
 */
#define	SYM_CONF_TIMER_INTERVAL		((HZ+1)/2)

#define SYM_OPT_HANDLE_DIR_UNKNOWN
#define SYM_OPT_HANDLE_DEVICE_QUEUEING
#define SYM_OPT_LIMIT_COMMAND_REORDERING
#define	SYM_OPT_ANNOUNCE_TRANSFER_RATE

/*
 *  Print a message with severity.
 */
#define printf_emerg(args...)	printk(KERN_EMERG args)
#define	printf_alert(args...)	printk(KERN_ALERT args)
#define	printf_crit(args...)	printk(KERN_CRIT args)
#define	printf_err(args...)	printk(KERN_ERR	args)
#define	printf_warning(args...)	printk(KERN_WARNING args)
#define	printf_notice(args...)	printk(KERN_NOTICE args)
#define	printf_info(args...)	printk(KERN_INFO args)
#define	printf_debug(args...)	printk(KERN_DEBUG args)
#define	printf(args...)		printk(args)

/*
 *  Insert a delay in micro-seconds
 */
#define sym_udelay(us)	udelay(us)

/*
 *  A 'read barrier' flushes any data that have been prefetched 
 *  by the processor due to out of order execution. Such a barrier 
 *  must notably be inserted prior to looking at data that have 
 *  been DMAed, assuming that program does memory READs in proper 
 *  order and that the device ensured proper ordering of WRITEs.
 *
 *  A 'write barrier' prevents any previous WRITEs to pass further 
 *  WRITEs. Such barriers must be inserted each time another agent 
 *  relies on ordering of WRITEs.
 *
 *  Note that, due to posting of PCI memory writes, we also must 
 *  insert dummy PCI read transactions when some ordering involving 
 *  both directions over the PCI does matter. PCI transactions are 
 *  fully ordered in each direction.
 */

#define MEMORY_READ_BARRIER()	rmb()
#define MEMORY_WRITE_BARRIER()	wmb()

/*
 *  Let the compiler know about driver data structure names.
 */
typedef struct sym_tcb *tcb_p;
typedef struct sym_lcb *lcb_p;
typedef struct sym_ccb *ccb_p;

/*
 *  IO functions definition for big/little endian CPU support.
 *  For now, PCI chips are only supported in little endian addressing mode, 
 */

#ifdef	__BIG_ENDIAN

#define	inw_l2b		inw
#define	inl_l2b		inl
#define	outw_b2l	outw
#define	outl_b2l	outl
#define	readw_l2b	readw
#define	readl_l2b	readl
#define	writew_b2l	writew
#define	writel_b2l	writel

#else	/* little endian */

#define	inw_raw		inw
#define	inl_raw		inl
#define	outw_raw	outw
#define	outl_raw	outl

#define	readw_raw	readw
#define	readl_raw	readl
#define	writew_raw	writew
#define	writel_raw	writel

#endif /* endian */

#ifdef	SYM_CONF_CHIP_BIG_ENDIAN
#error	"Chips in BIG ENDIAN addressing mode are not (yet) supported"
#endif


/*
 *  If the chip uses big endian addressing mode over the 
 *  PCI, actual io register addresses for byte and word 
 *  accesses must be changed according to lane routing.
 *  Btw, sym_offb() and sym_offw() macros only apply to 
 *  constants and so donnot generate bloated code.
 */

#if	defined(SYM_CONF_CHIP_BIG_ENDIAN)

#define sym_offb(o)	(((o)&~3)+((~((o)&3))&3))
#define sym_offw(o)	(((o)&~3)+((~((o)&3))&2))

#else

#define sym_offb(o)	(o)
#define sym_offw(o)	(o)

#endif

/*
 *  If the CPU and the chip use same endian-ness addressing,
 *  no byte reordering is needed for script patching.
 *  Macro cpu_to_scr() is to be used for script patching.
 *  Macro scr_to_cpu() is to be used for getting a DWORD 
 *  from the script.
 */

#if	defined(__BIG_ENDIAN) && !defined(SYM_CONF_CHIP_BIG_ENDIAN)

#define cpu_to_scr(dw)	cpu_to_le32(dw)
#define scr_to_cpu(dw)	le32_to_cpu(dw)

#elif	defined(__LITTLE_ENDIAN) && defined(SYM_CONF_CHIP_BIG_ENDIAN)

#define cpu_to_scr(dw)	cpu_to_be32(dw)
#define scr_to_cpu(dw)	be32_to_cpu(dw)

#else

#define cpu_to_scr(dw)	(dw)
#define scr_to_cpu(dw)	(dw)

#endif

/*
 *  Access to the controller chip.
 *
 *  If SYM_CONF_IOMAPPED is defined, the driver will use 
 *  normal IOs instead of the MEMORY MAPPED IO method  
 *  recommended by PCI specifications.
 *  If all PCI bridges, host brigdes and architectures 
 *  would have been correctly designed for PCI, this 
 *  option would be useless.
 *
 *  If the CPU and the chip use same endian-ness addressing,
 *  no byte reordering is needed for accessing chip io 
 *  registers. Functions suffixed by '_raw' are assumed 
 *  to access the chip over the PCI without doing byte 
 *  reordering. Functions suffixed by '_l2b' are 
 *  assumed to perform little-endian to big-endian byte 
 *  reordering, those suffixed by '_b2l' blah, blah,
 *  blah, ...
 */

#if defined(SYM_CONF_IOMAPPED)

/*
 *  IO mapped only input / ouput
 */

#define	INB_OFF(o)        inb (np->s.io_port + sym_offb(o))
#define	OUTB_OFF(o, val)  outb ((val), np->s.io_port + sym_offb(o))

#if	defined(__BIG_ENDIAN) && !defined(SYM_CONF_CHIP_BIG_ENDIAN)

#define	INW_OFF(o)        inw_l2b (np->s.io_port + sym_offw(o))
#define	INL_OFF(o)        inl_l2b (np->s.io_port + (o))

#define	OUTW_OFF(o, val)  outw_b2l ((val), np->s.io_port + sym_offw(o))
#define	OUTL_OFF(o, val)  outl_b2l ((val), np->s.io_port + (o))

#elif	defined(__LITTLE_ENDIAN) && defined(SYM_CONF_CHIP_BIG_ENDIAN)

#define	INW_OFF(o)        inw_b2l (np->s.io_port + sym_offw(o))
#define	INL_OFF(o)        inl_b2l (np->s.io_port + (o))

#define	OUTW_OFF(o, val)  outw_l2b ((val), np->s.io_port + sym_offw(o))
#define	OUTL_OFF(o, val)  outl_l2b ((val), np->s.io_port + (o))

#else

#define	INW_OFF(o)        inw_raw (np->s.io_port + sym_offw(o))
#define	INL_OFF(o)        inl_raw (np->s.io_port + (o))

#define	OUTW_OFF(o, val)  outw_raw ((val), np->s.io_port + sym_offw(o))
#define	OUTL_OFF(o, val)  outl_raw ((val), np->s.io_port + (o))

#endif	/* ENDIANs */

#else	/* defined SYM_CONF_IOMAPPED */

/*
 *  MEMORY mapped IO input / output
 */

#define INB_OFF(o)        readb(np->s.mmio_va + sym_offb(o))
#define OUTB_OFF(o, val)  writeb((val), np->s.mmio_va + sym_offb(o))

#if	defined(__BIG_ENDIAN) && !defined(SYM_CONF_CHIP_BIG_ENDIAN)

#define INW_OFF(o)        readw_l2b(np->s.mmio_va + sym_offw(o))
#define INL_OFF(o)        readl_l2b(np->s.mmio_va + (o))

#define OUTW_OFF(o, val)  writew_b2l((val), np->s.mmio_va + sym_offw(o))
#define OUTL_OFF(o, val)  writel_b2l((val), np->s.mmio_va + (o))

#elif	defined(__LITTLE_ENDIAN) && defined(SYM_CONF_CHIP_BIG_ENDIAN)

#define INW_OFF(o)        readw_b2l(np->s.mmio_va + sym_offw(o))
#define INL_OFF(o)        readl_b2l(np->s.mmio_va + (o))

#define OUTW_OFF(o, val)  writew_l2b((val), np->s.mmio_va + sym_offw(o))
#define OUTL_OFF(o, val)  writel_l2b((val), np->s.mmio_va + (o))

#else

#define INW_OFF(o)        readw_raw(np->s.mmio_va + sym_offw(o))
#define INL_OFF(o)        readl_raw(np->s.mmio_va + (o))

#define OUTW_OFF(o, val)  writew_raw((val), np->s.mmio_va + sym_offw(o))
#define OUTL_OFF(o, val)  writel_raw((val), np->s.mmio_va + (o))

#endif

#endif	/* defined SYM_CONF_IOMAPPED */

#define OUTRAM_OFF(o, a, l) memcpy_toio(np->s.ram_va + (o), (a), (l))

/*
 *  Remap some status field values.
 */
#define CAM_REQ_CMP		DID_OK
#define CAM_SEL_TIMEOUT		DID_NO_CONNECT
#define CAM_CMD_TIMEOUT		DID_TIME_OUT
#define CAM_REQ_ABORTED		DID_ABORT
#define CAM_UNCOR_PARITY	DID_PARITY
#define CAM_SCSI_BUS_RESET	DID_RESET	
#define CAM_REQUEUE_REQ		DID_SOFT_ERROR
#define	CAM_UNEXP_BUSFREE	DID_ERROR
#define	CAM_SCSI_BUSY		DID_BUS_BUSY

#define	CAM_DEV_NOT_THERE	DID_NO_CONNECT
#define	CAM_REQ_INVALID		DID_ERROR
#define	CAM_REQ_TOO_BIG		DID_ERROR

#define	CAM_RESRC_UNAVAIL	DID_ERROR

/*
 *  Remap data direction values.
 */
#define CAM_DIR_NONE		DMA_NONE
#define CAM_DIR_IN		DMA_FROM_DEVICE
#define CAM_DIR_OUT		DMA_TO_DEVICE
#define CAM_DIR_UNKNOWN		DMA_BIDIRECTIONAL

/*
 *  These ones are used as return code from 
 *  error recovery handlers under Linux.
 */
#define SCSI_SUCCESS	SUCCESS
#define SCSI_FAILED	FAILED

/*
 *  System specific target data structure.
 *  None for now, under Linux.
 */
/* #define SYM_HAVE_STCB */

/*
 *  System specific lun data structure.
 */
#define SYM_HAVE_SLCB
struct sym_slcb {
	u_short	reqtags;	/* Number of tags requested by user */
	u_short scdev_depth;	/* Queue depth set in select_queue_depth() */
};

/*
 *  System specific command data structure.
 *  Not needed under Linux.
 */
/* struct sym_sccb */

/*
 *  System specific host data structure.
 */
struct sym_shcb {
	/*
	 *  Chip and controller indentification.
	 */
	int		unit;
	char		inst_name[16];
	char		chip_name[8];
	struct pci_dev	*device;

	struct Scsi_Host *host;

	void __iomem *	mmio_va;	/* MMIO kernel virtual address	*/
	void __iomem *	ram_va;		/* RAM  kernel virtual address	*/
	u_long		io_port;	/* IO port address cookie	*/
	u_short		io_ws;		/* IO window size		*/
	int		irq;		/* IRQ number			*/

	struct timer_list timer;	/* Timer handler link header	*/
	u_long		lasttime;
	u_long		settle_time;	/* Resetting the SCSI BUS	*/
	u_char		settle_time_valid;
};

/*
 *  Return the name of the controller.
 */
#define sym_name(np) (np)->s.inst_name

/*
 *  Data structure used as input for the NVRAM reading.
 *  Must resolve the IO macros and sym_name(), when  
 *  used as sub-field 's' of another structure.
 */
struct sym_slot {
	u_long	base;
	u_long	base_2;
	u_long	base_c;
	u_long	base_2_c;
	int	irq;
/* port and address fields to fit INB, OUTB macros */
	u_long	io_port;
	void __iomem *	mmio_va;
	char	inst_name[16];
};

struct sym_nvram;

struct sym_device {
	struct pci_dev *pdev;
	struct sym_slot  s;
	struct sym_pci_chip chip;
	struct sym_nvram *nvram;
	u_short device_id;
	u_char host_id;
};

/*
 *  Driver host data structure.
 */
struct host_data {
	struct sym_hcb *ncb;
};

/*
 *  The driver definitions (sym_hipd.h) must know about a 
 *  couple of things related to the memory allocator.
 */
typedef u_long m_addr_t;	/* Enough bits to represent any address */
#define SYM_MEM_PAGE_ORDER 0	/* 1 PAGE  maximum */
#define SYM_MEM_CLUSTER_SHIFT	(PAGE_SHIFT+SYM_MEM_PAGE_ORDER)
#ifdef	MODULE
#define SYM_MEM_FREE_UNUSED	/* Free unused pages immediately */
#endif
typedef struct pci_dev *m_pool_ident_t;

/*
 *  Include driver soft definitions.
 */
#include "sym_fw.h"
#include "sym_hipd.h"

/*
 *  Memory allocator related stuff.
 */

#define SYM_MEM_GFP_FLAGS	GFP_ATOMIC
#define SYM_MEM_WARN	1	/* Warn on failed operations */

#define sym_get_mem_cluster()	\
	__get_free_pages(SYM_MEM_GFP_FLAGS, SYM_MEM_PAGE_ORDER)
#define sym_free_mem_cluster(p)	\
	free_pages(p, SYM_MEM_PAGE_ORDER)

void *sym_calloc(int size, char *name);
void sym_mfree(void *m, int size, char *name);

/*
 *  We have to provide the driver memory allocator with methods for 
 *  it to maintain virtual to bus physical address translations.
 */

#define sym_m_pool_match(mp_id1, mp_id2)	(mp_id1 == mp_id2)

static __inline m_addr_t sym_m_get_dma_mem_cluster(m_pool_p mp, m_vtob_p vbp)
{
	void *vaddr = NULL;
	dma_addr_t baddr = 0;

	vaddr = pci_alloc_consistent(mp->dev_dmat,SYM_MEM_CLUSTER_SIZE, &baddr);
	if (vaddr) {
		vbp->vaddr = (m_addr_t) vaddr;
		vbp->baddr = (m_addr_t) baddr;
	}
	return (m_addr_t) vaddr;
}

static __inline void sym_m_free_dma_mem_cluster(m_pool_p mp, m_vtob_p vbp)
{
	pci_free_consistent(mp->dev_dmat, SYM_MEM_CLUSTER_SIZE,
	                    (void *)vbp->vaddr, (dma_addr_t)vbp->baddr);
}

#define sym_m_create_dma_mem_tag(mp)	(0)
#define sym_m_delete_dma_mem_tag(mp)	do { ; } while (0)

void *__sym_calloc_dma(m_pool_ident_t dev_dmat, int size, char *name);
void __sym_mfree_dma(m_pool_ident_t dev_dmat, void *m, int size, char *name);
m_addr_t __vtobus(m_pool_ident_t dev_dmat, void *m);

/*
 *  Set the status field of a CAM CCB.
 */
static __inline void 
sym_set_cam_status(struct scsi_cmnd *ccb, int status)
{
	ccb->result &= ~(0xff  << 16);
	ccb->result |= (status << 16);
}

/*
 *  Get the status field of a CAM CCB.
 */
static __inline int 
sym_get_cam_status(struct scsi_cmnd *ccb)
{
	return ((ccb->result >> 16) & 0xff);
}

/*
 *  The dma mapping is mostly handled by the 
 *  SCSI layer and the driver glue under Linux.
 */
#define sym_data_dmamap_create(np, cp)		(0)
#define sym_data_dmamap_destroy(np, cp)		do { ; } while (0)
#define sym_data_dmamap_unload(np, cp)		do { ; } while (0)
#define sym_data_dmamap_presync(np, cp)		do { ; } while (0)
#define sym_data_dmamap_postsync(np, cp)	do { ; } while (0)

/*
 *  Async handler for negotiations.
 */
void sym_xpt_async_nego_wide(struct sym_hcb *np, int target);
#define sym_xpt_async_nego_sync(np, target)	\
	sym_announce_transfer_rate(np, target)
#define sym_xpt_async_nego_ppr(np, target)	\
	sym_announce_transfer_rate(np, target)

/*
 *  Build CAM result for a successful IO and for a failed IO.
 */
static __inline void sym_set_cam_result_ok(struct sym_hcb *np, ccb_p cp, int resid)
{
	struct scsi_cmnd *cmd = cp->cam_ccb;

	cmd->resid = resid;
	cmd->result = (((DID_OK) << 16) + ((cp->ssss_status) & 0x7f));
}
void sym_set_cam_result_error(struct sym_hcb *np, ccb_p cp, int resid);

/*
 *  Other O/S specific methods.
 */
#define sym_cam_target_id(ccb)	(ccb)->target
#define sym_cam_target_lun(ccb)	(ccb)->lun
#define	sym_freeze_cam_ccb(ccb)	do { ; } while (0)
void sym_xpt_done(struct sym_hcb *np, struct scsi_cmnd *ccb);
void sym_print_addr (ccb_p cp);
void sym_xpt_async_bus_reset(struct sym_hcb *np);
void sym_xpt_async_sent_bdr(struct sym_hcb *np, int target);
int  sym_setup_data_and_start (struct sym_hcb *np, struct scsi_cmnd *csio, ccb_p cp);
void sym_log_bus_error(struct sym_hcb *np);
void sym_sniff_inquiry(struct sym_hcb *np, struct scsi_cmnd *cmd, int resid);

#endif /* SYM_GLUE_H */
