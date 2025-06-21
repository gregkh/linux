/* net/core/custom_net_hook.c */
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/export.h>
#include <linux/netlink_custom_hook.h>

volatile custom_net_hook_fn_t custom_net_hook = NULL;
EXPORT_SYMBOL(custom_net_hook);

