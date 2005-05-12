/* -*- linux-c -*-
 *
 *	$Id: sysrq.h,v 1.3 1997/07/17 11:54:33 mj Exp $
 *
 *	Linux Magic System Request Key Hacks
 *
 *	(c) 1997 Martin Mares <mj@atrey.karlin.mff.cuni.cz>
 *
 *	(c) 2000 Crutcher Dunnavant <crutcher+kernel@datastacks.com>
 *	overhauled to use key registration
 *	based upon discusions in irc://irc.openprojects.net/#kernelnewbies
 */

#include <linux/config.h>

struct pt_regs;
struct tty_struct;

struct sysrq_key_op {
	void (*handler)(int, struct pt_regs *, struct tty_struct *);
	char *help_msg;
	char *action_msg;
};

#ifdef CONFIG_MAGIC_SYSRQ

/* Generic SysRq interface -- you may call it from any device driver, supplying
 * ASCII code of the key, pointer to registers and kbd/tty structs (if they
 * are available -- else NULL's).
 */

void handle_sysrq(int, struct pt_regs *, struct tty_struct *);
void __handle_sysrq(int, struct pt_regs *, struct tty_struct *);
int register_sysrq_key(int, struct sysrq_key_op *);
int unregister_sysrq_key(int, struct sysrq_key_op *);
struct sysrq_key_op *__sysrq_get_key_op(int key);

#else

static inline int __reterr(void)
{
	return -EINVAL;
}

#define register_sysrq_key(ig,nore) __reterr()
#define unregister_sysrq_key(ig,nore) __reterr()

#endif
