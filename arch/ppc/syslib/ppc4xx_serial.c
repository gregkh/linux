/*
 * arch/ppc/syslib/ppc405_serial.c
 *
 * Author: MontaVista Software, Inc.
 *         	frank_rowand@mvista.com or source@mvista.com
 * 	   	debbie_chu@mvista.com
 *
 * This is a fairly standard 165xx type device that will eventually
 * be merged with other similar processor/boards.	-- Dan
 *
 * 2000 (c) MontaVista, Software, Inc.  This file is licensed under
 * the terms of the GNU General Public License version 2.  This program
 * is licensed "as is" without any warranty of any kind, whether express
 * or implied.
 *
 * Console I/O support for Early kernel bringup.
 */

#include <linux/config.h>

#if defined(CONFIG_IBM405GP) || defined(CONFIG_IBM405CR)

#ifdef CONFIG_KGDB
#include <asm/kgdb.h>
#include <linux/init.h>
#endif

#ifdef CONFIG_DEBUG_BRINGUP

#include <linux/console.h>

extern void ftr_reset_preferred_console(void);


static int ppc405_sercons_setup(struct console *co, char *options)
{
#ifdef CONFIG_UART0_DEBUG_CONSOLE
    volatile unsigned char *uart_dll  = (char *)0xef600300;
    volatile unsigned char *uart_fcr  = (char *)0xef600302;
    volatile unsigned char *uart_lcr  = (char *)0xef600303;
#endif

#ifdef CONFIG_UART1_DEBUG_CONSOLE
    volatile unsigned char *uart_dll  = (char *)0xef600400;
    volatile unsigned char *uart_fcr  = (char *)0xef600402;
    volatile unsigned char *uart_lcr  = (char *)0xef600403;
#endif

    *uart_lcr = *uart_lcr | 0x80;   /* DLAB on  */

/* ftr revisit - there is no config option for this
**  also see include/asm-ppc/ppc405_serial.h
**
** #define CONFIG_IBM405GP_INTERNAL_CLOCK
*/


#ifdef  CONFIG_IBM405GP_INTERNAL_CLOCK
    /* ftr revisit
    ** why is bit 19 of chcr0 (0x1000) being set?
    */
    /* 0x2a results in data corruption, kgdb works with 0x28 */
    *uart_dll = 0x28;		    /* 9600 baud */
    _put_CHCR0((_get_CHCR0() & 0xffffe000) | 0x103e);
#else
    *uart_dll = 0x48;		    /* 9600 baud */
#endif
    *uart_lcr = *uart_lcr & 0x7f;   /* DLAB off */

    return 0;
}


/*
 * This is a bringup hack, writing directly to uart0 or uart1
 */

static void
ppc405_sercons_write(struct console *co, const char *ptr,
            unsigned nb)
{
    int i;

#ifdef CONFIG_UART0_DEBUG_CONSOLE
    volatile unsigned char *uart_xmit = (char *)0xef600300;
    volatile unsigned char *uart_lsr  = (char *)0xef600305;
#endif

#ifdef CONFIG_UART1_DEBUG_CONSOLE
    volatile unsigned char *uart_xmit = (char *)0xef600400;
    volatile unsigned char *uart_lsr  = (char *)0xef600405;
#endif

    for (i = 0; i < nb; ++i) {

	/* wait for transmit reg (possibly fifo) to empty */
	while ((*uart_lsr & 0x40) == 0)
	    ;

	*uart_xmit = (ptr[i] & 0xff);

	if (ptr[i] == '\n') {

	    /* add a carriage return */

	    /* wait for transmit reg (possibly fifo) to empty */
	    while ((*uart_lsr & 0x40) == 0)
		;

	    *uart_xmit = '\r';
	}
    }

    return;
}


static int
ppc405_sercons_read(struct console *co, char *ptr, unsigned nb)
{
#ifdef CONFIG_UART0_DEBUG_CONSOLE
    volatile unsigned char *uart_rcv  = (char *)0xef600300;
    volatile unsigned char *uart_lsr  = (char *)0xef600305;
#endif

#ifdef CONFIG_UART1_DEBUG_CONSOLE
    volatile unsigned char *uart_rcv  = (char *)0xef600400;
    volatile unsigned char *uart_lsr  = (char *)0xef600405;
#endif


    /* ftr revisit: not tested */

    if (nb == 0)
	return(0);

    if (!ptr)
	return(-1);

    /* wait for receive reg (possibly fifo) to contain data */
    while ((*uart_lsr & 0x01) == 0)
	;

    *ptr = *uart_rcv;

    return(1);
}

static struct console ppc405_sercons = {
	.name =		"dbg_cons",
	.write =	ppc405_console_write,
	.setup =	ppc405_console_setup,
	.flags =	CON_PRINTBUFFER,
	.index =	-1,
};

void
register_debug_console(void)
{
	register_console(&ppc405_sercons);
}

void
unregister_debug_console(void)
{
	unregister_console(&ppc405_sercons);
}

#endif	/* CONFIG_DEBUG_BRINGUP */

#endif	/* #if defined(CONFIG_IBM405GP) || defined(CONFIG_IBM405CR) */
