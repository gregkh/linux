/*
 * linux/arch/arm/mach-s3c2410/clock.h
 *
 * Copyright (c) 2004 Simtec Electronics
 *	Written by Ben Dooks, <ben@simtec.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

struct clk {
	struct list_head      list;
	struct module        *owner;
	struct clk           *parent;
	const char           *name;
	int		      id;
	atomic_t              used;
	unsigned long         rate;
	unsigned long         ctrlbit;
	int		    (*enable)(struct clk *, int enable);
};

/* other clocks which may be registered by board support */

extern struct clk s3c24xx_dclk0;
extern struct clk s3c24xx_dclk1;
extern struct clk s3c24xx_clkout0;
extern struct clk s3c24xx_clkout1;
extern struct clk s3c24xx_uclk;

/* processor clock settings, in Hz */

extern unsigned long s3c24xx_xtal;
extern unsigned long s3c24xx_pclk;
extern unsigned long s3c24xx_hclk;
extern unsigned long s3c24xx_fclk;

/* exports for arch/arm/mach-s3c2410
 *
 * Please DO NOT use these outside of arch/arm/mach-s3c2410
*/

extern int s3c24xx_clkcon_enable(struct clk *clk, int enable);
extern int s3c24xx_register_clock(struct clk *clk);

extern int s3c24xx_setup_clocks(void);
