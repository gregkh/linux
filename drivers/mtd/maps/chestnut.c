/*
 * drivers/mtd/maps/chestnut.c
 *
 * $Id: chestnut.c,v 1.1 2005/01/05 16:59:50 dwmw2 Exp $
 *
 * Flash map driver for IBM Chestnut (750FXGX Eval)
 *
 * Chose not to enable 8 bit flash as it contains the firmware and board
 * info.  Thus only the 32bit flash is supported.
 *
 * Author: <source@mvista.com>
 *
 * 2004 (c) MontaVista Software, Inc. This file is licensed under
 * the terms of the GNU General Public License version 2. This program
 * is licensed "as is" without any warranty of any kind, whether express
 * or implied.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <asm/io.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/map.h>
#include <linux/mtd/partitions.h>
#include <platforms/chestnut.h>

static struct map_info chestnut32_map = {
   	.name 		= "User FS",
   	.size 		= CHESTNUT_32BIT_SIZE,
   	.bankwidth 	= 4,
	.phys 		= CHESTNUT_32BIT_BASE,
};

static struct mtd_partition chestnut32_partitions[] = {
	{
		.name 	= "User FS",
		.offset	= 0,
		.size	= CHESTNUT_32BIT_SIZE,
	}
};

static struct mtd_info *flash32;

int __init init_chestnut(void)
{
	/* 32-bit FLASH */

   	chestnut32_map.virt = ioremap(chestnut32_map.phys, chestnut32_map.size);

   	if (!chestnut32_map.virt) {
      		printk(KERN_NOTICE "Failed to ioremap 32-bit flash\n");
		return -EIO;
   	}

	simple_map_init(&chestnut32_map);

   	flash32 = do_map_probe("cfi_probe", &chestnut32_map);
   	if (flash32) {
   		flash32->owner = THIS_MODULE;
   		add_mtd_partitions(flash32, chestnut32_partitions,
					ARRAY_SIZE(chestnut32_partitions));
   	} else {
      		printk(KERN_NOTICE "map probe failed for 32-bit flash\n");
		return -ENXIO;
	}

   	return 0;
}

static void __exit
cleanup_chestnut(void)
{
   	if (flash32) {
      		del_mtd_partitions(flash32);
		map_destroy(flash32);
   	}

   	if (chestnut32_map.virt) {
      		iounmap((void *)chestnut32_map.virt);
	  	chestnut32_map.virt = 0;
   	}
}

module_init(init_chestnut);
module_exit(cleanup_chestnut);

MODULE_DESCRIPTION("MTD map and partitions for IBM Chestnut (750fxgx Eval)");
MODULE_AUTHOR("<source@mvista.com>");
MODULE_LICENSE("GPL");
