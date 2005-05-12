/*
 * Common code for low-level network console, dump, and debugger code
 *
 * Derived from netconsole, kgdb-over-ethernet, and netdump patches
 */

#ifndef _LINUX_NETPOLL_H
#define _LINUX_NETPOLL_H

#include <linux/netdevice.h>
#include <linux/interrupt.h>
#include <linux/list.h>

struct netpoll;

struct netpoll {
	struct net_device *dev;
	char dev_name[16], *name;
	void (*rx_hook)(struct netpoll *, int, char *, int);
	u32 local_ip, remote_ip;
	u16 local_port, remote_port;
	unsigned char local_mac[6], remote_mac[6];
	struct list_head rx_list;
};

void netpoll_poll(struct netpoll *np);
void netpoll_send_udp(struct netpoll *np, const char *msg, int len);
int netpoll_parse_options(struct netpoll *np, char *opt);
int netpoll_setup(struct netpoll *np);
int netpoll_trap(void);
void netpoll_set_trap(int trap);
void netpoll_cleanup(struct netpoll *np);
int netpoll_rx(struct sk_buff *skb);


#endif
