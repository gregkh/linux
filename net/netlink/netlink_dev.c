/*
 * NETLINK	An implementation of a loadable kernel mode driver providing
 *		multiple kernel/user space bidirectional communications links.
 *
 * 		Author: 	Alan Cox <alan@redhat.com>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 * 
 *	Now netlink devices are emulated on the top of netlink sockets
 *	by compatibility reasons. Remove this file after a period. --ANK
 *
 */

#include <linux/module.h>

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/smp_lock.h>
#include <linux/device.h>
#include <linux/bitops.h>

#include <asm/system.h>
#include <asm/uaccess.h>

static long open_map;
static struct socket *netlink_user[MAX_LINKS];
static struct class_simple *netlink_class;

/*
 *	Device operations
 */
 
static unsigned int netlink_poll(struct file *file, poll_table * wait)
{
	struct socket *sock = netlink_user[iminor(file->f_dentry->d_inode)];

	if (sock->ops->poll==NULL)
		return 0;
	return sock->ops->poll(file, sock, wait);
}

/*
 *	Write a message to the kernel side of a communication link
 */
 
static ssize_t netlink_write(struct file * file, const char __user * buf,
			     size_t count, loff_t *pos)
{
	struct inode *inode = file->f_dentry->d_inode;
	struct socket *sock = netlink_user[iminor(inode)];
	struct msghdr msg;
	struct iovec iov;

	iov.iov_base = (void __user*)buf;
	iov.iov_len = count;
	msg.msg_name=NULL;
	msg.msg_namelen=0;
	msg.msg_controllen=0;
	msg.msg_flags=0;
	msg.msg_iov=&iov;
	msg.msg_iovlen=1;

	return sock_sendmsg(sock, &msg, count);
}

/*
 *	Read a message from the kernel side of the communication link
 */

static ssize_t netlink_read(struct file * file, char __user * buf,
			    size_t count, loff_t *pos)
{
	struct inode *inode = file->f_dentry->d_inode;
	struct socket *sock = netlink_user[iminor(inode)];
	struct msghdr msg;
	struct iovec iov;

	iov.iov_base = buf;
	iov.iov_len = count;
	msg.msg_name=NULL;
	msg.msg_namelen=0;
	msg.msg_controllen=0;
	msg.msg_flags=0;
	msg.msg_iov=&iov;
	msg.msg_iovlen=1;
	if (file->f_flags&O_NONBLOCK)
		msg.msg_flags=MSG_DONTWAIT;

	return sock_recvmsg(sock, &msg, count, msg.msg_flags);
}

static int netlink_open(struct inode * inode, struct file * file)
{
	unsigned int minor = iminor(inode);
	struct socket *sock;
	struct sockaddr_nl nladdr;
	int err;

	if (minor>=MAX_LINKS)
		return -ENODEV;
	if (test_and_set_bit(minor, &open_map))
		return -EBUSY;

	err = sock_create_kern(PF_NETLINK, SOCK_RAW, minor, &sock);
	if (err < 0)
		goto out;

	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family = AF_NETLINK;
	nladdr.nl_groups = ~0;
	if ((err = sock->ops->bind(sock, (struct sockaddr*)&nladdr, sizeof(nladdr))) < 0) {
		sock_release(sock);
		goto out;
	}

	netlink_user[minor] = sock;
	return 0;

out:
	clear_bit(minor, &open_map);
	return err;
}

static int netlink_release(struct inode * inode, struct file * file)
{
	unsigned int minor = iminor(inode);
	struct socket *sock;

	sock = netlink_user[minor];
	netlink_user[minor] = NULL;
	clear_bit(minor, &open_map);
	sock_release(sock);
	return 0;
}


static int netlink_ioctl(struct inode *inode, struct file *file,
		    unsigned int cmd, unsigned long arg)
{
	unsigned int minor = iminor(inode);
	int retval = 0;

	if (minor >= MAX_LINKS)
		return -ENODEV;
	switch ( cmd ) {
		default:
			retval = -EINVAL;
	}
	return retval;
}


static struct file_operations netlink_fops = {
	.owner =	THIS_MODULE,
	.llseek =	no_llseek,
	.read =		netlink_read,
	.write =	netlink_write,
	.poll =		netlink_poll,
	.ioctl =	netlink_ioctl,
	.open =		netlink_open,
	.release =	netlink_release,
};

static struct {
	char *name;
	int minor;
} entries[] = {
	{
		.name	= "route",
		.minor	= NETLINK_ROUTE,
	},
	{
		.name	= "skip",
		.minor	= NETLINK_SKIP,
	},
	{
		.name	= "usersock",
		.minor	= NETLINK_USERSOCK,
	},
	{
		.name	= "fwmonitor",
		.minor	= NETLINK_FIREWALL,
	},
	{
		.name	= "tcpdiag",
		.minor	= NETLINK_TCPDIAG,
	},
	{
		.name	= "nflog",
		.minor	= NETLINK_NFLOG,
	},
	{
		.name	= "xfrm",
		.minor	= NETLINK_XFRM,
	},
	{
		.name	= "arpd",
		.minor	= NETLINK_ARPD,
	},
	{
		.name	= "route6",
		.minor	= NETLINK_ROUTE6,
	},
	{
		.name	= "ip6_fw",
		.minor	= NETLINK_IP6_FW,
	},
	{
		.name	= "dnrtmsg",
		.minor	= NETLINK_DNRTMSG,
	},
};

static int __init init_netlink(void)
{
	int i;

	if (register_chrdev(NETLINK_MAJOR,"netlink", &netlink_fops)) {
		printk(KERN_ERR "netlink: unable to get major %d\n", NETLINK_MAJOR);
		return -EIO;
	}

	netlink_class = class_simple_create(THIS_MODULE, "netlink");
	if (IS_ERR(netlink_class)) {
		printk (KERN_ERR "Error creating netlink class.\n");
		unregister_chrdev(NETLINK_MAJOR, "netlink");
		return PTR_ERR(netlink_class);
	}

	devfs_mk_dir("netlink");

	/*  Someone tell me the official names for the uppercase ones  */
	for (i = 0; i < ARRAY_SIZE(entries); i++) {
		devfs_mk_cdev(MKDEV(NETLINK_MAJOR, entries[i].minor),
			S_IFCHR|S_IRUSR|S_IWUSR, "netlink/%s", entries[i].name);
		class_simple_device_add(netlink_class, MKDEV(NETLINK_MAJOR, entries[i].minor), NULL, "%s", entries[i].name);
	}

	for (i = 0; i < 16; i++) {
		devfs_mk_cdev(MKDEV(NETLINK_MAJOR, i + 16),
			S_IFCHR|S_IRUSR|S_IWUSR, "netlink/tap%d", i);
		class_simple_device_add(netlink_class, MKDEV(NETLINK_MAJOR, i + 16), NULL, "tap%d", i);
	}

	return 0;
}

static void __exit cleanup_netlink(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(entries); i++) {
		devfs_remove("netlink/%s", entries[i].name);
		class_simple_device_remove(MKDEV(NETLINK_MAJOR, entries[i].minor));
	}
	for (i = 0; i < 16; i++) {
		devfs_remove("netlink/tap%d", i);
		class_simple_device_remove(MKDEV(NETLINK_MAJOR, i + 16));
	}
	devfs_remove("netlink");
	class_simple_destroy(netlink_class);
	unregister_chrdev(NETLINK_MAJOR, "netlink");
}

MODULE_LICENSE("GPL");
module_init(init_netlink);
module_exit(cleanup_netlink);
