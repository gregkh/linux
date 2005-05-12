/*
 * arch/ppc/platforms/lite5200.c
 *
 * Platform support file for the Freescale LITE5200 based on MPC52xx.
 * A maximum of this file should be moved to syslib/mpc52xx_?????
 * so that new platform based on MPC52xx need a minimal platform file
 * ( avoid code duplication )
 *
 * 
 * Maintainer : Sylvain Munaut <tnt@246tNt.com>
 *
 * Based on the 2.4 code written by Kent Borg,
 * Dale Farnsworth <dale.farnsworth@mvista.com> and
 * Wolfgang Denk <wd@denx.de>
 * 
 * Copyright 2004 Sylvain Munaut <tnt@246tNt.com>
 * Copyright 2003 Motorola Inc.
 * Copyright 2003 MontaVista Software Inc.
 * Copyright 2003 DENX Software Engineering (wd@denx.de)
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2. This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/config.h>
#include <linux/initrd.h>
#include <linux/seq_file.h>
#include <linux/kdev_t.h>
#include <linux/root_dev.h>
#include <linux/console.h>

#include <asm/bootinfo.h>
#include <asm/io.h>
#include <asm/ocp.h>
#include <asm/mpc52xx.h>


extern int powersave_nap;

/* Board data given by U-Boot */
bd_t __res;
EXPORT_SYMBOL(__res);	/* For modules */


/* ======================================================================== */
/* OCP device definition                                                    */
/* For board/shared resources like PSCs                                     */
/* ======================================================================== */
/* Be sure not to load conficting devices : e.g. loading the UART drivers for
 * PSC1 and then also loading a AC97 for this same PSC.
 * For details about how to create an entry, look in the doc of the concerned
 * driver ( eg drivers/serial/mpc52xx_uart.c for the PSC in uart mode )
 */

struct ocp_def board_ocp[] = {
	{
		.vendor		= OCP_VENDOR_FREESCALE,
		.function	= OCP_FUNC_PSC_UART,
		.index		= 0,
		.paddr		= MPC52xx_PSC1,
		.irq		= MPC52xx_PSC1_IRQ,
		.pm		= OCP_CPM_NA,
	},
	{	/* Terminating entry */
		.vendor		= OCP_VENDOR_INVALID
	}
};


/* ======================================================================== */
/* Platform specific code                                                   */
/* ======================================================================== */

static int
lite5200_show_cpuinfo(struct seq_file *m)
{
	seq_printf(m, "machine\t\t: Freescale LITE5200\n");
	return 0;
}

static void __init
lite5200_setup_cpu(void)
{
	struct mpc52xx_intr *intr;

	u32 intr_ctrl;

	/* Map zones */
	intr = (struct mpc52xx_intr *)
		ioremap(MPC52xx_INTR,sizeof(struct mpc52xx_intr));

	if (!intr) {
		printk("lite5200.c: Error while mapping INTR during lite5200_setup_cpu\n");
		goto unmap_regs;
	}

	/* IRQ[0-3] setup : IRQ0     - Level Active Low  */
	/*                  IRQ[1-3] - Level Active High */
	intr_ctrl = in_be32(&intr->ctrl);
	intr_ctrl &= ~0x00ff0000;
	intr_ctrl |=  0x00c00000;
	out_be32(&intr->ctrl, intr_ctrl);

	/* Unmap reg zone */
unmap_regs:
	if (intr) iounmap(intr);
}

static void __init
lite5200_setup_arch(void)
{
	/* Add board OCP definitions */
	mpc52xx_add_board_devices(board_ocp);

	/* CPU & Port mux setup */
	lite5200_setup_cpu();
}

void __init
platform_init(unsigned long r3, unsigned long r4, unsigned long r5,
              unsigned long r6, unsigned long r7)
{
	/* Generic MPC52xx platform initialization */
	/* TODO Create one and move a max of stuff in it.
	   Put this init in the syslib */

	struct bi_record *bootinfo = find_bootinfo();

	if (bootinfo)
		parse_bootinfo(bootinfo);
	else {
		/* Load the bd_t board info structure */
		if (r3)
			memcpy((void*)&__res,(void*)(r3+KERNELBASE),
					sizeof(bd_t));

#ifdef CONFIG_BLK_DEV_INITRD
		/* Load the initrd */
		if (r4) {
			initrd_start = r4 + KERNELBASE;
			initrd_end = r5 + KERNELBASE;
		}
#endif

		/* Load the command line */
		if (r6) {
			*(char *)(r7+KERNELBASE) = 0;
			strcpy(cmd_line, (char *)(r6+KERNELBASE));
		}
	}

	/* BAT setup */
	mpc52xx_set_bat();

	/* No ISA bus AFAIK */
	isa_io_base		= 0;
	isa_mem_base		= 0;

	/* Powersave */
	powersave_nap = 1;	/* We allow this platform to NAP */

	/* Setup the ppc_md struct */
	ppc_md.setup_arch	= lite5200_setup_arch;
	ppc_md.show_cpuinfo	= lite5200_show_cpuinfo;
	ppc_md.show_percpuinfo	= NULL;
	ppc_md.init_IRQ		= mpc52xx_init_irq;
	ppc_md.get_irq		= mpc52xx_get_irq;

	ppc_md.find_end_of_memory = mpc52xx_find_end_of_memory;
	ppc_md.setup_io_mappings  = mpc52xx_map_io;

	ppc_md.restart		= mpc52xx_restart;
	ppc_md.power_off	= mpc52xx_power_off;
	ppc_md.halt		= mpc52xx_halt;

		/* No time keeper on the LITE5200 */
	ppc_md.time_init	= NULL;
	ppc_md.get_rtc_time	= NULL;
	ppc_md.set_rtc_time	= NULL;

	ppc_md.calibrate_decr	= mpc52xx_calibrate_decr;
#ifdef CONFIG_SERIAL_TEXT_DEBUG
	ppc_md.progress		= mpc52xx_progress;
#endif
}

