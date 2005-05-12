/*
 * Alchemy Semi Au1000 ethernet driver
 *
 * Copyright 2001 MontaVista Software Inc.
 * Author: MontaVista Software, Inc.
 *         	ppopov@mvista.com or source@mvista.com
 *
 *  This program is free software; you can distribute it and/or modify it
 *  under the terms of the GNU General Public License (Version 2) as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place - Suite 330, Boston MA 02111-1307, USA.
 */
#include <linux/config.h>

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/crc32.h>
#include <linux/bitops.h>

#include <asm/mipsregs.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/au1000.h>

#include "au1000_eth.h"

#ifdef AU1000_ETH_DEBUG
static int au1000_debug = 10;
#else
static int au1000_debug = 3;
#endif

// prototypes
static void *dma_alloc(size_t, dma_addr_t *);
static void dma_free(void *, size_t);
static void hard_stop(struct net_device *);
static void enable_rx_tx(struct net_device *dev);
static int __init au1000_probe1(long, int, int);
static int au1000_init(struct net_device *);
static int au1000_open(struct net_device *);
static int au1000_close(struct net_device *);
static int au1000_tx(struct sk_buff *, struct net_device *);
static int au1000_rx(struct net_device *);
static irqreturn_t au1000_interrupt(int, void *, struct pt_regs *);
static void au1000_tx_timeout(struct net_device *);
static int au1000_set_config(struct net_device *dev, struct ifmap *map);
static void set_rx_mode(struct net_device *);
static struct net_device_stats *au1000_get_stats(struct net_device *);
static inline void update_tx_stats(struct net_device *, u32, u32);
static inline void update_rx_stats(struct net_device *, u32);
static void au1000_timer(unsigned long);
static int au1000_ioctl(struct net_device *, struct ifreq *, int);
static int mdio_read(struct net_device *, int, int);
static void mdio_write(struct net_device *, int, int, u16);
static void dump_mii(struct net_device *dev, int phy_id);

// externs
extern  void ack_rise_edge_irq(unsigned int);
extern int get_ethernet_addr(char *ethernet_addr);
extern inline void str2eaddr(unsigned char *ea, unsigned char *str);
extern inline unsigned char str2hexnum(unsigned char c);
extern char * __init prom_getcmdline(void);

/*
 * Theory of operation
 *
 * The Au1000 MACs use a simple rx and tx descriptor ring scheme. 
 * There are four receive and four transmit descriptors.  These 
 * descriptors are not in memory; rather, they are just a set of 
 * hardware registers.
 *
 * Since the Au1000 has a coherent data cache, the receive and
 * transmit buffers are allocated from the KSEG0 segment. The 
 * hardware registers, however, are still mapped at KSEG1 to
 * make sure there's no out-of-order writes, and that all writes
 * complete immediately.
 */


/*
 * Base address and interrupt of the Au1xxx ethernet macs
 */
static struct {
	unsigned int port;
	int irq;
} au1000_iflist[NUM_INTERFACES] = {
		{AU1000_ETH0_BASE, AU1000_ETH0_IRQ}, 
		{AU1000_ETH1_BASE, AU1000_ETH1_IRQ}
	},
  au1500_iflist[NUM_INTERFACES] = {
		{AU1500_ETH0_BASE, AU1000_ETH0_IRQ}, 
		{AU1500_ETH1_BASE, AU1000_ETH1_IRQ}
	},
  au1100_iflist[NUM_INTERFACES] = {
		{AU1000_ETH0_BASE, AU1000_ETH0_IRQ}, 
		{0, 0}
	};

static char version[] __devinitdata =
    "au1000eth.c:1.0 ppopov@mvista.com\n";

/* These addresses are only used if yamon doesn't tell us what
 * the mac address is, and the mac address is not passed on the
 * command line.
 */
static unsigned char au1000_mac_addr[6] __devinitdata = { 
	0x00, 0x50, 0xc2, 0x0c, 0x30, 0x00
};

#define nibswap(x) ((((x) >> 4) & 0x0f) | (((x) << 4) & 0xf0))
#define RUN_AT(x) (jiffies + (x))

// For reading/writing 32-bit words from/to DMA memory
#define cpu_to_dma32 cpu_to_be32
#define dma32_to_cpu be32_to_cpu


/* FIXME 
 * All of the PHY code really should be detached from the MAC 
 * code.
 */

int bcm_5201_init(struct net_device *dev, int phy_addr)
{
	s16 data;
	
	/* Stop auto-negotiation */
	//printk("bcm_5201_init\n");
	data = mdio_read(dev, phy_addr, MII_CONTROL);
	mdio_write(dev, phy_addr, MII_CONTROL, data & ~MII_CNTL_AUTO);

	/* Set advertisement to 10/100 and Half/Full duplex
	 * (full capabilities) */
	data = mdio_read(dev, phy_addr, MII_ANADV);
	data |= MII_NWAY_TX | MII_NWAY_TX_FDX | MII_NWAY_T_FDX | MII_NWAY_T;
	mdio_write(dev, phy_addr, MII_ANADV, data);
	
	/* Restart auto-negotiation */
	data = mdio_read(dev, phy_addr, MII_CONTROL);
	data |= MII_CNTL_RST_AUTO | MII_CNTL_AUTO;
	mdio_write(dev, phy_addr, MII_CONTROL, data);

	/* Enable TX LED instead of FDX */
	data = mdio_read(dev, phy_addr, MII_INT);
	data &= ~MII_FDX_LED;
	mdio_write(dev, phy_addr, MII_INT, data);

	/* Enable TX LED instead of FDX */
	data = mdio_read(dev, phy_addr, MII_INT);
	data &= ~MII_FDX_LED;
	mdio_write(dev, phy_addr, MII_INT, data);

	if (au1000_debug > 4) dump_mii(dev, phy_addr);
	return 0;
}

int bcm_5201_reset(struct net_device *dev, int phy_addr)
{
	s16 mii_control, timeout;
	
	//printk("bcm_5201_reset\n");
	mii_control = mdio_read(dev, phy_addr, MII_CONTROL);
	mdio_write(dev, phy_addr, MII_CONTROL, mii_control | MII_CNTL_RESET);
	mdelay(1);
	for (timeout = 100; timeout > 0; --timeout) {
		mii_control = mdio_read(dev, phy_addr, MII_CONTROL);
		if ((mii_control & MII_CNTL_RESET) == 0)
			break;
		mdelay(1);
	}
	if (mii_control & MII_CNTL_RESET) {
		printk(KERN_ERR "%s PHY reset timeout !\n", dev->name);
		return -1;
	}
	return 0;
}

int 
bcm_5201_status(struct net_device *dev, int phy_addr, u16 *link, u16 *speed)
{
	u16 mii_data;
	struct au1000_private *aup;

	if (!dev) {
		printk(KERN_ERR "bcm_5201_status error: NULL dev\n");
		return -1;
	}
	aup = (struct au1000_private *) dev->priv;

	mii_data = mdio_read(dev, aup->phy_addr, MII_STATUS);
	if (mii_data & MII_STAT_LINK) {
		*link = 1;
		mii_data = mdio_read(dev, aup->phy_addr, MII_AUX_CNTRL);
		if (mii_data & MII_AUX_100) {
			if (mii_data & MII_AUX_FDX) {
				*speed = IF_PORT_100BASEFX;
				dev->if_port = IF_PORT_100BASEFX;
			}
			else {
				*speed = IF_PORT_100BASETX;
				dev->if_port = IF_PORT_100BASETX;
			}
		}
		else  {
			*speed = IF_PORT_10BASET;
			dev->if_port = IF_PORT_10BASET;
		}

	}
	else {
		*link = 0;
		*speed = 0;
		dev->if_port = IF_PORT_UNKNOWN;
	}
	return 0;
}

int lsi_80227_init(struct net_device *dev, int phy_addr)
{
	if (au1000_debug > 4)
		printk("lsi_80227_init\n");

	/* restart auto-negotiation */
	mdio_write(dev, phy_addr, 0, 0x3200);

	mdelay(1);

	/* set up LEDs to correct display */
	mdio_write(dev, phy_addr, 17, 0xffc0);

	if (au1000_debug > 4)
		dump_mii(dev, phy_addr);
	return 0;
}

int lsi_80227_reset(struct net_device *dev, int phy_addr)
{
	s16 mii_control, timeout;
	
	if (au1000_debug > 4) {
		printk("lsi_80227_reset\n");
		dump_mii(dev, phy_addr);
	}

	mii_control = mdio_read(dev, phy_addr, MII_CONTROL);
	mdio_write(dev, phy_addr, MII_CONTROL, mii_control | MII_CNTL_RESET);
	mdelay(1);
	for (timeout = 100; timeout > 0; --timeout) {
		mii_control = mdio_read(dev, phy_addr, MII_CONTROL);
		if ((mii_control & MII_CNTL_RESET) == 0)
			break;
		mdelay(1);
	}
	if (mii_control & MII_CNTL_RESET) {
		printk(KERN_ERR "%s PHY reset timeout !\n", dev->name);
		return -1;
	}
	return 0;
}

int
lsi_80227_status(struct net_device *dev, int phy_addr, u16 *link, u16 *speed)
{
	u16 mii_data;
	struct au1000_private *aup;

	if (!dev) {
		printk(KERN_ERR "lsi_80227_status error: NULL dev\n");
		return -1;
	}
	aup = (struct au1000_private *) dev->priv;

	mii_data = mdio_read(dev, aup->phy_addr, MII_STATUS);
	if (mii_data & MII_STAT_LINK) {
		*link = 1;
		mii_data = mdio_read(dev, aup->phy_addr, MII_LSI_STAT);
		if (mii_data & MII_LSI_STAT_SPD) {
			if (mii_data & MII_LSI_STAT_FDX) {
				*speed = IF_PORT_100BASEFX;
				dev->if_port = IF_PORT_100BASEFX;
			}
			else {
				*speed = IF_PORT_100BASETX;
				dev->if_port = IF_PORT_100BASETX;
			}
		}
		else  {
			*speed = IF_PORT_10BASET;
			dev->if_port = IF_PORT_10BASET;
		}

	}
	else {
		*link = 0;
		*speed = 0;
		dev->if_port = IF_PORT_UNKNOWN;
	}
	return 0;
}

int am79c901_init(struct net_device *dev, int phy_addr)
{
	printk("am79c901_init\n");
	return 0;
}

int am79c901_reset(struct net_device *dev, int phy_addr)
{
	printk("am79c901_reset\n");
	return 0;
}

int 
am79c901_status(struct net_device *dev, int phy_addr, u16 *link, u16 *speed)
{
	return 0;
}

struct phy_ops bcm_5201_ops = {
	bcm_5201_init,
	bcm_5201_reset,
	bcm_5201_status,
};

struct phy_ops am79c901_ops = {
	am79c901_init,
	am79c901_reset,
	am79c901_status,
};

struct phy_ops lsi_80227_ops = { 
	lsi_80227_init,
	lsi_80227_reset,
	lsi_80227_status,
};

static struct mii_chip_info {
	const char * name;
	u16 phy_id0;
	u16 phy_id1;
	struct phy_ops *phy_ops;	
} mii_chip_table[] = {
	{"Broadcom BCM5201 10/100 BaseT PHY",  0x0040, 0x6212, &bcm_5201_ops },
	{"AMD 79C901 HomePNA PHY",  0x0000, 0x35c8, &am79c901_ops },
	{"LSI 80227 10/100 BaseT PHY", 0x0016, 0xf840, &lsi_80227_ops },
	{"Broadcom BCM5221 10/100 BaseT PHY",  0x0040, 0x61e4, &bcm_5201_ops },
	{0,},
};

static int mdio_read(struct net_device *dev, int phy_id, int reg)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;
	u32 timedout = 20;
	u32 mii_control;

	while (aup->mac->mii_control & MAC_MII_BUSY) {
		mdelay(1);
		if (--timedout == 0) {
			printk(KERN_ERR "%s: read_MII busy timeout!!\n", 
					dev->name);
			return -1;
		}
	}

	mii_control = MAC_SET_MII_SELECT_REG(reg) | 
		MAC_SET_MII_SELECT_PHY(phy_id) | MAC_MII_READ;

	aup->mac->mii_control = mii_control;

	timedout = 20;
	while (aup->mac->mii_control & MAC_MII_BUSY) {
		mdelay(1);
		if (--timedout == 0) {
			printk(KERN_ERR "%s: mdio_read busy timeout!!\n", 
					dev->name);
			return -1;
		}
	}
	return (int)aup->mac->mii_data;
}

static void mdio_write(struct net_device *dev, int phy_id, int reg, u16 value)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;
	u32 timedout = 20;
	u32 mii_control;

	while (aup->mac->mii_control & MAC_MII_BUSY) {
		mdelay(1);
		if (--timedout == 0) {
			printk(KERN_ERR "%s: mdio_write busy timeout!!\n", 
					dev->name);
			return;
		}
	}

	mii_control = MAC_SET_MII_SELECT_REG(reg) | 
		MAC_SET_MII_SELECT_PHY(phy_id) | MAC_MII_WRITE;

	aup->mac->mii_data = value;
	aup->mac->mii_control = mii_control;
}


static void dump_mii(struct net_device *dev, int phy_id)
{
	int i, val;

	for (i = 0; i < 7; i++) {
		if ((val = mdio_read(dev, phy_id, i)) >= 0)
			printk("%s: MII Reg %d=%x\n", dev->name, i, val);
	}
	for (i = 16; i < 25; i++) {
		if ((val = mdio_read(dev, phy_id, i)) >= 0)
			printk("%s: MII Reg %d=%x\n", dev->name, i, val);
	}
}

static int __init mii_probe (struct net_device * dev)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;
	int phy_addr;

	aup->mii = NULL;

	/* search for total of 32 possible mii phy addresses */
	for (phy_addr = 0; phy_addr < 32; phy_addr++) {
		u16 mii_status;
		u16 phy_id0, phy_id1;
		int i;

		mii_status = mdio_read(dev, phy_addr, MII_STATUS);
		if (mii_status == 0xffff || mii_status == 0x0000)
			/* the mii is not accessible, try next one */
			continue;

		phy_id0 = mdio_read(dev, phy_addr, MII_PHY_ID0);
		phy_id1 = mdio_read(dev, phy_addr, MII_PHY_ID1);

		/* search our mii table for the current mii */ 
		for (i = 0; mii_chip_table[i].phy_id1; i++) {
			if (phy_id0 == mii_chip_table[i].phy_id0 &&
			    phy_id1 == mii_chip_table[i].phy_id1) {
				struct mii_phy * mii_phy;

				printk(KERN_INFO "%s: %s at phy address %d\n",
				       dev->name, mii_chip_table[i].name, 
				       phy_addr);
				mii_phy = kmalloc(sizeof(struct mii_phy), 
						GFP_KERNEL);
				if (mii_phy) {
					mii_phy->chip_info = mii_chip_table+i;
					mii_phy->phy_addr = phy_addr;
					mii_phy->next = aup->mii;
					aup->phy_ops = 
						mii_chip_table[i].phy_ops;
					aup->mii = mii_phy;
					aup->phy_ops->phy_init(dev,phy_addr);
				} else {
					printk(KERN_ERR "%s: out of memory\n",
							dev->name);
					return -1;
				}
				/* the current mii is on our mii_info_table,
				   try next address */
				break;
			}
		}
	}

	if (aup->mii == NULL) {
		printk(KERN_ERR "%s: No MII transceivers found!\n", dev->name);
		return -1;
	}

	/* use last PHY */
	aup->phy_addr = aup->mii->phy_addr;
	printk(KERN_INFO "%s: Using %s as default\n", 
			dev->name, aup->mii->chip_info->name);

	return 0;
}


/*
 * Buffer allocation/deallocation routines. The buffer descriptor returned
 * has the virtual and dma address of a buffer suitable for 
 * both, receive and transmit operations.
 */
static db_dest_t *GetFreeDB(struct au1000_private *aup)
{
	db_dest_t *pDB;
	pDB = aup->pDBfree;

	if (pDB) {
		aup->pDBfree = pDB->pnext;
	}
	//printk("GetFreeDB: %x\n", pDB);
	return pDB;
}

void ReleaseDB(struct au1000_private *aup, db_dest_t *pDB)
{
	db_dest_t *pDBfree = aup->pDBfree;
	if (pDBfree)
		pDBfree->pnext = pDB;
	aup->pDBfree = pDB;
}


/*
  DMA memory allocation, derived from pci_alloc_consistent.
  However, the Au1000 data cache is coherent (when programmed
  so), therefore we return KSEG0 address, not KSEG1.
*/
static void *dma_alloc(size_t size, dma_addr_t * dma_handle)
{
	void *ret;
	int gfp = GFP_ATOMIC | GFP_DMA;

	ret = (void *) __get_free_pages(gfp, get_order(size));

	if (ret != NULL) {
		memset(ret, 0, size);
		*dma_handle = virt_to_bus(ret);
		ret = (void *)KSEG0ADDR(ret);
	}
	return ret;
}


static void dma_free(void *vaddr, size_t size)
{
	vaddr = (void *)KSEG0ADDR(vaddr);
	free_pages((unsigned long) vaddr, get_order(size));
}


static void enable_rx_tx(struct net_device *dev)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;

	if (au1000_debug > 4)
		printk(KERN_INFO "%s: enable_rx_tx\n", dev->name);

	aup->mac->control |= (MAC_RX_ENABLE | MAC_TX_ENABLE);
	au_sync_delay(10);
}

static void hard_stop(struct net_device *dev)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;

	if (au1000_debug > 4)
		printk(KERN_INFO "%s: hard stop\n", dev->name);

	aup->mac->control &= ~(MAC_RX_ENABLE | MAC_TX_ENABLE);
	au_sync_delay(10);
}


static void reset_mac(struct net_device *dev)
{
	u32 flags;
	struct au1000_private *aup = (struct au1000_private *) dev->priv;

	if (au1000_debug > 4) 
		printk(KERN_INFO "%s: reset mac, aup %x\n", 
				dev->name, (unsigned)aup);

	spin_lock_irqsave(&aup->lock, flags);
	del_timer(&aup->timer);
	hard_stop(dev);
	*aup->enable = MAC_EN_CLOCK_ENABLE;
	au_sync_delay(2);
       	*aup->enable = 0;
	au_sync_delay(2);
	aup->tx_full = 0;
	spin_unlock_irqrestore(&aup->lock, flags);
}


/* 
 * Setup the receive and transmit "rings".  These pointers are the addresses
 * of the rx and tx MAC DMA registers so they are fixed by the hardware --
 * these are not descriptors sitting in memory.
 */
static void 
setup_hw_rings(struct au1000_private *aup, u32 rx_base, u32 tx_base)
{
	int i;

	for (i=0; i<NUM_RX_DMA; i++) {
		aup->rx_dma_ring[i] = 
			(volatile rx_dma_t *) (rx_base + sizeof(rx_dma_t)*i);
	}
	for (i=0; i<NUM_TX_DMA; i++) {
		aup->tx_dma_ring[i] = 
			(volatile tx_dma_t *) (tx_base + sizeof(tx_dma_t)*i);
	}
}

static int __init au1000_init_module(void)
{
	int i;
	int prid;
	int base_addr, irq;

	prid = read_c0_prid();
	for (i=0; i<NUM_INTERFACES; i++) {
		if ( (prid & 0xffff0000) == 0x00030000 ) {
			base_addr = au1000_iflist[i].port;
			irq = au1000_iflist[i].irq;
		} else if ( (prid & 0xffff0000) == 0x01030000 ) {
			base_addr = au1500_iflist[i].port;
			irq = au1500_iflist[i].irq;
		} else if ( (prid & 0xffff0000) == 0x02030000 ) {
			base_addr = au1100_iflist[i].port;
			irq = au1100_iflist[i].irq;
		} else {
			printk(KERN_ERR "au1000 eth: unknown Processor ID\n");
			return -ENODEV;
		}
		// check for valid entries, au1100 only has one entry
		if (base_addr && irq) {
			if (au1000_probe1(base_addr, irq, i) != 0)
				return -ENODEV;
		}
	}
	return 0;
}

static int __init
au1000_probe1(long ioaddr, int irq, int port_num)
{
	struct net_device *dev;
	static unsigned version_printed = 0;
	struct au1000_private *aup = NULL;
	int i, retval = 0;
	db_dest_t *pDB, *pDBfree;
	char *pmac, *argptr;
	char ethaddr[6];

	if (!request_region(PHYSADDR(ioaddr), MAC_IOSIZE, "Au1000 ENET"))
		 return -ENODEV;

	if (version_printed++ == 0)
		printk(version);

	retval = -ENOMEM;

	dev = alloc_etherdev(sizeof(struct au1000_private));
	if (!dev) {
		printk (KERN_ERR "au1000 eth: alloc_etherdev failed\n");  
		goto out;
	}

	SET_MODULE_OWNER(dev);

	printk("%s: Au1xxx ethernet found at 0x%lx, irq %d\n", 
	       dev->name, ioaddr, irq);

	aup = dev->priv;

	/* Allocate the data buffers */
	aup->vaddr = (u32)dma_alloc(MAX_BUF_SIZE * 
			(NUM_TX_BUFFS+NUM_RX_BUFFS), &aup->dma_addr);
	if (!aup->vaddr)
		goto out1;

	/* aup->mac is the base address of the MAC's registers */
	aup->mac = (volatile mac_reg_t *)((unsigned long)ioaddr);
	/* Setup some variables for quick register address access */
	switch (ioaddr) {
	case AU1000_ETH0_BASE:
	case AU1500_ETH0_BASE:
		/* check env variables first */
		if (!get_ethernet_addr(ethaddr)) { 
			memcpy(au1000_mac_addr, ethaddr, sizeof(dev->dev_addr));
		} else {
			/* Check command line */
			argptr = prom_getcmdline();
			if ((pmac = strstr(argptr, "ethaddr=")) == NULL) {
				printk(KERN_INFO "%s: No mac address found\n", 
						dev->name);
				/* use the hard coded mac addresses */
			} else {
				str2eaddr(ethaddr, pmac + strlen("ethaddr="));
				memcpy(au1000_mac_addr, ethaddr, 
						sizeof(dev->dev_addr));
			}
		}
		if (ioaddr == AU1000_ETH0_BASE)
			aup->enable = (volatile u32 *) 
				((unsigned long)AU1000_MAC0_ENABLE);
		else
			aup->enable = (volatile u32 *) 
				((unsigned long)AU1500_MAC0_ENABLE);
		memcpy(dev->dev_addr, au1000_mac_addr, sizeof(dev->dev_addr));
		setup_hw_rings(aup, MAC0_RX_DMA_ADDR, MAC0_TX_DMA_ADDR);
			break;
	case AU1000_ETH1_BASE:
	case AU1500_ETH1_BASE:
		if (ioaddr == AU1000_ETH1_BASE)
			aup->enable = (volatile u32 *) 
				((unsigned long)AU1000_MAC1_ENABLE);
		else
			aup->enable = (volatile u32 *) 
				((unsigned long)AU1500_MAC1_ENABLE);
		memcpy(dev->dev_addr, au1000_mac_addr, sizeof(dev->dev_addr));
		dev->dev_addr[4] += 0x10;
		setup_hw_rings(aup, MAC1_RX_DMA_ADDR, MAC1_TX_DMA_ADDR);
			break;
	default:
		printk(KERN_ERR "%s: bad ioaddr\n", dev->name);
		break;

	}

	aup->phy_addr = PHY_ADDRESS;

	/* bring the device out of reset, otherwise probing the mii
	 * will hang */
	*aup->enable = MAC_EN_CLOCK_ENABLE;
	au_sync_delay(2);
	*aup->enable = MAC_EN_RESET0 | MAC_EN_RESET1 | 
		MAC_EN_RESET2 | MAC_EN_CLOCK_ENABLE;
	au_sync_delay(2);

	retval = mii_probe(dev);
	if (retval)
		 goto out2;

	retval = -EINVAL;
	pDBfree = NULL;
	/* setup the data buffer descriptors and attach a buffer to each one */
	pDB = aup->db;
	for (i=0; i<(NUM_TX_BUFFS+NUM_RX_BUFFS); i++) {
		pDB->pnext = pDBfree;
		pDBfree = pDB;
		pDB->vaddr = (u32 *)((unsigned)aup->vaddr + MAX_BUF_SIZE*i);
		pDB->dma_addr = (dma_addr_t)virt_to_bus(pDB->vaddr);
		pDB++;
	}
	aup->pDBfree = pDBfree;

	for (i=0; i<NUM_RX_DMA; i++) {
		pDB = GetFreeDB(aup);
		if (!pDB) goto out2;
		aup->rx_dma_ring[i]->buff_stat = (unsigned)pDB->dma_addr;
		aup->rx_db_inuse[i] = pDB;
	}
	for (i=0; i<NUM_TX_DMA; i++) {
		pDB = GetFreeDB(aup);
		if (!pDB) goto out2;
		aup->tx_dma_ring[i]->buff_stat = (unsigned)pDB->dma_addr;
		aup->tx_dma_ring[i]->len = 0;
		aup->tx_db_inuse[i] = pDB;
	}

	spin_lock_init(&aup->lock);
	dev->base_addr = ioaddr;
	dev->irq = irq;
	dev->open = au1000_open;
	dev->hard_start_xmit = au1000_tx;
	dev->stop = au1000_close;
	dev->get_stats = au1000_get_stats;
	dev->set_multicast_list = &set_rx_mode;
	dev->do_ioctl = &au1000_ioctl;
	dev->set_config = &au1000_set_config;
	dev->tx_timeout = au1000_tx_timeout;
	dev->watchdog_timeo = ETH_TX_TIMEOUT;

	/* 
	 * The boot code uses the ethernet controller, so reset it to start 
	 * fresh.  au1000_init() expects that the device is in reset state.
	 */
	reset_mac(dev);

	retval = register_netdev(dev);
	if (retval)
		goto out2;
	return 0;

out2:
	dma_free(aup->vaddr, MAX_BUF_SIZE * (NUM_TX_BUFFS+NUM_RX_BUFFS));
out1:
	free_netdev(dev);
out:
	release_region(PHYSADDR(ioaddr), MAC_IOSIZE);
	printk(KERN_ERR "%s: au1000_probe1 failed.  Returns %d\n",
	       dev->name, retval);
	return retval;
}


/* 
 * Initialize the interface.
 *
 * When the device powers up, the clocks are disabled and the
 * mac is in reset state.  When the interface is closed, we
 * do the same -- reset the device and disable the clocks to
 * conserve power. Thus, whenever au1000_init() is called,
 * the device should already be in reset state.
 */
static int au1000_init(struct net_device *dev)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;
	u32 flags;
	int i;
	u32 control;
	u16 link, speed;

	if (au1000_debug > 4) printk("%s: au1000_init\n", dev->name);

	spin_lock_irqsave(&aup->lock, flags);

	/* bring the device out of reset */
	*aup->enable = MAC_EN_CLOCK_ENABLE;
        au_sync_delay(2);
	*aup->enable = MAC_EN_RESET0 | MAC_EN_RESET1 | 
		MAC_EN_RESET2 | MAC_EN_CLOCK_ENABLE;
	au_sync_delay(20);

	aup->mac->control = 0;
	aup->tx_head = (aup->tx_dma_ring[0]->buff_stat & 0xC) >> 2;
	aup->tx_tail = aup->tx_head;
	aup->rx_head = (aup->rx_dma_ring[0]->buff_stat & 0xC) >> 2;

	aup->mac->mac_addr_high = dev->dev_addr[5]<<8 | dev->dev_addr[4];
	aup->mac->mac_addr_low = dev->dev_addr[3]<<24 | dev->dev_addr[2]<<16 |
		dev->dev_addr[1]<<8 | dev->dev_addr[0];

	for (i=0; i<NUM_RX_DMA; i++) {
		aup->rx_dma_ring[i]->buff_stat |= RX_DMA_ENABLE;
	}
	au_sync();

	aup->phy_ops->phy_status(dev, aup->phy_addr, &link, &speed);
	control = MAC_DISABLE_RX_OWN | MAC_RX_ENABLE | MAC_TX_ENABLE;
#ifndef CONFIG_CPU_LITTLE_ENDIAN
	control |= MAC_BIG_ENDIAN;
#endif
	if (link && (dev->if_port == IF_PORT_100BASEFX)) {
		control |= MAC_FULL_DUPLEX;
	}
	aup->mac->control = control;
	au_sync();

	spin_unlock_irqrestore(&aup->lock, flags);
	return 0;
}

static void au1000_timer(unsigned long data)
{
	struct net_device *dev = (struct net_device *)data;
	struct au1000_private *aup = (struct au1000_private *) dev->priv;
	unsigned char if_port;
	u16 link, speed;

	if (!dev) {
		/* fatal error, don't restart the timer */
		printk(KERN_ERR "au1000_timer error: NULL dev\n");
		return;
	}

	if_port = dev->if_port;
	if (aup->phy_ops->phy_status(dev, aup->phy_addr, &link, &speed) == 0) {
		if (link) {
			if (!(dev->flags & IFF_RUNNING)) {
				netif_carrier_on(dev);
				dev->flags |= IFF_RUNNING;
				printk(KERN_INFO "%s: link up\n", dev->name);
			}
		}
		else {
			if (dev->flags & IFF_RUNNING) {
				netif_carrier_off(dev);
				dev->flags &= ~IFF_RUNNING;
				dev->if_port = 0;
				printk(KERN_INFO "%s: link down\n", dev->name);
			}
		}
	}

	if (link && (dev->if_port != if_port) && 
			(dev->if_port != IF_PORT_UNKNOWN)) {
		hard_stop(dev);
		if (dev->if_port == IF_PORT_100BASEFX) {
			printk(KERN_INFO "%s: going to full duplex\n", 
					dev->name);
			aup->mac->control |= MAC_FULL_DUPLEX;
			au_sync_delay(1);
		}
		else {
			aup->mac->control &= ~MAC_FULL_DUPLEX;
			au_sync_delay(1);
		}
		enable_rx_tx(dev);
	}

	aup->timer.expires = RUN_AT((1*HZ)); 
	aup->timer.data = (unsigned long)dev;
	aup->timer.function = &au1000_timer; /* timer handler */
	add_timer(&aup->timer);

}

static int au1000_open(struct net_device *dev)
{
	int retval;
	struct au1000_private *aup = (struct au1000_private *) dev->priv;

	if (au1000_debug > 4)
		printk("%s: open: dev=%p\n", dev->name, dev);

	if ((retval = au1000_init(dev))) {
		printk(KERN_ERR "%s: error in au1000_init\n", dev->name);
		free_irq(dev->irq, dev);
		return retval;
	}
	netif_start_queue(dev);

	if ((retval = request_irq(dev->irq, &au1000_interrupt, 0, 
					dev->name, dev))) {
		printk(KERN_ERR "%s: unable to get IRQ %d\n", 
				dev->name, dev->irq);
		return retval;
	}

	aup->timer.expires = RUN_AT((3*HZ)); 
	aup->timer.data = (unsigned long)dev;
	aup->timer.function = &au1000_timer; /* timer handler */
	add_timer(&aup->timer);

	if (au1000_debug > 4)
		printk("%s: open: Initialization done.\n", dev->name);

	return 0;
}

static int au1000_close(struct net_device *dev)
{
	u32 flags;
	struct au1000_private *aup = (struct au1000_private *) dev->priv;

	if (au1000_debug > 4)
		printk("%s: close: dev=%p\n", dev->name, dev);

	spin_lock_irqsave(&aup->lock, flags);
	
	/* stop the device */
	if (netif_device_present(dev))
		netif_stop_queue(dev);

	/* disable the interrupt */
	free_irq(dev->irq, dev);
	spin_unlock_irqrestore(&aup->lock, flags);

	reset_mac(dev);
	return 0;
}

static void __exit au1000_cleanup_module(void)
{
}


static inline void 
update_tx_stats(struct net_device *dev, u32 status, u32 pkt_len)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;
	struct net_device_stats *ps = &aup->stats;

	ps->tx_packets++;
	ps->tx_bytes += pkt_len;

	if (status & TX_FRAME_ABORTED) {
		if (dev->if_port == IF_PORT_100BASEFX) {
			if (status & (TX_JAB_TIMEOUT | TX_UNDERRUN)) {
				/* any other tx errors are only valid
				 * in half duplex mode */
				ps->tx_errors++;
				ps->tx_aborted_errors++;
			}
		}
		else {
			ps->tx_errors++;
			ps->tx_aborted_errors++;
			if (status & (TX_NO_CARRIER | TX_LOSS_CARRIER))
				ps->tx_carrier_errors++;
		}
	}
}


/*
 * Called from the interrupt service routine to acknowledge
 * the TX DONE bits.  This is a must if the irq is setup as
 * edge triggered.
 */
static void au1000_tx_ack(struct net_device *dev)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;
	volatile tx_dma_t *ptxd;

	ptxd = aup->tx_dma_ring[aup->tx_tail];

	while (ptxd->buff_stat & TX_T_DONE) {
 		update_tx_stats(dev, ptxd->status, aup->tx_len[aup->tx_tail]  & 0x3ff);
		ptxd->buff_stat &= ~TX_T_DONE;
 		aup->tx_len[aup->tx_tail] = 0;
		ptxd->len = 0;
		au_sync();

		aup->tx_tail = (aup->tx_tail + 1) & (NUM_TX_DMA - 1);
		ptxd = aup->tx_dma_ring[aup->tx_tail];

		if (aup->tx_full) {
			aup->tx_full = 0;
			netif_wake_queue(dev);
		}
	}
}


/*
 * Au1000 transmit routine.
 */
static int au1000_tx(struct sk_buff *skb, struct net_device *dev)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;
	volatile tx_dma_t *ptxd;
	u32 buff_stat;
	db_dest_t *pDB;
	int i;

	if (au1000_debug > 4)
		printk("%s: tx: aup %x len=%d, data=%p, head %d\n", 
				dev->name, (unsigned)aup, skb->len, 
				skb->data, aup->tx_head);

	ptxd = aup->tx_dma_ring[aup->tx_head];
	buff_stat = ptxd->buff_stat;
	if (buff_stat & TX_DMA_ENABLE) {
		/* We've wrapped around and the transmitter is still busy */
		netif_stop_queue(dev);
		aup->tx_full = 1;
		return 1;
	}
	else if (buff_stat & TX_T_DONE) {
 		update_tx_stats(dev, ptxd->status, aup->tx_len[aup->tx_head] & 0x3ff);
 		aup->tx_len[aup->tx_head] = 0;
		ptxd->len = 0;
	}

	if (aup->tx_full) {
		aup->tx_full = 0;
		netif_wake_queue(dev);
	}

	pDB = aup->tx_db_inuse[aup->tx_head];
	memcpy((void *)pDB->vaddr, skb->data, skb->len);
	if (skb->len < MAC_MIN_PKT_SIZE) {
		for (i=skb->len; i<MAC_MIN_PKT_SIZE; i++) { 
			((char *)pDB->vaddr)[i] = 0;
		}
 		aup->tx_len[aup->tx_head] = MAC_MIN_PKT_SIZE;
		ptxd->len = MAC_MIN_PKT_SIZE;
	}
	else {
 		aup->tx_len[aup->tx_head] = skb->len;
		ptxd->len = skb->len;
	}
	ptxd->buff_stat = pDB->dma_addr | TX_DMA_ENABLE;
	au_sync();
	dev_kfree_skb(skb);
	aup->tx_head = (aup->tx_head + 1) & (NUM_TX_DMA - 1);
	dev->trans_start = jiffies;
	return 0;
}


static inline void update_rx_stats(struct net_device *dev, u32 status)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;
	struct net_device_stats *ps = &aup->stats;

	ps->rx_packets++;
	if (status & RX_MCAST_FRAME)
		ps->multicast++;

	if (status & RX_ERROR) {
		ps->rx_errors++;
		if (status & RX_MISSED_FRAME)
			ps->rx_missed_errors++;
		if (status & (RX_OVERLEN | RX_OVERLEN | RX_LEN_ERROR))
			ps->rx_length_errors++;
		if (status & RX_CRC_ERROR)
			ps->rx_crc_errors++;
		if (status & RX_COLL)
			ps->collisions++;
	}
	else 
		ps->rx_bytes += status & RX_FRAME_LEN_MASK;

}

/*
 * Au1000 receive routine.
 */
static int au1000_rx(struct net_device *dev)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;
	struct sk_buff *skb;
	volatile rx_dma_t *prxd;
	u32 buff_stat, status;
	db_dest_t *pDB;

	if (au1000_debug > 4)
		printk("%s: au1000_rx head %d\n", dev->name, aup->rx_head);

	prxd = aup->rx_dma_ring[aup->rx_head];
	buff_stat = prxd->buff_stat;
	while (buff_stat & RX_T_DONE)  {
		status = prxd->status;
		pDB = aup->rx_db_inuse[aup->rx_head];
		update_rx_stats(dev, status);
		if (!(status & RX_ERROR))  {

			/* good frame */
			skb = dev_alloc_skb((status & RX_FRAME_LEN_MASK) + 2);
			if (skb == NULL) {
				printk(KERN_ERR
				       "%s: Memory squeeze, dropping packet.\n",
				       dev->name);
				aup->stats.rx_dropped++;
				continue;
			}
			skb->dev = dev;
			skb_reserve(skb, 2);	/* 16 byte IP header align */
			eth_copy_and_sum(skb, (unsigned char *)pDB->vaddr, 
					status & RX_FRAME_LEN_MASK, 0);
			skb_put(skb, status & RX_FRAME_LEN_MASK);
			skb->protocol = eth_type_trans(skb, dev);
			netif_rx(skb);	/* pass the packet to upper layers */
		}
		else {
			if (au1000_debug > 4) {
				if (status & RX_MISSED_FRAME) 
					printk("rx miss\n");
				if (status & RX_WDOG_TIMER) 
					printk("rx wdog\n");
				if (status & RX_RUNT) 
					printk("rx runt\n");
				if (status & RX_OVERLEN) 
					printk("rx overlen\n");
				if (status & RX_COLL)
					printk("rx coll\n");
				if (status & RX_MII_ERROR)
					printk("rx mii error\n");
				if (status & RX_CRC_ERROR)
					printk("rx crc error\n");
				if (status & RX_LEN_ERROR)
					printk("rx len error\n");
				if (status & RX_U_CNTRL_FRAME)
					printk("rx u control frame\n");
				if (status & RX_MISSED_FRAME)
					printk("rx miss\n");
			}
		}
		prxd->buff_stat = (u32)(pDB->dma_addr | RX_DMA_ENABLE);
		aup->rx_head = (aup->rx_head + 1) & (NUM_RX_DMA - 1);
		au_sync();

		/* next descriptor */
		prxd = aup->rx_dma_ring[aup->rx_head];
		buff_stat = prxd->buff_stat;
		dev->last_rx = jiffies;
	}
	return 0;
}


/*
 * Au1000 interrupt service routine.
 */
irqreturn_t au1000_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct net_device *dev = (struct net_device *) dev_id;

	if (dev == NULL) {
		printk(KERN_ERR "%s: isr: null dev ptr\n", dev->name);
		return IRQ_NONE;
	}
	au1000_tx_ack(dev);
	au1000_rx(dev);
	return IRQ_HANDLED;
}


/*
 * The Tx ring has been full longer than the watchdog timeout
 * value. The transmitter must be hung?
 */
static void au1000_tx_timeout(struct net_device *dev)
{
	printk(KERN_ERR "%s: au1000_tx_timeout: dev=%p\n", dev->name, dev);
	reset_mac(dev);
	au1000_init(dev);
	dev->trans_start = jiffies;
	netif_wake_queue(dev);
}

static void set_rx_mode(struct net_device *dev)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;

	if (au1000_debug > 4) 
		printk("%s: set_rx_mode: flags=%x\n", dev->name, dev->flags);

	if (dev->flags & IFF_PROMISC) {			/* Set promiscuous. */
		aup->mac->control |= MAC_PROMISCUOUS;
		printk(KERN_INFO "%s: Promiscuous mode enabled.\n", dev->name);
	} else if ((dev->flags & IFF_ALLMULTI)  ||
			   dev->mc_count > MULTICAST_FILTER_LIMIT) {
		aup->mac->control |= MAC_PASS_ALL_MULTI;
		aup->mac->control &= ~MAC_PROMISCUOUS;
		printk(KERN_INFO "%s: Pass all multicast\n", dev->name);
	} else {
		int i;
		struct dev_mc_list *mclist;
		u32 mc_filter[2];	/* Multicast hash filter */

		mc_filter[1] = mc_filter[0] = 0;
		for (i = 0, mclist = dev->mc_list; mclist && i < dev->mc_count;
			 i++, mclist = mclist->next) {
			set_bit(ether_crc_le(ETH_ALEN, mclist->dmi_addr)>>26, 
					mc_filter);
		}
		aup->mac->multi_hash_high = mc_filter[1];
		aup->mac->multi_hash_low = mc_filter[0];
		aup->mac->control &= ~MAC_PROMISCUOUS;
		aup->mac->control |= MAC_HASH_MODE;
	}
}


static int au1000_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	u16 *data = (u16 *)&rq->ifr_ifru;

	/* fixme */
	switch(cmd) { 
	case SIOCGMIIPHY:	/* Get the address of the PHY in use. */
		data[0] = PHY_ADDRESS;
		return 0;

	case SIOCGMIIREG:	/* Read the specified MII register. */
		//data[3] = mdio_read(ioaddr, data[0], data[1]); 
		return 0;

	case SIOCSMIIREG:	/* Write the specified MII register */
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;

		//mdio_write(ioaddr, data[0], data[1], data[2]);
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}


static int au1000_set_config(struct net_device *dev, struct ifmap *map)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;
	u16 control;

	if (au1000_debug > 4)  {
		printk("%s: set_config called: dev->if_port %d map->port %x\n", 
				dev->name, dev->if_port, map->port);
	}

	switch(map->port){
		case IF_PORT_UNKNOWN: /* use auto here */   
			printk(KERN_INFO "%s: config phy for aneg\n", 
					dev->name);
			dev->if_port = map->port;
			/* Link Down: the timer will bring it up */
			netif_carrier_off(dev);
	
			/* read current control */
			control = mdio_read(dev, aup->phy_addr, MII_CONTROL);
			control &= ~(MII_CNTL_FDX | MII_CNTL_F100);

			/* enable auto negotiation and reset the negotiation */
			mdio_write(dev, aup->phy_addr, MII_CONTROL, 
					control | MII_CNTL_AUTO | 
					MII_CNTL_RST_AUTO);

			break;
    
		case IF_PORT_10BASET: /* 10BaseT */         
			printk(KERN_INFO "%s: config phy for 10BaseT\n", 
					dev->name);
			dev->if_port = map->port;
	
			/* Link Down: the timer will bring it up */
			netif_carrier_off(dev);

			/* set Speed to 10Mbps, Half Duplex */
			control = mdio_read(dev, aup->phy_addr, MII_CONTROL);
			control &= ~(MII_CNTL_F100 | MII_CNTL_AUTO | 
					MII_CNTL_FDX);
	
			/* disable auto negotiation and force 10M/HD mode*/
			mdio_write(dev, aup->phy_addr, MII_CONTROL, control);
			break;
    
		case IF_PORT_100BASET: /* 100BaseT */
		case IF_PORT_100BASETX: /* 100BaseTx */ 
			printk(KERN_INFO "%s: config phy for 100BaseTX\n", 
					dev->name);
			dev->if_port = map->port;
	
			/* Link Down: the timer will bring it up */
			netif_carrier_off(dev);
	
			/* set Speed to 100Mbps, Half Duplex */
			/* disable auto negotiation and enable 100MBit Mode */
			control = mdio_read(dev, aup->phy_addr, MII_CONTROL);
			printk("read control %x\n", control);
			control &= ~(MII_CNTL_AUTO | MII_CNTL_FDX);
			control |= MII_CNTL_F100;
			mdio_write(dev, aup->phy_addr, MII_CONTROL, control);
			break;
    
		case IF_PORT_100BASEFX: /* 100BaseFx */
			printk(KERN_INFO "%s: config phy for 100BaseFX\n", 
					dev->name);
			dev->if_port = map->port;
	
			/* Link Down: the timer will bring it up */
			netif_carrier_off(dev);
	
			/* set Speed to 100Mbps, Full Duplex */
			/* disable auto negotiation and enable 100MBit Mode */
			control = mdio_read(dev, aup->phy_addr, MII_CONTROL);
			control &= ~MII_CNTL_AUTO;
			control |=  MII_CNTL_F100 | MII_CNTL_FDX;
			mdio_write(dev, aup->phy_addr, MII_CONTROL, control);
			break;
		case IF_PORT_10BASE2: /* 10Base2 */
		case IF_PORT_AUI: /* AUI */
		/* These Modes are not supported (are they?)*/
			printk(KERN_ERR "%s: 10Base2/AUI not supported", 
					dev->name);
			return -EOPNOTSUPP;
			break;
    
		default:
			printk(KERN_ERR "%s: Invalid media selected", 
					dev->name);
			return -EINVAL;
	}
	return 0;
}

static struct net_device_stats *au1000_get_stats(struct net_device *dev)
{
	struct au1000_private *aup = (struct au1000_private *) dev->priv;

	if (au1000_debug > 4)
		printk("%s: au1000_get_stats: dev=%p\n", dev->name, dev);

	if (netif_device_present(dev)) {
		return &aup->stats;
	}
	return 0;
}

module_init(au1000_init_module);
module_exit(au1000_cleanup_module);
