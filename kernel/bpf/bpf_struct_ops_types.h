/* SPDX-License-Identifier: GPL-2.0 */
/* internal file - do not include directly */

#ifdef CONFIG_BPF_JIT
#ifdef CONFIG_NET
BPF_STRUCT_OPS_TYPE(bpf_dummy_ops)
#endif
#ifdef CONFIG_INET
#include <net/tcp.h>
BPF_STRUCT_OPS_TYPE(tcp_congestion_ops)
#endif
#ifdef CONFIG_SCHED_CLASS_EXT
#include <linux/sched/ext.h>
BPF_STRUCT_OPS_TYPE(sched_ext_ops)
#endif
#endif
