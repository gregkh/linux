/* include/linux/netlink_custom_hook.h */
#ifndef _NETLINK_CUSTOM_HOOK_H
#define _NETLINK_CUSTOM_HOOK_H
#include <linux/skbuff.h>
typedef bool (*custom_net_hook_fn_t)(struct sk_buff *skb);
extern volatile custom_net_hook_fn_t custom_net_hook;
#endif
