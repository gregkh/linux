/*
 * include/asm-ppc/mpc85xx.h
 *
 * MPC85xx definitions
 *
 * Maintainer: Kumar Gala <kumar.gala@freescale.com>
 *
 * Copyright 2004 Freescale Semiconductor, Inc
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifdef __KERNEL__
#ifndef __ASM_MPC85xx_H__
#define __ASM_MPC85xx_H__

#include <linux/config.h>
#include <asm/mmu.h>

#ifdef CONFIG_85xx

#ifdef CONFIG_MPC8540_ADS
#include <platforms/85xx/mpc8540_ads.h>
#endif
#ifdef CONFIG_MPC8555_CDS
#include <platforms/85xx/mpc8555_cds.h>
#endif
#ifdef CONFIG_MPC8560_ADS
#include <platforms/85xx/mpc8560_ads.h>
#endif
#ifdef CONFIG_SBC8560
#include <platforms/85xx/sbc8560.h>
#endif
#ifdef CONFIG_STX_GP3
#include <platforms/85xx/stx_gp3.h>
#endif

#define _IO_BASE        isa_io_base
#define _ISA_MEM_BASE   isa_mem_base
#ifdef CONFIG_PCI
#define PCI_DRAM_OFFSET pci_dram_offset
#else
#define PCI_DRAM_OFFSET 0
#endif

/*
 * The "residual" board information structure the boot loader passes
 * into the kernel.
 */
extern unsigned char __res[];

/* Internal IRQs on MPC85xx OpenPIC */
/* Not all of these exist on all MPC85xx implementations */

#ifndef MPC85xx_OPENPIC_IRQ_OFFSET
#define MPC85xx_OPENPIC_IRQ_OFFSET	64
#endif

/* The 32 internal sources */
#define MPC85xx_IRQ_L2CACHE	( 0 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_ECM		( 1 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_DDR		( 2 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_LBIU	( 3 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_DMA0	( 4 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_DMA1	( 5 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_DMA2	( 6 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_DMA3	( 7 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_PCI1	( 8 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_PCI2	( 9 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_RIO_ERROR	( 9 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_RIO_BELL	(10 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_RIO_TX	(11 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_RIO_RX	(12 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_TSEC1_TX	(13 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_TSEC1_RX	(14 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_TSEC1_ERROR	(18 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_TSEC2_TX	(19 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_TSEC2_RX	(20 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_TSEC2_ERROR	(24 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_FEC		(25 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_DUART	(26 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_IIC1	(27 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_PERFMON	(28 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_SEC2	(29 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_CPM		(30 + MPC85xx_OPENPIC_IRQ_OFFSET)

/* The 12 external interrupt lines */
#define MPC85xx_IRQ_EXT0        (32 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_EXT1        (33 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_EXT2        (34 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_EXT3        (35 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_EXT4        (36 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_EXT5        (37 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_EXT6        (38 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_EXT7        (39 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_EXT8        (40 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_EXT9        (41 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_EXT10       (42 + MPC85xx_OPENPIC_IRQ_OFFSET)
#define MPC85xx_IRQ_EXT11       (43 + MPC85xx_OPENPIC_IRQ_OFFSET)

/* Offset from CCSRBAR */
#define MPC85xx_CPM_OFFSET	(0x80000)
#define MPC85xx_CPM_SIZE	(0x40000)
#define MPC85xx_DMA_OFFSET	(0x21000)
#define MPC85xx_DMA_SIZE	(0x01000)
#define MPC85xx_DMA0_OFFSET	(0x21100)
#define MPC85xx_DMA0_SIZE	(0x00080)
#define MPC85xx_DMA1_OFFSET	(0x21180)
#define MPC85xx_DMA1_SIZE	(0x00080)
#define MPC85xx_DMA2_OFFSET	(0x21200)
#define MPC85xx_DMA2_SIZE	(0x00080)
#define MPC85xx_DMA3_OFFSET	(0x21280)
#define MPC85xx_DMA3_SIZE	(0x00080)
#define MPC85xx_ENET1_OFFSET	(0x24000)
#define MPC85xx_ENET1_SIZE	(0x01000)
#define MPC85xx_ENET2_OFFSET	(0x25000)
#define MPC85xx_ENET2_SIZE	(0x01000)
#define MPC85xx_ENET3_OFFSET	(0x26000)
#define MPC85xx_ENET3_SIZE	(0x01000)
#define MPC85xx_GUTS_OFFSET	(0xe0000)
#define MPC85xx_GUTS_SIZE	(0x01000)
#define MPC85xx_IIC1_OFFSET	(0x03000)
#define MPC85xx_IIC1_SIZE	(0x01000)
#define MPC85xx_OPENPIC_OFFSET	(0x40000)
#define MPC85xx_OPENPIC_SIZE	(0x40000)
#define MPC85xx_PCI1_OFFSET	(0x08000)
#define MPC85xx_PCI1_SIZE	(0x01000)
#define MPC85xx_PCI2_OFFSET	(0x09000)
#define MPC85xx_PCI2_SIZE	(0x01000)
#define MPC85xx_PERFMON_OFFSET	(0xe1000)
#define MPC85xx_PERFMON_SIZE	(0x01000)
#define MPC85xx_SEC2_OFFSET	(0x30000)
#define MPC85xx_SEC2_SIZE	(0x10000)
#define MPC85xx_UART0_OFFSET	(0x04500)
#define MPC85xx_UART0_SIZE	(0x00100)
#define MPC85xx_UART1_OFFSET	(0x04600)
#define MPC85xx_UART1_SIZE	(0x00100)

#define MPC85xx_CCSRBAR_SIZE	(1024*1024)

/* Let modules/drivers get at CCSRBAR */
extern phys_addr_t get_ccsrbar(void);

#ifdef MODULE
#define CCSRBAR get_ccsrbar()
#else
#define CCSRBAR BOARD_CCSRBAR
#endif

enum ppc_sys_devices {
	MPC85xx_TSEC1,
	MPC85xx_TSEC2,
	MPC85xx_FEC,
	MPC85xx_IIC1,
	MPC85xx_DMA0,
	MPC85xx_DMA1,
	MPC85xx_DMA2,
	MPC85xx_DMA3,
	MPC85xx_DUART,
	MPC85xx_PERFMON,
	MPC85xx_SEC2,
	MPC85xx_CPM_SPI,
	MPC85xx_CPM_I2C,
	MPC85xx_CPM_USB,
	MPC85xx_CPM_SCC1,
	MPC85xx_CPM_SCC2,
	MPC85xx_CPM_SCC3,
	MPC85xx_CPM_SCC4,
	MPC85xx_CPM_FCC1,
	MPC85xx_CPM_FCC2,
	MPC85xx_CPM_FCC3,
	MPC85xx_CPM_MCC1,
	MPC85xx_CPM_MCC2,
	MPC85xx_CPM_SMC1,
	MPC85xx_CPM_SMC2,
};

#endif /* CONFIG_85xx */
#endif /* __ASM_MPC85xx_H__ */
#endif /* __KERNEL__ */
