// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

int cork_byte;
int push_start;
int push_end;
int apply_bytes;
int pop_start;
int pop_end;

struct {
	__uint(type, BPF_MAP_TYPE_SOCKMAP);
	__uint(max_entries, 20);
	__type(key, int);
	__type(value, int);
} sock_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_SOCKMAP);
	__uint(max_entries, 2);
	__type(key, int);
	__type(value, int);
} sock_map_verdict SEC(".maps");

SEC("sk_msg")
int prog_sk_policy(struct sk_msg_md *msg)
{
	if (cork_byte > 0)
		bpf_msg_cork_bytes(msg, cork_byte);
	if (push_start > 0 && push_end > 0)
		bpf_msg_push_data(msg, push_start, push_end, 0);
	if (pop_start >= 0 && pop_end > 0)
		bpf_msg_pop_data(msg, pop_start, pop_end, 0);

	return SK_PASS;
}

SEC("sk_msg")
int prog_sk_policy_redir(struct sk_msg_md *msg)
{
	int two = 2;

	bpf_msg_apply_bytes(msg, apply_bytes);
	return bpf_msg_redirect_map(msg, &sock_map, two, 0);
}

/*
 * Verdict program for the reverse-order TLS/sockmap regression test.
 * Returns SK_PASS so tcp_read_skb() drains the receive queue via
 * sk_psock_verdict_recv() without calling tcp_eat_skb(), which is
 * the precondition for the KTLS strparser frag_list UAF.
 */
SEC("sk_skb/verdict")
int prog_skb_verdict_pass(struct __sk_buff *skb)
{
	return SK_PASS;
}

char _license[] SEC("license") = "GPL";
