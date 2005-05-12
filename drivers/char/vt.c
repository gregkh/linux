/*
 *  linux/drivers/char/vt.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * Hopefully this will be a rather complete VT102 implementation.
 *
 * Beeping thanks to John T Kohl.
 *
 * Virtual Consoles, Screen Blanking, Screen Dumping, Color, Graphics
 *   Chars, and VT100 enhancements by Peter MacDonald.
 *
 * Copy and paste function by Andrew Haylett,
 *   some enhancements by Alessandro Rubini.
 *
 * Code to check for different video-cards mostly by Galen Hunt,
 * <g-hunt@ee.utah.edu>
 *
 * Rudimentary ISO 10646/Unicode/UTF-8 character set support by
 * Markus Kuhn, <mskuhn@immd4.informatik.uni-erlangen.de>.
 *
 * Dynamic allocation of consoles, aeb@cwi.nl, May 1994
 * Resizing of consoles, aeb, 940926
 *
 * Code for xterm like mouse click reporting by Peter Orbaek 20-Jul-94
 * <poe@daimi.aau.dk>
 *
 * User-defined bell sound, new setterm control sequences and printk
 * redirection by Martin Mares <mj@k332.feld.cvut.cz> 19-Nov-95
 *
 * APM screenblank bug fixed Takashi Manabe <manabe@roy.dsl.tutics.tut.jp>
 *
 * Merge with the abstract console driver by Geert Uytterhoeven
 * <geert@linux-m68k.org>, Jan 1997.
 *
 *   Original m68k console driver modifications by
 *
 *     - Arno Griffioen <arno@usn.nl>
 *     - David Carter <carter@cs.bris.ac.uk>
 * 
 *   The abstract console driver provides a generic interface for a text
 *   console. It supports VGA text mode, frame buffer based graphical consoles
 *   and special graphics processors that are only accessible through some
 *   registers (e.g. a TMS340x0 GSP).
 *
 *   The interface to the hardware is specified using a special structure
 *   (struct consw) which contains function pointers to console operations
 *   (see <linux/console.h> for more information).
 *
 * Support for changeable cursor shape
 * by Pavel Machek <pavel@atrey.karlin.mff.cuni.cz>, August 1997
 *
 * Ported to i386 and con_scrolldelta fixed
 * by Emmanuel Marty <core@ggi-project.org>, April 1998
 *
 * Resurrected character buffers in videoram plus lots of other trickery
 * by Martin Mares <mj@atrey.karlin.mff.cuni.cz>, July 1998
 *
 * Removed old-style timers, introduced console_timer, made timer
 * deletion SMP-safe.  17Jun00, Andrew Morton <andrewm@uow.edu.au>
 *
 * Removed console_lock, enabled interrupts across all console operations
 * 13 March 2001, Andrew Morton
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kd.h>
#include <linux/slab.h>
#include <linux/major.h>
#include <linux/mm.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/vt_kern.h>
#include <linux/selection.h>
#include <linux/tiocl.h>
#include <linux/kbd_kern.h>
#include <linux/consolemap.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/config.h>
#include <linux/workqueue.h>
#include <linux/bootmem.h>
#include <linux/pm.h>
#include <linux/font.h>
#include <linux/bitops.h>

#include <asm/io.h>
#include <asm/system.h>
#include <asm/uaccess.h>

#include "console_macros.h"


const struct consw *conswitchp;

/* A bitmap for codes <32. A bit of 1 indicates that the code
 * corresponding to that bit number invokes some special action
 * (such as cursor movement) and should not be displayed as a
 * glyph unless the disp_ctrl mode is explicitly enabled.
 */
#define CTRL_ACTION 0x0d00ff81
#define CTRL_ALWAYS 0x0800f501	/* Cannot be overridden by disp_ctrl */

/*
 * Here is the default bell parameters: 750HZ, 1/8th of a second
 */
#define DEFAULT_BELL_PITCH	750
#define DEFAULT_BELL_DURATION	(HZ/8)

extern void vcs_make_devfs(struct tty_struct *tty);
extern void vcs_remove_devfs(struct tty_struct *tty);

extern void console_map_init(void);
#ifdef CONFIG_PROM_CONSOLE
extern void prom_con_init(void);
#endif
#ifdef CONFIG_MDA_CONSOLE
extern int mda_console_init(void);
#endif

struct vc vc_cons [MAX_NR_CONSOLES];

#ifndef VT_SINGLE_DRIVER
static const struct consw *con_driver_map[MAX_NR_CONSOLES];
#endif

static int con_open(struct tty_struct *, struct file *);
static void vc_init(unsigned int console, unsigned int rows,
		    unsigned int cols, int do_clear);
static void gotoxy(struct vc_data *vc, int new_x, int new_y);
static void save_cur(int currcons);
static void reset_terminal(int currcons, int do_clear);
static void con_flush_chars(struct tty_struct *tty);
static void set_vesa_blanking(char __user *p);
static void set_cursor(struct vc_data *vc);
static void hide_cursor(struct vc_data *vc);
static void console_callback(void *ignored);
static void blank_screen_t(unsigned long dummy);

static int printable;		/* Is console ready for printing? */

/*
 * ignore_poke: don't unblank the screen when things are typed.  This is
 * mainly for the privacy of braille terminal users.
 */
static int ignore_poke;

int do_poke_blanked_console;
int console_blanked;

static int vesa_blank_mode; /* 0:none 1:suspendV 2:suspendH 3:powerdown */
static int blankinterval = 10*60*HZ;
static int vesa_off_interval;

static DECLARE_WORK(console_work, console_callback, NULL);

/*
 * fg_console is the current virtual console,
 * last_console is the last used one,
 * want_console is the console we want to switch to,
 * kmsg_redirect is the console for kernel messages,
 */
int fg_console;
int last_console;
int want_console = -1;
int kmsg_redirect;

/*
 * For each existing display, we have a pointer to console currently visible
 * on that display, allowing consoles other than fg_console to be refreshed
 * appropriately. Unless the low-level driver supplies its own display_fg
 * variable, we use this one for the "master display".
 */
static struct vc_data *master_display_fg;

/*
 * Unfortunately, we need to delay tty echo when we're currently writing to the
 * console since the code is (and always was) not re-entrant, so we schedule
 * all flip requests to process context with schedule-task() and run it from
 * console_callback().
 */

/*
 * For the same reason, we defer scrollback to the console callback.
 */
static int scrollback_delta;

/*
 * Hook so that the power management routines can (un)blank
 * the console on our behalf.
 */
int (*console_blank_hook)(int);

static struct timer_list console_timer;
static int blank_state;
static int blank_timer_expired;
enum {
	blank_off = 0,
	blank_normal_wait,
	blank_vesa_wait,
};

/*
 *	Low-Level Functions
 */

#define IS_FG			(currcons == fg_console)
#define IS_FG_VC(vc)		(vc == vc_cons[fg_console].d)

#define IS_VISIBLE CON_IS_VISIBLE(vc_cons[currcons].d)

#ifdef VT_BUF_VRAM_ONLY
#define DO_UPDATE		0
#define DO_UPDATE_VC(vc)	0
#else
#define DO_UPDATE 		IS_VISIBLE
#define DO_UPDATE_VC(vc)	CON_IS_VISIBLE(vc)
#endif

static int pm_con_request(struct pm_dev *dev, pm_request_t rqst, void *data);
static struct pm_dev *pm_con;

static inline unsigned short *screenpos(struct vc_data *vc, int offset, int viewed)
{
	unsigned short *p;
	
	if (!viewed)
		p = (unsigned short *)(vc->vc_origin + offset);
	else if (!vc->vc_sw->con_screen_pos)
		p = (unsigned short *)(vc->vc_visible_origin + offset);
	else
		p = vc->vc_sw->con_screen_pos(vc, offset);
	return p;
}

static inline void scrolldelta(int lines)
{
	scrollback_delta += lines;
	schedule_console_callback();
}

void schedule_console_callback(void)
{
	schedule_work(&console_work);
}

static void scrup(int currcons, unsigned int t, unsigned int b, int nr)
{
	unsigned short *d, *s;

	if (t+nr >= b)
		nr = b - t - 1;
	if (b > vc_cons[currcons].d->vc_rows || t >= b || nr < 1)
		return;
	if (IS_VISIBLE && sw->con_scroll(vc_cons[currcons].d, t, b, SM_UP, nr))
		return;
	d = (unsigned short *) (origin+vc_cons[currcons].d->vc_size_row*t);
	s = (unsigned short *) (origin+vc_cons[currcons].d->vc_size_row*(t+nr));
	scr_memmovew(d, s, (b-t-nr) * vc_cons[currcons].d->vc_size_row);
	scr_memsetw(d + (b-t-nr) * vc_cons[currcons].d->vc_cols, video_erase_char,
			vc_cons[currcons].d->vc_size_row * nr);
}

static void
scrdown(int currcons, unsigned int t, unsigned int b, int nr)
{
	unsigned short *s;
	unsigned int step;

	if (t+nr >= b)
		nr = b - t - 1;
	if (b > vc_cons[currcons].d->vc_rows || t >= b || nr < 1)
		return;
	if (IS_VISIBLE && sw->con_scroll(vc_cons[currcons].d, t, b, SM_DOWN, nr))
		return;
	s = (unsigned short *) (origin+vc_cons[currcons].d->vc_size_row*t);
	step = vc_cons[currcons].d->vc_cols * nr;
	scr_memmovew(s + step, s, (b-t-nr)*vc_cons[currcons].d->vc_size_row);
	scr_memsetw(s, video_erase_char, 2*step);
}

static void do_update_region(struct vc_data *vc, unsigned long start, int count)
{
#ifndef VT_BUF_VRAM_ONLY
	unsigned int xx, yy, offset;
	u16 *p;

	p = (u16 *) start;
	if (!vc->vc_sw->con_getxy) {
		offset = (start - vc->vc_origin) / 2;
		xx = offset % vc->vc_cols;
		yy = offset / vc->vc_cols;
	} else {
		int nxx, nyy;
		start = vc->vc_sw->con_getxy(vc, start, &nxx, &nyy);
		xx = nxx; yy = nyy;
	}
	for(;;) {
		u16 attrib = scr_readw(p) & 0xff00;
		int startx = xx;
		u16 *q = p;
		while (xx < vc->vc_cols && count) {
			if (attrib != (scr_readw(p) & 0xff00)) {
				if (p > q)
					vc->vc_sw->con_putcs(vc, q, p-q, yy, startx);
				startx = xx;
				q = p;
				attrib = scr_readw(p) & 0xff00;
			}
			p++;
			xx++;
			count--;
		}
		if (p > q)
			vc->vc_sw->con_putcs(vc, q, p-q, yy, startx);
		if (!count)
			break;
		xx = 0;
		yy++;
		if (vc->vc_sw->con_getxy) {
			p = (u16 *)start;
			start = vc->vc_sw->con_getxy(vc, start, NULL, NULL);
		}
	}
#endif
}

void update_region(int currcons, unsigned long start, int count)
{
	WARN_CONSOLE_UNLOCKED();

	if (DO_UPDATE) {
		hide_cursor(vc_cons[currcons].d);
		do_update_region(vc_cons[currcons].d, start, count);
		set_cursor(vc_cons[currcons].d);
	}
}

/* Structure of attributes is hardware-dependent */

static u8 build_attr(int currcons, u8 _color, u8 _intensity, u8 _blink, u8 _underline, u8 _reverse)
{
	if (sw->con_build_attr)
		return sw->con_build_attr(vc_cons[currcons].d, _color, _intensity, _blink, _underline, _reverse);

#ifndef VT_BUF_VRAM_ONLY
/*
 * ++roman: I completely changed the attribute format for monochrome
 * mode (!can_do_color). The formerly used MDA (monochrome display
 * adapter) format didn't allow the combination of certain effects.
 * Now the attribute is just a bit vector:
 *  Bit 0..1: intensity (0..2)
 *  Bit 2   : underline
 *  Bit 3   : reverse
 *  Bit 7   : blink
 */
	{
	u8 a = color;
	if (!vc_cons[currcons].d->vc_can_do_color)
		return _intensity |
		       (_underline ? 4 : 0) |
		       (_reverse ? 8 : 0) |
		       (_blink ? 0x80 : 0);
	if (_underline)
		a = (a & 0xf0) | ulcolor;
	else if (_intensity == 0)
		a = (a & 0xf0) | halfcolor;
	if (_reverse)
		a = ((a) & 0x88) | ((((a) >> 4) | ((a) << 4)) & 0x77);
	if (_blink)
		a ^= 0x80;
	if (_intensity == 2)
		a ^= 0x08;
	if (hi_font_mask == 0x100)
		a <<= 1;
	return a;
	}
#else
	return 0;
#endif
}

static void update_attr(int currcons)
{
	attr = build_attr(currcons, color, intensity, blink, underline, reverse ^ decscnm);
	video_erase_char = (build_attr(currcons, color, 1, blink, 0, decscnm) << 8) | ' ';
}

/* Note: inverting the screen twice should revert to the original state */
void invert_screen(struct vc_data *vc, int offset, int count, int viewed)
{
	unsigned short *p;

	WARN_CONSOLE_UNLOCKED();

	count /= 2;
	p = screenpos(vc, offset, viewed);
	if (vc->vc_sw->con_invert_region)
		vc->vc_sw->con_invert_region(vc, p, count);
#ifndef VT_BUF_VRAM_ONLY
	else {
		u16 *q = p;
		int cnt = count;
		u16 a;

		if (!vc->vc_can_do_color) {
			while (cnt--) {
			    a = scr_readw(q);
			    a ^= 0x0800;
			    scr_writew(a, q);
			    q++;
			}
		} else if (vc->vc_hi_font_mask == 0x100) {
			while (cnt--) {
				a = scr_readw(q);
				a = ((a) & 0x11ff) | (((a) & 0xe000) >> 4) | (((a) & 0x0e00) << 4);
				scr_writew(a, q);
				q++;
			}
		} else {
			while (cnt--) {
				a = scr_readw(q);
				a = ((a) & 0x88ff) | (((a) & 0x7000) >> 4) | (((a) & 0x0700) << 4);
				scr_writew(a, q);
				q++;
			}
		}
	}
#endif
	if (DO_UPDATE_VC(vc))
		do_update_region(vc, (unsigned long) p, count);
}

/* used by selection: complement pointer position */
void complement_pos(struct vc_data *vc, int offset)
{
	static unsigned short *p;
	static unsigned short old;
	static unsigned short oldx, oldy;

	WARN_CONSOLE_UNLOCKED();

	if (p) {
		scr_writew(old, p);
		if (DO_UPDATE_VC(vc))
			vc->vc_sw->con_putc(vc, old, oldy, oldx);
	}
	if (offset == -1)
		p = NULL;
	else {
		unsigned short new;
		p = screenpos(vc, offset, 1);
		old = scr_readw(p);
		new = old ^ vc->vc_complement_mask;
		scr_writew(new, p);
		if (DO_UPDATE_VC(vc)) {
			oldx = (offset >> 1) % vc->vc_cols;
			oldy = (offset >> 1) / vc->vc_cols;
			vc->vc_sw->con_putc(vc, new, oldy, oldx);
		}
	}
}

static void insert_char(int currcons, unsigned int nr)
{
	unsigned short *p, *q = (unsigned short *) pos;

	p = q + vc_cons[currcons].d->vc_cols - nr - x;
	while (--p >= q)
		scr_writew(scr_readw(p), p + nr);
	scr_memsetw(q, video_erase_char, nr*2);
	need_wrap = 0;
	if (DO_UPDATE) {
		unsigned short oldattr = attr;
		sw->con_bmove(vc_cons[currcons].d,y,x,y,x+nr,1,
			      vc_cons[currcons].d->vc_cols-x-nr);
		attr = video_erase_char >> 8;
		while (nr--)
			sw->con_putc(vc_cons[currcons].d,
				     video_erase_char,y,x+nr);
		attr = oldattr;
	}
}

static void delete_char(int currcons, unsigned int nr)
{
	unsigned int i = x;
	unsigned short *p = (unsigned short *) pos;

	while (++i <= vc_cons[currcons].d->vc_cols - nr) {
		scr_writew(scr_readw(p+nr), p);
		p++;
	}
	scr_memsetw(p, video_erase_char, nr*2);
	need_wrap = 0;
	if (DO_UPDATE) {
		unsigned short oldattr = attr;
		sw->con_bmove(vc_cons[currcons].d, y, x+nr, y, x, 1,
			      vc_cons[currcons].d->vc_cols-x-nr);
		attr = video_erase_char >> 8;
		while (nr--)
			sw->con_putc(vc_cons[currcons].d,
				     video_erase_char, y,
				     vc_cons[currcons].d->vc_cols-1-nr);
		attr = oldattr;
	}
}

static int softcursor_original;

static void add_softcursor(struct vc_data *vc)
{
	int i = scr_readw((u16 *) vc->vc_pos);
	u32 type = vc->vc_cursor_type;

	if (! (type & 0x10)) return;
	if (softcursor_original != -1) return;
	softcursor_original = i;
	i |= ((type >> 8) & 0xff00 );
	i ^= ((type) & 0xff00 );
	if ((type & 0x20) && ((softcursor_original & 0x7000) == (i & 0x7000))) i ^= 0x7000;
	if ((type & 0x40) && ((i & 0x700) == ((i & 0x7000) >> 4))) i ^= 0x0700;
	scr_writew(i, (u16 *) vc->vc_pos);
	if (DO_UPDATE_VC(vc))
		vc->vc_sw->con_putc(vc, i, vc->vc_y, vc->vc_x);
}

static void hide_softcursor(struct vc_data *vc)
{
	if (softcursor_original != -1) {
		scr_writew(softcursor_original, (u16 *)vc->vc_pos);
		if (DO_UPDATE_VC(vc))
			vc->vc_sw->con_putc(vc, softcursor_original,
					vc->vc_y, vc->vc_x);
		softcursor_original = -1;
	}
}

static void hide_cursor(struct vc_data *vc)
{
	if (vc == sel_cons)
		clear_selection();
	vc->vc_sw->con_cursor(vc, CM_ERASE);
	hide_softcursor(vc);
}

static void set_cursor(struct vc_data *vc)
{
	if (!IS_FG_VC(vc) || console_blanked ||
	    vc->vc_vt->vc_mode == KD_GRAPHICS)
		return;
	if (vc->vc_deccm) {
		if (vc == sel_cons)
			clear_selection();
		add_softcursor(vc);
		if ((vc->vc_cursor_type & 0x0f) != 1)
			vc->vc_sw->con_cursor(vc, CM_DRAW);
	} else
		hide_cursor(vc);
}

static void set_origin(int currcons)
{
	WARN_CONSOLE_UNLOCKED();

	if (!IS_VISIBLE ||
	    !sw->con_set_origin ||
	    !sw->con_set_origin(vc_cons[currcons].d))
		origin = (unsigned long) screenbuf;
	visible_origin = origin;
	scr_end = origin + screenbuf_size;
	pos = origin + vc_cons[currcons].d->vc_size_row*y + 2*x;
}

static inline void save_screen(int currcons)
{
	WARN_CONSOLE_UNLOCKED();

	if (sw->con_save_screen)
		sw->con_save_screen(vc_cons[currcons].d);
}

/*
 *	Redrawing of screen
 */

static void clear_buffer_attributes(int currcons)
{
	unsigned short *p = (unsigned short *) origin;
	int count = screenbuf_size/2;
	int mask = hi_font_mask | 0xff;

	for (; count > 0; count--, p++) {
		scr_writew((scr_readw(p)&mask) | (video_erase_char&~mask), p);
	}
}

void redraw_screen(int new_console, int is_switch)
{
	int redraw = 1;
	int currcons, old_console;

	WARN_CONSOLE_UNLOCKED();

	if (!vc_cons_allocated(new_console)) {
		/* strange ... */
		/* printk("redraw_screen: tty %d not allocated ??\n", new_console+1); */
		return;
	}

	if (is_switch) {
		currcons = fg_console;
		hide_cursor(vc_cons[currcons].d);
		if (fg_console != new_console) {
			struct vc_data **display = vc_cons[new_console].d->vc_display_fg;
			old_console = (*display) ? (*display)->vc_num : fg_console;
			*display = vc_cons[new_console].d;
			fg_console = new_console;
			currcons = old_console;
			if (!IS_VISIBLE) {
				save_screen(currcons);
				set_origin(currcons);
			}
			currcons = new_console;
			if (old_console == new_console)
				redraw = 0;
		}
	} else {
		currcons = new_console;
		hide_cursor(vc_cons[currcons].d);
	}

	if (redraw) {
		int update;
		int old_was_color = vc_cons[currcons].d->vc_can_do_color;

		set_origin(currcons);
		update = sw->con_switch(vc_cons[currcons].d);
		set_palette(currcons);
		/*
		 * If console changed from mono<->color, the best we can do
		 * is to clear the buffer attributes. As it currently stands,
		 * rebuilding new attributes from the old buffer is not doable
		 * without overly complex code.
		 */
		if (old_was_color != vc_cons[currcons].d->vc_can_do_color) {
			update_attr(currcons);
			clear_buffer_attributes(currcons);
		}
		if (update && vcmode != KD_GRAPHICS)
			do_update_region(vc_cons[currcons].d, origin, screenbuf_size/2);
	}
	set_cursor(vc_cons[currcons].d);
	if (is_switch) {
		set_leds();
		compute_shiftstate();
	}
}

/*
 *	Allocation, freeing and resizing of VTs.
 */

int vc_cons_allocated(unsigned int i)
{
	return (i < MAX_NR_CONSOLES && vc_cons[i].d);
}

static void visual_init(int currcons, int init)
{
    /* ++Geert: sw->con_init determines console size */
    if (sw)
	module_put(sw->owner);
    sw = conswitchp;
#ifndef VT_SINGLE_DRIVER
    if (con_driver_map[currcons])
	sw = con_driver_map[currcons];
#endif
    __module_get(sw->owner);
    cons_num = currcons;
    display_fg = &master_display_fg;
    vc_cons[currcons].d->vc_uni_pagedir_loc = &vc_cons[currcons].d->vc_uni_pagedir;
    vc_cons[currcons].d->vc_uni_pagedir = 0;
    hi_font_mask = 0;
    complement_mask = 0;
    vc_cons[currcons].d->vc_can_do_color = 0;
    sw->con_init(vc_cons[currcons].d, init);
    if (!complement_mask)
        complement_mask =
		vc_cons[currcons].d->vc_can_do_color ? 0x7700 : 0x0800;
    s_complement_mask = complement_mask;
    vc_cons[currcons].d->vc_size_row = vc_cons[currcons].d->vc_cols<<1;
    screenbuf_size = vc_cons[currcons].d->vc_rows * vc_cons[currcons].d->vc_size_row;
}

int vc_allocate(unsigned int currcons)	/* return 0 on success */
{
	WARN_CONSOLE_UNLOCKED();

	if (currcons >= MAX_NR_CONSOLES)
		return -ENXIO;
	if (!vc_cons[currcons].d) {
	    long p, q;

	    /* prevent users from taking too much memory */
	    if (currcons >= MAX_NR_USER_CONSOLES && !capable(CAP_SYS_RESOURCE))
	      return -EPERM;

	    /* due to the granularity of kmalloc, we waste some memory here */
	    /* the alloc is done in two steps, to optimize the common situation
	       of a 25x80 console (structsize=216, screenbuf_size=4000) */
	    /* although the numbers above are not valid since long ago, the
	       point is still up-to-date and the comment still has its value
	       even if only as a historical artifact.  --mj, July 1998 */
	    p = (long) kmalloc(structsize, GFP_KERNEL);
	    if (!p)
		return -ENOMEM;
	    memset((void *)p, 0, structsize);
	    vc_cons[currcons].d = (struct vc_data *)p;
	    vt_cons[currcons] = (struct vt_struct *)(p+sizeof(struct vc_data));
	    vc_cons[currcons].d->vc_vt = vt_cons[currcons];
	    visual_init(currcons, 1);
	    if (!*vc_cons[currcons].d->vc_uni_pagedir_loc)
		con_set_default_unimap(currcons);
	    q = (long)kmalloc(screenbuf_size, GFP_KERNEL);
	    if (!q) {
		kfree((char *) p);
		vc_cons[currcons].d = NULL;
		vt_cons[currcons] = NULL;
		return -ENOMEM;
	    }
	    screenbuf = (unsigned short *) q;
	    kmalloced = 1;
	    vc_init(currcons, vc_cons[currcons].d->vc_rows, vc_cons[currcons].d->vc_cols, 1);

	    if (!pm_con) {
		    pm_con = pm_register(PM_SYS_DEV,
					 PM_SYS_VGA,
					 pm_con_request);
	    }
	}
	return 0;
}

inline int resize_screen(int currcons, int width, int height)
{
	/* Resizes the resolution of the display adapater */
	int err = 0;

	if (vcmode != KD_GRAPHICS && sw->con_resize)
		err = sw->con_resize(vc_cons[currcons].d, width, height);
	return err;
}

/*
 * Change # of rows and columns (0 means unchanged/the size of fg_console)
 * [this is to be used together with some user program
 * like resize that changes the hardware videomode]
 */
#define VC_RESIZE_MAXCOL (32767)
#define VC_RESIZE_MAXROW (32767)
int vc_resize(int currcons, unsigned int cols, unsigned int lines)
{
	unsigned long old_origin, new_origin, new_scr_end, rlth, rrem, err = 0;
	unsigned int old_cols, old_rows, old_row_size, old_screen_size;
	unsigned int new_cols, new_rows, new_row_size, new_screen_size;
	unsigned short *newscreen;

	WARN_CONSOLE_UNLOCKED();

	if (!vc_cons_allocated(currcons))
		return -ENXIO;

	if (cols > VC_RESIZE_MAXCOL || lines > VC_RESIZE_MAXROW)
		return -EINVAL;

	new_cols = (cols ? cols : vc_cons[currcons].d->vc_cols);
	new_rows = (lines ? lines : vc_cons[currcons].d->vc_rows);
	new_row_size = new_cols << 1;
	new_screen_size = new_row_size * new_rows;

	if (new_cols == vc_cons[currcons].d->vc_cols && new_rows == vc_cons[currcons].d->vc_rows)
		return 0;

	newscreen = (unsigned short *) kmalloc(new_screen_size, GFP_USER);
	if (!newscreen)
		return -ENOMEM;

	old_rows = vc_cons[currcons].d->vc_rows;
	old_cols = vc_cons[currcons].d->vc_cols;
	old_row_size = vc_cons[currcons].d->vc_size_row;
	old_screen_size = screenbuf_size;

	err = resize_screen(currcons, new_cols, new_rows);
	if (err) {
		kfree(newscreen);
		return err;
	}

	vc_cons[currcons].d->vc_rows = new_rows;
	vc_cons[currcons].d->vc_cols = new_cols;
	vc_cons[currcons].d->vc_size_row = new_row_size;
	screenbuf_size = new_screen_size;

	rlth = min(old_row_size, new_row_size);
	rrem = new_row_size - rlth;
	old_origin = origin;
	new_origin = (long) newscreen;
	new_scr_end = new_origin + new_screen_size;
	if (new_rows < old_rows)
		old_origin += (old_rows - new_rows) * old_row_size;

	update_attr(currcons);

	while (old_origin < scr_end) {
		scr_memcpyw((unsigned short *) new_origin, (unsigned short *) old_origin, rlth);
		if (rrem)
			scr_memsetw((void *)(new_origin + rlth), video_erase_char, rrem);
		old_origin += old_row_size;
		new_origin += new_row_size;
	}
	if (new_scr_end > new_origin)
		scr_memsetw((void *) new_origin, video_erase_char, new_scr_end - new_origin);
	if (kmalloced)
		kfree(screenbuf);
	screenbuf = newscreen;
	kmalloced = 1;
	screenbuf_size = new_screen_size;
	set_origin(currcons);

	/* do part of a reset_terminal() */
	top = 0;
	bottom = vc_cons[currcons].d->vc_rows;
	gotoxy(vc_cons[currcons].d, x, y);
	save_cur(currcons);

	if (vc_cons[currcons].d->vc_tty) {
		struct winsize ws, *cws = &vc_cons[currcons].d->vc_tty->winsize;

		memset(&ws, 0, sizeof(ws));
		ws.ws_row = vc_cons[currcons].d->vc_rows;
		ws.ws_col = vc_cons[currcons].d->vc_cols;
		ws.ws_ypixel = video_scan_lines;
		if ((ws.ws_row != cws->ws_row || ws.ws_col != cws->ws_col) &&
		    vc_cons[currcons].d->vc_tty->pgrp > 0)
			kill_pg(vc_cons[currcons].d->vc_tty->pgrp, SIGWINCH, 1);
		*cws = ws;
	}

	if (IS_VISIBLE)
		update_screen(currcons);
	return err;
}


void vc_disallocate(unsigned int currcons)
{
	WARN_CONSOLE_UNLOCKED();

	if (vc_cons_allocated(currcons)) {
	    sw->con_deinit(vc_cons[currcons].d);
	    if (kmalloced)
		kfree(screenbuf);
	    if (currcons >= MIN_NR_CONSOLES)
		kfree(vc_cons[currcons].d);
	    vc_cons[currcons].d = NULL;
	}
}

/*
 *	VT102 emulator
 */

#define set_kbd(x) set_vc_kbd_mode(kbd_table+currcons,x)
#define clr_kbd(x) clr_vc_kbd_mode(kbd_table+currcons,x)
#define is_kbd(x) vc_kbd_mode(kbd_table+currcons,x)

#define decarm		VC_REPEAT
#define decckm		VC_CKMODE
#define kbdapplic	VC_APPLIC
#define lnm		VC_CRLF

/*
 * this is what the terminal answers to a ESC-Z or csi0c query.
 */
#define VT100ID "\033[?1;2c"
#define VT102ID "\033[?6c"

unsigned char color_table[] = { 0, 4, 2, 6, 1, 5, 3, 7,
				       8,12,10,14, 9,13,11,15 };

/* the default colour table, for VGA+ colour systems */
int default_red[] = {0x00,0xaa,0x00,0xaa,0x00,0xaa,0x00,0xaa,
    0x55,0xff,0x55,0xff,0x55,0xff,0x55,0xff};
int default_grn[] = {0x00,0x00,0xaa,0x55,0x00,0x00,0xaa,0xaa,
    0x55,0x55,0xff,0xff,0x55,0x55,0xff,0xff};
int default_blu[] = {0x00,0x00,0x00,0x00,0xaa,0xaa,0xaa,0xaa,
    0x55,0x55,0x55,0x55,0xff,0xff,0xff,0xff};

/*
 * gotoxy() must verify all boundaries, because the arguments
 * might also be negative. If the given position is out of
 * bounds, the cursor is placed at the nearest margin.
 */
static void gotoxy(struct vc_data *vc, int new_x, int new_y)
{
	int min_y, max_y;

	if (new_x < 0)
		vc->vc_x = 0;
	else {
		if (new_x >= vc->vc_cols)
			vc->vc_x = vc->vc_cols - 1;
		else
			vc->vc_x = new_x;
	}

 	if (vc->vc_decom) {
		min_y = vc->vc_top;
		max_y = vc->vc_bottom;
	} else {
		min_y = 0;
		max_y = vc->vc_rows;
	}
	if (new_y < min_y)
		vc->vc_y = min_y;
	else if (new_y >= max_y)
		vc->vc_y = max_y - 1;
	else
		vc->vc_y = new_y;
	vc->vc_pos = vc->vc_origin + vc->vc_y * vc->vc_size_row + (vc->vc_x<<1);
	vc->vc_need_wrap = 0;
}

/* for absolute user moves, when decom is set */
static void gotoxay(int currcons, int new_x, int new_y)
{
	gotoxy(vc_cons[currcons].d, new_x, decom ? (top+new_y) : new_y);
}

void scrollback(int lines)
{
	int currcons = fg_console;

	if (!lines)
		lines = vc_cons[currcons].d->vc_rows/2;
	scrolldelta(-lines);
}

void scrollfront(int lines)
{
	int currcons = fg_console;

	if (!lines)
		lines = vc_cons[currcons].d->vc_rows/2;
	scrolldelta(lines);
}

static void lf(int currcons)
{
    	/* don't scroll if above bottom of scrolling region, or
	 * if below scrolling region
	 */
    	if (y+1 == bottom)
		scrup(currcons,top,bottom,1);
	else if (y < vc_cons[currcons].d->vc_rows-1) {
	    	y++;
		pos += vc_cons[currcons].d->vc_size_row;
	}
	need_wrap = 0;
}

static void ri(int currcons)
{
    	/* don't scroll if below top of scrolling region, or
	 * if above scrolling region
	 */
	if (y == top)
		scrdown(currcons,top,bottom,1);
	else if (y > 0) {
		y--;
		pos -= vc_cons[currcons].d->vc_size_row;
	}
	need_wrap = 0;
}

static inline void cr(int currcons)
{
	pos -= x<<1;
	need_wrap = x = 0;
}

static inline void bs(int currcons)
{
	if (x) {
		pos -= 2;
		x--;
		need_wrap = 0;
	}
}

static inline void del(int currcons)
{
	/* ignored */
}

static void csi_J(int currcons, int vpar)
{
	unsigned int count;
	unsigned short * start;

	switch (vpar) {
		case 0:	/* erase from cursor to end of display */
			count = (scr_end-pos)>>1;
			start = (unsigned short *) pos;
			if (DO_UPDATE) {
				/* do in two stages */
				sw->con_clear(vc_cons[currcons].d, y, x, 1,
					      vc_cons[currcons].d->vc_cols-x);
				sw->con_clear(vc_cons[currcons].d, y+1, 0,
					      vc_cons[currcons].d->vc_rows-y-1,
					      vc_cons[currcons].d->vc_cols);
			}
			break;
		case 1:	/* erase from start to cursor */
			count = ((pos-origin)>>1)+1;
			start = (unsigned short *) origin;
			if (DO_UPDATE) {
				/* do in two stages */
				sw->con_clear(vc_cons[currcons].d, 0, 0, y,
					      vc_cons[currcons].d->vc_cols);
				sw->con_clear(vc_cons[currcons].d, y, 0, 1,
					      x + 1);
			}
			break;
		case 2: /* erase whole display */
			count = vc_cons[currcons].d->vc_cols * vc_cons[currcons].d->vc_rows;
			start = (unsigned short *) origin;
			if (DO_UPDATE)
				sw->con_clear(vc_cons[currcons].d, 0, 0,
					      vc_cons[currcons].d->vc_rows,
					      vc_cons[currcons].d->vc_cols);
			break;
		default:
			return;
	}
	scr_memsetw(start, video_erase_char, 2*count);
	need_wrap = 0;
}

static void csi_K(int currcons, int vpar)
{
	unsigned int count;
	unsigned short * start;

	switch (vpar) {
		case 0:	/* erase from cursor to end of line */
			count = vc_cons[currcons].d->vc_cols-x;
			start = (unsigned short *) pos;
			if (DO_UPDATE)
				sw->con_clear(vc_cons[currcons].d, y, x, 1,
					      vc_cons[currcons].d->vc_cols-x);
			break;
		case 1:	/* erase from start of line to cursor */
			start = (unsigned short *) (pos - (x<<1));
			count = x+1;
			if (DO_UPDATE)
				sw->con_clear(vc_cons[currcons].d, y, 0, 1,
					      x + 1);
			break;
		case 2: /* erase whole line */
			start = (unsigned short *) (pos - (x<<1));
			count = vc_cons[currcons].d->vc_cols;
			if (DO_UPDATE)
				sw->con_clear(vc_cons[currcons].d, y, 0, 1,
					      vc_cons[currcons].d->vc_cols);
			break;
		default:
			return;
	}
	scr_memsetw(start, video_erase_char, 2 * count);
	need_wrap = 0;
}

static void csi_X(int currcons, int vpar) /* erase the following vpar positions */
{					  /* not vt100? */
	int count;

	if (!vpar)
		vpar++;
	count = (vpar > vc_cons[currcons].d->vc_cols-x) ? (vc_cons[currcons].d->vc_cols-x) : vpar;

	scr_memsetw((unsigned short *) pos, video_erase_char, 2 * count);
	if (DO_UPDATE)
		sw->con_clear(vc_cons[currcons].d, y, x, 1, count);
	need_wrap = 0;
}

static void default_attr(int currcons)
{
	intensity = 1;
	underline = 0;
	reverse = 0;
	blink = 0;
	color = def_color;
}

/* console_sem is held */
static void csi_m(int currcons)
{
	int i;

	for (i=0;i<=npar;i++)
		switch (par[i]) {
			case 0:	/* all attributes off */
				default_attr(currcons);
				break;
			case 1:
				intensity = 2;
				break;
			case 2:
				intensity = 0;
				break;
			case 4:
				underline = 1;
				break;
			case 5:
				blink = 1;
				break;
			case 7:
				reverse = 1;
				break;
			case 10: /* ANSI X3.64-1979 (SCO-ish?)
				  * Select primary font, don't display
				  * control chars if defined, don't set
				  * bit 8 on output.
				  */
				translate = set_translate(charset == 0
						? G0_charset
						: G1_charset,currcons);
				disp_ctrl = 0;
				toggle_meta = 0;
				break;
			case 11: /* ANSI X3.64-1979 (SCO-ish?)
				  * Select first alternate font, lets
				  * chars < 32 be displayed as ROM chars.
				  */
				translate = set_translate(IBMPC_MAP,currcons);
				disp_ctrl = 1;
				toggle_meta = 0;
				break;
			case 12: /* ANSI X3.64-1979 (SCO-ish?)
				  * Select second alternate font, toggle
				  * high bit before displaying as ROM char.
				  */
				translate = set_translate(IBMPC_MAP,currcons);
				disp_ctrl = 1;
				toggle_meta = 1;
				break;
			case 21:
			case 22:
				intensity = 1;
				break;
			case 24:
				underline = 0;
				break;
			case 25:
				blink = 0;
				break;
			case 27:
				reverse = 0;
				break;
			case 38: /* ANSI X3.64-1979 (SCO-ish?)
				  * Enables underscore, white foreground
				  * with white underscore (Linux - use
				  * default foreground).
				  */
				color = (def_color & 0x0f) | background;
				underline = 1;
				break;
			case 39: /* ANSI X3.64-1979 (SCO-ish?)
				  * Disable underline option.
				  * Reset colour to default? It did this
				  * before...
				  */
				color = (def_color & 0x0f) | background;
				underline = 0;
				break;
			case 49:
				color = (def_color & 0xf0) | foreground;
				break;
			default:
				if (par[i] >= 30 && par[i] <= 37)
					color = color_table[par[i]-30]
						| background;
				else if (par[i] >= 40 && par[i] <= 47)
					color = (color_table[par[i]-40]<<4)
						| foreground;
				break;
		}
	update_attr(currcons);
}

static void respond_string(const char *p, struct tty_struct *tty)
{
	while (*p) {
		tty_insert_flip_char(tty, *p, 0);
		p++;
	}
	con_schedule_flip(tty);
}

static void cursor_report(int currcons, struct tty_struct *tty)
{
	char buf[40];

	sprintf(buf, "\033[%d;%dR", y + (decom ? top+1 : 1), x+1);
	respond_string(buf, tty);
}

static inline void status_report(struct tty_struct *tty)
{
	respond_string("\033[0n", tty);	/* Terminal ok */
}

static inline void respond_ID(struct tty_struct * tty)
{
	respond_string(VT102ID, tty);
}

void mouse_report(struct tty_struct *tty, int butt, int mrx, int mry)
{
	char buf[8];

	sprintf(buf, "\033[M%c%c%c", (char)(' ' + butt), (char)('!' + mrx),
		(char)('!' + mry));
	respond_string(buf, tty);
}

/* invoked via ioctl(TIOCLINUX) and through set_selection */
int mouse_reporting(void)
{
	int currcons = fg_console;

	return report_mouse;
}

/* console_sem is held */
static void set_mode(int currcons, int on_off)
{
	int i;

	for (i=0; i<=npar; i++)
		if (ques) switch(par[i]) {	/* DEC private modes set/reset */
			case 1:			/* Cursor keys send ^[Ox/^[[x */
				if (on_off)
					set_kbd(decckm);
				else
					clr_kbd(decckm);
				break;
			case 3:	/* 80/132 mode switch unimplemented */
				deccolm = on_off;
#if 0
				(void) vc_resize(deccolm ? 132 : 80, vc_cons[currcons].d->vc_rows);
				/* this alone does not suffice; some user mode
				   utility has to change the hardware regs */
#endif
				break;
			case 5:			/* Inverted screen on/off */
				if (decscnm != on_off) {
					decscnm = on_off;
					invert_screen(vc_cons[currcons].d, 0, screenbuf_size, 0);
					update_attr(currcons);
				}
				break;
			case 6:			/* Origin relative/absolute */
				decom = on_off;
				gotoxay(currcons,0,0);
				break;
			case 7:			/* Autowrap on/off */
				decawm = on_off;
				break;
			case 8:			/* Autorepeat on/off */
				if (on_off)
					set_kbd(decarm);
				else
					clr_kbd(decarm);
				break;
			case 9:
				report_mouse = on_off ? 1 : 0;
				break;
			case 25:		/* Cursor on/off */
				deccm = on_off;
				break;
			case 1000:
				report_mouse = on_off ? 2 : 0;
				break;
		} else switch(par[i]) {		/* ANSI modes set/reset */
			case 3:			/* Monitor (display ctrls) */
				disp_ctrl = on_off;
				break;
			case 4:			/* Insert Mode on/off */
				decim = on_off;
				break;
			case 20:		/* Lf, Enter == CrLf/Lf */
				if (on_off)
					set_kbd(lnm);
				else
					clr_kbd(lnm);
				break;
		}
}

/* console_sem is held */
static void setterm_command(int currcons)
{
	switch(par[0]) {
		case 1:	/* set color for underline mode */
			if (vc_cons[currcons].d->vc_can_do_color &&
					par[1] < 16) {
				ulcolor = color_table[par[1]];
				if (underline)
					update_attr(currcons);
			}
			break;
		case 2:	/* set color for half intensity mode */
			if (vc_cons[currcons].d->vc_can_do_color &&
					par[1] < 16) {
				halfcolor = color_table[par[1]];
				if (intensity == 0)
					update_attr(currcons);
			}
			break;
		case 8:	/* store colors as defaults */
			def_color = attr;
			if (hi_font_mask == 0x100)
				def_color >>= 1;
			default_attr(currcons);
			update_attr(currcons);
			break;
		case 9:	/* set blanking interval */
			blankinterval = ((par[1] < 60) ? par[1] : 60) * 60 * HZ;
			poke_blanked_console();
			break;
		case 10: /* set bell frequency in Hz */
			if (npar >= 1)
				bell_pitch = par[1];
			else
				bell_pitch = DEFAULT_BELL_PITCH;
			break;
		case 11: /* set bell duration in msec */
			if (npar >= 1)
				bell_duration = (par[1] < 2000) ?
					par[1]*HZ/1000 : 0;
			else
				bell_duration = DEFAULT_BELL_DURATION;
			break;
		case 12: /* bring specified console to the front */
			if (par[1] >= 1 && vc_cons_allocated(par[1]-1))
				set_console(par[1] - 1);
			break;
		case 13: /* unblank the screen */
			poke_blanked_console();
			break;
		case 14: /* set vesa powerdown interval */
			vesa_off_interval = ((par[1] < 60) ? par[1] : 60) * 60 * HZ;
			break;
		case 15: /* activate the previous console */
			set_console(last_console);
			break;
	}
}

/* console_sem is held */
static void csi_at(int currcons, unsigned int nr)
{
	if (nr > vc_cons[currcons].d->vc_cols - x)
		nr = vc_cons[currcons].d->vc_cols - x;
	else if (!nr)
		nr = 1;
	insert_char(currcons, nr);
}

/* console_sem is held */
static void csi_L(int currcons, unsigned int nr)
{
	if (nr > vc_cons[currcons].d->vc_rows - y)
		nr = vc_cons[currcons].d->vc_rows - y;
	else if (!nr)
		nr = 1;
	scrdown(currcons,y,bottom,nr);
	need_wrap = 0;
}

/* console_sem is held */
static void csi_P(int currcons, unsigned int nr)
{
	if (nr > vc_cons[currcons].d->vc_cols - x)
		nr = vc_cons[currcons].d->vc_cols - x;
	else if (!nr)
		nr = 1;
	delete_char(currcons, nr);
}

/* console_sem is held */
static void csi_M(int currcons, unsigned int nr)
{
	if (nr > vc_cons[currcons].d->vc_rows - y)
		nr = vc_cons[currcons].d->vc_rows - y;
	else if (!nr)
		nr=1;
	scrup(currcons,y,bottom,nr);
	need_wrap = 0;
}

/* console_sem is held (except via vc_init->reset_terminal */
static void save_cur(int currcons)
{
	saved_x		= x;
	saved_y		= y;
	s_intensity	= intensity;
	s_underline	= underline;
	s_blink		= blink;
	s_reverse	= reverse;
	s_charset	= charset;
	s_color		= color;
	saved_G0	= G0_charset;
	saved_G1	= G1_charset;
}

/* console_sem is held */
static void restore_cur(int currcons)
{
	gotoxy(vc_cons[currcons].d,saved_x,saved_y);
	intensity	= s_intensity;
	underline	= s_underline;
	blink		= s_blink;
	reverse		= s_reverse;
	charset		= s_charset;
	color		= s_color;
	G0_charset	= saved_G0;
	G1_charset	= saved_G1;
	translate	= set_translate(charset ? G1_charset : G0_charset,currcons);
	update_attr(currcons);
	need_wrap = 0;
}

enum { ESnormal, ESesc, ESsquare, ESgetpars, ESgotpars, ESfunckey,
	EShash, ESsetG0, ESsetG1, ESpercent, ESignore, ESnonstd,
	ESpalette };

/* console_sem is held (except via vc_init()) */
static void reset_terminal(int currcons, int do_clear)
{
	top		= 0;
	bottom		= vc_cons[currcons].d->vc_rows;
	vc_state	= ESnormal;
	ques		= 0;
	translate	= set_translate(LAT1_MAP,currcons);
	G0_charset	= LAT1_MAP;
	G1_charset	= GRAF_MAP;
	charset		= 0;
	need_wrap	= 0;
	report_mouse	= 0;
	utf             = 0;
	utf_count       = 0;

	disp_ctrl	= 0;
	toggle_meta	= 0;

	decscnm		= 0;
	decom		= 0;
	decawm		= 1;
	deccm		= 1;
	decim		= 0;

	set_kbd(decarm);
	clr_kbd(decckm);
	clr_kbd(kbdapplic);
	clr_kbd(lnm);
	kbd_table[currcons].lockstate = 0;
	kbd_table[currcons].slockstate = 0;
	kbd_table[currcons].ledmode = LED_SHOW_FLAGS;
	kbd_table[currcons].ledflagstate = kbd_table[currcons].default_ledflagstate;
	/* do not do set_leds here because this causes an endless tasklet loop
	   when the keyboard hasn't been initialized yet */

	cursor_type = CUR_DEFAULT;
	complement_mask = s_complement_mask;

	default_attr(currcons);
	update_attr(currcons);

	tab_stop[0]	= 0x01010100;
	tab_stop[1]	=
	tab_stop[2]	=
	tab_stop[3]	=
	tab_stop[4]	= 0x01010101;

	bell_pitch = DEFAULT_BELL_PITCH;
	bell_duration = DEFAULT_BELL_DURATION;

	gotoxy(vc_cons[currcons].d, 0, 0);
	save_cur(currcons);
	if (do_clear)
	    csi_J(currcons,2);
}

/* console_sem is held */
static void do_con_trol(struct tty_struct *tty, unsigned int currcons, int c)
{
	/*
	 *  Control characters can be used in the _middle_
	 *  of an escape sequence.
	 */
	switch (c) {
	case 0:
		return;
	case 7:
		if (bell_duration)
			kd_mksound(bell_pitch, bell_duration);
		return;
	case 8:
		bs(currcons);
		return;
	case 9:
		pos -= (x << 1);
		while (x < vc_cons[currcons].d->vc_cols - 1) {
			x++;
			if (tab_stop[x >> 5] & (1 << (x & 31)))
				break;
		}
		pos += (x << 1);
		return;
	case 10: case 11: case 12:
		lf(currcons);
		if (!is_kbd(lnm))
			return;
	case 13:
		cr(currcons);
		return;
	case 14:
		charset = 1;
		translate = set_translate(G1_charset,currcons);
		disp_ctrl = 1;
		return;
	case 15:
		charset = 0;
		translate = set_translate(G0_charset,currcons);
		disp_ctrl = 0;
		return;
	case 24: case 26:
		vc_state = ESnormal;
		return;
	case 27:
		vc_state = ESesc;
		return;
	case 127:
		del(currcons);
		return;
	case 128+27:
		vc_state = ESsquare;
		return;
	}
	switch(vc_state) {
	case ESesc:
		vc_state = ESnormal;
		switch (c) {
		case '[':
			vc_state = ESsquare;
			return;
		case ']':
			vc_state = ESnonstd;
			return;
		case '%':
			vc_state = ESpercent;
			return;
		case 'E':
			cr(currcons);
			lf(currcons);
			return;
		case 'M':
			ri(currcons);
			return;
		case 'D':
			lf(currcons);
			return;
		case 'H':
			tab_stop[x >> 5] |= (1 << (x & 31));
			return;
		case 'Z':
			respond_ID(tty);
			return;
		case '7':
			save_cur(currcons);
			return;
		case '8':
			restore_cur(currcons);
			return;
		case '(':
			vc_state = ESsetG0;
			return;
		case ')':
			vc_state = ESsetG1;
			return;
		case '#':
			vc_state = EShash;
			return;
		case 'c':
			reset_terminal(currcons,1);
			return;
		case '>':  /* Numeric keypad */
			clr_kbd(kbdapplic);
			return;
		case '=':  /* Appl. keypad */
			set_kbd(kbdapplic);
			return;
		}
		return;
	case ESnonstd:
		if (c=='P') {   /* palette escape sequence */
			for (npar=0; npar<NPAR; npar++)
				par[npar] = 0 ;
			npar = 0 ;
			vc_state = ESpalette;
			return;
		} else if (c=='R') {   /* reset palette */
			reset_palette(currcons);
			vc_state = ESnormal;
		} else
			vc_state = ESnormal;
		return;
	case ESpalette:
		if ( (c>='0'&&c<='9') || (c>='A'&&c<='F') || (c>='a'&&c<='f') ) {
			par[npar++] = (c>'9' ? (c&0xDF)-'A'+10 : c-'0') ;
			if (npar==7) {
				int i = par[0]*3, j = 1;
				palette[i] = 16*par[j++];
				palette[i++] += par[j++];
				palette[i] = 16*par[j++];
				palette[i++] += par[j++];
				palette[i] = 16*par[j++];
				palette[i] += par[j];
				set_palette(currcons);
				vc_state = ESnormal;
			}
		} else
			vc_state = ESnormal;
		return;
	case ESsquare:
		for(npar = 0 ; npar < NPAR ; npar++)
			par[npar] = 0;
		npar = 0;
		vc_state = ESgetpars;
		if (c == '[') { /* Function key */
			vc_state=ESfunckey;
			return;
		}
		ques = (c=='?');
		if (ques)
			return;
	case ESgetpars:
		if (c==';' && npar<NPAR-1) {
			npar++;
			return;
		} else if (c>='0' && c<='9') {
			par[npar] *= 10;
			par[npar] += c-'0';
			return;
		} else vc_state=ESgotpars;
	case ESgotpars:
		vc_state = ESnormal;
		switch(c) {
		case 'h':
			set_mode(currcons,1);
			return;
		case 'l':
			set_mode(currcons,0);
			return;
		case 'c':
			if (ques) {
				if (par[0])
					cursor_type = par[0] | (par[1]<<8) | (par[2]<<16);
				else
					cursor_type = CUR_DEFAULT;
				return;
			}
			break;
		case 'm':
			if (ques) {
				clear_selection();
				if (par[0])
					complement_mask = par[0]<<8 | par[1];
				else
					complement_mask = s_complement_mask;
				return;
			}
			break;
		case 'n':
			if (!ques) {
				if (par[0] == 5)
					status_report(tty);
				else if (par[0] == 6)
					cursor_report(currcons,tty);
			}
			return;
		}
		if (ques) {
			ques = 0;
			return;
		}
		switch(c) {
		case 'G': case '`':
			if (par[0]) par[0]--;
			gotoxy(vc_cons[currcons].d, par[0], y);
			return;
		case 'A':
			if (!par[0]) par[0]++;
			gotoxy(vc_cons[currcons].d, x, y-par[0]);
			return;
		case 'B': case 'e':
			if (!par[0]) par[0]++;
			gotoxy(vc_cons[currcons].d, x, y+par[0]);
			return;
		case 'C': case 'a':
			if (!par[0]) par[0]++;
			gotoxy(vc_cons[currcons].d, x+par[0], y);
			return;
		case 'D':
			if (!par[0]) par[0]++;
			gotoxy(vc_cons[currcons].d, x-par[0], y);
			return;
		case 'E':
			if (!par[0]) par[0]++;
			gotoxy(vc_cons[currcons].d, 0, y+par[0]);
			return;
		case 'F':
			if (!par[0]) par[0]++;
			gotoxy(vc_cons[currcons].d, 0, y-par[0]);
			return;
		case 'd':
			if (par[0]) par[0]--;
			gotoxay(currcons,x,par[0]);
			return;
		case 'H': case 'f':
			if (par[0]) par[0]--;
			if (par[1]) par[1]--;
			gotoxay(currcons,par[1],par[0]);
			return;
		case 'J':
			csi_J(currcons,par[0]);
			return;
		case 'K':
			csi_K(currcons,par[0]);
			return;
		case 'L':
			csi_L(currcons,par[0]);
			return;
		case 'M':
			csi_M(currcons,par[0]);
			return;
		case 'P':
			csi_P(currcons,par[0]);
			return;
		case 'c':
			if (!par[0])
				respond_ID(tty);
			return;
		case 'g':
			if (!par[0])
				tab_stop[x >> 5] &= ~(1 << (x & 31));
			else if (par[0] == 3) {
				tab_stop[0] =
					tab_stop[1] =
					tab_stop[2] =
					tab_stop[3] =
					tab_stop[4] = 0;
			}
			return;
		case 'm':
			csi_m(currcons);
			return;
		case 'q': /* DECLL - but only 3 leds */
			/* map 0,1,2,3 to 0,1,2,4 */
			if (par[0] < 4)
				setledstate(kbd_table + currcons,
					    (par[0] < 3) ? par[0] : 4);
			return;
		case 'r':
			if (!par[0])
				par[0]++;
			if (!par[1])
				par[1] = vc_cons[currcons].d->vc_rows;
			/* Minimum allowed region is 2 lines */
			if (par[0] < par[1] &&
			    par[1] <= vc_cons[currcons].d->vc_rows) {
				top=par[0]-1;
				bottom=par[1];
				gotoxay(currcons,0,0);
			}
			return;
		case 's':
			save_cur(currcons);
			return;
		case 'u':
			restore_cur(currcons);
			return;
		case 'X':
			csi_X(currcons, par[0]);
			return;
		case '@':
			csi_at(currcons,par[0]);
			return;
		case ']': /* setterm functions */
			setterm_command(currcons);
			return;
		}
		return;
	case ESpercent:
		vc_state = ESnormal;
		switch (c) {
		case '@':  /* defined in ISO 2022 */
			utf = 0;
			return;
		case 'G':  /* prelim official escape code */
		case '8':  /* retained for compatibility */
			utf = 1;
			return;
		}
		return;
	case ESfunckey:
		vc_state = ESnormal;
		return;
	case EShash:
		vc_state = ESnormal;
		if (c == '8') {
			/* DEC screen alignment test. kludge :-) */
			video_erase_char =
				(video_erase_char & 0xff00) | 'E';
			csi_J(currcons, 2);
			video_erase_char =
				(video_erase_char & 0xff00) | ' ';
			do_update_region(vc_cons[currcons].d, origin, screenbuf_size/2);
		}
		return;
	case ESsetG0:
		if (c == '0')
			G0_charset = GRAF_MAP;
		else if (c == 'B')
			G0_charset = LAT1_MAP;
		else if (c == 'U')
			G0_charset = IBMPC_MAP;
		else if (c == 'K')
			G0_charset = USER_MAP;
		if (charset == 0)
			translate = set_translate(G0_charset,currcons);
		vc_state = ESnormal;
		return;
	case ESsetG1:
		if (c == '0')
			G1_charset = GRAF_MAP;
		else if (c == 'B')
			G1_charset = LAT1_MAP;
		else if (c == 'U')
			G1_charset = IBMPC_MAP;
		else if (c == 'K')
			G1_charset = USER_MAP;
		if (charset == 1)
			translate = set_translate(G1_charset,currcons);
		vc_state = ESnormal;
		return;
	default:
		vc_state = ESnormal;
	}
}

/* This is a temporary buffer used to prepare a tty console write
 * so that we can easily avoid touching user space while holding the
 * console spinlock.  It is allocated in con_init and is shared by
 * this code and the vc_screen read/write tty calls.
 *
 * We have to allocate this statically in the kernel data section
 * since console_init (and thus con_init) are called before any
 * kernel memory allocation is available.
 */
char con_buf[CON_BUF_SIZE];
DECLARE_MUTEX(con_buf_sem);

/* acquires console_sem */
static int do_con_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
#ifdef VT_BUF_VRAM_ONLY
#define FLUSH do { } while(0);
#else
#define FLUSH if (draw_x >= 0) { \
	sw->con_putcs(vc_cons[currcons].d, (u16 *)draw_from, (u16 *)draw_to-(u16 *)draw_from, y, draw_x); \
	draw_x = -1; \
	}
#endif

	int c, tc, ok, n = 0, draw_x = -1;
	unsigned int currcons;
	unsigned long draw_from = 0, draw_to = 0;
	struct vt_struct *vt;
	u16 himask, charmask;
	const unsigned char *orig_buf = NULL;
	int orig_count;

	if (in_interrupt())
		return count;

	might_sleep();

	acquire_console_sem();
	vt = tty->driver_data;
	if (vt == NULL) {
		printk(KERN_ERR "vt: argh, driver_data is NULL !\n");
		release_console_sem();
		return 0;
	}

	currcons = vt->vc_num;
	if (!vc_cons_allocated(currcons)) {
	    /* could this happen? */
	    static int error = 0;
	    if (!error) {
		error = 1;
		printk("con_write: tty %d not allocated\n", currcons+1);
	    }
	    release_console_sem();
	    return 0;
	}
	release_console_sem();

	orig_buf = buf;
	orig_count = count;

	/* At this point 'buf' is guaranteed to be a kernel buffer
	 * and therefore no access to userspace (and therefore sleeping)
	 * will be needed.  The con_buf_sem serializes all tty based
	 * console rendering and vcs write/read operations.  We hold
	 * the console spinlock during the entire write.
	 */

	acquire_console_sem();

	vt = tty->driver_data;
	if (vt == NULL) {
		printk(KERN_ERR "vt: argh, driver_data _became_ NULL !\n");
		release_console_sem();
		goto out;
	}

	himask = hi_font_mask;
	charmask = himask ? 0x1ff : 0xff;

	/* undraw cursor first */
	if (IS_FG)
		hide_cursor(vc_cons[currcons].d);

	while (!tty->stopped && count) {
		int orig = *buf;
		c = orig;
		buf++;
		n++;
		count--;

		/* Do no translation at all in control states */
		if (vc_state != ESnormal) {
			tc = c;
		} else if (utf) {
		    /* Combine UTF-8 into Unicode */
		    /* Incomplete characters silently ignored */
		    if(c > 0x7f) {
			if (utf_count > 0 && (c & 0xc0) == 0x80) {
				utf_char = (utf_char << 6) | (c & 0x3f);
				utf_count--;
				if (utf_count == 0)
				    tc = c = utf_char;
				else continue;
			} else {
				if ((c & 0xe0) == 0xc0) {
				    utf_count = 1;
				    utf_char = (c & 0x1f);
				} else if ((c & 0xf0) == 0xe0) {
				    utf_count = 2;
				    utf_char = (c & 0x0f);
				} else if ((c & 0xf8) == 0xf0) {
				    utf_count = 3;
				    utf_char = (c & 0x07);
				} else if ((c & 0xfc) == 0xf8) {
				    utf_count = 4;
				    utf_char = (c & 0x03);
				} else if ((c & 0xfe) == 0xfc) {
				    utf_count = 5;
				    utf_char = (c & 0x01);
				} else
				    utf_count = 0;
				continue;
			      }
		    } else {
		      tc = c;
		      utf_count = 0;
		    }
		} else {	/* no utf */
		  tc = translate[toggle_meta ? (c|0x80) : c];
		}

                /* If the original code was a control character we
                 * only allow a glyph to be displayed if the code is
                 * not normally used (such as for cursor movement) or
                 * if the disp_ctrl mode has been explicitly enabled.
                 * Certain characters (as given by the CTRL_ALWAYS
                 * bitmap) are always displayed as control characters,
                 * as the console would be pretty useless without
                 * them; to display an arbitrary font position use the
                 * direct-to-font zone in UTF-8 mode.
                 */
                ok = tc && (c >= 32 ||
                            (!utf && !(((disp_ctrl ? CTRL_ALWAYS
                                         : CTRL_ACTION) >> c) & 1)))
                        && (c != 127 || disp_ctrl)
			&& (c != 128+27);

		if (vc_state == ESnormal && ok) {
			/* Now try to find out how to display it */
			tc = conv_uni_to_pc(vc_cons[currcons].d, tc);
			if ( tc == -4 ) {
                                /* If we got -4 (not found) then see if we have
                                   defined a replacement character (U+FFFD) */
                                tc = conv_uni_to_pc(vc_cons[currcons].d, 0xfffd);

				/* One reason for the -4 can be that we just
				   did a clear_unimap();
				   try at least to show something. */
				if (tc == -4)
				     tc = c;
                        } else if ( tc == -3 ) {
                                /* Bad hash table -- hope for the best */
                                tc = c;
                        }
			if (tc & ~charmask)
                                continue; /* Conversion failed */

			if (need_wrap || decim)
				FLUSH
			if (need_wrap) {
				cr(currcons);
				lf(currcons);
			}
			if (decim)
				insert_char(currcons, 1);
			scr_writew(himask ?
				     ((attr << 8) & ~himask) + ((tc & 0x100) ? himask : 0) + (tc & 0xff) :
				     (attr << 8) + tc,
				   (u16 *) pos);
			if (DO_UPDATE && draw_x < 0) {
				draw_x = x;
				draw_from = pos;
			}
			if (x == vc_cons[currcons].d->vc_cols - 1) {
				need_wrap = decawm;
				draw_to = pos+2;
			} else {
				x++;
				draw_to = (pos+=2);
			}
			continue;
		}
		FLUSH
		do_con_trol(tty, currcons, orig);
	}
	FLUSH
	console_conditional_schedule();
	release_console_sem();

out:
	return n;
#undef FLUSH
}

/*
 * This is the console switching callback.
 *
 * Doing console switching in a process context allows
 * us to do the switches asynchronously (needed when we want
 * to switch due to a keyboard interrupt).  Synchronization
 * with other console code and prevention of re-entrancy is
 * ensured with console_sem.
 */
static void console_callback(void *ignored)
{
	acquire_console_sem();

	if (want_console >= 0) {
		if (want_console != fg_console &&
		    vc_cons_allocated(want_console)) {
			hide_cursor(vc_cons[fg_console].d);
			change_console(want_console);
			/* we only changed when the console had already
			   been allocated - a new console is not created
			   in an interrupt routine */
		}
		want_console = -1;
	}
	if (do_poke_blanked_console) { /* do not unblank for a LED change */
		do_poke_blanked_console = 0;
		poke_blanked_console();
	}
	if (scrollback_delta) {
		int currcons = fg_console;
		clear_selection();
		if (vcmode == KD_TEXT)
			sw->con_scrolldelta(vc_cons[currcons].d, scrollback_delta);
		scrollback_delta = 0;
	}
	if (blank_timer_expired) {
		do_blank_screen(0);
		blank_timer_expired = 0;
	}

	release_console_sem();
}

void set_console(int nr)
{
	want_console = nr;
	schedule_console_callback();
}

struct tty_driver *console_driver;

#ifdef CONFIG_VT_CONSOLE

/*
 *	Console on virtual terminal
 *
 * The console must be locked when we get here.
 */

void vt_console_print(struct console *co, const char *b, unsigned count)
{
	int currcons = fg_console;
	unsigned char c;
	static unsigned long printing;
	const ushort *start;
	ushort cnt = 0;
	ushort myx;

	/* console busy or not yet initialized */
	if (!printable || test_and_set_bit(0, &printing))
		return;

	if (kmsg_redirect && vc_cons_allocated(kmsg_redirect - 1))
		currcons = kmsg_redirect - 1;

	/* read `x' only after setting currcons properly (otherwise
	   the `x' macro will read the x of the foreground console). */
	myx = x;

	if (!vc_cons_allocated(currcons)) {
		/* impossible */
		/* printk("vt_console_print: tty %d not allocated ??\n", currcons+1); */
		goto quit;
	}

	if (vcmode != KD_TEXT)
		goto quit;

	/* undraw cursor first */
	if (IS_FG)
		hide_cursor(vc_cons[currcons].d);

	start = (ushort *)pos;

	/* Contrived structure to try to emulate original need_wrap behaviour
	 * Problems caused when we have need_wrap set on '\n' character */
	while (count--) {
		c = *b++;
		if (c == 10 || c == 13 || c == 8 || need_wrap) {
			if (cnt > 0) {
				if (IS_VISIBLE)
					sw->con_putcs(vc_cons[currcons].d, start, cnt, y, x);
				x += cnt;
				if (need_wrap)
					x--;
				cnt = 0;
			}
			if (c == 8) {		/* backspace */
				bs(currcons);
				start = (ushort *)pos;
				myx = x;
				continue;
			}
			if (c != 13)
				lf(currcons);
			cr(currcons);
			start = (ushort *)pos;
			myx = x;
			if (c == 10 || c == 13)
				continue;
		}
		scr_writew((attr << 8) + c, (unsigned short *) pos);
		cnt++;
		if (myx == vc_cons[currcons].d->vc_cols - 1) {
			need_wrap = 1;
			continue;
		}
		pos+=2;
		myx++;
	}
	if (cnt > 0) {
		if (IS_VISIBLE)
			sw->con_putcs(vc_cons[currcons].d, start, cnt, y, x);
		x += cnt;
		if (x == vc_cons[currcons].d->vc_cols) {
			x--;
			need_wrap = 1;
		}
	}
	set_cursor(vc_cons[currcons].d);

	if (!oops_in_progress)
		poke_blanked_console();

quit:
	clear_bit(0, &printing);
}

static struct tty_driver *vt_console_device(struct console *c, int *index)
{
	*index = c->index ? c->index-1 : fg_console;
	return console_driver;
}

struct console vt_console_driver = {
	.name		= "tty",
	.write		= vt_console_print,
	.device		= vt_console_device,
	.unblank	= unblank_screen,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
};
#endif

/*
 *	Handling of Linux-specific VC ioctls
 */

/*
 * Generally a bit racy with respect to console_sem().
 *
 * There are some functions which don't need it.
 *
 * There are some functions which can sleep for arbitrary periods
 * (paste_selection) but we don't need the lock there anyway.
 *
 * set_selection has locking, and definitely needs it
 */

int tioclinux(struct tty_struct *tty, unsigned long arg)
{
	char type, data;
	char __user *p = (char __user *)arg;
	int lines;
	int ret;

	if (tty->driver->type != TTY_DRIVER_TYPE_CONSOLE)
		return -EINVAL;
	if (current->signal->tty != tty && !capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (get_user(type, p))
		return -EFAULT;
	ret = 0;
	switch (type)
	{
		case TIOCL_SETSEL:
			acquire_console_sem();
			ret = set_selection((struct tiocl_selection __user *)(p+1), tty);
			release_console_sem();
			break;
		case TIOCL_PASTESEL:
			ret = paste_selection(tty);
			break;
		case TIOCL_UNBLANKSCREEN:
			unblank_screen();
			break;
		case TIOCL_SELLOADLUT:
			ret = sel_loadlut(p);
			break;
		case TIOCL_GETSHIFTSTATE:
			
	/*
	 * Make it possible to react to Shift+Mousebutton.
	 * Note that 'shift_state' is an undocumented
	 * kernel-internal variable; programs not closely
	 * related to the kernel should not use this.
	 */
	 		data = shift_state;
			ret = __put_user(data, p);
			break;
		case TIOCL_GETMOUSEREPORTING:
			data = mouse_reporting();
			ret = __put_user(data, p);
			break;
		case TIOCL_SETVESABLANK:
			set_vesa_blanking(p);
			break;
		case TIOCL_SETKMSGREDIRECT:
			if (!capable(CAP_SYS_ADMIN)) {
				ret = -EPERM;
			} else {
				if (get_user(data, p+1))
					ret = -EFAULT;
				else
					kmsg_redirect = data;
			}
			break;
		case TIOCL_GETFGCONSOLE:
			ret = fg_console;
			break;
		case TIOCL_SCROLLCONSOLE:
			if (get_user(lines, (s32 __user *)(p+4))) {
				ret = -EFAULT;
			} else {
				scrollfront(lines);
				ret = 0;
			}
			break;
		case TIOCL_BLANKSCREEN:	/* until explicitly unblanked, not only poked */
			ignore_poke = 1;
			do_blank_screen(0);
			break;
		case TIOCL_BLANKEDSCREEN:
			ret = console_blanked;
			break;
		default:
			ret = -EINVAL;
			break;
	}
	return ret;
}

/*
 * /dev/ttyN handling
 */

static int con_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
	int	retval;

	retval = do_con_write(tty, buf, count);
	con_flush_chars(tty);

	return retval;
}

static void con_put_char(struct tty_struct *tty, unsigned char ch)
{
	if (in_interrupt())
		return;	/* n_r3964 calls put_char() from interrupt context */
	do_con_write(tty, &ch, 1);
}

static int con_write_room(struct tty_struct *tty)
{
	if (tty->stopped)
		return 0;
	return 4096;		/* No limit, really; we're not buffering */
}

static int con_chars_in_buffer(struct tty_struct *tty)
{
	return 0;		/* we're not buffering */
}

/*
 * con_throttle and con_unthrottle are only used for
 * paste_selection(), which has to stuff in a large number of
 * characters...
 */
static void con_throttle(struct tty_struct *tty)
{
}

static void con_unthrottle(struct tty_struct *tty)
{
	struct vt_struct *vt = tty->driver_data;

	wake_up_interruptible(&vt->paste_wait);
}

/*
 * Turn the Scroll-Lock LED on when the tty is stopped
 */
static void con_stop(struct tty_struct *tty)
{
	int console_num;
	if (!tty)
		return;
	console_num = tty->index;
	if (!vc_cons_allocated(console_num))
		return;
	set_vc_kbd_led(kbd_table + console_num, VC_SCROLLOCK);
	set_leds();
}

/*
 * Turn the Scroll-Lock LED off when the console is started
 */
static void con_start(struct tty_struct *tty)
{
	int console_num;
	if (!tty)
		return;
	console_num = tty->index;
	if (!vc_cons_allocated(console_num))
		return;
	clr_vc_kbd_led(kbd_table + console_num, VC_SCROLLOCK);
	set_leds();
}

static void con_flush_chars(struct tty_struct *tty)
{
	struct vt_struct *vt;

	if (in_interrupt())	/* from flush_to_ldisc */
		return;

	/* if we race with con_close(), vt may be null */
	acquire_console_sem();
	vt = tty->driver_data;
	if (vt)
		set_cursor(vc_cons[vt->vc_num].d);
	release_console_sem();
}

/*
 * Allocate the console screen memory.
 */
static int con_open(struct tty_struct *tty, struct file *filp)
{
	unsigned int currcons = tty->index;
	int ret = 0;

	acquire_console_sem();
	if (tty->count == 1) {
		ret = vc_allocate(currcons);
		if (ret == 0) {
			vt_cons[currcons]->vc_num = currcons;
			tty->driver_data = vt_cons[currcons];
			vc_cons[currcons].d->vc_tty = tty;

			if (!tty->winsize.ws_row && !tty->winsize.ws_col) {
				tty->winsize.ws_row = vc_cons[currcons].d->vc_rows;
				tty->winsize.ws_col = vc_cons[currcons].d->vc_cols;
			}
			release_console_sem();
			vcs_make_devfs(tty);
			return ret;
		}
	}
	release_console_sem();
	return ret;
}

/*
 * We take tty_sem in here to prevent another thread from coming in via init_dev
 * and taking a ref against the tty while we're in the process of forgetting
 * about it and cleaning things up.
 *
 * This is because vcs_remove_devfs() can sleep and will drop the BKL.
 */
static void con_close(struct tty_struct *tty, struct file *filp)
{
	down(&tty_sem);
	acquire_console_sem();
	if (tty && tty->count == 1) {
		struct vt_struct *vt;

		vt = tty->driver_data;
		if (vt)
			vc_cons[vt->vc_num].d->vc_tty = NULL;
		tty->driver_data = NULL;
		release_console_sem();
		vcs_remove_devfs(tty);
		up(&tty_sem);
		/*
		 * tty_sem is released, but we still hold BKL, so there is
		 * still exclusion against init_dev()
		 */
		return;
	}
	release_console_sem();
	up(&tty_sem);
}

static void vc_init(unsigned int currcons, unsigned int rows,
			unsigned int cols, int do_clear)
{
	int j, k ;

	vc_cons[currcons].d->vc_cols = cols;
	vc_cons[currcons].d->vc_rows = rows;
	vc_cons[currcons].d->vc_size_row = cols<<1;
	screenbuf_size = vc_cons[currcons].d->vc_rows * vc_cons[currcons].d->vc_size_row;

	set_origin(currcons);
	pos = origin;
	reset_vc(currcons);
	for (j=k=0; j<16; j++) {
		vc_cons[currcons].d->vc_palette[k++] = default_red[j] ;
		vc_cons[currcons].d->vc_palette[k++] = default_grn[j] ;
		vc_cons[currcons].d->vc_palette[k++] = default_blu[j] ;
	}
	def_color       = 0x07;   /* white */
	ulcolor		= 0x0f;   /* bold white */
	halfcolor       = 0x08;   /* grey */
	init_waitqueue_head(&vt_cons[currcons]->paste_wait);
	reset_terminal(currcons, do_clear);
}

/*
 * This routine initializes console interrupts, and does nothing
 * else. If you want the screen to clear, call tty_write with
 * the appropriate escape-sequence.
 */

static int __init con_init(void)
{
	const char *display_desc = NULL;
	unsigned int currcons = 0;

	acquire_console_sem();

	if (conswitchp)
		display_desc = conswitchp->con_startup();
	if (!display_desc) {
		fg_console = 0;
		release_console_sem();
		return 0;
	}

	init_timer(&console_timer);
	console_timer.function = blank_screen_t;
	if (blankinterval) {
		blank_state = blank_normal_wait;
		mod_timer(&console_timer, jiffies + blankinterval);
	}

	/*
	 * kmalloc is not running yet - we use the bootmem allocator.
	 */
	for (currcons = 0; currcons < MIN_NR_CONSOLES; currcons++) {
		vc_cons[currcons].d = (struct vc_data *)
				alloc_bootmem(sizeof(struct vc_data));
		vt_cons[currcons] = (struct vt_struct *)
				alloc_bootmem(sizeof(struct vt_struct));
		vc_cons[currcons].d->vc_vt = vt_cons[currcons];
		visual_init(currcons, 1);
		screenbuf = (unsigned short *) alloc_bootmem(screenbuf_size);
		kmalloced = 0;
		vc_init(currcons, vc_cons[currcons].d->vc_rows, vc_cons[currcons].d->vc_cols,
			currcons || !sw->con_save_screen);
	}
	currcons = fg_console = 0;
	master_display_fg = vc_cons[currcons].d;
	set_origin(currcons);
	save_screen(currcons);
	gotoxy(vc_cons[currcons].d, x, y);
	csi_J(currcons, 0);
	update_screen(fg_console);
	printk("Console: %s %s %dx%d",
		vc_cons[currcons].d->vc_can_do_color ? "colour" : "mono",
		display_desc, vc_cons[currcons].d->vc_cols, vc_cons[currcons].d->vc_rows);
	printable = 1;
	printk("\n");

	release_console_sem();

#ifdef CONFIG_VT_CONSOLE
	register_console(&vt_console_driver);
#endif
	return 0;
}
console_initcall(con_init);

static struct tty_operations con_ops = {
	.open = con_open,
	.close = con_close,
	.write = con_write,
	.write_room = con_write_room,
	.put_char = con_put_char,
	.flush_chars = con_flush_chars,
	.chars_in_buffer = con_chars_in_buffer,
	.ioctl = vt_ioctl,
	.stop = con_stop,
	.start = con_start,
	.throttle = con_throttle,
	.unthrottle = con_unthrottle,
};

int __init vty_init(void)
{
	vcs_init();

	console_driver = alloc_tty_driver(MAX_NR_CONSOLES);
	if (!console_driver)
		panic("Couldn't allocate console driver\n");
	console_driver->owner = THIS_MODULE;
	console_driver->devfs_name = "vc/";
	console_driver->name = "tty";
	console_driver->name_base = 1;
	console_driver->major = TTY_MAJOR;
	console_driver->minor_start = 1;
	console_driver->type = TTY_DRIVER_TYPE_CONSOLE;
	console_driver->init_termios = tty_std_termios;
	console_driver->flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_RESET_TERMIOS;
	tty_set_operations(console_driver, &con_ops);
	if (tty_register_driver(console_driver))
		panic("Couldn't register console driver\n");

	kbd_init();
	console_map_init();
#ifdef CONFIG_PROM_CONSOLE
	prom_con_init();
#endif
#ifdef CONFIG_MDA_CONSOLE
	mda_console_init();
#endif
	return 0;
}

#ifndef VT_SINGLE_DRIVER

/*
 *	If we support more console drivers, this function is used
 *	when a driver wants to take over some existing consoles
 *	and become default driver for newly opened ones.
 */

int take_over_console(const struct consw *csw, int first, int last, int deflt)
{
	int i, j = -1;
	const char *desc;
	struct module *owner;

	owner = csw->owner;
	if (!try_module_get(owner))
		return -ENODEV;

	acquire_console_sem();

	desc = csw->con_startup();
	if (!desc) {
		release_console_sem();
		module_put(owner);
		return -ENODEV;
	}
	if (deflt) {
		if (conswitchp)
			module_put(conswitchp->owner);
		__module_get(owner);
		conswitchp = csw;
	}

	for (i = first; i <= last; i++) {
		int old_was_color;
		int currcons = i;

		if (con_driver_map[i])
			module_put(con_driver_map[i]->owner);
		__module_get(owner);
		con_driver_map[i] = csw;

		if (!vc_cons[i].d || !vc_cons[i].d->vc_sw)
			continue;

		j = i;
		if (IS_VISIBLE)
			save_screen(i);
		old_was_color = vc_cons[i].d->vc_can_do_color;
		vc_cons[i].d->vc_sw->con_deinit(vc_cons[i].d);
		origin = (unsigned long) screenbuf;
		visible_origin = origin;
		scr_end = origin + screenbuf_size;
		pos = origin + vc_cons[currcons].d->vc_size_row*y + 2*x;
		visual_init(i, 0);
		update_attr(i);

		/* If the console changed between mono <-> color, then
		 * the attributes in the screenbuf will be wrong.  The
		 * following resets all attributes to something sane.
		 */
		if (old_was_color != vc_cons[i].d->vc_can_do_color)
			clear_buffer_attributes(i);

		if (IS_VISIBLE)
			update_screen(i);
	}
	printk("Console: switching ");
	if (!deflt)
		printk("consoles %d-%d ", first+1, last+1);
	if (j >= 0)
		printk("to %s %s %dx%d\n",
		       vc_cons[j].d->vc_can_do_color ? "colour" : "mono",
		       desc, vc_cons[j].d->vc_cols, vc_cons[j].d->vc_rows);
	else
		printk("to %s\n", desc);

	release_console_sem();

	module_put(owner);
	return 0;
}

void give_up_console(const struct consw *csw)
{
	int i;

	for(i = 0; i < MAX_NR_CONSOLES; i++)
		if (con_driver_map[i] == csw) {
			module_put(csw->owner);
			con_driver_map[i] = NULL;
		}
}

#endif

/*
 *	Screen blanking
 */

static void set_vesa_blanking(char __user *p)
{
    unsigned int mode;
    get_user(mode, p + 1);
    vesa_blank_mode = (mode < 4) ? mode : 0;
}

/*
 * This is called by a timer handler
 */
static void vesa_powerdown(void)
{
    struct vc_data *c = vc_cons[fg_console].d;
    /*
     *  Power down if currently suspended (1 or 2),
     *  suspend if currently blanked (0),
     *  else do nothing (i.e. already powered down (3)).
     *  Called only if powerdown features are allowed.
     */
    switch (vesa_blank_mode) {
    case VESA_NO_BLANKING:
	    c->vc_sw->con_blank(c, VESA_VSYNC_SUSPEND+1, 0);
	    break;
    case VESA_VSYNC_SUSPEND:
    case VESA_HSYNC_SUSPEND:
	    c->vc_sw->con_blank(c, VESA_POWERDOWN+1, 0);
	    break;
    }
}

void do_blank_screen(int entering_gfx)
{
	int currcons = fg_console;
	int i;

	WARN_CONSOLE_UNLOCKED();

	if (console_blanked) {
		if (blank_state == blank_vesa_wait) {
			blank_state = blank_off;
			vesa_powerdown();

		}
		return;
	}
	if (blank_state != blank_normal_wait)
		return;
	blank_state = blank_off;

	/* entering graphics mode? */
	if (entering_gfx) {
		hide_cursor(vc_cons[currcons].d);
		save_screen(currcons);
		sw->con_blank(vc_cons[currcons].d, -1, 1);
		console_blanked = fg_console + 1;
		set_origin(currcons);
		return;
	}

	/* don't blank graphics */
	if (vcmode != KD_TEXT) {
		console_blanked = fg_console + 1;
		return;
	}

	hide_cursor(vc_cons[currcons].d);
	del_timer_sync(&console_timer);
	blank_timer_expired = 0;

	save_screen(currcons);
	/* In case we need to reset origin, blanking hook returns 1 */
	i = sw->con_blank(vc_cons[currcons].d, 1, 0);
	console_blanked = fg_console + 1;
	if (i)
		set_origin(currcons);

	if (console_blank_hook && console_blank_hook(1))
		return;

	if (vesa_off_interval) {
		blank_state = blank_vesa_wait,
		mod_timer(&console_timer, jiffies + vesa_off_interval);
	}

    	if (vesa_blank_mode)
		sw->con_blank(vc_cons[currcons].d, vesa_blank_mode + 1, 0);
}
EXPORT_SYMBOL(do_blank_screen);

/*
 * Called by timer as well as from vt_console_driver
 */
void do_unblank_screen(int leaving_gfx)
{
	int currcons;

	WARN_CONSOLE_UNLOCKED();

	ignore_poke = 0;
	if (!console_blanked)
		return;
	if (!vc_cons_allocated(fg_console)) {
		/* impossible */
		printk("unblank_screen: tty %d not allocated ??\n", fg_console+1);
		return;
	}
	currcons = fg_console;
	if (vcmode != KD_TEXT)
		return; /* but leave console_blanked != 0 */

	if (blankinterval) {
		mod_timer(&console_timer, jiffies + blankinterval);
		blank_state = blank_normal_wait;
	}

	console_blanked = 0;
	if (sw->con_blank(vc_cons[currcons].d, 0, leaving_gfx))
		/* Low-level driver cannot restore -> do it ourselves */
		update_screen(fg_console);
	if (console_blank_hook)
		console_blank_hook(0);
	set_palette(currcons);
	set_cursor(vc_cons[fg_console].d);
}
EXPORT_SYMBOL(do_unblank_screen);

/*
 * This is called by the outside world to cause a forced unblank, mostly for
 * oopses. Currently, I just call do_unblank_screen(0), but we could eventually
 * call it with 1 as an argument and so force a mode restore... that may kill
 * X or at least garbage the screen but would also make the Oops visible...
 */
void unblank_screen(void)
{
	do_unblank_screen(0);
}

/*
 * We defer the timer blanking to work queue so it can take the console semaphore
 * (console operations can still happen at irq time, but only from printk which
 * has the console semaphore. Not perfect yet, but better than no locking
 */
static void blank_screen_t(unsigned long dummy)
{
	blank_timer_expired = 1;
	schedule_work(&console_work);
}

void poke_blanked_console(void)
{
	WARN_CONSOLE_UNLOCKED();

	/* This isn't perfectly race free, but a race here would be mostly harmless,
	 * at worse, we'll do a spurrious blank and it's unlikely
	 */
	del_timer(&console_timer);
	blank_timer_expired = 0;

	if (ignore_poke || !vt_cons[fg_console] || vt_cons[fg_console]->vc_mode == KD_GRAPHICS)
		return;
	if (console_blanked)
		unblank_screen();
	else if (blankinterval) {
		mod_timer(&console_timer, jiffies + blankinterval);
		blank_state = blank_normal_wait;
	}
}

/*
 *	Palettes
 */

void set_palette(int currcons)
{
	WARN_CONSOLE_UNLOCKED();

	if (vcmode != KD_GRAPHICS)
		sw->con_set_palette(vc_cons[currcons].d, color_table);
}

static int set_get_cmap(unsigned char __user *arg, int set)
{
    int i, j, k;

    WARN_CONSOLE_UNLOCKED();

    for (i = 0; i < 16; i++)
	if (set) {
	    get_user(default_red[i], arg++);
	    get_user(default_grn[i], arg++);
	    get_user(default_blu[i], arg++);
	} else {
	    put_user(default_red[i], arg++);
	    put_user(default_grn[i], arg++);
	    put_user(default_blu[i], arg++);
	}
    if (set) {
	for (i = 0; i < MAX_NR_CONSOLES; i++)
	    if (vc_cons_allocated(i)) {
		for (j = k = 0; j < 16; j++) {
		    vc_cons[i].d->vc_palette[k++] = default_red[j];
		    vc_cons[i].d->vc_palette[k++] = default_grn[j];
		    vc_cons[i].d->vc_palette[k++] = default_blu[j];
		}
		set_palette(i);
	    }
    }
    return 0;
}

/*
 * Load palette into the DAC registers. arg points to a colour
 * map, 3 bytes per colour, 16 colours, range from 0 to 255.
 */

int con_set_cmap(unsigned char __user *arg)
{
	int rc;

	acquire_console_sem();
	rc = set_get_cmap (arg,1);
	release_console_sem();

	return rc;
}

int con_get_cmap(unsigned char __user *arg)
{
	int rc;

	acquire_console_sem();
	rc = set_get_cmap (arg,0);
	release_console_sem();

	return rc;
}

void reset_palette(int currcons)
{
	int j, k;
	for (j=k=0; j<16; j++) {
		palette[k++] = default_red[j];
		palette[k++] = default_grn[j];
		palette[k++] = default_blu[j];
	}
	set_palette(currcons);
}

/*
 *  Font switching
 *
 *  Currently we only support fonts up to 32 pixels wide, at a maximum height
 *  of 32 pixels. Userspace fontdata is stored with 32 bytes (shorts/ints, 
 *  depending on width) reserved for each character which is kinda wasty, but 
 *  this is done in order to maintain compatibility with the EGA/VGA fonts. It 
 *  is upto the actual low-level console-driver convert data into its favorite
 *  format (maybe we should add a `fontoffset' field to the `display'
 *  structure so we won't have to convert the fontdata all the time.
 *  /Jes
 */

#define max_font_size 65536

int con_font_get(int currcons, struct console_font_op *op)
{
	struct console_font font;
	int rc = -EINVAL;
	int c;

	if (vt_cons[currcons]->vc_mode != KD_TEXT)
		return -EINVAL;

	if (op->data) {
		font.data = kmalloc(max_font_size, GFP_KERNEL);
		if (!font.data)
			return -ENOMEM;
	} else
		font.data = NULL;

	acquire_console_sem();
	if (sw->con_font_get)
		rc = sw->con_font_get(vc_cons[currcons].d, &font);
	else
		rc = -ENOSYS;
	release_console_sem();

	if (rc)
		goto out;

	c = (font.width+7)/8 * 32 * font.charcount;
	
	if (op->data && font.charcount > op->charcount)
		rc = -ENOSPC;
	if (!(op->flags & KD_FONT_FLAG_OLD)) {
		if (font.width > op->width || font.height > op->height) 
			rc = -ENOSPC;
	} else {
		if (font.width != 8)
			rc = -EIO;
		else if ((op->height && font.height > op->height) ||
			 font.height > 32)
			rc = -ENOSPC;
	}
	if (rc)
		goto out;

	op->height = font.height;
	op->width = font.width;
	op->charcount = font.charcount;

	if (op->data && copy_to_user(op->data, font.data, c))
		rc = -EFAULT;

out:
	kfree(font.data);
	return rc;
}

int con_font_set(int currcons, struct console_font_op *op)
{
	struct console_font font;
	int rc = -EINVAL;
	int size;

	if (vt_cons[currcons]->vc_mode != KD_TEXT)
		return -EINVAL;
	if (!op->data)
		return -EINVAL;
	if (op->charcount > 512)
		return -EINVAL;
	if (!op->height) {		/* Need to guess font height [compat] */
		int h, i;
		u8 __user *charmap = op->data;
		u8 tmp;
		
		/* If from KDFONTOP ioctl, don't allow things which can be done in userland,
		   so that we can get rid of this soon */
		if (!(op->flags & KD_FONT_FLAG_OLD))
			return -EINVAL;
		for (h = 32; h > 0; h--)
			for (i = 0; i < op->charcount; i++) {
				if (get_user(tmp, &charmap[32*i+h-1]))
					return -EFAULT;
				if (tmp)
					goto nonzero;
			}
		return -EINVAL;
	nonzero:
		op->height = h;
	}
	if (op->width <= 0 || op->width > 32 || op->height > 32)
		return -EINVAL;
	size = (op->width+7)/8 * 32 * op->charcount;
	if (size > max_font_size)
		return -ENOSPC;
	font.charcount = op->charcount;
	font.height = op->height;
	font.width = op->width;
	font.data = kmalloc(size, GFP_KERNEL);
	if (!font.data)
		return -ENOMEM;
	if (copy_from_user(font.data, op->data, size)) {
		kfree(font.data);
		return -EFAULT;
	}
	acquire_console_sem();
	if (sw->con_font_set)
		rc = sw->con_font_set(vc_cons[currcons].d, &font, op->flags);
	else
		rc = -ENOSYS;
	release_console_sem();
	kfree(font.data);
	return rc;
}

int con_font_default(int currcons, struct console_font_op *op)
{
	struct console_font font = {.width = op->width, .height = op->height};
	char name[MAX_FONT_NAME];
	char *s = name;
	int rc;

	if (vt_cons[currcons]->vc_mode != KD_TEXT)
		return -EINVAL;

	if (!op->data)
		s = NULL;
	else if (strncpy_from_user(name, op->data, MAX_FONT_NAME - 1) < 0)
		return -EFAULT;
	else
		name[MAX_FONT_NAME - 1] = 0;

	acquire_console_sem();
	if (sw->con_font_default)
		rc = sw->con_font_default(vc_cons[currcons].d, &font, s);
	else
		rc = -ENOSYS;
	release_console_sem();
	if (!rc) {
		op->width = font.width;
		op->height = font.height;
	}
	return rc;
}

int con_font_copy(int currcons, struct console_font_op *op)
{
	int con = op->height;
	struct vc_data *vc;
	int rc;

	if (vt_cons[currcons]->vc_mode != KD_TEXT)
		return -EINVAL;

	acquire_console_sem();
	vc = vc_cons[currcons].d;
	if (!sw->con_font_copy)
		rc = -ENOSYS;
	else if (con < 0 || !vc_cons_allocated(con))
		rc = -ENOTTY;
	else if (con == vc->vc_num)	/* nothing to do */
		rc = 0;
	else
		rc = sw->con_font_copy(vc, con);
	release_console_sem();
	return rc;
}

int con_font_op(int currcons, struct console_font_op *op)
{
	switch (op->op) {
	case KD_FONT_OP_SET:
		return con_font_set(currcons, op);
	case KD_FONT_OP_GET:
		return con_font_get(currcons, op);
	case KD_FONT_OP_SET_DEFAULT:
		return con_font_default(currcons, op);
	case KD_FONT_OP_COPY:
		return con_font_copy(currcons, op);
	}
	return -ENOSYS;
}

/*
 *	Interface exported to selection and vcs.
 */

/* used by selection */
u16 screen_glyph(struct vc_data *vc, int offset)
{
	u16 w = scr_readw(screenpos(vc, offset, 1));
	u16 c = w & 0xff;

	if (w & vc->vc_hi_font_mask)
		c |= 0x100;
	return c;
}

/* used by vcs - note the word offset */
unsigned short *screen_pos(struct vc_data *vc, int w_offset, int viewed)
{
	return screenpos(vc, 2 * w_offset, viewed);
}

void getconsxy(struct vc_data *vc, unsigned char *p)
{
	p[0] = vc->vc_x;
	p[1] = vc->vc_y;
}

void putconsxy(struct vc_data *vc, unsigned char *p)
{
	gotoxy(vc, p[0], p[1]);
	set_cursor(vc);
}

u16 vcs_scr_readw(struct vc_data *vc, const u16 *org)
{
	if ((unsigned long)org == vc->vc_pos && softcursor_original != -1)
		return softcursor_original;
	return scr_readw(org);
}

void vcs_scr_writew(struct vc_data *vc, u16 val, u16 *org)
{
	scr_writew(val, org);
	if ((unsigned long)org == vc->vc_pos) {
		softcursor_original = -1;
		add_softcursor(vc);
	}
}

static int pm_con_request(struct pm_dev *dev, pm_request_t rqst, void *data)
{
	switch (rqst)
	{
	case PM_RESUME:
		acquire_console_sem();
		unblank_screen();
		release_console_sem();
		break;
	case PM_SUSPEND:
		acquire_console_sem();
		do_blank_screen(0);
		release_console_sem();
		break;
	}
	return 0;
}

/*
 *	Visible symbols for modules
 */

EXPORT_SYMBOL(color_table);
EXPORT_SYMBOL(default_red);
EXPORT_SYMBOL(default_grn);
EXPORT_SYMBOL(default_blu);
EXPORT_SYMBOL(update_region);
EXPORT_SYMBOL(redraw_screen);
EXPORT_SYMBOL(vc_resize);
EXPORT_SYMBOL(fg_console);
EXPORT_SYMBOL(console_blank_hook);
EXPORT_SYMBOL(console_blanked);
EXPORT_SYMBOL(vt_cons);
EXPORT_SYMBOL(vc_cons);
#ifndef VT_SINGLE_DRIVER
EXPORT_SYMBOL(take_over_console);
EXPORT_SYMBOL(give_up_console);
#endif
