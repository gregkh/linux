/*
 *  Generic function for frame buffer with packed pixels of any depth.
 *
 *      Copyright (C)  June 1999 James Simmons
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 *
 * NOTES:
 * 
 *  This is for cfb packed pixels. Iplan and such are incorporated in the 
 *  drivers that need them.
 * 
 *  FIXME
 *  The code for 24 bit is horrible. It copies byte by byte size instead of 
 *  longs like the other sizes. Needs to be optimized. 
 *
 *  Also need to add code to deal with cards endians that are different than 
 *  the native cpu endians. I also need to deal with MSB position in the word.
 *  
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/fb.h>
#include <linux/slab.h>
#include <asm/types.h>
#include <asm/io.h>

#define LONG_MASK  (BITS_PER_LONG - 1)

#if BITS_PER_LONG == 32
#define FB_WRITEL fb_writel
#define FB_READL  fb_readl
#define SHIFT_PER_LONG 5
#define BYTES_PER_LONG 4
#else
#define FB_WRITEL fb_writeq
#define FB_READL  fb_readq
#define SHIFT_PER_LONG 6
#define BYTES_PER_LONG 8
#endif

static void bitcpy(unsigned long __iomem *dst, int dst_idx,
		   const unsigned long __iomem *src, int src_idx,
		   unsigned long n)
{
	unsigned long first, last;
	int shift = dst_idx-src_idx, left, right;
	unsigned long d0, d1;
	int m;
	
	if (!n)
		return;
	
	shift = dst_idx-src_idx;
	first = ~0UL >> dst_idx;
	last = ~(~0UL >> ((dst_idx+n) % BITS_PER_LONG));
	
	if (!shift) {
		// Same alignment for source and dest
		
		if (dst_idx+n <= BITS_PER_LONG) {
			// Single word
			if (last)
				first &= last;
			FB_WRITEL((FB_READL(src) & first) | (FB_READL(dst) & ~first), dst);
		} else {
			// Multiple destination words
			// Leading bits
			if (first) {
				
				FB_WRITEL((FB_READL(src) & first) | 
					  (FB_READL(dst) & ~first), dst);
				dst++;
				src++;
				n -= BITS_PER_LONG-dst_idx;
			}
			
			// Main chunk
			n /= BITS_PER_LONG;
			while (n >= 8) {
				FB_WRITEL(FB_READL(src++), dst++);
				FB_WRITEL(FB_READL(src++), dst++);
				FB_WRITEL(FB_READL(src++), dst++);
				FB_WRITEL(FB_READL(src++), dst++);
				FB_WRITEL(FB_READL(src++), dst++);
				FB_WRITEL(FB_READL(src++), dst++);
				FB_WRITEL(FB_READL(src++), dst++);
				FB_WRITEL(FB_READL(src++), dst++);
				n -= 8;
			}
			while (n--)
				FB_WRITEL(FB_READL(src++), dst++);
			// Trailing bits
			if (last)
				FB_WRITEL((FB_READL(src) & last) | (FB_READL(dst) & ~last), dst);
		}
	} else {
		// Different alignment for source and dest
		
		right = shift & (BITS_PER_LONG-1);
		left = -shift & (BITS_PER_LONG-1);
		
		if (dst_idx+n <= BITS_PER_LONG) {
			// Single destination word
			if (last)
				first &= last;
			if (shift > 0) {
				// Single source word
				FB_WRITEL(((FB_READL(src) >> right) & first) | 
					  (FB_READL(dst) & ~first), dst);
			} else if (src_idx+n <= BITS_PER_LONG) {
				// Single source word
				FB_WRITEL(((FB_READL(src) << left) & first) | 
					  (FB_READL(dst) & ~first), dst);
			} else {
				// 2 source words
				d0 = FB_READL(src++);
				d1 = FB_READL(src);
				FB_WRITEL(((d0<<left | d1>>right) & first) | 
					  (FB_READL(dst) & ~first), dst);
			}
		} else {
			// Multiple destination words
			d0 = FB_READL(src++);
			// Leading bits
			if (shift > 0) {
				// Single source word
				FB_WRITEL(((d0 >> right) & first) | 
					  (FB_READL(dst) & ~first), dst);
				dst++;
				n -= BITS_PER_LONG-dst_idx;
			} else {
				// 2 source words
				d1 = FB_READL(src++);
				FB_WRITEL(((d0<<left | d1>>right) & first) | 
					  (FB_READL(dst) & ~first), dst);
				d0 = d1;
				dst++;
				n -= BITS_PER_LONG-dst_idx;
			}
			
			// Main chunk
			m = n % BITS_PER_LONG;
			n /= BITS_PER_LONG;
			while (n >= 4) {
				d1 = FB_READL(src++);
				FB_WRITEL(d0 << left | d1 >> right, dst++);
				d0 = d1;
				d1 = FB_READL(src++);
				FB_WRITEL(d0 << left | d1 >> right, dst++);
				d0 = d1;
				d1 = FB_READL(src++);
				FB_WRITEL(d0 << left | d1 >> right, dst++);
				d0 = d1;
				d1 = FB_READL(src++);
				FB_WRITEL(d0 << left | d1 >> right, dst++);
				d0 = d1;
				n -= 4;
			}
			while (n--) {
				d1 = FB_READL(src++);
				FB_WRITEL(d0 << left | d1 >> right, dst++);
				d0 = d1;
			}
			
			// Trailing bits
			if (last) {
				if (m <= right) {
					// Single source word
					FB_WRITEL(((d0 << left) & last) | 
						  (FB_READL(dst) & ~last), 
						  dst);
				} else {
					// 2 source words
					d1 = FB_READL(src);
					FB_WRITEL(((d0<<left | d1>>right) & 
						   last) | (FB_READL(dst) & 
							    ~last), dst);
				}
			}
		}
	}
}

static void bitcpy_rev(unsigned long __iomem *dst, int dst_idx,
		       const unsigned long __iomem *src, int src_idx, unsigned long n)
{
	unsigned long first, last;
	int shift = dst_idx-src_idx, left, right;
	unsigned long d0, d1;
	int m;
	
	if (!n)
		return;
	
	dst += (n-1)/BITS_PER_LONG;
	src += (n-1)/BITS_PER_LONG;
	if ((n-1) % BITS_PER_LONG) {
		dst_idx += (n-1) % BITS_PER_LONG;
		dst += dst_idx >> SHIFT_PER_LONG;
		dst_idx &= BITS_PER_LONG-1;
		src_idx += (n-1) % BITS_PER_LONG;
		src += src_idx >> SHIFT_PER_LONG;
		src_idx &= BITS_PER_LONG-1;
	}
	
	shift = dst_idx-src_idx;
	first = ~0UL << (BITS_PER_LONG-1-dst_idx);
	last = ~(~0UL << (BITS_PER_LONG-1-((dst_idx-n) % BITS_PER_LONG)));
	
	if (!shift) {
		// Same alignment for source and dest
		
		if ((unsigned long)dst_idx+1 >= n) {
			// Single word
			if (last)
				first &= last;
			FB_WRITEL((FB_READL(src) & first) | (FB_READL(dst) & ~first), dst); 
		} else {
			// Multiple destination words
			// Leading bits
			if (first) {
				FB_WRITEL((FB_READL(src) & first) | (FB_READL(dst) & ~first), dst); 
				dst--;
				src--;
				n -= dst_idx+1;
			}
			
			// Main chunk
			n /= BITS_PER_LONG;
			while (n >= 8) {
				FB_WRITEL(FB_READL(src--), dst--);
				FB_WRITEL(FB_READL(src--), dst--);
				FB_WRITEL(FB_READL(src--), dst--);
				FB_WRITEL(FB_READL(src--), dst--);
				FB_WRITEL(FB_READL(src--), dst--);
				FB_WRITEL(FB_READL(src--), dst--);
				FB_WRITEL(FB_READL(src--), dst--);
				FB_WRITEL(FB_READL(src--), dst--);
				n -= 8;
			}
			while (n--)
				FB_WRITEL(FB_READL(src--), dst--);
			
			// Trailing bits
			if (last)
				FB_WRITEL((FB_READL(src) & last) | (FB_READL(dst) & ~last), dst);
		}
	} else {
		// Different alignment for source and dest
		
		right = shift & (BITS_PER_LONG-1);
		left = -shift & (BITS_PER_LONG-1);
		
		if ((unsigned long)dst_idx+1 >= n) {
			// Single destination word
			if (last)
				first &= last;
			if (shift < 0) {
				// Single source word
				FB_WRITEL((FB_READL(src) << left & first) | 
					  (FB_READL(dst) & ~first), dst);
			} else if (1+(unsigned long)src_idx >= n) {
				// Single source word
				FB_WRITEL(((FB_READL(src) >> right) & first) | 
					  (FB_READL(dst) & ~first), dst);
			} else {
				// 2 source words
				d0 = FB_READL(src--);
				d1 = FB_READL(src);
				FB_WRITEL(((d0>>right | d1<<left) & first) | 
					  (FB_READL(dst) & ~first), dst);
			}
		} else {
			// Multiple destination words
			d0 = FB_READL(src--);
			// Leading bits
			if (shift < 0) {
				// Single source word
				FB_WRITEL(((d0 << left) & first) | 
					  (FB_READL(dst) & ~first), dst);
				dst--;
				n -= dst_idx+1;
			} else {
				// 2 source words
				d1 = FB_READL(src--);
				FB_WRITEL(((d0>>right | d1<<left) & first) | 
					  (FB_READL(dst) & ~first), dst);
				d0 = d1;
				dst--;
				n -= dst_idx+1;
			}
			
			// Main chunk
			m = n % BITS_PER_LONG;
			n /= BITS_PER_LONG;
			while (n >= 4) {
				d1 = FB_READL(src--);
				FB_WRITEL(d0 >> right | d1 << left, dst--);
				d0 = d1;
				d1 = FB_READL(src--);
				FB_WRITEL(d0 >> right | d1 << left, dst--);
				d0 = d1;
				d1 = FB_READL(src--);
				FB_WRITEL(d0 >> right | d1 << left, dst--);
				d0 = d1;
				d1 = FB_READL(src--);
				FB_WRITEL(d0 >> right | d1 << left, dst--);
				d0 = d1;
				n -= 4;
			}
			while (n--) {
				d1 = FB_READL(src--);
				FB_WRITEL(d0 >> right | d1 << left, dst--);
				d0 = d1;
			}
			
			// Trailing bits
			if (last) {
				if (m <= left) {
					// Single source word
					FB_WRITEL(((d0 >> right) & last) | 
						  (FB_READL(dst) & ~last), 
						  dst);
				} else {
					// 2 source words
					d1 = FB_READL(src);
					FB_WRITEL(((d0>>right | d1<<left) & 
						   last) | (FB_READL(dst) & 
							    ~last), dst);
				}
			}
		}
	}
}

void cfb_copyarea(struct fb_info *p, const struct fb_copyarea *area)
{
	u32 dx = area->dx, dy = area->dy, sx = area->sx, sy = area->sy;
	u32 height = area->height, width = area->width;
	int x2, y2, old_dx, old_dy, vxres, vyres;
	unsigned long next_line = p->fix.line_length;
	int dst_idx = 0, src_idx = 0, rev_copy = 0;
	unsigned long __iomem *dst = NULL, *src = NULL;

	if (p->state != FBINFO_STATE_RUNNING)
		return;

	/* We want rotation but lack hardware to do it for us. */
	if (!p->fbops->fb_rotate && p->var.rotate) {
	}	
	
	vxres = p->var.xres_virtual;
	vyres = p->var.yres_virtual;

	if (area->dx > vxres || area->sx > vxres || 
	    area->dy > vyres || area->sy > vyres)
		return;

	/* clip the destination */
	old_dx = area->dx;
	old_dy = area->dy;

	/*
	 * We could use hardware clipping but on many cards you get around
	 * hardware clipping by writing to framebuffer directly.
	 */
	x2 = area->dx + area->width;
	y2 = area->dy + area->height;
	dx = area->dx > 0 ? area->dx : 0;
	dy = area->dy > 0 ? area->dy : 0;
	x2 = x2 < vxres ? x2 : vxres;
	y2 = y2 < vyres ? y2 : vyres;
	width = x2 - dx;
	height = y2 - dy;

	/* update sx1,sy1 */
	sx += (dx - old_dx);
	sy += (dy - old_dy);

	/* the source must be completely inside the virtual screen */
	if (sx < 0 || sy < 0 ||
	    (sx + width) > vxres ||
	    (sy + height) > vyres)
		return;

	if ((dy == sy && dx > sx) ||	
	    (dy > sy)) { 
		dy += height;
		sy += height;
		rev_copy = 1;
	}

	dst = src = (unsigned long __iomem *)((unsigned long)p->screen_base & 
				      ~(BYTES_PER_LONG-1));
	dst_idx = src_idx = (unsigned long)p->screen_base & (BYTES_PER_LONG-1);
	dst_idx += dy*next_line*8 + dx*p->var.bits_per_pixel;
	src_idx += sy*next_line*8 + sx*p->var.bits_per_pixel;
	
	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);
	if (rev_copy) {
		while (height--) {
			dst_idx -= next_line*8;
			src_idx -= next_line*8;
			dst += dst_idx >> SHIFT_PER_LONG;
			dst_idx &= (BYTES_PER_LONG-1);
			src += src_idx >> SHIFT_PER_LONG;
			src_idx &= (BYTES_PER_LONG-1);
			bitcpy_rev(dst, dst_idx, src, src_idx, 
				   width*p->var.bits_per_pixel);
		}	
	} else {
		while (height--) {
			dst += dst_idx >> SHIFT_PER_LONG;
			dst_idx &= (BYTES_PER_LONG-1);
			src += src_idx >> SHIFT_PER_LONG;
			src_idx &= (BYTES_PER_LONG-1);
			bitcpy(dst, dst_idx, src, src_idx, 
			       width*p->var.bits_per_pixel);
			dst_idx += next_line*8;
			src_idx += next_line*8;
		}	
	}
}

EXPORT_SYMBOL(cfb_copyarea);

MODULE_AUTHOR("James Simmons <jsimmons@users.sf.net>");
MODULE_DESCRIPTION("Generic software accelerated copyarea");
MODULE_LICENSE("GPL");

