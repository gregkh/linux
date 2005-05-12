/*
 * This is a module which is used for rejecting packets.
 * Added support for customized reject packets (Jozsef Kadlecsik).
 * Added support for ICMP type-3-code-13 (Maciej Soltysiak). [RFC 1812]
 */

/* (C) 1999-2001 Paul `Rusty' Russell
 * (C) 2002-2004 Netfilter Core Team <coreteam@netfilter.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/icmp.h>
#include <net/icmp.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/route.h>
#include <net/dst.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_ipv4/ipt_REJECT.h>
#ifdef CONFIG_BRIDGE_NETFILTER
#include <linux/netfilter_bridge.h>
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Netfilter Core Team <coreteam@netfilter.org>");
MODULE_DESCRIPTION("iptables REJECT target module");

#if 0
#define DEBUGP printk
#else
#define DEBUGP(format, args...)
#endif

static inline struct rtable *route_reverse(struct sk_buff *skb, 
					   struct tcphdr *tcph, int hook)
{
	struct iphdr *iph = skb->nh.iph;
	struct dst_entry *odst;
	struct flowi fl = {};
	struct rtable *rt;

	/* We don't require ip forwarding to be enabled to be able to
	 * send a RST reply for bridged traffic. */
	if (hook != NF_IP_FORWARD
#ifdef CONFIG_BRIDGE_NETFILTER
	    || (skb->nf_bridge && skb->nf_bridge->mask & BRNF_BRIDGED)
#endif
	   ) {
		fl.nl_u.ip4_u.daddr = iph->saddr;
		if (hook == NF_IP_LOCAL_IN)
			fl.nl_u.ip4_u.saddr = iph->daddr;
		fl.nl_u.ip4_u.tos = RT_TOS(iph->tos);

		if (ip_route_output_key(&rt, &fl) != 0)
			return NULL;
	} else {
		/* non-local src, find valid iif to satisfy
		 * rp-filter when calling ip_route_input. */
		fl.nl_u.ip4_u.daddr = iph->daddr;
		if (ip_route_output_key(&rt, &fl) != 0)
			return NULL;

		odst = skb->dst;
		if (ip_route_input(skb, iph->saddr, iph->daddr,
		                   RT_TOS(iph->tos), rt->u.dst.dev) != 0) {
			dst_release(&rt->u.dst);
			return NULL;
		}
		dst_release(&rt->u.dst);
		rt = (struct rtable *)skb->dst;
		skb->dst = odst;

		fl.nl_u.ip4_u.daddr = iph->saddr;
		fl.nl_u.ip4_u.saddr = iph->daddr;
		fl.nl_u.ip4_u.tos = RT_TOS(iph->tos);
	}

	if (rt->u.dst.error) {
		dst_release(&rt->u.dst);
		return NULL;
	}

	fl.proto = IPPROTO_TCP;
	fl.fl_ip_sport = tcph->dest;
	fl.fl_ip_dport = tcph->source;

	if (xfrm_lookup((struct dst_entry **)&rt, &fl, NULL, 0)) {
		dst_release(&rt->u.dst);
		rt = NULL;
	}

	return rt;
}

/* Send RST reply */
static void send_reset(struct sk_buff *oldskb, int hook)
{
	struct sk_buff *nskb;
	struct tcphdr _otcph, *oth, *tcph;
	struct rtable *rt;
	u_int16_t tmp_port;
	u_int32_t tmp_addr;
	int needs_ack;
	int hh_len;

	/* IP header checks: fragment. */
	if (oldskb->nh.iph->frag_off & htons(IP_OFFSET))
		return;

	oth = skb_header_pointer(oldskb, oldskb->nh.iph->ihl * 4,
				 sizeof(_otcph), &_otcph);
	if (oth == NULL)
 		return;

	/* No RST for RST. */
	if (oth->rst)
		return;

	/* FIXME: Check checksum --RR */
	if ((rt = route_reverse(oldskb, oth, hook)) == NULL)
		return;

	hh_len = LL_RESERVED_SPACE(rt->u.dst.dev);

	/* We need a linear, writeable skb.  We also need to expand
	   headroom in case hh_len of incoming interface < hh_len of
	   outgoing interface */
	nskb = skb_copy_expand(oldskb, hh_len, skb_tailroom(oldskb),
			       GFP_ATOMIC);
	if (!nskb) {
		dst_release(&rt->u.dst);
		return;
	}

	dst_release(nskb->dst);
	nskb->dst = &rt->u.dst;

	/* This packet will not be the same as the other: clear nf fields */
	nf_reset(nskb);
	nskb->nfcache = 0;
	nskb->nfmark = 0;
#ifdef CONFIG_BRIDGE_NETFILTER
	nf_bridge_put(nskb->nf_bridge);
	nskb->nf_bridge = NULL;
#endif

	tcph = (struct tcphdr *)((u_int32_t*)nskb->nh.iph + nskb->nh.iph->ihl);

	/* Swap source and dest */
	tmp_addr = nskb->nh.iph->saddr;
	nskb->nh.iph->saddr = nskb->nh.iph->daddr;
	nskb->nh.iph->daddr = tmp_addr;
	tmp_port = tcph->source;
	tcph->source = tcph->dest;
	tcph->dest = tmp_port;

	/* Truncate to length (no data) */
	tcph->doff = sizeof(struct tcphdr)/4;
	skb_trim(nskb, nskb->nh.iph->ihl*4 + sizeof(struct tcphdr));
	nskb->nh.iph->tot_len = htons(nskb->len);

	if (tcph->ack) {
		needs_ack = 0;
		tcph->seq = oth->ack_seq;
		tcph->ack_seq = 0;
	} else {
		needs_ack = 1;
		tcph->ack_seq = htonl(ntohl(oth->seq) + oth->syn + oth->fin
				      + oldskb->len - oldskb->nh.iph->ihl*4
				      - (oth->doff<<2));
		tcph->seq = 0;
	}

	/* Reset flags */
	((u_int8_t *)tcph)[13] = 0;
	tcph->rst = 1;
	tcph->ack = needs_ack;

	tcph->window = 0;
	tcph->urg_ptr = 0;

	/* Adjust TCP checksum */
	tcph->check = 0;
	tcph->check = tcp_v4_check(tcph, sizeof(struct tcphdr),
				   nskb->nh.iph->saddr,
				   nskb->nh.iph->daddr,
				   csum_partial((char *)tcph,
						sizeof(struct tcphdr), 0));

	/* Adjust IP TTL, DF */
	nskb->nh.iph->ttl = MAXTTL;
	/* Set DF, id = 0 */
	nskb->nh.iph->frag_off = htons(IP_DF);
	nskb->nh.iph->id = 0;

	/* Adjust IP checksum */
	nskb->nh.iph->check = 0;
	nskb->nh.iph->check = ip_fast_csum((unsigned char *)nskb->nh.iph, 
					   nskb->nh.iph->ihl);

	/* "Never happens" */
	if (nskb->len > dst_pmtu(nskb->dst))
		goto free_nskb;

	nf_ct_attach(nskb, oldskb);

	NF_HOOK(PF_INET, NF_IP_LOCAL_OUT, nskb, NULL, nskb->dst->dev,
		ip_finish_output);
	return;

 free_nskb:
	kfree_skb(nskb);
}

static void send_unreach(struct sk_buff *skb_in, int code)
{
	struct iphdr *iph;
	struct icmphdr *icmph;
	struct sk_buff *nskb;
	u32 saddr;
	u8 tos;
	int hh_len, length;
	struct rtable *rt = (struct rtable*)skb_in->dst;
	unsigned char *data;

	if (!rt)
		return;

	/* FIXME: Use sysctl number. --RR */
	if (!xrlim_allow(&rt->u.dst, 1*HZ))
		return;

	iph = skb_in->nh.iph;

	/* No replies to physical multicast/broadcast */
	if (skb_in->pkt_type!=PACKET_HOST)
		return;

	/* Now check at the protocol level */
	if (rt->rt_flags&(RTCF_BROADCAST|RTCF_MULTICAST))
		return;

	/* Only reply to fragment 0. */
	if (iph->frag_off&htons(IP_OFFSET))
		return;

	/* Ensure we have at least 8 bytes of proto header. */
	if (skb_in->len < skb_in->nh.iph->ihl*4 + 8)
		return;

	/* If we send an ICMP error to an ICMP error a mess would result.. */
	if (iph->protocol == IPPROTO_ICMP) {
		struct icmphdr ihdr;

		icmph = skb_header_pointer(skb_in, skb_in->nh.iph->ihl*4,
					   sizeof(ihdr), &ihdr);
		if (!icmph)
			return;

		/* Between echo-reply (0) and timestamp (13),
		   everything except echo-request (8) is an error.
		   Also, anything greater than NR_ICMP_TYPES is
		   unknown, and hence should be treated as an error... */
		if ((icmph->type < ICMP_TIMESTAMP
		     && icmph->type != ICMP_ECHOREPLY
		     && icmph->type != ICMP_ECHO)
		    || icmph->type > NR_ICMP_TYPES)
			return;
	}

	saddr = iph->daddr;
	if (!(rt->rt_flags & RTCF_LOCAL))
		saddr = 0;

	tos = (iph->tos & IPTOS_TOS_MASK) | IPTOS_PREC_INTERNETCONTROL;

	{
		struct flowi fl = {
			.nl_u = {
				.ip4_u = {
					.daddr = skb_in->nh.iph->saddr,
					.saddr = saddr,
					.tos = RT_TOS(tos)
				}
			},
			.proto = IPPROTO_ICMP,
			.uli_u = {
				.icmpt = {
					.type = ICMP_DEST_UNREACH,
					.code = code
				}
			}
		};

		if (ip_route_output_key(&rt, &fl))
			return;
	}
	/* RFC says return as much as we can without exceeding 576 bytes. */
	length = skb_in->len + sizeof(struct iphdr) + sizeof(struct icmphdr);

	if (length > dst_pmtu(&rt->u.dst))
		length = dst_pmtu(&rt->u.dst);
	if (length > 576)
		length = 576;

	hh_len = LL_RESERVED_SPACE(rt->u.dst.dev);

	nskb = alloc_skb(hh_len + length, GFP_ATOMIC);
	if (!nskb) {
		ip_rt_put(rt);
		return;
	}

	nskb->priority = 0;
	nskb->dst = &rt->u.dst;
	skb_reserve(nskb, hh_len);

	/* Set up IP header */
	iph = nskb->nh.iph
		= (struct iphdr *)skb_put(nskb, sizeof(struct iphdr));
	iph->version=4;
	iph->ihl=5;
	iph->tos=tos;
	iph->tot_len = htons(length);

	/* PMTU discovery never applies to ICMP packets. */
	iph->frag_off = 0;

	iph->ttl = MAXTTL;
	ip_select_ident(iph, &rt->u.dst, NULL);
	iph->protocol=IPPROTO_ICMP;
	iph->saddr=rt->rt_src;
	iph->daddr=rt->rt_dst;
	iph->check=0;
	iph->check = ip_fast_csum((unsigned char *)iph, iph->ihl);

	/* Set up ICMP header. */
	icmph = nskb->h.icmph
		= (struct icmphdr *)skb_put(nskb, sizeof(struct icmphdr));
	icmph->type = ICMP_DEST_UNREACH;
	icmph->code = code;	
	icmph->un.gateway = 0;
	icmph->checksum = 0;
	
	/* Copy as much of original packet as will fit */
	data = skb_put(nskb,
		       length - sizeof(struct iphdr) - sizeof(struct icmphdr));

	skb_copy_bits(skb_in, 0, data,
		      length - sizeof(struct iphdr) - sizeof(struct icmphdr));

	icmph->checksum = ip_compute_csum((unsigned char *)icmph,
					  length - sizeof(struct iphdr));

	nf_ct_attach(nskb, skb_in);

	NF_HOOK(PF_INET, NF_IP_LOCAL_OUT, nskb, NULL, nskb->dst->dev,
		ip_finish_output);
}	

static unsigned int reject(struct sk_buff **pskb,
			   const struct net_device *in,
			   const struct net_device *out,
			   unsigned int hooknum,
			   const void *targinfo,
			   void *userinfo)
{
	const struct ipt_reject_info *reject = targinfo;

	/* Our naive response construction doesn't deal with IP
           options, and probably shouldn't try. */
	if ((*pskb)->nh.iph->ihl<<2 != sizeof(struct iphdr))
		return NF_DROP;

	/* WARNING: This code causes reentry within iptables.
	   This means that the iptables jump stack is now crap.  We
	   must return an absolute verdict. --RR */
    	switch (reject->with) {
    	case IPT_ICMP_NET_UNREACHABLE:
    		send_unreach(*pskb, ICMP_NET_UNREACH);
    		break;
    	case IPT_ICMP_HOST_UNREACHABLE:
    		send_unreach(*pskb, ICMP_HOST_UNREACH);
    		break;
    	case IPT_ICMP_PROT_UNREACHABLE:
    		send_unreach(*pskb, ICMP_PROT_UNREACH);
    		break;
    	case IPT_ICMP_PORT_UNREACHABLE:
    		send_unreach(*pskb, ICMP_PORT_UNREACH);
    		break;
    	case IPT_ICMP_NET_PROHIBITED:
    		send_unreach(*pskb, ICMP_NET_ANO);
    		break;
	case IPT_ICMP_HOST_PROHIBITED:
    		send_unreach(*pskb, ICMP_HOST_ANO);
    		break;
    	case IPT_ICMP_ADMIN_PROHIBITED:
		send_unreach(*pskb, ICMP_PKT_FILTERED);
		break;
	case IPT_TCP_RESET:
		send_reset(*pskb, hooknum);
	case IPT_ICMP_ECHOREPLY:
		/* Doesn't happen. */
		break;
	}

	return NF_DROP;
}

static int check(const char *tablename,
		 const struct ipt_entry *e,
		 void *targinfo,
		 unsigned int targinfosize,
		 unsigned int hook_mask)
{
 	const struct ipt_reject_info *rejinfo = targinfo;

 	if (targinfosize != IPT_ALIGN(sizeof(struct ipt_reject_info))) {
  		DEBUGP("REJECT: targinfosize %u != 0\n", targinfosize);
  		return 0;
  	}

	/* Only allow these for packet filtering. */
	if (strcmp(tablename, "filter") != 0) {
		DEBUGP("REJECT: bad table `%s'.\n", tablename);
		return 0;
	}
	if ((hook_mask & ~((1 << NF_IP_LOCAL_IN)
			   | (1 << NF_IP_FORWARD)
			   | (1 << NF_IP_LOCAL_OUT))) != 0) {
		DEBUGP("REJECT: bad hook mask %X\n", hook_mask);
		return 0;
	}

	if (rejinfo->with == IPT_ICMP_ECHOREPLY) {
		printk("REJECT: ECHOREPLY no longer supported.\n");
		return 0;
	} else if (rejinfo->with == IPT_TCP_RESET) {
		/* Must specify that it's a TCP packet */
		if (e->ip.proto != IPPROTO_TCP
		    || (e->ip.invflags & IPT_INV_PROTO)) {
			DEBUGP("REJECT: TCP_RESET invalid for non-tcp\n");
			return 0;
		}
	}

	return 1;
}

static struct ipt_target ipt_reject_reg = {
	.name		= "REJECT",
	.target		= reject,
	.checkentry	= check,
	.me		= THIS_MODULE,
};

static int __init init(void)
{
	return ipt_register_target(&ipt_reject_reg);
}

static void __exit fini(void)
{
	ipt_unregister_target(&ipt_reject_reg);
}

module_init(init);
module_exit(fini);
