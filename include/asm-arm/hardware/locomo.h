/*
 * linux/include/asm-arm/hardware/locomo.h
 *
 * This file contains the definitions for the LoCoMo G/A Chip
 *
 * (C) Copyright 2004 John Lenz
 *
 * May be copied or modified under the terms of the GNU General Public
 * License.  See linux/COPYING for more information.
 *
 * Based on sa1111.h
 */
#ifndef _ASM_ARCH_LOCOMO
#define _ASM_ARCH_LOCOMO

#define locomo_writel(val,addr)	({ *(volatile u16 *)(addr) = (val); })
#define locomo_readl(addr)	(*(volatile u16 *)(addr))

/* LOCOMO version */
#define LOCOMO_VER	0x00

/* Pin status */
#define LOCOMO_ST	0x04

/* Pin status */
#define LOCOMO_C32K	0x08

/* Interrupt controller */
#define LOCOMO_ICR	0x0C

/* MCS decoder for boot selecting */
#define LOCOMO_MCSX0	0x10
#define LOCOMO_MCSX1	0x14
#define LOCOMO_MCSX2	0x18
#define LOCOMO_MCSX3	0x1c

/* Touch panel controller */
#define LOCOMO_ASD	0x20	/* AD start delay */
#define LOCOMO_HSD	0x28	/* HSYS delay */
#define LOCOMO_HSC	0x2c	/* HSYS period */
#define LOCOMO_TADC	0x30	/* tablet ADC clock */

/* TFT signal */
#define LOCOMO_TC	0x38	/* TFT control signal */
#define LOCOMO_CPSD	0x3c	/* CPS delay */

/* Key controller */
#define LOCOMO_KIB	0x40	/* KIB level */
#define LOCOMO_KSC	0x44	/* KSTRB control */
#define LOCOMO_KCMD	0x48	/* KSTRB command */
#define LOCOMO_KIC	0x4c	/* Key interrupt */

/* Audio clock */
#define LOCOMO_ACC	0x54

/* SPI interface */
#define LOCOMO_SPIMD	0x60	/* SPI mode setting */
#define LOCOMO_SPICT	0x64	/* SPI mode control */
#define LOCOMO_SPIST	0x68	/* SPI status */
#define LOCOMO_SPIIS	0x70	/* SPI interrupt status */
#define LOCOMO_SPIWE	0x74	/* SPI interrupt status write enable */
#define LOCOMO_SPIIE	0x78	/* SPI interrupt enable */
#define LOCOMO_SPIIR	0x7c	/* SPI interrupt request */
#define LOCOMO_SPITD	0x80	/* SPI transfer data write */
#define LOCOMO_SPIRD	0x84	/* SPI receive data read */
#define LOCOMO_SPITS	0x88	/* SPI transfer data shift */
#define LOCOMO_SPIRS	0x8C	/* SPI receive data shift */

#define	LOCOMO_SPI_TEND	(1 << 3)	/* Transfer end bit */
#define	LOCOMO_SPI_OVRN	(1 << 2)	/* Over Run bit */
#define	LOCOMO_SPI_RFW	(1 << 1)	/* write buffer bit */
#define	LOCOMO_SPI_RFR	(1)		/* read buffer bit */

/* GPIO */
#define LOCOMO_GPD	0x90	/* GPIO direction */
#define LOCOMO_GPE	0x94	/* GPIO input enable */
#define LOCOMO_GPL	0x98	/* GPIO level */
#define LOCOMO_GPO	0x9c	/* GPIO out data setteing */
#define LOCOMO_GRIE	0xa0	/* GPIO rise detection */
#define LOCOMO_GFIE	0xa4	/* GPIO fall detection */
#define LOCOMO_GIS	0xa8	/* GPIO edge detection status */
#define LOCOMO_GWE	0xac	/* GPIO status write enable */
#define LOCOMO_GIE	0xb0	/* GPIO interrupt enable */
#define LOCOMO_GIR	0xb4	/* GPIO interrupt request */

#define LOCOMO_GPIO0	(1<<0)
#define LOCOMO_GPIO1	(1<<1)
#define LOCOMO_GPIO2	(1<<2)
#define LOCOMO_GPIO3	(1<<3)
#define LOCOMO_GPIO4	(1<<4)
#define LOCOMO_GPIO5	(1<<5)
#define LOCOMO_GPIO6	(1<<6)
#define LOCOMO_GPIO7	(1<<7)
#define LOCOMO_GPIO8	(1<<8)
#define LOCOMO_GPIO9	(1<<9)
#define LOCOMO_GPIO10	(1<<10)
#define LOCOMO_GPIO11	(1<<11)
#define LOCOMO_GPIO12	(1<<12)
#define LOCOMO_GPIO13	(1<<13)
#define LOCOMO_GPIO14	(1<<14)
#define LOCOMO_GPIO15	(1<<15)

/* Front light adjustment controller */
#define LOCOMO_ALS	0xc8	/* Adjust light cycle */
#define LOCOMO_ALD	0xcc	/* Adjust light duty */

/* PCM audio interface */
#define LOCOMO_PAIF	0xd0

/* Long time timer */
#define LOCOMO_LTC	0xd8	/* LTC interrupt setting */
#define LOCOMO_LTINT	0xdc	/* LTC interrupt */

/* DAC control signal for LCD (COMADJ ) */
#define LOCOMO_DAC	0xe0

/* DAC control */
#define	LOCOMO_DAC_SCLOEB	0x08	/* SCL pin output data       */
#define	LOCOMO_DAC_TEST		0x04	/* Test bit                  */
#define	LOCOMO_DAC_SDA		0x02	/* SDA pin level (read-only) */
#define	LOCOMO_DAC_SDAOEB	0x01	/* SDA pin output data       */

/* LED controller */
#define LOCOMO_LPT0		0xe8	/* LEDPWM0 timer */
#define LOCOMO_LPT1		0xec	/* LEDPWM1 timer */

#define LOCOMO_LPT_TOFH		0x80			/* */
#define LOCOMO_LPT_TOFL		0x08			/* */
#define LOCOMO_LPT_TOH(TOH)	((TOH & 0x7) << 4)	/* */
#define LOCOMO_LPT_TOL(TOL)	((TOL & 0x7))		/* */

/* Audio clock */
#define	LOCOMO_ACC_XON		0x80	/*  */
#define	LOCOMO_ACC_XEN		0x40	/*  */
#define	LOCOMO_ACC_XSEL0	0x00	/*  */
#define	LOCOMO_ACC_XSEL1	0x20	/*  */
#define	LOCOMO_ACC_MCLKEN	0x10	/*  */
#define	LOCOMO_ACC_64FSEN	0x08	/*  */
#define	LOCOMO_ACC_CLKSEL000	0x00	/* mclk  2 */
#define	LOCOMO_ACC_CLKSEL001	0x01	/* mclk  3 */
#define	LOCOMO_ACC_CLKSEL010	0x02	/* mclk  4 */
#define	LOCOMO_ACC_CLKSEL011	0x03	/* mclk  6 */
#define	LOCOMO_ACC_CLKSEL100	0x04	/* mclk  8 */
#define	LOCOMO_ACC_CLKSEL101	0x05	/* mclk 12 */

/* PCM audio interface */
#define	LOCOMO_PAIF_SCINV	0x20	/*  */
#define	LOCOMO_PAIF_SCEN	0x10	/*  */
#define	LOCOMO_PAIF_LRCRST	0x08	/*  */
#define	LOCOMO_PAIF_LRCEVE	0x04	/*  */
#define	LOCOMO_PAIF_LRCINV	0x02	/*  */
#define	LOCOMO_PAIF_LRCEN	0x01	/*  */

/* GPIO */
#define	LOCOMO_GPIO(Nb)		(0x01 << (Nb))	/* LoCoMo GPIO [0...15] */
#define LOCOMO_GPIO_RTS		LOCOMO_GPIO(0)	/* LoCoMo GPIO  [0] */
#define LOCOMO_GPIO_CTS		LOCOMO_GPIO(1)	/* LoCoMo GPIO  [1] */
#define LOCOMO_GPIO_DSR		LOCOMO_GPIO(2)	/* LoCoMo GPIO  [2] */
#define LOCOMO_GPIO_DTR		LOCOMO_GPIO(3)	/* LoCoMo GPIO  [3] */
#define LOCOMO_GPIO_LCD_VSHA_ON	LOCOMO_GPIO(4)	/* LoCoMo GPIO  [4] */
#define LOCOMO_GPIO_LCD_VSHD_ON	LOCOMO_GPIO(5)	/* LoCoMo GPIO  [5] */
#define LOCOMO_GPIO_LCD_VEE_ON	LOCOMO_GPIO(6)	/* LoCoMo GPIO  [6] */
#define LOCOMO_GPIO_LCD_MOD	LOCOMO_GPIO(7)	/* LoCoMo GPIO  [7] */
#define LOCOMO_GPIO_DAC_ON	LOCOMO_GPIO(8)	/* LoCoMo GPIO  [8] */
#define LOCOMO_GPIO_FL_VR	LOCOMO_GPIO(9)	/* LoCoMo GPIO  [9] */
#define LOCOMO_GPIO_DAC_SDATA	LOCOMO_GPIO(10)	/* LoCoMo GPIO [10] */
#define LOCOMO_GPIO_DAC_SCK	LOCOMO_GPIO(11)	/* LoCoMo GPIO [11] */
#define LOCOMO_GPIO_DAC_SLOAD	LOCOMO_GPIO(12)	/* LoCoMo GPIO [12] */

extern struct bus_type locomo_bus_type;

struct locomo_dev {
	struct device	dev;
	unsigned int	devid;
	struct resource	res;
	void		*mapbase;
	unsigned int	irq[1];
	u64		dma_mask;
};

#define LOCOMO_DEV(_d)	container_of((_d), struct locomo_dev, dev)

#define locomo_get_drvdata(d)	dev_get_drvdata(&(d)->dev)
#define locomo_set_drvdata(d,p)	dev_set_drvdata(&(d)->dev, p)

struct locomo_driver {
	struct device_driver	drv;
	unsigned int		devid;
	int (*probe)(struct locomo_dev *);
	int (*remove)(struct locomo_dev *);
	int (*suspend)(struct locomo_dev *, u32);
	int (*resume)(struct locomo_dev *);
};

#define LOCOMO_DRV(_d)	container_of((_d), struct locomo_driver, drv)

#define LOCOMO_DRIVER_NAME(_ldev) ((_ldev)->dev.driver->name)

void locomo_lcd_power(struct locomo_dev *, int, unsigned int);

int locomo_driver_register(struct locomo_driver *);
void locomo_driver_unregister(struct locomo_driver *);

#endif
