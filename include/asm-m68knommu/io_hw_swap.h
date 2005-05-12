#ifndef _M68K_IO_HW_SWAP_H
#define _M68K_IO_HW_SWAP_H

/*
 * swap functions are sometimes needed to interface little-endian hardware
 */
static inline unsigned short _swapw(volatile unsigned short v)
{
    return ((v << 8) | (v >> 8));
}

static inline unsigned int _swapl(volatile unsigned long v)
{
    return ((v << 24) | ((v & 0xff00) << 8) | ((v & 0xff0000) >> 8) | (v >> 24));
}

/*
 * readX/writeX() are used to access memory mapped devices. On some
 * architectures the memory mapped IO stuff needs to be accessed
 * differently. On the m68k architecture, we just read/write the
 * memory location directly.
 */
/* ++roman: The assignments to temp. vars avoid that gcc sometimes generates
 * two accesses to memory, which may be undesireable for some devices.
 */
#define readb(addr) \
    ({ unsigned char __v = (*(volatile unsigned char *) (addr)); __v; })
#define readw(addr) \
    ({ unsigned short __v = (*(volatile unsigned short *) (addr)); __v; })
#define readl(addr) \
    ({ unsigned int __v = (*(volatile unsigned int *) (addr)); __v; })

#define writeb(b,addr) ((*(volatile unsigned char *) (addr)) = (b))
#define writew(b,addr) ((*(volatile unsigned short *) (addr)) = (b))
#define writel(b,addr) ((*(volatile unsigned int *) (addr)) = (b))

/* There is no difference between I/O and memory on 68k, these are the same */
#define inb(addr) \
    ({ unsigned char __v = (*(volatile unsigned char *) (addr)); __v; })
#define inw(addr) \
    ({ unsigned short __v = (*(volatile unsigned short *) (addr)); \
       _swapw(__v); })
#define inl(addr) \
    ({ unsigned int __v = (*(volatile unsigned int *) (addr)); _swapl(__v); })

#define outb(b,addr) ((*(volatile unsigned char *) (addr)) = (b))
#define outw(b,addr) ((*(volatile unsigned short *) (addr)) = (_swapw(b)))
#define outl(b,addr) ((*(volatile unsigned int *) (addr)) = (_swapl(b)))

/* FIXME: these need to be optimized.  Watch out for byte swapping, they
 * are used mostly for Intel devices... */
#define outsw(addr,buf,len) \
    ({ unsigned short * __p = (unsigned short *)(buf); \
       unsigned short * __e = (unsigned short *)(__p) + (len); \
       while (__p < __e) { \
	  *(volatile unsigned short *)(addr) = *__p++;\
       } \
     })

#define insw(addr,buf,len) \
    ({ unsigned short * __p = (unsigned short *)(buf); \
       unsigned short * __e = (unsigned short *)(__p) + (len); \
       while (__p < __e) { \
          *(__p++) = *(volatile unsigned short *)(addr); \
       } \
     })


static inline unsigned char get_user_byte_io(const char * addr)
{
	register unsigned char _v;

	__asm__ __volatile__ ("moveb %1,%0":"=dm" (_v):"m" (*addr));
	return _v;
}
#define inb_p(addr) get_user_byte_io((char *)(addr))

static inline void put_user_byte_io(char val,char *addr)
{
	__asm__ __volatile__ ("moveb %0,%1"
			      : /* no outputs */
			      :"idm" (val),"m" (*addr)
			      : "memory");
}
#define outb_p(x,addr) put_user_byte_io((x),(char *)(addr))

/*
 * Change virtual addresses to physical addresses and vv.
 * These are trivial on the 1:1 Linux/i386 mapping (but if we ever
 * make the kernel segment mapped at 0, we need to do translation
 * on the i386 as well)
 */
extern unsigned long mm_vtop(unsigned long addr);
extern unsigned long mm_ptov(unsigned long addr);

extern inline unsigned long virt_to_phys(volatile void * address)
{
	return (unsigned long) mm_vtop((unsigned long)address);
}

extern inline void * phys_to_virt(unsigned long address)
{
	return (void *) mm_ptov(address);
}

/*
 * IO bus memory addresses are also 1:1 with the physical address
 */
#define virt_to_bus virt_to_phys
#define bus_to_virt phys_to_virt


#endif /* _M68K_IO_HW_SWAP_H */
