/*
 *	Ethertap: A network device for bouncing packets via user space
 *
 *	This is a very simple ethernet driver. It bounces ethernet frames
 *	to user space on /dev/tap0->/dev/tap15 and expects ethernet frames
 *	to be written back to it. By default it does not ARP. If you turn ARP
 *	on it will attempt to ARP the user space and reply to ARPS from the
 *	user space.
 *
 *	As this is an ethernet device you can use it for appletalk, IPX etc
 *	even for building bridging tunnels.
 */
 
#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>

#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>

#include <net/sock.h>
#include <linux/netlink.h>

/*
 *	Index to functions.
 */

static int  ethertap_open(struct net_device *dev);
static int  ethertap_start_xmit(struct sk_buff *skb, struct net_device *dev);
static int  ethertap_close(struct net_device *dev);
static struct net_device_stats *ethertap_get_stats(struct net_device *dev);
static void ethertap_rx(struct sock *sk, int len);
#ifdef CONFIG_ETHERTAP_MC
static void set_multicast_list(struct net_device *dev);
#endif

static int ethertap_debug;

static int max_taps = 1;
module_param(max_taps, int, 0);
MODULE_PARM_DESC(max_taps,"Max number of ethernet tap devices");

static struct net_device **tap_map;	/* Returns the tap device for a given netlink */

/*
 *	Board-specific info in dev->priv.
 */

struct net_local
{
	struct sock	*nl;
#ifdef CONFIG_ETHERTAP_MC
	__u32		groups;
#endif
	struct net_device_stats stats;
};

/*
 *	To call this a probe is a bit misleading, however for real
 *	hardware it would have to check what was present.
 */
static int  __init ethertap_probe(int unit)
{
	struct net_device *dev;
	int err = -ENOMEM;

	dev = alloc_etherdev(sizeof(struct net_local));

	if (!dev)
		goto out;

	SET_MODULE_OWNER(dev);

	sprintf(dev->name, "tap%d", unit);
	dev->base_addr = unit + NETLINK_TAPBASE;

	netdev_boot_setup_check(dev);

	memcpy(dev->dev_addr, "\xFE\xFD\x00\x00\x00\x00", 6);
	if (dev->mem_start & 0xf)
		ethertap_debug = dev->mem_start & 0x7;

	/*
	 *	The tap specific entries in the device structure.
	 */

	dev->open = ethertap_open;
	dev->hard_start_xmit = ethertap_start_xmit;
	dev->stop = ethertap_close;
	dev->get_stats = ethertap_get_stats;
#ifdef CONFIG_ETHERTAP_MC
	dev->set_multicast_list = set_multicast_list;
#endif

	dev->tx_queue_len = 0;
	dev->flags|=IFF_NOARP;

	err = register_netdev(dev);
	if (err)
		goto out_free;

	tap_map[unit]=dev;
	return 0;
out_free:
	free_netdev(dev);
out:
	return err;
}

/*
 *	Open/initialize the board.
 */

static int ethertap_open(struct net_device *dev)
{
	struct net_local *lp = netdev_priv(dev);

	if (ethertap_debug > 2)
		printk(KERN_DEBUG "%s: Doing ethertap_open()...\n", dev->name);

	lp->nl = netlink_kernel_create(dev->base_addr, ethertap_rx);
	if (lp->nl == NULL)
		return -ENOBUFS;

	netif_start_queue(dev);
	return 0;
}

#ifdef CONFIG_ETHERTAP_MC
static unsigned ethertap_mc_hash(__u8 *dest)
{
	unsigned idx = 0;
	idx ^= dest[0];
	idx ^= dest[1];
	idx ^= dest[2];
	idx ^= dest[3];
	idx ^= dest[4];
	idx ^= dest[5];
	return 1U << (idx&0x1F);
}

static void set_multicast_list(struct net_device *dev)
{
	unsigned groups = ~0;
	struct net_local *lp = netdev_priv(dev);

	if (!(dev->flags&(IFF_NOARP|IFF_PROMISC|IFF_ALLMULTI))) {
		struct dev_mc_list *dmi;

		groups = ethertap_mc_hash(dev->broadcast);

		for (dmi=dev->mc_list; dmi; dmi=dmi->next) {
			if (dmi->dmi_addrlen != 6)
				continue;
			groups |= ethertap_mc_hash(dmi->dmi_addr);
		}
	}
	lp->groups = groups;
	if (lp->nl)
		lp->nl->protinfo.af_netlink.groups = groups;
}
#endif

/*
 *	We transmit by throwing the packet at netlink. We have to clone
 *	it for 2.0 so that we dev_kfree_skb() the locked original.
 */
 
static int ethertap_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct net_local *lp = netdev_priv(dev);
#ifdef CONFIG_ETHERTAP_MC
	struct ethhdr *eth = (struct ethhdr*)skb->data;
#endif

	if (skb_headroom(skb) < 2) {
		static int once;
	  	struct sk_buff *skb2;

		if (!once) {
			once = 1;
			printk(KERN_DEBUG "%s: not aligned xmit by protocol %04x\n", dev->name, skb->protocol);
		}

		skb2 = skb_realloc_headroom(skb, 2);
		dev_kfree_skb(skb);
		if (skb2 == NULL)
			return 0;
		skb = skb2;
	}
	__skb_push(skb, 2);

	/* Make the same thing, which loopback does. */
	if (skb_shared(skb)) {
	  	struct sk_buff *skb2 = skb;
	  	skb = skb_clone(skb, GFP_ATOMIC);	/* Clone the buffer */
	  	if (skb==NULL) {
			dev_kfree_skb(skb2);
			return 0;
		}
	  	dev_kfree_skb(skb2);
	}
	/* ... but do not orphan it here, netlink does it in any case. */

	lp->stats.tx_bytes+=skb->len;
	lp->stats.tx_packets++;

#ifndef CONFIG_ETHERTAP_MC
	netlink_broadcast(lp->nl, skb, 0, ~0, GFP_ATOMIC);
#else
	if (dev->flags&IFF_NOARP) {
		netlink_broadcast(lp->nl, skb, 0, ~0, GFP_ATOMIC);
		return 0;
	}

	if (!(eth->h_dest[0]&1)) {
		/* Unicast packet */
		__u32 pid;
		memcpy(&pid, eth->h_dest+2, 4);
		netlink_unicast(lp->nl, skb, ntohl(pid), MSG_DONTWAIT);
	} else
		netlink_broadcast(lp->nl, skb, 0, ethertap_mc_hash(eth->h_dest), GFP_ATOMIC);
#endif
	return 0;
}

static __inline__ int ethertap_rx_skb(struct sk_buff *skb, struct net_device *dev)
{
	struct net_local *lp = netdev_priv(dev);
#ifdef CONFIG_ETHERTAP_MC
	struct ethhdr *eth = (struct ethhdr*)(skb->data + 2);
#endif
	int len = skb->len;

	if (len < 16) {
		printk(KERN_DEBUG "%s : rx len = %d\n", dev->name, len);
		kfree_skb(skb);
		return -EINVAL;
	}
	if (NETLINK_CREDS(skb)->uid) {
		printk(KERN_INFO "%s : user %d\n", dev->name, NETLINK_CREDS(skb)->uid);
		kfree_skb(skb);
		return -EPERM;
	}

#ifdef CONFIG_ETHERTAP_MC
	if (!(dev->flags&(IFF_NOARP|IFF_PROMISC))) {
		int drop = 0;

		if (eth->h_dest[0]&1) {
			if (!(ethertap_mc_hash(eth->h_dest)&lp->groups))
				drop = 1;
		} else if (memcmp(eth->h_dest, dev->dev_addr, 6) != 0)
			drop = 1;

		if (drop) {
			if (ethertap_debug > 3)
				printk(KERN_DEBUG "%s : not for us\n", dev->name);
			kfree_skb(skb);
			return -EINVAL;
		}
	}
#endif

	if (skb_shared(skb)) {
	  	struct sk_buff *skb2 = skb;
	  	skb = skb_clone(skb, GFP_KERNEL);	/* Clone the buffer */
	  	if (skb==NULL) {
			kfree_skb(skb2);
			return -ENOBUFS;
		}
	  	kfree_skb(skb2);
	} else
		skb_orphan(skb);

	skb_pull(skb, 2);
	skb->dev = dev;
	skb->protocol=eth_type_trans(skb,dev);
	memset(skb->cb, 0, sizeof(skb->cb));
	lp->stats.rx_packets++;
	lp->stats.rx_bytes+=len;
	netif_rx(skb);
	dev->last_rx = jiffies;
	return len;
}

/*
 *	The typical workload of the driver:
 *	Handle the ether interface interrupts.
 *
 *	(In this case handle the packets posted from user space..)
 */

static void ethertap_rx(struct sock *sk, int len)
{
	unsigned unit = sk->sk_protocol - NETLINK_TAPBASE; 
	struct net_device *dev;
	struct sk_buff *skb;

	if (unit >= max_taps || (dev = tap_map[unit]) == NULL) { 
		printk(KERN_CRIT "ethertap: bad unit %u!\n", unit);
		skb_queue_purge(&sk->sk_receive_queue);
		return;
	}

	if (ethertap_debug > 3)
		printk(KERN_DEBUG "%s: ethertap_rx()\n", dev->name);

	while ((skb = skb_dequeue(&sk->sk_receive_queue)) != NULL)
		ethertap_rx_skb(skb, dev);
}

static int ethertap_close(struct net_device *dev)
{
	struct net_local *lp = netdev_priv(dev);
	struct sock *sk = lp->nl;

	if (ethertap_debug > 2)
		printk(KERN_DEBUG "%s: Shutting down.\n", dev->name);

	netif_stop_queue(dev);

	if (sk) {
		lp->nl = NULL;
		sock_release(sk->sk_socket);
	}

	return 0;
}

static struct net_device_stats *ethertap_get_stats(struct net_device *dev)
{
	struct net_local *lp = netdev_priv(dev);
	return &lp->stats;
}


int __init ethertap_init(void)
{
	int i, err = 0;

	/* netlink can only hande 16 entries unless modified */
	if (max_taps > MAX_LINKS - NETLINK_TAPBASE)
		return -E2BIG;

	tap_map = kmalloc(sizeof(struct net_device *)*max_taps, GFP_KERNEL);
	if (!tap_map)
		return -ENOMEM;

	for (i = 0; i < max_taps; i++) {
		err = ethertap_probe(i);
		if (err) {
			while (--i > 0) {
				unregister_netdev(tap_map[i]);
				free_netdev(tap_map[i]);
			}
			break;
		}
	}
	if (err)
		kfree(tap_map);
	return err;
}
module_init(ethertap_init);

void __exit ethertap_cleanup(void)
{
	int i;

	for (i = 0; i < max_taps; i++) {
		struct net_device *dev = tap_map[i];
		if (dev) {
			tap_map[i] = NULL;
			unregister_netdev(dev);
			free_netdev(dev);
		}
	}
	kfree(tap_map);
}
module_exit(ethertap_cleanup);

MODULE_LICENSE("GPL");
