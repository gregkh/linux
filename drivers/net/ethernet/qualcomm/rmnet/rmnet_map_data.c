// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2013-2018, 2021, The Linux Foundation. All rights reserved.
 *
 * RMNET Data MAP protocol
 */

#include <linux/netdevice.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <net/ip6_checksum.h>
#include <linux/bitfield.h>
#include "rmnet_config.h"
#include "rmnet_map.h"
#include "rmnet_private.h"
#include "rmnet_vnd.h"

#define RMNET_MAP_DEAGGR_SPACING  64
#define RMNET_MAP_DEAGGR_HEADROOM (RMNET_MAP_DEAGGR_SPACING / 2)

static __sum16 *rmnet_map_get_csum_field(unsigned char protocol,
					 const void *txporthdr)
{
	__sum16 *check = NULL;

	switch (protocol) {
	case IPPROTO_TCP:
		check = &(((struct tcphdr *)txporthdr)->check);
		break;

	case IPPROTO_UDP:
		check = &(((struct udphdr *)txporthdr)->check);
		break;

	default:
		check = NULL;
		break;
	}

	return check;
}

static int
rmnet_map_ipv4_dl_csum_trailer(struct sk_buff *skb,
			       struct rmnet_map_dl_csum_trailer *csum_trailer,
			       struct rmnet_priv *priv)
{
	__sum16 *csum_field, csum_temp, pseudo_csum, hdr_csum, ip_payload_csum;
	u16 csum_value, csum_value_final;
	struct iphdr *ip4h;
	void *txporthdr;
	__be16 addend;

	ip4h = (struct iphdr *)(skb->data);
	if ((ntohs(ip4h->frag_off) & IP_MF) ||
	    ((ntohs(ip4h->frag_off) & IP_OFFSET) > 0)) {
		priv->stats.csum_fragmented_pkt++;
		return -EOPNOTSUPP;
	}

	txporthdr = skb->data + ip4h->ihl * 4;

	csum_field = rmnet_map_get_csum_field(ip4h->protocol, txporthdr);

	if (!csum_field) {
		priv->stats.csum_err_invalid_transport++;
		return -EPROTONOSUPPORT;
	}

	/* RFC 768 - Skip IPv4 UDP packets where sender checksum field is 0 */
	if (*csum_field == 0 && ip4h->protocol == IPPROTO_UDP) {
		priv->stats.csum_skipped++;
		return 0;
	}

	csum_value = ~ntohs(csum_trailer->csum_value);
	hdr_csum = ~ip_fast_csum(ip4h, (int)ip4h->ihl);
	ip_payload_csum = csum16_sub((__force __sum16)csum_value,
				     (__force __be16)hdr_csum);

	pseudo_csum = ~csum_tcpudp_magic(ip4h->saddr, ip4h->daddr,
					 ntohs(ip4h->tot_len) - ip4h->ihl * 4,
					 ip4h->protocol, 0);
	addend = (__force __be16)ntohs((__force __be16)pseudo_csum);
	pseudo_csum = csum16_add(ip_payload_csum, addend);

	addend = (__force __be16)ntohs((__force __be16)*csum_field);
	csum_temp = ~csum16_sub(pseudo_csum, addend);
	csum_value_final = (__force u16)csum_temp;

	if (unlikely(csum_value_final == 0)) {
		switch (ip4h->protocol) {
		case IPPROTO_UDP:
			/* RFC 768 - DL4 1's complement rule for UDP csum 0 */
			csum_value_final = ~csum_value_final;
			break;

		case IPPROTO_TCP:
			/* DL4 Non-RFC compliant TCP checksum found */
			if (*csum_field == (__force __sum16)0xFFFF)
				csum_value_final = ~csum_value_final;
			break;
		}
	}

	if (csum_value_final == ntohs((__force __be16)*csum_field)) {
		priv->stats.csum_ok++;
		return 0;
	} else {
		priv->stats.csum_validation_failed++;
		return -EINVAL;
	}
}

#if IS_ENABLED(CONFIG_IPV6)
static int
rmnet_map_ipv6_dl_csum_trailer(struct sk_buff *skb,
			       struct rmnet_map_dl_csum_trailer *csum_trailer,
			       struct rmnet_priv *priv)
{
	__sum16 *csum_field, ip6_payload_csum, pseudo_csum, csum_temp;
	u16 csum_value, csum_value_final;
	__be16 ip6_hdr_csum, addend;
	struct ipv6hdr *ip6h;
	void *txporthdr;
	u32 length;

	ip6h = (struct ipv6hdr *)(skb->data);

	txporthdr = skb->data + sizeof(struct ipv6hdr);
	csum_field = rmnet_map_get_csum_field(ip6h->nexthdr, txporthdr);

	if (!csum_field) {
		priv->stats.csum_err_invalid_transport++;
		return -EPROTONOSUPPORT;
	}

	csum_value = ~ntohs(csum_trailer->csum_value);
	ip6_hdr_csum = (__force __be16)
			~ntohs((__force __be16)ip_compute_csum(ip6h,
			       (int)(txporthdr - (void *)(skb->data))));
	ip6_payload_csum = csum16_sub((__force __sum16)csum_value,
				      ip6_hdr_csum);

	length = (ip6h->nexthdr == IPPROTO_UDP) ?
		 ntohs(((struct udphdr *)txporthdr)->len) :
		 ntohs(ip6h->payload_len);
	pseudo_csum = ~(csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr,
			     length, ip6h->nexthdr, 0));
	addend = (__force __be16)ntohs((__force __be16)pseudo_csum);
	pseudo_csum = csum16_add(ip6_payload_csum, addend);

	addend = (__force __be16)ntohs((__force __be16)*csum_field);
	csum_temp = ~csum16_sub(pseudo_csum, addend);
	csum_value_final = (__force u16)csum_temp;

	if (unlikely(csum_value_final == 0)) {
		switch (ip6h->nexthdr) {
		case IPPROTO_UDP:
			/* RFC 2460 section 8.1
			 * DL6 One's complement rule for UDP checksum 0
			 */
			csum_value_final = ~csum_value_final;
			break;

		case IPPROTO_TCP:
			/* DL6 Non-RFC compliant TCP checksum found */
			if (*csum_field == (__force __sum16)0xFFFF)
				csum_value_final = ~csum_value_final;
			break;
		}
	}

	if (csum_value_final == ntohs((__force __be16)*csum_field)) {
		priv->stats.csum_ok++;
		return 0;
	} else {
		priv->stats.csum_validation_failed++;
		return -EINVAL;
	}
}
#endif

static void rmnet_map_complement_ipv4_txporthdr_csum_field(void *iphdr)
{
	struct iphdr *ip4h = (struct iphdr *)iphdr;
	void *txphdr;
	u16 *csum;

	txphdr = iphdr + ip4h->ihl * 4;

	if (ip4h->protocol == IPPROTO_TCP || ip4h->protocol == IPPROTO_UDP) {
		csum = (u16 *)rmnet_map_get_csum_field(ip4h->protocol, txphdr);
		*csum = ~(*csum);
	}
}

static void
rmnet_map_ipv4_ul_csum_header(void *iphdr,
			      struct rmnet_map_ul_csum_header *ul_header,
			      struct sk_buff *skb)
{
	struct iphdr *ip4h = iphdr;
	u16 val;

	val = MAP_CSUM_UL_ENABLED_FLAG;
	if (ip4h->protocol == IPPROTO_UDP)
		val |= MAP_CSUM_UL_UDP_FLAG;
	val |= skb->csum_offset & MAP_CSUM_UL_OFFSET_MASK;

	ul_header->csum_start_offset = htons(skb_network_header_len(skb));
	ul_header->csum_info = htons(val);

	skb->ip_summed = CHECKSUM_NONE;

	rmnet_map_complement_ipv4_txporthdr_csum_field(iphdr);
}

#if IS_ENABLED(CONFIG_IPV6)
static void rmnet_map_complement_ipv6_txporthdr_csum_field(void *ip6hdr)
{
	struct ipv6hdr *ip6h = (struct ipv6hdr *)ip6hdr;
	void *txphdr;
	u16 *csum;

	txphdr = ip6hdr + sizeof(struct ipv6hdr);

	if (ip6h->nexthdr == IPPROTO_TCP || ip6h->nexthdr == IPPROTO_UDP) {
		csum = (u16 *)rmnet_map_get_csum_field(ip6h->nexthdr, txphdr);
		*csum = ~(*csum);
	}
}

static void
rmnet_map_ipv6_ul_csum_header(void *ip6hdr,
			      struct rmnet_map_ul_csum_header *ul_header,
			      struct sk_buff *skb)
{
	struct ipv6hdr *ip6h = ip6hdr;
	u16 val;

	val = MAP_CSUM_UL_ENABLED_FLAG;
	if (ip6h->nexthdr == IPPROTO_UDP)
		val |= MAP_CSUM_UL_UDP_FLAG;
	val |= skb->csum_offset & MAP_CSUM_UL_OFFSET_MASK;

	ul_header->csum_start_offset = htons(skb_network_header_len(skb));
	ul_header->csum_info = htons(val);

	skb->ip_summed = CHECKSUM_NONE;

	rmnet_map_complement_ipv6_txporthdr_csum_field(ip6hdr);
}
#endif

/* Adds MAP header to front of skb->data
 * Padding is calculated and set appropriately in MAP header. Mux ID is
 * initialized to 0.
 */
struct rmnet_map_header *rmnet_map_add_map_header(struct sk_buff *skb,
						  int hdrlen, int pad)
{
	struct rmnet_map_header *map_header;
	u32 padding, map_datalen;
	u8 *padbytes;

	map_datalen = skb->len - hdrlen;
	map_header = (struct rmnet_map_header *)
			skb_push(skb, sizeof(struct rmnet_map_header));
	memset(map_header, 0, sizeof(struct rmnet_map_header));

	if (pad == RMNET_MAP_NO_PAD_BYTES) {
		map_header->pkt_len = htons(map_datalen);
		return map_header;
	}

	BUILD_BUG_ON(MAP_PAD_LEN_MASK < 3);
	padding = ALIGN(map_datalen, 4) - map_datalen;

	if (padding == 0)
		goto done;

	if (skb_tailroom(skb) < padding)
		return NULL;

	padbytes = (u8 *)skb_put(skb, padding);
	memset(padbytes, 0, padding);

done:
	map_header->pkt_len = htons(map_datalen + padding);
	/* This is a data packet, so the CMD bit is 0 */
	map_header->flags = padding & MAP_PAD_LEN_MASK;

	return map_header;
}

u32 rmnet_map_validate_packet_len(struct sk_buff *skb, struct rmnet_port *port)
{
	struct rmnet_map_v5_csum_header *next_hdr = NULL;
	struct rmnet_map_header *maph;
	void *data = skb->data;
	u32 packet_len;

	if (skb->len < sizeof(*maph))
		return 0;

	maph = (struct rmnet_map_header *)skb->data;

	/* Some hardware can send us empty frames. Catch them */
	if (!maph->pkt_len)
		return 0;

	packet_len = ntohs(maph->pkt_len) + sizeof(*maph);

	if (port->data_format & RMNET_FLAGS_INGRESS_MAP_CKSUMV4) {
		packet_len += sizeof(struct rmnet_map_dl_csum_trailer);
	} else if ((port->data_format & RMNET_FLAGS_INGRESS_MAP_CKSUMV5) &&
		   !(maph->flags & MAP_CMD_FLAG)) {
		/* Mapv5 data pkt without csum hdr is invalid */
		if (!(maph->flags & MAP_NEXT_HEADER_FLAG))
			return 0;

		packet_len += sizeof(*next_hdr);
		next_hdr = data + sizeof(*maph);
	}

	if (skb->len < packet_len)
		return 0;

	if (next_hdr &&
	    u8_get_bits(next_hdr->header_info, MAPV5_HDRINFO_HDR_TYPE_FMASK) !=
	    RMNET_MAP_HEADER_TYPE_CSUM_OFFLOAD)
		return 0;

	return packet_len;
}

/* Deaggregates a single packet
 * A whole new buffer is allocated for each portion of an aggregated frame.
 * Caller should keep calling deaggregate() on the source skb until 0 is
 * returned, indicating that there are no more packets to deaggregate. Caller
 * is responsible for freeing the original skb.
 */
struct sk_buff *rmnet_map_deaggregate(struct sk_buff *skb,
				      struct rmnet_port *port)
{
	struct sk_buff *skbn;
	u32 packet_len;

	packet_len = rmnet_map_validate_packet_len(skb, port);
	if (!packet_len)
		return NULL;

	skbn = alloc_skb(packet_len + RMNET_MAP_DEAGGR_SPACING, GFP_ATOMIC);
	if (!skbn)
		return NULL;

	skb_reserve(skbn, RMNET_MAP_DEAGGR_HEADROOM);
	skb_put(skbn, packet_len);
	memcpy(skbn->data, skb->data, packet_len);
	skb_pull(skb, packet_len);

	return skbn;
}

/* Validates packet checksums. Function takes a pointer to
 * the beginning of a buffer which contains the IP payload +
 * padding + checksum trailer.
 * Only IPv4 and IPv6 are supported along with TCP & UDP.
 * Fragmented or tunneled packets are not supported.
 */
int rmnet_map_checksum_downlink_packet(struct sk_buff *skb, u16 len)
{
	struct rmnet_priv *priv = netdev_priv(skb->dev);
	struct rmnet_map_dl_csum_trailer *csum_trailer;

	if (unlikely(!(skb->dev->features & NETIF_F_RXCSUM))) {
		priv->stats.csum_sw++;
		return -EOPNOTSUPP;
	}

	csum_trailer = (struct rmnet_map_dl_csum_trailer *)(skb->data + len);

	if (!csum_trailer->valid) {
		priv->stats.csum_valid_unset++;
		return -EINVAL;
	}

	if (skb->protocol == htons(ETH_P_IP)) {
		return rmnet_map_ipv4_dl_csum_trailer(skb, csum_trailer, priv);
	} else if (skb->protocol == htons(ETH_P_IPV6)) {
#if IS_ENABLED(CONFIG_IPV6)
		return rmnet_map_ipv6_dl_csum_trailer(skb, csum_trailer, priv);
#else
		priv->stats.csum_err_invalid_ip_version++;
		return -EPROTONOSUPPORT;
#endif
	} else {
		priv->stats.csum_err_invalid_ip_version++;
		return -EPROTONOSUPPORT;
	}

	return 0;
}

/* Generates UL checksum meta info header for IPv4 and IPv6 over TCP and UDP
 * packets that are supported for UL checksum offload.
 */
void rmnet_map_checksum_uplink_packet(struct sk_buff *skb,
				      struct net_device *orig_dev)
{
	struct rmnet_priv *priv = netdev_priv(orig_dev);
	struct rmnet_map_ul_csum_header *ul_header;
	void *iphdr;

	ul_header = (struct rmnet_map_ul_csum_header *)
		    skb_push(skb, sizeof(struct rmnet_map_ul_csum_header));

	if (unlikely(!(orig_dev->features &
		     (NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM))))
		goto sw_csum;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		iphdr = (char *)ul_header +
			sizeof(struct rmnet_map_ul_csum_header);

		if (skb->protocol == htons(ETH_P_IP)) {
			rmnet_map_ipv4_ul_csum_header(iphdr, ul_header, skb);
			return;
		} else if (skb->protocol == htons(ETH_P_IPV6)) {
#if IS_ENABLED(CONFIG_IPV6)
			rmnet_map_ipv6_ul_csum_header(iphdr, ul_header, skb);
			return;
#else
			priv->stats.csum_err_invalid_ip_version++;
			goto sw_csum;
#endif
		} else {
			priv->stats.csum_err_invalid_ip_version++;
		}
	}

sw_csum:
	memset(ul_header, 0, sizeof(*ul_header));

	priv->stats.csum_sw++;
}

/* Process a MAPv5 packet header */
int rmnet_map_process_next_hdr_packet(struct sk_buff *skb,
				      u16 len)
{
	struct rmnet_priv *priv = netdev_priv(skb->dev);
	struct rmnet_map_v5_csum_header *next_hdr;
	u8 nexthdr_type;

	next_hdr = (struct rmnet_map_v5_csum_header *)(skb->data +
			sizeof(struct rmnet_map_header));

	nexthdr_type = u8_get_bits(next_hdr->header_info,
				   MAPV5_HDRINFO_HDR_TYPE_FMASK);

	if (nexthdr_type != RMNET_MAP_HEADER_TYPE_CSUM_OFFLOAD)
		return -EINVAL;

	if (unlikely(!(skb->dev->features & NETIF_F_RXCSUM))) {
		priv->stats.csum_sw++;
	} else if (next_hdr->csum_info & MAPV5_CSUMINFO_VALID_FLAG) {
		priv->stats.csum_ok++;
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	} else {
		priv->stats.csum_valid_unset++;
	}

	/* Pull csum v5 header */
	skb_pull(skb, sizeof(*next_hdr));

	return 0;
}

#define RMNET_AGG_BYPASS_TIME_NSEC 10000000L

static void reset_aggr_params(struct rmnet_port *port)
{
	port->skbagg_head = NULL;
	port->agg_count = 0;
	port->agg_state = 0;
	memset(&port->agg_time, 0, sizeof(struct timespec64));
}

static void rmnet_send_skb(struct rmnet_port *port, struct sk_buff *skb)
{
	if (skb_needs_linearize(skb, port->dev->features)) {
		if (unlikely(__skb_linearize(skb))) {
			struct rmnet_priv *priv;

			priv = netdev_priv(port->rmnet_dev);
			this_cpu_inc(priv->pcpu_stats->stats.tx_drops);
			dev_kfree_skb_any(skb);
			return;
		}
	}

	dev_queue_xmit(skb);
}

static void rmnet_map_flush_tx_packet_work(struct work_struct *work)
{
	struct sk_buff *skb = NULL;
	struct rmnet_port *port;

	port = container_of(work, struct rmnet_port, agg_wq);

	spin_lock_bh(&port->agg_lock);
	if (likely(port->agg_state == -EINPROGRESS)) {
		/* Buffer may have already been shipped out */
		if (likely(port->skbagg_head)) {
			skb = port->skbagg_head;
			reset_aggr_params(port);
		}
		port->agg_state = 0;
	}

	spin_unlock_bh(&port->agg_lock);
	if (skb)
		rmnet_send_skb(port, skb);
}

static enum hrtimer_restart rmnet_map_flush_tx_packet_queue(struct hrtimer *t)
{
	struct rmnet_port *port;

	port = container_of(t, struct rmnet_port, hrtimer);

	schedule_work(&port->agg_wq);

	return HRTIMER_NORESTART;
}

unsigned int rmnet_map_tx_aggregate(struct sk_buff *skb, struct rmnet_port *port,
				    struct net_device *orig_dev)
{
	struct timespec64 diff, last;
	unsigned int len = skb->len;
	struct sk_buff *agg_skb;
	int size;

	spin_lock_bh(&port->agg_lock);
	memcpy(&last, &port->agg_last, sizeof(struct timespec64));
	ktime_get_real_ts64(&port->agg_last);

	if (!port->skbagg_head) {
		/* Check to see if we should agg first. If the traffic is very
		 * sparse, don't aggregate.
		 */
new_packet:
		diff = timespec64_sub(port->agg_last, last);
		size = port->egress_agg_params.bytes - skb->len;

		if (size < 0) {
			/* dropped */
			spin_unlock_bh(&port->agg_lock);
			return 0;
		}

		if (diff.tv_sec > 0 || diff.tv_nsec > RMNET_AGG_BYPASS_TIME_NSEC ||
		    size == 0)
			goto no_aggr;

		port->skbagg_head = skb_copy_expand(skb, 0, size, GFP_ATOMIC);
		if (!port->skbagg_head)
			goto no_aggr;

		dev_kfree_skb_any(skb);
		port->skbagg_head->protocol = htons(ETH_P_MAP);
		port->agg_count = 1;
		ktime_get_real_ts64(&port->agg_time);
		skb_frag_list_init(port->skbagg_head);
		goto schedule;
	}
	diff = timespec64_sub(port->agg_last, port->agg_time);
	size = port->egress_agg_params.bytes - port->skbagg_head->len;

	if (skb->len > size) {
		agg_skb = port->skbagg_head;
		reset_aggr_params(port);
		spin_unlock_bh(&port->agg_lock);
		hrtimer_cancel(&port->hrtimer);
		rmnet_send_skb(port, agg_skb);
		spin_lock_bh(&port->agg_lock);
		goto new_packet;
	}

	if (skb_has_frag_list(port->skbagg_head))
		port->skbagg_tail->next = skb;
	else
		skb_shinfo(port->skbagg_head)->frag_list = skb;

	port->skbagg_head->len += skb->len;
	port->skbagg_head->data_len += skb->len;
	port->skbagg_head->truesize += skb->truesize;
	port->skbagg_tail = skb;
	port->agg_count++;

	if (diff.tv_sec > 0 || diff.tv_nsec > port->egress_agg_params.time_nsec ||
	    port->agg_count >= port->egress_agg_params.count ||
	    port->skbagg_head->len == port->egress_agg_params.bytes) {
		agg_skb = port->skbagg_head;
		reset_aggr_params(port);
		spin_unlock_bh(&port->agg_lock);
		hrtimer_cancel(&port->hrtimer);
		rmnet_send_skb(port, agg_skb);
		return len;
	}

schedule:
	if (!hrtimer_active(&port->hrtimer) && port->agg_state != -EINPROGRESS) {
		port->agg_state = -EINPROGRESS;
		hrtimer_start(&port->hrtimer,
			      ns_to_ktime(port->egress_agg_params.time_nsec),
			      HRTIMER_MODE_REL);
	}
	spin_unlock_bh(&port->agg_lock);

	return len;

no_aggr:
	spin_unlock_bh(&port->agg_lock);
	skb->protocol = htons(ETH_P_MAP);
	dev_queue_xmit(skb);

	return len;
}

void rmnet_map_update_ul_agg_config(struct rmnet_port *port, u32 size,
				    u32 count, u32 time)
{
	spin_lock_bh(&port->agg_lock);
	port->egress_agg_params.bytes = size;
	WRITE_ONCE(port->egress_agg_params.count, count);
	port->egress_agg_params.time_nsec = time * NSEC_PER_USEC;
	spin_unlock_bh(&port->agg_lock);
}

void rmnet_map_tx_aggregate_init(struct rmnet_port *port)
{
	hrtimer_init(&port->hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	port->hrtimer.function = rmnet_map_flush_tx_packet_queue;
	spin_lock_init(&port->agg_lock);
	rmnet_map_update_ul_agg_config(port, 4096, 1, 800);
	INIT_WORK(&port->agg_wq, rmnet_map_flush_tx_packet_work);
}

void rmnet_map_tx_aggregate_exit(struct rmnet_port *port)
{
	hrtimer_cancel(&port->hrtimer);
	cancel_work_sync(&port->agg_wq);

	spin_lock_bh(&port->agg_lock);
	if (port->agg_state == -EINPROGRESS) {
		if (port->skbagg_head) {
			dev_kfree_skb_any(port->skbagg_head);
			reset_aggr_params(port);
		}

		port->agg_state = 0;
	}
	spin_unlock_bh(&port->agg_lock);
}
