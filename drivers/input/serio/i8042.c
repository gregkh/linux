/*
 *  i8042 keyboard and mouse controller driver for Linux
 *
 *  Copyright (c) 1999-2004 Vojtech Pavlik
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/config.h>
#include <linux/init.h>
#include <linux/serio.h>
#include <linux/err.h>

#include <asm/io.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@suse.cz>");
MODULE_DESCRIPTION("i8042 keyboard and mouse controller driver");
MODULE_LICENSE("GPL");

static unsigned int i8042_noaux;
module_param_named(noaux, i8042_noaux, bool, 0);
MODULE_PARM_DESC(noaux, "Do not probe or use AUX (mouse) port.");

static unsigned int i8042_nomux;
module_param_named(nomux, i8042_nomux, bool, 0);
MODULE_PARM_DESC(nomux, "Do not check whether an active multiplexing conrtoller is present.");

static unsigned int i8042_unlock;
module_param_named(unlock, i8042_unlock, bool, 0);
MODULE_PARM_DESC(unlock, "Ignore keyboard lock.");

static unsigned int i8042_reset;
module_param_named(reset, i8042_reset, bool, 0);
MODULE_PARM_DESC(reset, "Reset controller during init and cleanup.");

static unsigned int i8042_direct;
module_param_named(direct, i8042_direct, bool, 0);
MODULE_PARM_DESC(direct, "Put keyboard port into non-translated mode.");

static unsigned int i8042_dumbkbd;
module_param_named(dumbkbd, i8042_dumbkbd, bool, 0);
MODULE_PARM_DESC(dumbkbd, "Pretend that controller can only read data from keyboard");

static unsigned int i8042_noloop;
module_param_named(noloop, i8042_noloop, bool, 0);
MODULE_PARM_DESC(dumbkbd, "Disable the AUX Loopback command while probing for the AUX port");

#ifdef CONFIG_ACPI
static int i8042_noacpi;
module_param_named(noacpi, i8042_noacpi, bool, 0);
MODULE_PARM_DESC(noacpi, "Do not use ACPI to detect controller settings");
#endif

#define DEBUG
#ifdef DEBUG
static int i8042_debug;
module_param_named(debug, i8042_debug, bool, 0600);
MODULE_PARM_DESC(debug, "Turn i8042 debugging mode on and off");
#endif

__obsolete_setup("i8042_noaux");
__obsolete_setup("i8042_nomux");
__obsolete_setup("i8042_unlock");
__obsolete_setup("i8042_reset");
__obsolete_setup("i8042_direct");
__obsolete_setup("i8042_dumbkbd");

#include "i8042.h"

DEFINE_SPINLOCK(i8042_lock);

struct i8042_values {
	int irq;
	unsigned char disable;
	unsigned char irqen;
	unsigned char exists;
	signed char mux;
	char name[8];
};

static struct i8042_values i8042_kbd_values = {
	.disable	= I8042_CTR_KBDDIS,
	.irqen 		= I8042_CTR_KBDINT,
	.mux		= -1,
	.name		= "KBD",
};

static struct i8042_values i8042_aux_values = {
	.disable	= I8042_CTR_AUXDIS,
	.irqen		= I8042_CTR_AUXINT,
	.mux		= -1,
	.name		= "AUX",
};

static struct i8042_values i8042_mux_values[I8042_NUM_MUX_PORTS];

static struct serio *i8042_kbd_port;
static struct serio *i8042_aux_port;
static struct serio *i8042_mux_port[I8042_NUM_MUX_PORTS];
static unsigned char i8042_initial_ctr;
static unsigned char i8042_ctr;
static unsigned char i8042_mux_open;
static unsigned char i8042_mux_present;
static struct timer_list i8042_timer;
static struct platform_device *i8042_platform_device;

/*
 * Shared IRQ's require a device pointer, but this driver doesn't support
 * multiple devices
 */
#define i8042_request_irq_cookie (&i8042_timer)

static irqreturn_t i8042_interrupt(int irq, void *dev_id, struct pt_regs *regs);

/*
 * The i8042_wait_read() and i8042_wait_write functions wait for the i8042 to
 * be ready for reading values from it / writing values to it.
 * Called always with i8042_lock held.
 */

static int i8042_wait_read(void)
{
	int i = 0;
	while ((~i8042_read_status() & I8042_STR_OBF) && (i < I8042_CTL_TIMEOUT)) {
		udelay(50);
		i++;
	}
	return -(i == I8042_CTL_TIMEOUT);
}

static int i8042_wait_write(void)
{
	int i = 0;
	while ((i8042_read_status() & I8042_STR_IBF) && (i < I8042_CTL_TIMEOUT)) {
		udelay(50);
		i++;
	}
	return -(i == I8042_CTL_TIMEOUT);
}

/*
 * i8042_flush() flushes all data that may be in the keyboard and mouse buffers
 * of the i8042 down the toilet.
 */

static int i8042_flush(void)
{
	unsigned long flags;
	unsigned char data;
	int i = 0;

	spin_lock_irqsave(&i8042_lock, flags);

	while ((i8042_read_status() & I8042_STR_OBF) && (i++ < I8042_BUFFER_SIZE)) {
		udelay(50);
		data = i8042_read_data();
		dbg("%02x <- i8042 (flush, %s)", data,
			i8042_read_status() & I8042_STR_AUXDATA ? "aux" : "kbd");
	}

	spin_unlock_irqrestore(&i8042_lock, flags);

	return i;
}

/*
 * i8042_command() executes a command on the i8042. It also sends the input
 * parameter(s) of the commands to it, and receives the output value(s). The
 * parameters are to be stored in the param array, and the output is placed
 * into the same array. The number of the parameters and output values is
 * encoded in bits 8-11 of the command number.
 */

static int i8042_command(unsigned char *param, int command)
{
	unsigned long flags;
	int retval = 0, i = 0;

	if (i8042_noloop && command == I8042_CMD_AUX_LOOP)
		return -1;

	spin_lock_irqsave(&i8042_lock, flags);

	retval = i8042_wait_write();
	if (!retval) {
		dbg("%02x -> i8042 (command)", command & 0xff);
		i8042_write_command(command & 0xff);
	}

	if (!retval)
		for (i = 0; i < ((command >> 12) & 0xf); i++) {
			if ((retval = i8042_wait_write())) break;
			dbg("%02x -> i8042 (parameter)", param[i]);
			i8042_write_data(param[i]);
		}

	if (!retval)
		for (i = 0; i < ((command >> 8) & 0xf); i++) {
			if ((retval = i8042_wait_read())) break;
			if (i8042_read_status() & I8042_STR_AUXDATA)
				param[i] = ~i8042_read_data();
			else
				param[i] = i8042_read_data();
			dbg("%02x <- i8042 (return)", param[i]);
		}

	spin_unlock_irqrestore(&i8042_lock, flags);

	if (retval)
		dbg("     -- i8042 (timeout)");

	return retval;
}

/*
 * i8042_kbd_write() sends a byte out through the keyboard interface.
 */

static int i8042_kbd_write(struct serio *port, unsigned char c)
{
	unsigned long flags;
	int retval = 0;

	spin_lock_irqsave(&i8042_lock, flags);

	if(!(retval = i8042_wait_write())) {
		dbg("%02x -> i8042 (kbd-data)", c);
		i8042_write_data(c);
	}

	spin_unlock_irqrestore(&i8042_lock, flags);

	return retval;
}

/*
 * i8042_aux_write() sends a byte out through the aux interface.
 */

static int i8042_aux_write(struct serio *port, unsigned char c)
{
	struct i8042_values *values = port->port_data;
	int retval;

/*
 * Send the byte out.
 */

	if (values->mux == -1)
		retval = i8042_command(&c, I8042_CMD_AUX_SEND);
	else
		retval = i8042_command(&c, I8042_CMD_MUX_SEND + values->mux);

/*
 * Make sure the interrupt happens and the character is received even
 * in the case the IRQ isn't wired, so that we can receive further
 * characters later.
 */

	i8042_interrupt(0, NULL, NULL);
	return retval;
}

/*
 * i8042_activate_port() enables port on a chip.
 */

static int i8042_activate_port(struct serio *port)
{
	struct i8042_values *values = port->port_data;

	i8042_flush();

	/*
	 * Enable port again here because it is disabled if we are
	 * resuming (normally it is enabled already).
	 */
	i8042_ctr &= ~values->disable;

	i8042_ctr |= values->irqen;

	if (i8042_command(&i8042_ctr, I8042_CMD_CTL_WCTR)) {
		i8042_ctr &= ~values->irqen;
		return -1;
	}

	return 0;
}


/*
 * i8042_open() is called when a port is open by the higher layer.
 * It allocates the interrupt and calls i8042_enable_port.
 */

static int i8042_open(struct serio *port)
{
	struct i8042_values *values = port->port_data;

	if (values->mux != -1)
		if (i8042_mux_open++)
			return 0;

	if (request_irq(values->irq, i8042_interrupt,
			SA_SHIRQ, "i8042", i8042_request_irq_cookie)) {
		printk(KERN_ERR "i8042.c: Can't get irq %d for %s, unregistering the port.\n", values->irq, values->name);
		goto irq_fail;
	}

	if (i8042_activate_port(port)) {
		printk(KERN_ERR "i8042.c: Can't activate %s, unregistering the port\n", values->name);
		goto activate_fail;
	}

	i8042_interrupt(0, NULL, NULL);

	return 0;

activate_fail:
	free_irq(values->irq, i8042_request_irq_cookie);

irq_fail:
	values->exists = 0;
	serio_unregister_port_delayed(port);

	return -1;
}

/*
 * i8042_close() frees the interrupt, so that it can possibly be used
 * by another driver. We never know - if the user doesn't have a mouse,
 * the BIOS could have used the AUX interrupt for PCI.
 */

static void i8042_close(struct serio *port)
{
	struct i8042_values *values = port->port_data;

	if (values->mux != -1)
		if (--i8042_mux_open)
			return;

	i8042_ctr &= ~values->irqen;

	if (i8042_command(&i8042_ctr, I8042_CMD_CTL_WCTR)) {
		printk(KERN_ERR "i8042.c: Can't write CTR while closing %s.\n", values->name);
		return;
	}

	free_irq(values->irq, i8042_request_irq_cookie);

	i8042_flush();
}

/*
 * i8042_interrupt() is the most important function in this driver -
 * it handles the interrupts from the i8042, and sends incoming bytes
 * to the upper layers.
 */

static irqreturn_t i8042_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned long flags;
	unsigned char str, data = 0;
	unsigned int dfl;
	unsigned int aux_idx;
	int ret;

	mod_timer(&i8042_timer, jiffies + I8042_POLL_PERIOD);

	spin_lock_irqsave(&i8042_lock, flags);
	str = i8042_read_status();
	if (str & I8042_STR_OBF)
		data = i8042_read_data();
	spin_unlock_irqrestore(&i8042_lock, flags);

	if (~str & I8042_STR_OBF) {
		if (irq) dbg("Interrupt %d, without any data", irq);
		ret = 0;
		goto out;
	}

	if (i8042_mux_present && (str & I8042_STR_AUXDATA)) {
		static unsigned long last_transmit;
		static unsigned char last_str;

		dfl = 0;
		if (str & I8042_STR_MUXERR) {
			dbg("MUX error, status is %02x, data is %02x", str, data);
			switch (data) {
				default:
/*
 * When MUXERR condition is signalled the data register can only contain
 * 0xfd, 0xfe or 0xff if implementation follows the spec. Unfortunately
 * it is not always the case. Some KBC just get confused which port the
 * data came from and signal error leaving the data intact. They _do not_
 * revert to legacy mode (actually I've never seen KBC reverting to legacy
 * mode yet, when we see one we'll add proper handling).
 * Anyway, we will assume that the data came from the same serio last byte
 * was transmitted (if transmission happened not too long ago).
 */
					if (time_before(jiffies, last_transmit + HZ/10)) {
						str = last_str;
						break;
					}
					/* fall through - report timeout */
				case 0xfd:
				case 0xfe: dfl = SERIO_TIMEOUT; data = 0xfe; break;
				case 0xff: dfl = SERIO_PARITY;  data = 0xfe; break;
			}
		}

		aux_idx = (str >> 6) & 3;

		dbg("%02x <- i8042 (interrupt, aux%d, %d%s%s)",
			data, aux_idx, irq,
			dfl & SERIO_PARITY ? ", bad parity" : "",
			dfl & SERIO_TIMEOUT ? ", timeout" : "");

		if (likely(i8042_mux_values[aux_idx].exists))
			serio_interrupt(i8042_mux_port[aux_idx], data, dfl, regs);

		last_str = str;
		last_transmit = jiffies;
		goto irq_ret;
	}

	dfl = ((str & I8042_STR_PARITY) ? SERIO_PARITY : 0) |
	      ((str & I8042_STR_TIMEOUT) ? SERIO_TIMEOUT : 0);

	dbg("%02x <- i8042 (interrupt, %s, %d%s%s)",
		data, (str & I8042_STR_AUXDATA) ? "aux" : "kbd", irq,
		dfl & SERIO_PARITY ? ", bad parity" : "",
		dfl & SERIO_TIMEOUT ? ", timeout" : "");


	if (str & I8042_STR_AUXDATA) {
		if (likely(i8042_aux_values.exists))
			serio_interrupt(i8042_aux_port, data, dfl, regs);
	} else {
		if (likely(i8042_kbd_values.exists))
			serio_interrupt(i8042_kbd_port, data, dfl, regs);
	}

irq_ret:
	ret = 1;
out:
	return IRQ_RETVAL(ret);
}

/*
 * i8042_set_mux_mode checks whether the controller has an active
 * multiplexor and puts the chip into Multiplexed (1) or Legacy (0) mode.
 */

static int i8042_set_mux_mode(unsigned int mode, unsigned char *mux_version)
{

	unsigned char param;
/*
 * Get rid of bytes in the queue.
 */

	i8042_flush();

/*
 * Internal loopback test - send three bytes, they should come back from the
 * mouse interface, the last should be version. Note that we negate mouseport
 * command responses for the i8042_check_aux() routine.
 */

	param = 0xf0;
	if (i8042_command(&param, I8042_CMD_AUX_LOOP) || param != 0x0f)
		return -1;
	param = mode ? 0x56 : 0xf6;
	if (i8042_command(&param, I8042_CMD_AUX_LOOP) || param != (mode ? 0xa9 : 0x09))
		return -1;
	param = mode ? 0xa4 : 0xa5;
	if (i8042_command(&param, I8042_CMD_AUX_LOOP) || param == (mode ? 0x5b : 0x5a))
		return -1;

	if (mux_version)
		*mux_version = ~param;

	return 0;
}


/*
 * i8042_enable_mux_ports enables 4 individual AUX ports after
 * the controller has been switched into Multiplexed mode
 */

static int i8042_enable_mux_ports(struct i8042_values *values)
{
	unsigned char param;
	int i;
/*
 * Disable all muxed ports by disabling AUX.
 */

	i8042_ctr |= I8042_CTR_AUXDIS;
	i8042_ctr &= ~I8042_CTR_AUXINT;

	if (i8042_command(&i8042_ctr, I8042_CMD_CTL_WCTR)) {
		printk(KERN_ERR "i8042.c: Failed to disable AUX port, can't use MUX.\n");
		return -1;
	}

/*
 * Enable all muxed ports.
 */

	for (i = 0; i < 4; i++) {
		i8042_command(&param, I8042_CMD_MUX_PFX + i);
		i8042_command(&param, I8042_CMD_AUX_ENABLE);
	}

	return 0;
}


/*
 * i8042_check_mux() checks whether the controller supports the PS/2 Active
 * Multiplexing specification by Synaptics, Phoenix, Insyde and
 * LCS/Telegraphics.
 */

static int __init i8042_check_mux(struct i8042_values *values)
{
	unsigned char mux_version;

	if (i8042_set_mux_mode(1, &mux_version))
		return -1;

	/* Workaround for interference with USB Legacy emulation */
	/* that causes a v10.12 MUX to be found. */
	if (mux_version == 0xAC)
		return -1;

	printk(KERN_INFO "i8042.c: Detected active multiplexing controller, rev %d.%d.\n",
		(mux_version >> 4) & 0xf, mux_version & 0xf);

	if (i8042_enable_mux_ports(values))
		return -1;

	i8042_mux_present = 1;
	return 0;
}


/*
 * i8042_check_aux() applies as much paranoia as it can at detecting
 * the presence of an AUX interface.
 */

static int __init i8042_check_aux(struct i8042_values *values)
{
	unsigned char param;
	static int i8042_check_aux_cookie;

/*
 * Check if AUX irq is available. If it isn't, then there is no point
 * in trying to detect AUX presence.
 */

	if (request_irq(values->irq, i8042_interrupt, SA_SHIRQ,
				"i8042", &i8042_check_aux_cookie))
                return -1;
	free_irq(values->irq, &i8042_check_aux_cookie);

/*
 * Get rid of bytes in the queue.
 */

	i8042_flush();

/*
 * Internal loopback test - filters out AT-type i8042's. Unfortunately
 * SiS screwed up and their 5597 doesn't support the LOOP command even
 * though it has an AUX port.
 */

	param = 0x5a;
	if (i8042_command(&param, I8042_CMD_AUX_LOOP) || param != 0xa5) {

/*
 * External connection test - filters out AT-soldered PS/2 i8042's
 * 0x00 - no error, 0x01-0x03 - clock/data stuck, 0xff - general error
 * 0xfa - no error on some notebooks which ignore the spec
 * Because it's common for chipsets to return error on perfectly functioning
 * AUX ports, we test for this only when the LOOP command failed.
 */

		if (i8042_command(&param, I8042_CMD_AUX_TEST)
		    	|| (param && param != 0xfa && param != 0xff))
				return -1;
	}

/*
 * Bit assignment test - filters out PS/2 i8042's in AT mode
 */

	if (i8042_command(&param, I8042_CMD_AUX_DISABLE))
		return -1;
	if (i8042_command(&param, I8042_CMD_CTL_RCTR) || (~param & I8042_CTR_AUXDIS)) {
		printk(KERN_WARNING "Failed to disable AUX port, but continuing anyway... Is this a SiS?\n");
		printk(KERN_WARNING "If AUX port is really absent please use the 'i8042.noaux' option.\n");
	}

	if (i8042_command(&param, I8042_CMD_AUX_ENABLE))
		return -1;
	if (i8042_command(&param, I8042_CMD_CTL_RCTR) || (param & I8042_CTR_AUXDIS))
		return -1;

/*
 * Disable the interface.
 */

	i8042_ctr |= I8042_CTR_AUXDIS;
	i8042_ctr &= ~I8042_CTR_AUXINT;

	if (i8042_command(&i8042_ctr, I8042_CMD_CTL_WCTR))
		return -1;

	return 0;
}


/*
 * i8042_port_register() marks the device as existing,
 * registers it, and reports to the user.
 */

static int __init i8042_port_register(struct serio *port)
{
	struct i8042_values *values = port->port_data;

	values->exists = 1;

	i8042_ctr &= ~values->disable;

	if (i8042_command(&i8042_ctr, I8042_CMD_CTL_WCTR)) {
		printk(KERN_WARNING "i8042.c: Can't write CTR while registering.\n");
		values->exists = 0;
		return -1;
	}

	printk(KERN_INFO "serio: i8042 %s port at %#lx,%#lx irq %d\n",
	       values->name,
	       (unsigned long) I8042_DATA_REG,
	       (unsigned long) I8042_COMMAND_REG,
	       values->irq);

	serio_register_port(port);

	return 0;
}


static void i8042_timer_func(unsigned long data)
{
	i8042_interrupt(0, NULL, NULL);
}


/*
 * i8042_controller init initializes the i8042 controller, and,
 * most importantly, sets it into non-xlated mode if that's
 * desired.
 */

static int i8042_controller_init(void)
{
	unsigned long flags;

/*
 * Test the i8042. We need to know if it thinks it's working correctly
 * before doing anything else.
 */

	i8042_flush();

	if (i8042_reset) {

		unsigned char param;

		if (i8042_command(&param, I8042_CMD_CTL_TEST)) {
			printk(KERN_ERR "i8042.c: i8042 controller self test timeout.\n");
			return -1;
		}

		if (param != I8042_RET_CTL_TEST) {
			printk(KERN_ERR "i8042.c: i8042 controller selftest failed. (%#x != %#x)\n",
				 param, I8042_RET_CTL_TEST);
			return -1;
		}
	}

/*
 * Save the CTR for restoral on unload / reboot.
 */

	if (i8042_command(&i8042_ctr, I8042_CMD_CTL_RCTR)) {
		printk(KERN_ERR "i8042.c: Can't read CTR while initializing i8042.\n");
		return -1;
	}

	i8042_initial_ctr = i8042_ctr;

/*
 * Disable the keyboard interface and interrupt.
 */

	i8042_ctr |= I8042_CTR_KBDDIS;
	i8042_ctr &= ~I8042_CTR_KBDINT;

/*
 * Handle keylock.
 */

	spin_lock_irqsave(&i8042_lock, flags);
	if (~i8042_read_status() & I8042_STR_KEYLOCK) {
		if (i8042_unlock)
			i8042_ctr |= I8042_CTR_IGNKEYLOCK;
		 else
			printk(KERN_WARNING "i8042.c: Warning: Keylock active.\n");
	}
	spin_unlock_irqrestore(&i8042_lock, flags);

/*
 * If the chip is configured into nontranslated mode by the BIOS, don't
 * bother enabling translating and be happy.
 */

	if (~i8042_ctr & I8042_CTR_XLATE)
		i8042_direct = 1;

/*
 * Set nontranslated mode for the kbd interface if requested by an option.
 * After this the kbd interface becomes a simple serial in/out, like the aux
 * interface is. We don't do this by default, since it can confuse notebook
 * BIOSes.
 */

	if (i8042_direct)
		i8042_ctr &= ~I8042_CTR_XLATE;

/*
 * Write CTR back.
 */

	if (i8042_command(&i8042_ctr, I8042_CMD_CTL_WCTR)) {
		printk(KERN_ERR "i8042.c: Can't write CTR while initializing i8042.\n");
		return -1;
	}

	return 0;
}


/*
 * Reset the controller.
 */
void i8042_controller_reset(void)
{
	unsigned char param;

/*
 * Reset the controller if requested.
 */

	if (i8042_reset)
		if (i8042_command(&param, I8042_CMD_CTL_TEST))
			printk(KERN_ERR "i8042.c: i8042 controller reset timeout.\n");

/*
 * Disable MUX mode if present.
 */

	if (i8042_mux_present)
		i8042_set_mux_mode(0, NULL);

/*
 * Restore the original control register setting.
 */

	i8042_ctr = i8042_initial_ctr;

	if (i8042_command(&i8042_ctr, I8042_CMD_CTL_WCTR))
		printk(KERN_WARNING "i8042.c: Can't restore CTR.\n");
}


/*
 * Here we try to reset everything back to a state in which the BIOS will be
 * able to talk to the hardware when rebooting.
 */

void i8042_controller_cleanup(void)
{
	int i;

	i8042_flush();

/*
 * Reset anything that is connected to the ports.
 */

	if (i8042_kbd_values.exists)
		serio_cleanup(i8042_kbd_port);

	if (i8042_aux_values.exists)
		serio_cleanup(i8042_aux_port);

	for (i = 0; i < I8042_NUM_MUX_PORTS; i++)
		if (i8042_mux_values[i].exists)
			serio_cleanup(i8042_mux_port[i]);

	i8042_controller_reset();
}


static int blink_frequency = 500;
module_param_named(panicblink, blink_frequency, int, 0600);

/* Catch the case when the kbd interrupt is off */
#define DELAY do { mdelay(1); if (++delay > 10) return delay; } while(0)

/* Tell the user who may be running in X and not see the console that we have
   panic'ed. This is to distingush panics from "real" lockups.  */
static long i8042_panic_blink(long count)
{
	long delay = 0;
	static long last_blink;
	static char led;
	/* Roughly 1/2s frequency. KDB uses about 1s. Make sure it is
	   different. */
	if (!blink_frequency)
		return 0;
	if (count - last_blink < blink_frequency)
		return 0;
	led ^= 0x01 | 0x04;
	while (i8042_read_status() & I8042_STR_IBF)
		DELAY;
	i8042_write_data(0xed); /* set leds */
	DELAY;
	while (i8042_read_status() & I8042_STR_IBF)
		DELAY;
	DELAY;
	i8042_write_data(led);
	DELAY;
	last_blink = count;
	return delay;
}

#undef DELAY

/*
 * Here we try to restore the original BIOS settings
 */

static int i8042_suspend(struct device *dev, u32 state, u32 level)
{
	if (level == SUSPEND_DISABLE) {
		del_timer_sync(&i8042_timer);
		i8042_controller_reset();
	}

	return 0;
}


/*
 * Here we try to reset everything back to a state in which suspended
 */

static int i8042_resume(struct device *dev, u32 level)
{
	int i;

	if (level != RESUME_ENABLE)
		return 0;

	if (i8042_controller_init()) {
		printk(KERN_ERR "i8042: resume failed\n");
		return -1;
	}

	if (i8042_mux_present)
		if (i8042_set_mux_mode(1, NULL) ||
		    i8042_enable_mux_ports(&i8042_aux_values)) {
			printk(KERN_WARNING "i8042: failed to resume active multiplexor, mouse won't work.\n");
		}

/*
 * Reconnect anything that was connected to the ports.
 */

	if (i8042_kbd_values.exists && i8042_activate_port(i8042_kbd_port) == 0)
		serio_reconnect(i8042_kbd_port);

	if (i8042_aux_values.exists && i8042_activate_port(i8042_aux_port) == 0)
		serio_reconnect(i8042_aux_port);

	for (i = 0; i < I8042_NUM_MUX_PORTS; i++)
		if (i8042_mux_values[i].exists && i8042_activate_port(i8042_mux_port[i]) == 0)
			serio_reconnect(i8042_mux_port[i]);
/*
 * Restart timer (for polling "stuck" data)
 */
	mod_timer(&i8042_timer, jiffies + I8042_POLL_PERIOD);

	panic_blink = i8042_panic_blink;

	return 0;

}

/*
 * We need to reset the 8042 back to original mode on system shutdown,
 * because otherwise BIOSes will be confused.
 */

static void i8042_shutdown(struct device *dev)
{
	i8042_controller_cleanup();
}

static struct device_driver i8042_driver = {
	.name		= "i8042",
	.bus		= &platform_bus_type,
	.suspend	= i8042_suspend,
	.resume		= i8042_resume,
	.shutdown	= i8042_shutdown,
};

static struct serio * __init i8042_allocate_kbd_port(void)
{
	struct serio *serio;

	serio = kmalloc(sizeof(struct serio), GFP_KERNEL);
	if (serio) {
		memset(serio, 0, sizeof(struct serio));
		serio->type		= i8042_direct ? SERIO_8042 : SERIO_8042_XL,
		serio->write		= i8042_dumbkbd ? NULL : i8042_kbd_write,
		serio->open		= i8042_open,
		serio->close		= i8042_close,
		serio->port_data	= &i8042_kbd_values,
		serio->dev.parent	= &i8042_platform_device->dev;
		strlcpy(serio->name, "i8042 Kbd Port", sizeof(serio->name));
		strlcpy(serio->phys, I8042_KBD_PHYS_DESC, sizeof(serio->phys));
	}

	return serio;
}

static struct serio * __init i8042_allocate_aux_port(void)
{
	struct serio *serio;

	serio = kmalloc(sizeof(struct serio), GFP_KERNEL);
	if (serio) {
		memset(serio, 0, sizeof(struct serio));
		serio->type		= SERIO_8042;
		serio->write		= i8042_aux_write;
		serio->open		= i8042_open;
		serio->close		= i8042_close;
		serio->port_data	= &i8042_aux_values,
		serio->dev.parent	= &i8042_platform_device->dev;
		strlcpy(serio->name, "i8042 Aux Port", sizeof(serio->name));
		strlcpy(serio->phys, I8042_AUX_PHYS_DESC, sizeof(serio->phys));
	}

	return serio;
}

static struct serio * __init i8042_allocate_mux_port(int index)
{
	struct serio *serio;
	struct i8042_values *values = &i8042_mux_values[index];

	serio = kmalloc(sizeof(struct serio), GFP_KERNEL);
	if (serio) {
		*values = i8042_aux_values;
		snprintf(values->name, sizeof(values->name), "AUX%d", index);
		values->mux = index;

		memset(serio, 0, sizeof(struct serio));
		serio->type		= SERIO_8042;
		serio->write		= i8042_aux_write;
		serio->open		= i8042_open;
		serio->close		= i8042_close;
		serio->port_data	= values;
		serio->dev.parent	= &i8042_platform_device->dev;
		snprintf(serio->name, sizeof(serio->name), "i8042 Aux-%d Port", index);
		snprintf(serio->phys, sizeof(serio->phys), I8042_MUX_PHYS_DESC, index + 1);
	}

	return serio;
}

int __init i8042_init(void)
{
	int i;
	int err;

	dbg_init();

	init_timer(&i8042_timer);
	i8042_timer.function = i8042_timer_func;

	if (i8042_platform_init())
		return -EBUSY;

	i8042_aux_values.irq = I8042_AUX_IRQ;
	i8042_kbd_values.irq = I8042_KBD_IRQ;

	if (i8042_controller_init())
		return -ENODEV;

	err = driver_register(&i8042_driver);
	if (err)
		return err;

	i8042_platform_device = platform_device_register_simple("i8042", -1, NULL, 0);
	if (IS_ERR(i8042_platform_device)) {
		driver_unregister(&i8042_driver);
		return PTR_ERR(i8042_platform_device);
	}

	if (!i8042_noaux && !i8042_check_aux(&i8042_aux_values)) {
		if (!i8042_nomux && !i8042_check_mux(&i8042_aux_values))
			for (i = 0; i < I8042_NUM_MUX_PORTS; i++) {
				i8042_mux_port[i] = i8042_allocate_mux_port(i);
				if (i8042_mux_port[i])
					i8042_port_register(i8042_mux_port[i]);
			}
		else {
			i8042_aux_port = i8042_allocate_aux_port();
			if (i8042_aux_port)
				i8042_port_register(i8042_aux_port);
		}
	}

	i8042_kbd_port = i8042_allocate_kbd_port();
	if (i8042_kbd_port)
		i8042_port_register(i8042_kbd_port);

	mod_timer(&i8042_timer, jiffies + I8042_POLL_PERIOD);

	return 0;
}

void __exit i8042_exit(void)
{
	int i;

	i8042_controller_cleanup();

	if (i8042_kbd_values.exists)
		serio_unregister_port(i8042_kbd_port);

	if (i8042_aux_values.exists)
		serio_unregister_port(i8042_aux_port);

	for (i = 0; i < I8042_NUM_MUX_PORTS; i++)
		if (i8042_mux_values[i].exists)
			serio_unregister_port(i8042_mux_port[i]);

	del_timer_sync(&i8042_timer);

	platform_device_unregister(i8042_platform_device);
	driver_unregister(&i8042_driver);

	i8042_platform_exit();

	panic_blink = NULL;
}

module_init(i8042_init);
module_exit(i8042_exit);
