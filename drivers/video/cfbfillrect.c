/*
 *  Generic fillrect for frame buffers with packed pixels of any depth. 
 *
 *      Copyright (C)  2000 James Simmons (jsimmons@linux-fbdev.org) 
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 *
 * NOTES:
 *
 *  The code for depths like 24 that don't have integer number of pixels per 
 *  long is broken and needs to be fixed. For now I turned these types of 
 *  mode off.
 *
 *  Also need to add code to deal with cards endians that are different than
 *  the native cpu endians. I also need to deal with MSB position in the word.
 *
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/fb.h>
#include <asm/types.h>

#if BITS_PER_LONG == 32
#define FB_WRITEL fb_writel
#define FB_READL  fb_readl
#define BYTES_PER_LONG 4
#define SHIFT_PER_LONG 5
#else
#define FB_WRITEL fb_writeq
#define FB_READL  fb_readq
#define BYTES_PER_LONG 8
#define SHIFT_PER_LONG 6
#endif

#define EXP1(x)		0xffffffffU*x
#define EXP2(x)		0x55555555U*x
#define EXP4(x)		0x11111111U*0x ## x

typedef u32 pixel_t;

static const u32 bpp1tab[2] = {
    EXP1(0), EXP1(1)
};

static const u32 bpp2tab[4] = {
    EXP2(0), EXP2(1), EXP2(2), EXP2(3)
};

static const u32 bpp4tab[16] = {
    EXP4(0), EXP4(1), EXP4(2), EXP4(3), EXP4(4), EXP4(5), EXP4(6), EXP4(7),
    EXP4(8), EXP4(9), EXP4(a), EXP4(b), EXP4(c), EXP4(d), EXP4(e), EXP4(f)
};

    /*
     *  Compose two values, using a bitmask as decision value
     *  This is equivalent to (a & mask) | (b & ~mask)
     */

static inline unsigned long comp(unsigned long a, unsigned long b,
				 unsigned long mask)
{
    return ((a ^ b) & mask) ^ b;
}

static inline u32 pixel_to_pat32(const struct fb_info *p, pixel_t pixel)
{
    u32 pat = pixel;

    switch (p->var.bits_per_pixel) {
	case 1:
	    pat = bpp1tab[pat];
	    break;

	case 2:
	    pat = bpp2tab[pat];
	    break;

	case 4:
	    pat = bpp4tab[pat];
	    break;

	case 8:
	    pat |= pat << 8;
	    // Fall through
	case 16:
	    pat |= pat << 16;
	    // Fall through
	case 32:
	    break;
    }
    return pat;
}

    /*
     *  Expand a pixel value to a generic 32/64-bit pattern and rotate it to
     *  the correct start position
     */

static inline unsigned long pixel_to_pat(const struct fb_info *p, 
					 pixel_t pixel, int left)
{
    unsigned long pat = pixel;
    u32 bpp = p->var.bits_per_pixel;
    int i;

    /* expand pixel value */
    for (i = bpp; i < BITS_PER_LONG; i *= 2)
	pat |= pat << i;

    /* rotate pattern to correct start position */
    pat = pat << left | pat >> (bpp-left);
    return pat;
}

    /*
     *  Unaligned 32-bit pattern fill using 32/64-bit memory accesses
     */

void bitfill32(unsigned long __iomem *dst, int dst_idx, u32 pat, u32 n)
{
	unsigned long val = pat;
	unsigned long first, last;
	
	if (!n)
		return;
	
#if BITS_PER_LONG == 64
	val |= val << 32;
#endif
	
	first = ~0UL >> dst_idx;
	last = ~(~0UL >> ((dst_idx+n) % BITS_PER_LONG));
	
	if (dst_idx+n <= BITS_PER_LONG) {
		// Single word
		if (last)
			first &= last;
		FB_WRITEL(comp(val, FB_READL(dst), first), dst);
	} else {
		// Multiple destination words
		// Leading bits
		if (first) {
			FB_WRITEL(comp(val, FB_READL(dst), first), dst);
			dst++;
			n -= BITS_PER_LONG-dst_idx;
		}
		
		// Main chunk
		n /= BITS_PER_LONG;
		while (n >= 8) {
			FB_WRITEL(val, dst++);
			FB_WRITEL(val, dst++);
			FB_WRITEL(val, dst++);
			FB_WRITEL(val, dst++);
			FB_WRITEL(val, dst++);
			FB_WRITEL(val, dst++);
			FB_WRITEL(val, dst++);
			FB_WRITEL(val, dst++);
			n -= 8;
		}
		while (n--)
			FB_WRITEL(val, dst++);
		
		// Trailing bits
		if (last)
			FB_WRITEL(comp(val, FB_READL(dst), first), dst);
	}
}


    /*
     *  Unaligned generic pattern fill using 32/64-bit memory accesses
     *  The pattern must have been expanded to a full 32/64-bit value
     *  Left/right are the appropriate shifts to convert to the pattern to be
     *  used for the next 32/64-bit word
     */

void bitfill(unsigned long __iomem *dst, int dst_idx, unsigned long pat, int left,
	     int right, u32 n)
{
	unsigned long first, last;

	if (!n)
		return;
	
	first = ~0UL >> dst_idx;
	last = ~(~0UL >> ((dst_idx+n) % BITS_PER_LONG));
	
	if (dst_idx+n <= BITS_PER_LONG) {
		// Single word
		if (last)
			first &= last;
		FB_WRITEL(comp(pat, FB_READL(dst), first), dst);
	} else {
		// Multiple destination words
		// Leading bits
		if (first) {
			FB_WRITEL(comp(pat, FB_READL(dst), first), dst);
			dst++;
			pat = pat << left | pat >> right;
			n -= BITS_PER_LONG-dst_idx;
		}
		
		// Main chunk
		n /= BITS_PER_LONG;
		while (n >= 4) {
			FB_WRITEL(pat, dst++);
			pat = pat << left | pat >> right;
			FB_WRITEL(pat, dst++);
			pat = pat << left | pat >> right;
			FB_WRITEL(pat, dst++);
			pat = pat << left | pat >> right;
			FB_WRITEL(pat, dst++);
			pat = pat << left | pat >> right;
			n -= 4;
		}
		while (n--) {
			FB_WRITEL(pat, dst++);
			pat = pat << left | pat >> right;
		}
		
		// Trailing bits
		if (last)
			FB_WRITEL(comp(pat, FB_READL(dst), first), dst);
	}
}

void bitfill32_rev(unsigned long __iomem *dst, int dst_idx, u32 pat, u32 n)
{
	unsigned long val = pat, dat;
	unsigned long first, last;
	
	if (!n)
		return;
	
#if BITS_PER_LONG == 64
	val |= val << 32;
#endif
	
	first = ~0UL >> dst_idx;
	last = ~(~0UL >> ((dst_idx+n) % BITS_PER_LONG));
	
	if (dst_idx+n <= BITS_PER_LONG) {
		// Single word
		if (last)
			first &= last;
		dat = FB_READL(dst);
		FB_WRITEL(comp(dat ^ val, dat, first), dst);
	} else {
		// Multiple destination words
		// Leading bits
		if (first) {
			dat = FB_READL(dst);
			FB_WRITEL(comp(dat ^ val, dat, first), dst);
			dst++;
			n -= BITS_PER_LONG-dst_idx;
		}
		
		// Main chunk
		n /= BITS_PER_LONG;
		while (n >= 8) {
			FB_WRITEL(FB_READL(dst) ^ val, dst);
			dst++;
			FB_WRITEL(FB_READL(dst) ^ val, dst);
			dst++;
			FB_WRITEL(FB_READL(dst) ^ val, dst);
			dst++;
			FB_WRITEL(FB_READL(dst) ^ val, dst);
			dst++;
			FB_WRITEL(FB_READL(dst) ^ val, dst);
			dst++;
			FB_WRITEL(FB_READL(dst) ^ val, dst);
			dst++;
			FB_WRITEL(FB_READL(dst) ^ val, dst);
			dst++;
			FB_WRITEL(FB_READL(dst) ^ val, dst);
			dst++;
			n -= 8;
		}
		while (n--) {
			FB_WRITEL(FB_READL(dst) ^ val, dst);
			dst++;
		}		
		// Trailing bits
		if (last) {
			dat = FB_READL(dst);
			FB_WRITEL(comp(dat ^ val, dat, first), dst);
		}
	}
}


    /*
     *  Unaligned generic pattern fill using 32/64-bit memory accesses
     *  The pattern must have been expanded to a full 32/64-bit value
     *  Left/right are the appropriate shifts to convert to the pattern to be
     *  used for the next 32/64-bit word
     */

void bitfill_rev(unsigned long __iomem *dst, int dst_idx, unsigned long pat, int left,
	     int right, u32 n)
{
	unsigned long first, last, dat;

	if (!n)
		return;
	
	first = ~0UL >> dst_idx;
	last = ~(~0UL >> ((dst_idx+n) % BITS_PER_LONG));
	
	if (dst_idx+n <= BITS_PER_LONG) {
		// Single word
		if (last)
			first &= last;
		dat = FB_READL(dst);
		FB_WRITEL(comp(dat ^ pat, dat, first), dst);
	} else {
		// Multiple destination words
		// Leading bits
		if (first) {
			dat = FB_READL(dst);
			FB_WRITEL(comp(dat ^ pat, dat, first), dst);
			dst++;
			pat = pat << left | pat >> right;
			n -= BITS_PER_LONG-dst_idx;
		}
		
		// Main chunk
		n /= BITS_PER_LONG;
		while (n >= 4) {
			FB_WRITEL(FB_READL(dst) ^ pat, dst);
			dst++;
			pat = pat << left | pat >> right;
			FB_WRITEL(FB_READL(dst) ^ pat, dst);
			dst++;
			pat = pat << left | pat >> right;
			FB_WRITEL(FB_READL(dst) ^ pat, dst);
			dst++;
			pat = pat << left | pat >> right;
			FB_WRITEL(FB_READL(dst) ^ pat, dst);
			dst++;
			pat = pat << left | pat >> right;
			n -= 4;
		}
		while (n--) {
			FB_WRITEL(FB_READL(dst) ^ pat, dst);
			dst++;
			pat = pat << left | pat >> right;
		}
		
		// Trailing bits
		if (last) {
			dat = FB_READL(dst);
			FB_WRITEL(comp(dat ^ pat, dat, first), dst);
		}
	}
}

void cfb_fillrect(struct fb_info *p, const struct fb_fillrect *rect)
{
	u32 bpp = p->var.bits_per_pixel;
	unsigned long x2, y2, vxres, vyres;
	unsigned long height, width, fg;
	unsigned long __iomem *dst;
	int dst_idx, left;

	if (p->state != FBINFO_STATE_RUNNING)
		return;

	/* We want rotation but lack hardware to do it for us. */
	if (!p->fbops->fb_rotate && p->var.rotate) {
	}	
	
	vxres = p->var.xres_virtual;
	vyres = p->var.yres_virtual;

	if (!rect->width || !rect->height || 
	    rect->dx > vxres || rect->dy > vyres)
		return;

	/* We could use hardware clipping but on many cards you get around
	 * hardware clipping by writing to framebuffer directly. */
	
	x2 = rect->dx + rect->width;
	y2 = rect->dy + rect->height;
	x2 = x2 < vxres ? x2 : vxres;
	y2 = y2 < vyres ? y2 : vyres;
	width = x2 - rect->dx;
	height = y2 - rect->dy;
	
	if (p->fix.visual == FB_VISUAL_TRUECOLOR ||
	    p->fix.visual == FB_VISUAL_DIRECTCOLOR )
		fg = ((u32 *) (p->pseudo_palette))[rect->color];
	else
		fg = rect->color;
	
	dst = (unsigned long __iomem *)((unsigned long)p->screen_base & 
				~(BYTES_PER_LONG-1));
	dst_idx = ((unsigned long)p->screen_base & (BYTES_PER_LONG-1))*8;
	dst_idx += rect->dy*p->fix.line_length*8+rect->dx*bpp;
	/* FIXME For now we support 1-32 bpp only */
	left = BITS_PER_LONG % bpp;
	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);
	if (!left) {
		u32 pat = pixel_to_pat32(p, fg);
		void (*fill_op32)(unsigned long __iomem *dst, int dst_idx, u32 pat, 
				  u32 n) = NULL;
		
		switch (rect->rop) {
		case ROP_XOR:
			fill_op32 = bitfill32_rev;
			break;
		case ROP_COPY:
		default:
			fill_op32 = bitfill32;
			break;
		}
		while (height--) {
			dst += dst_idx >> SHIFT_PER_LONG;
			dst_idx &= (BITS_PER_LONG-1);
			fill_op32(dst, dst_idx, pat, width*bpp);
			dst_idx += p->fix.line_length*8;
		}
	} else {
		unsigned long pat = pixel_to_pat(p, fg, (left-dst_idx) % bpp);
		int right = bpp-left;
		int r;
		void (*fill_op)(unsigned long __iomem *dst, int dst_idx, 
				unsigned long pat, int left, int right, 
				u32 n) = NULL;
		
		switch (rect->rop) {
		case ROP_XOR:
			fill_op = bitfill_rev;
			break;
		case ROP_COPY:
		default:
			fill_op = bitfill;
			break;
		}
		while (height--) {
			dst += dst_idx >> SHIFT_PER_LONG;
			dst_idx &= (BITS_PER_LONG-1);
			fill_op(dst, dst_idx, pat, left, right, 
				width*bpp);
			r = (p->fix.line_length*8) % bpp;
			pat = pat << (bpp-r) | pat >> r;
			dst_idx += p->fix.line_length*8;
		}
	}
}

EXPORT_SYMBOL(cfb_fillrect);

MODULE_AUTHOR("James Simmons <jsimmons@users.sf.net>");
MODULE_DESCRIPTION("Generic software accelerated fill rectangle");
MODULE_LICENSE("GPL");
