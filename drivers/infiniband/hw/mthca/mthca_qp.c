/*
 * Copyright (c) 2004 Topspin Communications.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * $Id: mthca_qp.c 1355 2004-12-17 15:23:43Z roland $
 */

#include <linux/init.h>

#include <ib_verbs.h>
#include <ib_cache.h>
#include <ib_pack.h>

#include "mthca_dev.h"
#include "mthca_cmd.h"

enum {
	MTHCA_MAX_DIRECT_QP_SIZE = 4 * PAGE_SIZE,
	MTHCA_ACK_REQ_FREQ       = 10,
	MTHCA_FLIGHT_LIMIT       = 9,
	MTHCA_UD_HEADER_SIZE     = 72 /* largest UD header possible */
};

enum {
	MTHCA_QP_STATE_RST  = 0,
	MTHCA_QP_STATE_INIT = 1,
	MTHCA_QP_STATE_RTR  = 2,
	MTHCA_QP_STATE_RTS  = 3,
	MTHCA_QP_STATE_SQE  = 4,
	MTHCA_QP_STATE_SQD  = 5,
	MTHCA_QP_STATE_ERR  = 6,
	MTHCA_QP_STATE_DRAINING = 7
};

enum {
	MTHCA_QP_ST_RC 	= 0x0,
	MTHCA_QP_ST_UC 	= 0x1,
	MTHCA_QP_ST_RD 	= 0x2,
	MTHCA_QP_ST_UD 	= 0x3,
	MTHCA_QP_ST_MLX = 0x7
};

enum {
	MTHCA_QP_PM_MIGRATED = 0x3,
	MTHCA_QP_PM_ARMED    = 0x0,
	MTHCA_QP_PM_REARM    = 0x1
};

enum {
	/* qp_context flags */
	MTHCA_QP_BIT_DE  = 1 <<  8,
	/* params1 */
	MTHCA_QP_BIT_SRE = 1 << 15,
	MTHCA_QP_BIT_SWE = 1 << 14,
	MTHCA_QP_BIT_SAE = 1 << 13,
	MTHCA_QP_BIT_SIC = 1 <<  4,
	MTHCA_QP_BIT_SSC = 1 <<  3,
	/* params2 */
	MTHCA_QP_BIT_RRE = 1 << 15,
	MTHCA_QP_BIT_RWE = 1 << 14,
	MTHCA_QP_BIT_RAE = 1 << 13,
	MTHCA_QP_BIT_RIC = 1 <<  4,
	MTHCA_QP_BIT_RSC = 1 <<  3
};

struct mthca_qp_path {
	u32 port_pkey;
	u8  rnr_retry;
	u8  g_mylmc;
	u16 rlid;
	u8  ackto;
	u8  mgid_index;
	u8  static_rate;
	u8  hop_limit;
	u32 sl_tclass_flowlabel;
	u8  rgid[16];
} __attribute__((packed));

struct mthca_qp_context {
	u32 flags;
	u32 sched_queue;
	u32 mtu_msgmax;
	u32 usr_page;
	u32 local_qpn;
	u32 remote_qpn;
	u32 reserved1[2];
	struct mthca_qp_path pri_path;
	struct mthca_qp_path alt_path;
	u32 rdd;
	u32 pd;
	u32 wqe_base;
	u32 wqe_lkey;
	u32 params1;
	u32 reserved2;
	u32 next_send_psn;
	u32 cqn_snd;
	u32 next_snd_wqe[2];
	u32 last_acked_psn;
	u32 ssn;
	u32 params2;
	u32 rnr_nextrecvpsn;
	u32 ra_buff_indx;
	u32 cqn_rcv;
	u32 next_rcv_wqe[2];
	u32 qkey;
	u32 srqn;
	u32 rmsn;
	u32 reserved3[19];
} __attribute__((packed));

struct mthca_qp_param {
	u32 opt_param_mask;
	u32 reserved1;
	struct mthca_qp_context context;
	u32 reserved2[62];
} __attribute__((packed));

enum {
	MTHCA_QP_OPTPAR_ALT_ADDR_PATH     = 1 << 0,
	MTHCA_QP_OPTPAR_RRE               = 1 << 1,
	MTHCA_QP_OPTPAR_RAE               = 1 << 2,
	MTHCA_QP_OPTPAR_RWE               = 1 << 3,
	MTHCA_QP_OPTPAR_PKEY_INDEX        = 1 << 4,
	MTHCA_QP_OPTPAR_Q_KEY             = 1 << 5,
	MTHCA_QP_OPTPAR_RNR_TIMEOUT       = 1 << 6,
	MTHCA_QP_OPTPAR_PRIMARY_ADDR_PATH = 1 << 7,
	MTHCA_QP_OPTPAR_SRA_MAX           = 1 << 8,
	MTHCA_QP_OPTPAR_RRA_MAX           = 1 << 9,
	MTHCA_QP_OPTPAR_PM_STATE          = 1 << 10,
	MTHCA_QP_OPTPAR_PORT_NUM          = 1 << 11,
	MTHCA_QP_OPTPAR_RETRY_COUNT       = 1 << 12,
	MTHCA_QP_OPTPAR_ALT_RNR_RETRY     = 1 << 13,
	MTHCA_QP_OPTPAR_ACK_TIMEOUT       = 1 << 14,
	MTHCA_QP_OPTPAR_RNR_RETRY         = 1 << 15,
	MTHCA_QP_OPTPAR_SCHED_QUEUE       = 1 << 16
};

enum {
	MTHCA_OPCODE_NOP            = 0x00,
	MTHCA_OPCODE_RDMA_WRITE     = 0x08,
	MTHCA_OPCODE_RDMA_WRITE_IMM = 0x09,
	MTHCA_OPCODE_SEND           = 0x0a,
	MTHCA_OPCODE_SEND_IMM       = 0x0b,
	MTHCA_OPCODE_RDMA_READ      = 0x10,
	MTHCA_OPCODE_ATOMIC_CS      = 0x11,
	MTHCA_OPCODE_ATOMIC_FA      = 0x12,
	MTHCA_OPCODE_BIND_MW        = 0x18,
	MTHCA_OPCODE_INVALID        = 0xff
};

enum {
	MTHCA_NEXT_DBD       = 1 << 7,
	MTHCA_NEXT_FENCE     = 1 << 6,
	MTHCA_NEXT_CQ_UPDATE = 1 << 3,
	MTHCA_NEXT_EVENT_GEN = 1 << 2,
	MTHCA_NEXT_SOLICIT   = 1 << 1,

	MTHCA_MLX_VL15       = 1 << 17,
	MTHCA_MLX_SLR        = 1 << 16
};

struct mthca_next_seg {
	u32 nda_op;		/* [31:6] next WQE [4:0] next opcode */
	u32 ee_nds;		/* [31:8] next EE  [7] DBD [6] F [5:0] next WQE size */
	u32 flags;		/* [3] CQ [2] Event [1] Solicit */
	u32 imm;		/* immediate data */
};

struct mthca_ud_seg {
	u32 reserved1;
	u32 lkey;
	u64 av_addr;
	u32 reserved2[4];
	u32 dqpn;
	u32 qkey;
	u32 reserved3[2];
};

struct mthca_bind_seg {
	u32 flags;		/* [31] Atomic [30] rem write [29] rem read */
	u32 reserved;
	u32 new_rkey;
	u32 lkey;
	u64 addr;
	u64 length;
};

struct mthca_raddr_seg {
	u64 raddr;
	u32 rkey;
	u32 reserved;
};

struct mthca_atomic_seg {
	u64 swap_add;
	u64 compare;
};

struct mthca_data_seg {
	u32 byte_count;
	u32 lkey;
	u64 addr;
};

struct mthca_mlx_seg {
	u32 nda_op;
	u32 nds;
	u32 flags;		/* [17] VL15 [16] SLR [14:12] static rate
				   [11:8] SL [3] C [2] E */
	u16 rlid;
	u16 vcrc;
};

static int is_sqp(struct mthca_dev *dev, struct mthca_qp *qp)
{
	return qp->qpn >= dev->qp_table.sqp_start &&
		qp->qpn <= dev->qp_table.sqp_start + 3;
}

static int is_qp0(struct mthca_dev *dev, struct mthca_qp *qp)
{
	return qp->qpn >= dev->qp_table.sqp_start &&
		qp->qpn <= dev->qp_table.sqp_start + 1;
}

static void *get_recv_wqe(struct mthca_qp *qp, int n)
{
	if (qp->is_direct)
		return qp->queue.direct.buf + (n << qp->rq.wqe_shift);
	else
		return qp->queue.page_list[(n << qp->rq.wqe_shift) >> PAGE_SHIFT].buf +
			((n << qp->rq.wqe_shift) & (PAGE_SIZE - 1));
}

static void *get_send_wqe(struct mthca_qp *qp, int n)
{
	if (qp->is_direct)
		return qp->queue.direct.buf + qp->send_wqe_offset +
			(n << qp->sq.wqe_shift);
	else
		return qp->queue.page_list[(qp->send_wqe_offset +
					    (n << qp->sq.wqe_shift)) >>
					   PAGE_SHIFT].buf +
			((qp->send_wqe_offset + (n << qp->sq.wqe_shift)) &
			 (PAGE_SIZE - 1));
}

void mthca_qp_event(struct mthca_dev *dev, u32 qpn,
		    enum ib_event_type event_type)
{
	struct mthca_qp *qp;
	struct ib_event event;

	spin_lock(&dev->qp_table.lock);
	qp = mthca_array_get(&dev->qp_table.qp, qpn & (dev->limits.num_qps - 1));
	if (qp)
		atomic_inc(&qp->refcount);
	spin_unlock(&dev->qp_table.lock);

	if (!qp) {
		mthca_warn(dev, "Async event for bogus QP %08x\n", qpn);
		return;
	}

	event.device      = &dev->ib_dev;
	event.event       = event_type;
	event.element.qp  = &qp->ibqp;
	if (qp->ibqp.event_handler)
		qp->ibqp.event_handler(&event, qp->ibqp.qp_context);

	if (atomic_dec_and_test(&qp->refcount))
		wake_up(&qp->wait);
}

static int to_mthca_state(enum ib_qp_state ib_state)
{
	switch (ib_state) {
	case IB_QPS_RESET: return MTHCA_QP_STATE_RST;
	case IB_QPS_INIT:  return MTHCA_QP_STATE_INIT;
	case IB_QPS_RTR:   return MTHCA_QP_STATE_RTR;
	case IB_QPS_RTS:   return MTHCA_QP_STATE_RTS;
	case IB_QPS_SQD:   return MTHCA_QP_STATE_SQD;
	case IB_QPS_SQE:   return MTHCA_QP_STATE_SQE;
	case IB_QPS_ERR:   return MTHCA_QP_STATE_ERR;
	default:                return -1;
	}
}

enum { RC, UC, UD, RD, RDEE, MLX, NUM_TRANS };

static int to_mthca_st(int transport)
{
	switch (transport) {
	case RC:  return MTHCA_QP_ST_RC;
	case UC:  return MTHCA_QP_ST_UC;
	case UD:  return MTHCA_QP_ST_UD;
	case RD:  return MTHCA_QP_ST_RD;
	case MLX: return MTHCA_QP_ST_MLX;
	default:  return -1;
	}
}

static const struct {
	int trans;
	u32 req_param[NUM_TRANS];
	u32 opt_param[NUM_TRANS];
} state_table[IB_QPS_ERR + 1][IB_QPS_ERR + 1] = {
	[IB_QPS_RESET] = {
		[IB_QPS_RESET] = { .trans = MTHCA_TRANS_ANY2RST },
		[IB_QPS_ERR] = { .trans = MTHCA_TRANS_ANY2ERR },
		[IB_QPS_INIT]  = {
			.trans = MTHCA_TRANS_RST2INIT,
			.req_param = {
				[UD]  = (IB_QP_PKEY_INDEX |
					 IB_QP_PORT       |
					 IB_QP_QKEY),
				[RC]  = (IB_QP_PKEY_INDEX |
					 IB_QP_PORT       |
					 IB_QP_ACCESS_FLAGS),
				[MLX] = (IB_QP_PKEY_INDEX |
					 IB_QP_QKEY),
			},
			/* bug-for-bug compatibility with VAPI: */
			.opt_param = {
				[MLX] = IB_QP_PORT
			}
		},
	},
	[IB_QPS_INIT]  = {
		[IB_QPS_RESET] = { .trans = MTHCA_TRANS_ANY2RST },
		[IB_QPS_ERR] = { .trans = MTHCA_TRANS_ANY2ERR },
		[IB_QPS_INIT]  = {
			.trans = MTHCA_TRANS_INIT2INIT,
			.opt_param = {
				[UD]  = (IB_QP_PKEY_INDEX |
					 IB_QP_PORT       |
					 IB_QP_QKEY),
				[RC]  = (IB_QP_PKEY_INDEX |
					 IB_QP_PORT       |
					 IB_QP_ACCESS_FLAGS),
				[MLX] = (IB_QP_PKEY_INDEX |
					 IB_QP_QKEY),
			}
		},
		[IB_QPS_RTR]   = {
			.trans = MTHCA_TRANS_INIT2RTR,
			.req_param = {
				[RC]  = (IB_QP_AV                  |
					 IB_QP_PATH_MTU            |
					 IB_QP_DEST_QPN            |
					 IB_QP_RQ_PSN              |
					 IB_QP_MAX_DEST_RD_ATOMIC  |
					 IB_QP_MIN_RNR_TIMER),
			},
			.opt_param = {
				[UD]  = (IB_QP_PKEY_INDEX |
					 IB_QP_QKEY),
				[RC]  = (IB_QP_ALT_PATH     |
					 IB_QP_ACCESS_FLAGS |
					 IB_QP_PKEY_INDEX),
				[MLX] = (IB_QP_PKEY_INDEX |
					 IB_QP_QKEY),
			}
		}
	},
	[IB_QPS_RTR]   = {
		[IB_QPS_RESET] = { .trans = MTHCA_TRANS_ANY2RST },
		[IB_QPS_ERR] = { .trans = MTHCA_TRANS_ANY2ERR },
		[IB_QPS_RTS]   = {
			.trans = MTHCA_TRANS_RTR2RTS,
			.req_param = {
				[UD]  = IB_QP_SQ_PSN,
				[RC]  = (IB_QP_TIMEOUT           |
					 IB_QP_RETRY_CNT         |
					 IB_QP_RNR_RETRY         |
					 IB_QP_SQ_PSN            |
					 IB_QP_MAX_QP_RD_ATOMIC),
				[MLX] = IB_QP_SQ_PSN,
			},
			.opt_param = {
				[UD]  = (IB_QP_CUR_STATE             |
					 IB_QP_QKEY),
				[RC]  = (IB_QP_CUR_STATE             |
					 IB_QP_ALT_PATH              |
					 IB_QP_ACCESS_FLAGS          |
					 IB_QP_PKEY_INDEX            |
					 IB_QP_MIN_RNR_TIMER         |
					 IB_QP_PATH_MIG_STATE),
				[MLX] = (IB_QP_CUR_STATE             |
					 IB_QP_QKEY),
			}
		}
	},
	[IB_QPS_RTS]   = {
		[IB_QPS_RESET] = { .trans = MTHCA_TRANS_ANY2RST },
		[IB_QPS_ERR] = { .trans = MTHCA_TRANS_ANY2ERR },
		[IB_QPS_RTS]   = {
			.trans = MTHCA_TRANS_RTS2RTS,
			.opt_param = {
				[UD]  = (IB_QP_CUR_STATE             |
					 IB_QP_QKEY),
				[RC]  = (IB_QP_ACCESS_FLAGS          |
					 IB_QP_ALT_PATH              |
					 IB_QP_PATH_MIG_STATE        |
					 IB_QP_MIN_RNR_TIMER),
				[MLX] = (IB_QP_CUR_STATE             |
					 IB_QP_QKEY),
			}
		},
		[IB_QPS_SQD]   = {
			.trans = MTHCA_TRANS_RTS2SQD,
		},
	},
	[IB_QPS_SQD]   = {
		[IB_QPS_RESET] = { .trans = MTHCA_TRANS_ANY2RST },
		[IB_QPS_ERR] = { .trans = MTHCA_TRANS_ANY2ERR },
		[IB_QPS_RTS]   = {
			.trans = MTHCA_TRANS_SQD2RTS,
			.opt_param = {
				[UD]  = (IB_QP_CUR_STATE             |
					 IB_QP_QKEY),
				[RC]  = (IB_QP_CUR_STATE             |
					 IB_QP_ALT_PATH              |
					 IB_QP_ACCESS_FLAGS          |
					 IB_QP_MIN_RNR_TIMER         |
					 IB_QP_PATH_MIG_STATE),
				[MLX] = (IB_QP_CUR_STATE             |
					 IB_QP_QKEY),
			}
		},
		[IB_QPS_SQD]   = {
			.trans = MTHCA_TRANS_SQD2SQD,
			.opt_param = {
				[UD]  = (IB_QP_PKEY_INDEX            |
					 IB_QP_QKEY),
				[RC]  = (IB_QP_AV                    |
					 IB_QP_TIMEOUT               |
					 IB_QP_RETRY_CNT             |
					 IB_QP_RNR_RETRY             |
					 IB_QP_MAX_QP_RD_ATOMIC      |
					 IB_QP_MAX_DEST_RD_ATOMIC    |
					 IB_QP_CUR_STATE             |
					 IB_QP_ALT_PATH              |
					 IB_QP_ACCESS_FLAGS          |
					 IB_QP_PKEY_INDEX            |
					 IB_QP_MIN_RNR_TIMER         |
					 IB_QP_PATH_MIG_STATE),
				[MLX] = (IB_QP_PKEY_INDEX            |
					 IB_QP_QKEY),
			}
		}
	},
	[IB_QPS_SQE]   = {
		[IB_QPS_RESET] = { .trans = MTHCA_TRANS_ANY2RST },
		[IB_QPS_ERR] = { .trans = MTHCA_TRANS_ANY2ERR },
		[IB_QPS_RTS]   = {
			.trans = MTHCA_TRANS_SQERR2RTS,
			.opt_param = {
				[UD]  = (IB_QP_CUR_STATE             |
					 IB_QP_QKEY),
				[RC]  = (IB_QP_CUR_STATE             |
					 IB_QP_MIN_RNR_TIMER),
				[MLX] = (IB_QP_CUR_STATE             |
					 IB_QP_QKEY),
			}
		}
	},
	[IB_QPS_ERR] = {
		[IB_QPS_RESET] = { .trans = MTHCA_TRANS_ANY2RST },
		[IB_QPS_ERR] = { .trans = MTHCA_TRANS_ANY2ERR }
	}
};

static void store_attrs(struct mthca_sqp *sqp, struct ib_qp_attr *attr,
			int attr_mask)
{
	if (attr_mask & IB_QP_PKEY_INDEX)
		sqp->pkey_index = attr->pkey_index;
	if (attr_mask & IB_QP_QKEY)
		sqp->qkey = attr->qkey;
	if (attr_mask & IB_QP_SQ_PSN)
		sqp->send_psn = attr->sq_psn;
}

static void init_port(struct mthca_dev *dev, int port)
{
	int err;
	u8 status;
	struct mthca_init_ib_param param;

	memset(&param, 0, sizeof param);

	param.enable_1x = 1;
	param.enable_4x = 1;
	param.vl_cap    = dev->limits.vl_cap;
	param.mtu_cap   = dev->limits.mtu_cap;
	param.gid_cap   = dev->limits.gid_table_len;
	param.pkey_cap  = dev->limits.pkey_table_len;

	err = mthca_INIT_IB(dev, &param, port, &status);
	if (err)
		mthca_warn(dev, "INIT_IB failed, return code %d.\n", err);
	if (status)
		mthca_warn(dev, "INIT_IB returned status %02x.\n", status);
}

int mthca_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr, int attr_mask)
{
	struct mthca_dev *dev = to_mdev(ibqp->device);
	struct mthca_qp *qp = to_mqp(ibqp);
	enum ib_qp_state cur_state, new_state;
	void *mailbox = NULL;
	struct mthca_qp_param *qp_param;
	struct mthca_qp_context *qp_context;
	u32 req_param, opt_param;
	u8 status;
	int err;

	if (attr_mask & IB_QP_CUR_STATE) {
		if (attr->cur_qp_state != IB_QPS_RTR &&
		    attr->cur_qp_state != IB_QPS_RTS &&
		    attr->cur_qp_state != IB_QPS_SQD &&
		    attr->cur_qp_state != IB_QPS_SQE)
			return -EINVAL;
		else
			cur_state = attr->cur_qp_state;
	} else {
		spin_lock_irq(&qp->lock);
		cur_state = qp->state;
		spin_unlock_irq(&qp->lock);
	}

	if (attr_mask & IB_QP_STATE) {
               if (attr->qp_state < 0 || attr->qp_state > IB_QPS_ERR)
			return -EINVAL;
		new_state = attr->qp_state;
	} else
		new_state = cur_state;

	if (state_table[cur_state][new_state].trans == MTHCA_TRANS_INVALID) {
		mthca_dbg(dev, "Illegal QP transition "
			  "%d->%d\n", cur_state, new_state);
		return -EINVAL;
	}

	req_param = state_table[cur_state][new_state].req_param[qp->transport];
	opt_param = state_table[cur_state][new_state].opt_param[qp->transport];

	if ((req_param & attr_mask) != req_param) {
		mthca_dbg(dev, "QP transition "
			  "%d->%d missing req attr 0x%08x\n",
			  cur_state, new_state,
			  req_param & ~attr_mask);
		return -EINVAL;
	}

	if (attr_mask & ~(req_param | opt_param | IB_QP_STATE)) {
		mthca_dbg(dev, "QP transition (transport %d) "
			  "%d->%d has extra attr 0x%08x\n",
			  qp->transport,
			  cur_state, new_state,
			  attr_mask & ~(req_param | opt_param |
						 IB_QP_STATE));
		return -EINVAL;
	}

	mailbox = kmalloc(sizeof (*qp_param) + MTHCA_CMD_MAILBOX_EXTRA, GFP_KERNEL);
	if (!mailbox)
		return -ENOMEM;
	qp_param = MAILBOX_ALIGN(mailbox);
	qp_context = &qp_param->context;
	memset(qp_param, 0, sizeof *qp_param);

	qp_context->flags      = cpu_to_be32((to_mthca_state(new_state) << 28) |
					     (to_mthca_st(qp->transport) << 16));
	qp_context->flags     |= cpu_to_be32(MTHCA_QP_BIT_DE);
	if (!(attr_mask & IB_QP_PATH_MIG_STATE))
		qp_context->flags |= cpu_to_be32(MTHCA_QP_PM_MIGRATED << 11);
	else {
		qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_PM_STATE);
		switch (attr->path_mig_state) {
		case IB_MIG_MIGRATED:
			qp_context->flags |= cpu_to_be32(MTHCA_QP_PM_MIGRATED << 11);
			break;
		case IB_MIG_REARM:
			qp_context->flags |= cpu_to_be32(MTHCA_QP_PM_REARM << 11);
			break;
		case IB_MIG_ARMED:
			qp_context->flags |= cpu_to_be32(MTHCA_QP_PM_ARMED << 11);
			break;
		}
	}
	/* leave sched_queue as 0 */
	if (qp->transport == MLX || qp->transport == UD)
		qp_context->mtu_msgmax = cpu_to_be32((IB_MTU_2048 << 29) |
						     (11 << 24));
	else if (attr_mask & IB_QP_PATH_MTU) {
		qp_context->mtu_msgmax = cpu_to_be32((attr->path_mtu << 29) |
						     (31 << 24));
	}
	qp_context->usr_page   = cpu_to_be32(MTHCA_KAR_PAGE);
	qp_context->local_qpn  = cpu_to_be32(qp->qpn);
	if (attr_mask & IB_QP_DEST_QPN) {
		qp_context->remote_qpn = cpu_to_be32(attr->dest_qp_num);
	}

	if (qp->transport == MLX)
		qp_context->pri_path.port_pkey |=
			cpu_to_be32(to_msqp(qp)->port << 24);
	else {
		if (attr_mask & IB_QP_PORT) {
			qp_context->pri_path.port_pkey |=
				cpu_to_be32(attr->port_num << 24);
			qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_PORT_NUM);
		}
	}

	if (attr_mask & IB_QP_PKEY_INDEX) {
		qp_context->pri_path.port_pkey |=
			cpu_to_be32(attr->pkey_index);
		qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_PKEY_INDEX);
	}

	if (attr_mask & IB_QP_RNR_RETRY) {
		qp_context->pri_path.rnr_retry = attr->rnr_retry << 5;
		qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_RNR_RETRY);
	}

	if (attr_mask & IB_QP_AV) {
		qp_context->pri_path.g_mylmc     = attr->ah_attr.src_path_bits & 0x7f;
		qp_context->pri_path.rlid        = cpu_to_be16(attr->ah_attr.dlid);
		qp_context->pri_path.static_rate = (!!attr->ah_attr.static_rate) << 3;
		if (attr->ah_attr.ah_flags & IB_AH_GRH) {
			qp_context->pri_path.g_mylmc |= 1 << 7;
			qp_context->pri_path.mgid_index = attr->ah_attr.grh.sgid_index;
			qp_context->pri_path.hop_limit = attr->ah_attr.grh.hop_limit;
			qp_context->pri_path.sl_tclass_flowlabel =
				cpu_to_be32((attr->ah_attr.sl << 28)                |
					    (attr->ah_attr.grh.traffic_class << 20) |
					    (attr->ah_attr.grh.flow_label));
			memcpy(qp_context->pri_path.rgid,
			       attr->ah_attr.grh.dgid.raw, 16);
		} else {
			qp_context->pri_path.sl_tclass_flowlabel =
				cpu_to_be32(attr->ah_attr.sl << 28);
		}
		qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_PRIMARY_ADDR_PATH);
	}

	if (attr_mask & IB_QP_TIMEOUT) {
		qp_context->pri_path.ackto = attr->timeout;
		qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_ACK_TIMEOUT);
	}

	/* XXX alt_path */

	/* leave rdd as 0 */
	qp_context->pd         = cpu_to_be32(to_mpd(ibqp->pd)->pd_num);
	/* leave wqe_base as 0 (we always create an MR based at 0 for WQs) */
	qp_context->wqe_lkey   = cpu_to_be32(qp->mr.ibmr.lkey);
	qp_context->params1    = cpu_to_be32((MTHCA_ACK_REQ_FREQ << 28) |
					     (MTHCA_FLIGHT_LIMIT << 24) |
					     MTHCA_QP_BIT_SRE           |
					     MTHCA_QP_BIT_SWE           |
					     MTHCA_QP_BIT_SAE);
	if (qp->sq.policy == IB_SIGNAL_ALL_WR)
		qp_context->params1 |= cpu_to_be32(MTHCA_QP_BIT_SSC);
	if (attr_mask & IB_QP_RETRY_CNT) {
		qp_context->params1 |= cpu_to_be32(attr->retry_cnt << 16);
		qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_RETRY_COUNT);
	}

	if (attr_mask & IB_QP_MAX_DEST_RD_ATOMIC) {
		qp_context->params1 |= cpu_to_be32(min(attr->max_dest_rd_atomic ?
						       ffs(attr->max_dest_rd_atomic) - 1 : 0,
						       7) << 21);
		qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_SRA_MAX);
	}

	if (attr_mask & IB_QP_SQ_PSN)
		qp_context->next_send_psn = cpu_to_be32(attr->sq_psn);
	qp_context->cqn_snd = cpu_to_be32(to_mcq(ibqp->send_cq)->cqn);

	if (attr_mask & IB_QP_ACCESS_FLAGS) {
		/*
		 * Only enable RDMA/atomics if we have responder
		 * resources set to a non-zero value.
		 */
		if (qp->resp_depth) {
			qp_context->params2 |=
				cpu_to_be32(attr->qp_access_flags & IB_ACCESS_REMOTE_WRITE ?
					    MTHCA_QP_BIT_RWE : 0);
			qp_context->params2 |=
				cpu_to_be32(attr->qp_access_flags & IB_ACCESS_REMOTE_READ ?
					    MTHCA_QP_BIT_RRE : 0);
			qp_context->params2 |=
				cpu_to_be32(attr->qp_access_flags & IB_ACCESS_REMOTE_ATOMIC ?
					    MTHCA_QP_BIT_RAE : 0);
		}

		qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_RWE |
							MTHCA_QP_OPTPAR_RRE |
							MTHCA_QP_OPTPAR_RAE);

		qp->atomic_rd_en = attr->qp_access_flags;
	}

	if (attr_mask & IB_QP_MAX_QP_RD_ATOMIC) {
		u8 rra_max;

		if (qp->resp_depth && !attr->max_rd_atomic) {
			/*
			 * Lowering our responder resources to zero.
			 * Turn off RDMA/atomics as responder.
			 * (RWE/RRE/RAE in params2 already zero)
			 */
			qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_RWE |
								MTHCA_QP_OPTPAR_RRE |
								MTHCA_QP_OPTPAR_RAE);
		}

		if (!qp->resp_depth && attr->max_rd_atomic) {
			/*
			 * Increasing our responder resources from
			 * zero.  Turn on RDMA/atomics as appropriate.
			 */
			qp_context->params2 |=
				cpu_to_be32(qp->atomic_rd_en & IB_ACCESS_REMOTE_WRITE ?
					    MTHCA_QP_BIT_RWE : 0);
			qp_context->params2 |=
				cpu_to_be32(qp->atomic_rd_en & IB_ACCESS_REMOTE_READ ?
					    MTHCA_QP_BIT_RRE : 0);
			qp_context->params2 |=
				cpu_to_be32(qp->atomic_rd_en & IB_ACCESS_REMOTE_ATOMIC ?
					    MTHCA_QP_BIT_RAE : 0);

			qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_RWE |
								MTHCA_QP_OPTPAR_RRE |
								MTHCA_QP_OPTPAR_RAE);
		}

		for (rra_max = 0;
		     1 << rra_max < attr->max_rd_atomic &&
			     rra_max < dev->qp_table.rdb_shift;
		     ++rra_max)
			; /* nothing */

		qp_context->params2      |= cpu_to_be32(rra_max << 21);
		qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_RRA_MAX);

		qp->resp_depth = attr->max_rd_atomic;
	}

	if (qp->rq.policy == IB_SIGNAL_ALL_WR)
		qp_context->params2 |= cpu_to_be32(MTHCA_QP_BIT_RSC);
	if (attr_mask & IB_QP_MIN_RNR_TIMER) {
		qp_context->rnr_nextrecvpsn |= cpu_to_be32(attr->min_rnr_timer << 24);
		qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_RNR_TIMEOUT);
	}
	if (attr_mask & IB_QP_RQ_PSN)
		qp_context->rnr_nextrecvpsn |= cpu_to_be32(attr->rq_psn);

	qp_context->ra_buff_indx = dev->qp_table.rdb_base +
		((qp->qpn & (dev->limits.num_qps - 1)) * MTHCA_RDB_ENTRY_SIZE <<
		 dev->qp_table.rdb_shift);

	qp_context->cqn_rcv = cpu_to_be32(to_mcq(ibqp->recv_cq)->cqn);

	if (attr_mask & IB_QP_QKEY) {
		qp_context->qkey = cpu_to_be32(attr->qkey);
		qp_param->opt_param_mask |= cpu_to_be32(MTHCA_QP_OPTPAR_Q_KEY);
	}

	err = mthca_MODIFY_QP(dev, state_table[cur_state][new_state].trans,
			      qp->qpn, 0, qp_param, 0, &status);
	if (status) {
		mthca_warn(dev, "modify QP %d returned status %02x.\n",
			   state_table[cur_state][new_state].trans, status);
		err = -EINVAL;
	}

	if (!err)
		qp->state = new_state;

	kfree(mailbox);

	if (is_sqp(dev, qp))
		store_attrs(to_msqp(qp), attr, attr_mask);

	/*
	 * If we are moving QP0 to RTR, bring the IB link up; if we
	 * are moving QP0 to RESET or ERROR, bring the link back down.
	 */
	if (is_qp0(dev, qp)) {
		if (cur_state != IB_QPS_RTR &&
		    new_state == IB_QPS_RTR)
			init_port(dev, to_msqp(qp)->port);

		if (cur_state != IB_QPS_RESET &&
		    cur_state != IB_QPS_ERR &&
		    (new_state == IB_QPS_RESET ||
		     new_state == IB_QPS_ERR))
			mthca_CLOSE_IB(dev, to_msqp(qp)->port, &status);
	}

	return err;
}

/*
 * Allocate and register buffer for WQEs.  qp->rq.max, sq.max,
 * rq.max_gs and sq.max_gs must all be assigned.
 * mthca_alloc_wqe_buf will calculate rq.wqe_shift and
 * sq.wqe_shift (as well as send_wqe_offset, is_direct, and
 * queue)
 */
static int mthca_alloc_wqe_buf(struct mthca_dev *dev,
			       struct mthca_pd *pd,
			       struct mthca_qp *qp)
{
	int size;
	int i;
	int npages, shift;
	dma_addr_t t;
	u64 *dma_list = NULL;
	int err = -ENOMEM;

	size = sizeof (struct mthca_next_seg) +
		qp->rq.max_gs * sizeof (struct mthca_data_seg);

	for (qp->rq.wqe_shift = 6; 1 << qp->rq.wqe_shift < size;
	     qp->rq.wqe_shift++)
		; /* nothing */

	size = sizeof (struct mthca_next_seg) +
		qp->sq.max_gs * sizeof (struct mthca_data_seg);
	if (qp->transport == MLX)
		size += 2 * sizeof (struct mthca_data_seg);
	else if (qp->transport == UD)
		size += sizeof (struct mthca_ud_seg);
	else /* bind seg is as big as atomic + raddr segs */
		size += sizeof (struct mthca_bind_seg);

	for (qp->sq.wqe_shift = 6; 1 << qp->sq.wqe_shift < size;
	     qp->sq.wqe_shift++)
		; /* nothing */

	qp->send_wqe_offset = ALIGN(qp->rq.max << qp->rq.wqe_shift,
				    1 << qp->sq.wqe_shift);
	size = PAGE_ALIGN(qp->send_wqe_offset +
			  (qp->sq.max << qp->sq.wqe_shift));

	qp->wrid = kmalloc((qp->rq.max + qp->sq.max) * sizeof (u64),
			   GFP_KERNEL);
	if (!qp->wrid)
		goto err_out;

	if (size <= MTHCA_MAX_DIRECT_QP_SIZE) {
		qp->is_direct = 1;
		npages = 1;
		shift = get_order(size) + PAGE_SHIFT;

		if (0)
			mthca_dbg(dev, "Creating direct QP of size %d (shift %d)\n",
				  size, shift);

		qp->queue.direct.buf = pci_alloc_consistent(dev->pdev, size, &t);
		if (!qp->queue.direct.buf)
			goto err_out;

		pci_unmap_addr_set(&qp->queue.direct, mapping, t);

		memset(qp->queue.direct.buf, 0, size);

		while (t & ((1 << shift) - 1)) {
			--shift;
			npages *= 2;
		}

		dma_list = kmalloc(npages * sizeof *dma_list, GFP_KERNEL);
		if (!dma_list)
			goto err_out_free;

		for (i = 0; i < npages; ++i)
			dma_list[i] = t + i * (1 << shift);
	} else {
		qp->is_direct = 0;
		npages = size / PAGE_SIZE;
		shift = PAGE_SHIFT;

		if (0)
			mthca_dbg(dev, "Creating indirect QP with %d pages\n", npages);

		dma_list = kmalloc(npages * sizeof *dma_list, GFP_KERNEL);
		if (!dma_list)
			goto err_out;

		qp->queue.page_list = kmalloc(npages *
					      sizeof *qp->queue.page_list,
					      GFP_KERNEL);
		if (!qp->queue.page_list)
			goto err_out;

		for (i = 0; i < npages; ++i) {
			qp->queue.page_list[i].buf =
				pci_alloc_consistent(dev->pdev, PAGE_SIZE, &t);
			if (!qp->queue.page_list[i].buf)
				goto err_out_free;

			memset(qp->queue.page_list[i].buf, 0, PAGE_SIZE);

			pci_unmap_addr_set(&qp->queue.page_list[i], mapping, t);
			dma_list[i] = t;
		}
	}

	err = mthca_mr_alloc_phys(dev, pd->pd_num, dma_list, shift,
				  npages, 0, size,
				  MTHCA_MPT_FLAG_LOCAL_WRITE |
				  MTHCA_MPT_FLAG_LOCAL_READ,
				  &qp->mr);
	if (err)
		goto err_out_free;

	kfree(dma_list);
	return 0;

 err_out_free:
	if (qp->is_direct) {
		pci_free_consistent(dev->pdev, size,
				    qp->queue.direct.buf,
				    pci_unmap_addr(&qp->queue.direct, mapping));
	} else
		for (i = 0; i < npages; ++i) {
			if (qp->queue.page_list[i].buf)
				pci_free_consistent(dev->pdev, PAGE_SIZE,
						    qp->queue.page_list[i].buf,
						    pci_unmap_addr(&qp->queue.page_list[i],
								   mapping));

		}

 err_out:
	kfree(qp->wrid);
	kfree(dma_list);
	return err;
}

static int mthca_alloc_qp_common(struct mthca_dev *dev,
				 struct mthca_pd *pd,
				 struct mthca_cq *send_cq,
				 struct mthca_cq *recv_cq,
				 enum ib_sig_type send_policy,
				 enum ib_sig_type recv_policy,
				 struct mthca_qp *qp)
{
	int err;

	spin_lock_init(&qp->lock);
	atomic_set(&qp->refcount, 1);
	qp->state    	 = IB_QPS_RESET;
	qp->atomic_rd_en = 0;
	qp->resp_depth   = 0;
	qp->sq.policy    = send_policy;
	qp->rq.policy    = recv_policy;
	qp->rq.cur       = 0;
	qp->sq.cur       = 0;
	qp->rq.next      = 0;
	qp->sq.next      = 0;
	qp->rq.last_comp = qp->rq.max - 1;
	qp->sq.last_comp = qp->sq.max - 1;
	qp->rq.last      = NULL;
	qp->sq.last      = NULL;

	err = mthca_alloc_wqe_buf(dev, pd, qp);
	return err;
}

int mthca_alloc_qp(struct mthca_dev *dev,
		   struct mthca_pd *pd,
		   struct mthca_cq *send_cq,
		   struct mthca_cq *recv_cq,
		   enum ib_qp_type type,
		   enum ib_sig_type send_policy,
		   enum ib_sig_type recv_policy,
		   struct mthca_qp *qp)
{
	int err;

	switch (type) {
	case IB_QPT_RC: qp->transport = RC; break;
	case IB_QPT_UC: qp->transport = UC; break;
	case IB_QPT_UD: qp->transport = UD; break;
	default: return -EINVAL;
	}

	qp->qpn = mthca_alloc(&dev->qp_table.alloc);
	if (qp->qpn == -1)
		return -ENOMEM;

	err = mthca_alloc_qp_common(dev, pd, send_cq, recv_cq,
				    send_policy, recv_policy, qp);
	if (err) {
		mthca_free(&dev->qp_table.alloc, qp->qpn);
		return err;
	}

	spin_lock_irq(&dev->qp_table.lock);
	mthca_array_set(&dev->qp_table.qp,
			qp->qpn & (dev->limits.num_qps - 1), qp);
	spin_unlock_irq(&dev->qp_table.lock);

	return 0;
}

int mthca_alloc_sqp(struct mthca_dev *dev,
		    struct mthca_pd *pd,
		    struct mthca_cq *send_cq,
		    struct mthca_cq *recv_cq,
		    enum ib_sig_type send_policy,
		    enum ib_sig_type recv_policy,
		    int qpn,
		    int port,
		    struct mthca_sqp *sqp)
{
	int err = 0;
	u32 mqpn = qpn * 2 + dev->qp_table.sqp_start + port - 1;

	sqp->header_buf_size = sqp->qp.sq.max * MTHCA_UD_HEADER_SIZE;
	sqp->header_buf = dma_alloc_coherent(&dev->pdev->dev, sqp->header_buf_size,
					     &sqp->header_dma, GFP_KERNEL);
	if (!sqp->header_buf)
		return -ENOMEM;

	spin_lock_irq(&dev->qp_table.lock);
	if (mthca_array_get(&dev->qp_table.qp, mqpn))
		err = -EBUSY;
	else
		mthca_array_set(&dev->qp_table.qp, mqpn, sqp);
	spin_unlock_irq(&dev->qp_table.lock);

	if (err)
		goto err_out;

	sqp->port = port;
	sqp->qp.qpn       = mqpn;
	sqp->qp.transport = MLX;

	err = mthca_alloc_qp_common(dev, pd, send_cq, recv_cq,
				    send_policy, recv_policy,
				    &sqp->qp);
	if (err)
		goto err_out_free;

	atomic_inc(&pd->sqp_count);

	return 0;

 err_out_free:
	spin_lock_irq(&dev->qp_table.lock);
	mthca_array_clear(&dev->qp_table.qp, mqpn);
	spin_unlock_irq(&dev->qp_table.lock);

 err_out:
	dma_free_coherent(&dev->pdev->dev, sqp->header_buf_size,
			  sqp->header_buf, sqp->header_dma);

	return err;
}

void mthca_free_qp(struct mthca_dev *dev,
		   struct mthca_qp *qp)
{
	u8 status;
	int size;
	int i;

	spin_lock_irq(&dev->qp_table.lock);
	mthca_array_clear(&dev->qp_table.qp,
			  qp->qpn & (dev->limits.num_qps - 1));
	spin_unlock_irq(&dev->qp_table.lock);

	atomic_dec(&qp->refcount);
	wait_event(qp->wait, !atomic_read(&qp->refcount));

	if (qp->state != IB_QPS_RESET)
		mthca_MODIFY_QP(dev, MTHCA_TRANS_ANY2RST, qp->qpn, 0, NULL, 0, &status);

	mthca_cq_clean(dev, to_mcq(qp->ibqp.send_cq)->cqn, qp->qpn);
	if (qp->ibqp.send_cq != qp->ibqp.recv_cq)
		mthca_cq_clean(dev, to_mcq(qp->ibqp.recv_cq)->cqn, qp->qpn);

	mthca_free_mr(dev, &qp->mr);

	size = PAGE_ALIGN(qp->send_wqe_offset +
			  (qp->sq.max << qp->sq.wqe_shift));

	if (qp->is_direct) {
		pci_free_consistent(dev->pdev, size,
				    qp->queue.direct.buf,
				    pci_unmap_addr(&qp->queue.direct, mapping));
	} else {
		for (i = 0; i < size / PAGE_SIZE; ++i) {
			pci_free_consistent(dev->pdev, PAGE_SIZE,
					    qp->queue.page_list[i].buf,
					    pci_unmap_addr(&qp->queue.page_list[i],
							   mapping));
		}
	}

	kfree(qp->wrid);

	if (is_sqp(dev, qp)) {
		atomic_dec(&(to_mpd(qp->ibqp.pd)->sqp_count));
		dma_free_coherent(&dev->pdev->dev,
				  to_msqp(qp)->header_buf_size,
				  to_msqp(qp)->header_buf,
				  to_msqp(qp)->header_dma);
	}
	else
		mthca_free(&dev->qp_table.alloc, qp->qpn);
}

/* Create UD header for an MLX send and build a data segment for it */
static int build_mlx_header(struct mthca_dev *dev, struct mthca_sqp *sqp,
			    int ind, struct ib_send_wr *wr,
			    struct mthca_mlx_seg *mlx,
			    struct mthca_data_seg *data)
{
	int header_size;
	int err;

	ib_ud_header_init(256, /* assume a MAD */
			  sqp->ud_header.grh_present,
			  &sqp->ud_header);

	err = mthca_read_ah(dev, to_mah(wr->wr.ud.ah), &sqp->ud_header);
	if (err)
		return err;
	mlx->flags &= ~cpu_to_be32(MTHCA_NEXT_SOLICIT | 1);
	mlx->flags |= cpu_to_be32((!sqp->qp.ibqp.qp_num ? MTHCA_MLX_VL15 : 0) |
				  (sqp->ud_header.lrh.destination_lid == 0xffff ?
				   MTHCA_MLX_SLR : 0) |
				  (sqp->ud_header.lrh.service_level << 8));
	mlx->rlid = sqp->ud_header.lrh.destination_lid;
	mlx->vcrc = 0;

	switch (wr->opcode) {
	case IB_WR_SEND:
		sqp->ud_header.bth.opcode = IB_OPCODE_UD_SEND_ONLY;
		sqp->ud_header.immediate_present = 0;
		break;
	case IB_WR_SEND_WITH_IMM:
		sqp->ud_header.bth.opcode = IB_OPCODE_UD_SEND_ONLY_WITH_IMMEDIATE;
		sqp->ud_header.immediate_present = 1;
		sqp->ud_header.immediate_data = wr->imm_data;
		break;
	default:
		return -EINVAL;
	}

	sqp->ud_header.lrh.virtual_lane    = !sqp->qp.ibqp.qp_num ? 15 : 0;
	if (sqp->ud_header.lrh.destination_lid == 0xffff)
		sqp->ud_header.lrh.source_lid = 0xffff;
	sqp->ud_header.bth.solicited_event = !!(wr->send_flags & IB_SEND_SOLICITED);
	if (!sqp->qp.ibqp.qp_num)
		ib_get_cached_pkey(&dev->ib_dev, sqp->port,
				   sqp->pkey_index,
				   &sqp->ud_header.bth.pkey);
	else
		ib_get_cached_pkey(&dev->ib_dev, sqp->port,
				   wr->wr.ud.pkey_index,
				   &sqp->ud_header.bth.pkey);
	cpu_to_be16s(&sqp->ud_header.bth.pkey);
	sqp->ud_header.bth.destination_qpn = cpu_to_be32(wr->wr.ud.remote_qpn);
	sqp->ud_header.bth.psn = cpu_to_be32((sqp->send_psn++) & ((1 << 24) - 1));
	sqp->ud_header.deth.qkey = cpu_to_be32(wr->wr.ud.remote_qkey & 0x80000000 ?
					       sqp->qkey : wr->wr.ud.remote_qkey);
	sqp->ud_header.deth.source_qpn = cpu_to_be32(sqp->qp.ibqp.qp_num);

	header_size = ib_ud_header_pack(&sqp->ud_header,
					sqp->header_buf +
					ind * MTHCA_UD_HEADER_SIZE);

	data->byte_count = cpu_to_be32(header_size);
	data->lkey       = cpu_to_be32(to_mpd(sqp->qp.ibqp.pd)->ntmr.ibmr.lkey);
	data->addr       = cpu_to_be64(sqp->header_dma +
				       ind * MTHCA_UD_HEADER_SIZE);

	return 0;
}

int mthca_post_send(struct ib_qp *ibqp, struct ib_send_wr *wr,
		    struct ib_send_wr **bad_wr)
{
	struct mthca_dev *dev = to_mdev(ibqp->device);
	struct mthca_qp *qp = to_mqp(ibqp);
	void *wqe;
	void *prev_wqe;
	unsigned long flags;
	int err = 0;
	int nreq;
	int i;
	int size;
	int size0 = 0;
	u32 f0 = 0;
	int ind;
	u8 op0 = 0;

	static const u8 opcode[] = {
		[IB_WR_SEND]                 = MTHCA_OPCODE_SEND,
		[IB_WR_SEND_WITH_IMM]        = MTHCA_OPCODE_SEND_IMM,
		[IB_WR_RDMA_WRITE]           = MTHCA_OPCODE_RDMA_WRITE,
		[IB_WR_RDMA_WRITE_WITH_IMM]  = MTHCA_OPCODE_RDMA_WRITE_IMM,
		[IB_WR_RDMA_READ]            = MTHCA_OPCODE_RDMA_READ,
		[IB_WR_ATOMIC_CMP_AND_SWP]   = MTHCA_OPCODE_ATOMIC_CS,
		[IB_WR_ATOMIC_FETCH_AND_ADD] = MTHCA_OPCODE_ATOMIC_FA,
	};

	spin_lock_irqsave(&qp->lock, flags);

	/* XXX check that state is OK to post send */

	ind = qp->sq.next;

	for (nreq = 0; wr; ++nreq, wr = wr->next) {
		if (qp->sq.cur + nreq >= qp->sq.max) {
			mthca_err(dev, "SQ full (%d posted, %d max, %d nreq)\n",
				  qp->sq.cur, qp->sq.max, nreq);
			err = -ENOMEM;
			*bad_wr = wr;
			goto out;
		}

		wqe = get_send_wqe(qp, ind);
		prev_wqe = qp->sq.last;
		qp->sq.last = wqe;

		((struct mthca_next_seg *) wqe)->nda_op = 0;
		((struct mthca_next_seg *) wqe)->ee_nds = 0;
		((struct mthca_next_seg *) wqe)->flags =
			((wr->send_flags & IB_SEND_SIGNALED) ?
			 cpu_to_be32(MTHCA_NEXT_CQ_UPDATE) : 0) |
			((wr->send_flags & IB_SEND_SOLICITED) ?
			 cpu_to_be32(MTHCA_NEXT_SOLICIT) : 0)   |
			cpu_to_be32(1);
		if (wr->opcode == IB_WR_SEND_WITH_IMM ||
		    wr->opcode == IB_WR_RDMA_WRITE_WITH_IMM)
			((struct mthca_next_seg *) wqe)->flags = wr->imm_data;

		wqe += sizeof (struct mthca_next_seg);
		size = sizeof (struct mthca_next_seg) / 16;

		switch (qp->transport) {
		case RC:
			switch (wr->opcode) {
			case IB_WR_ATOMIC_CMP_AND_SWP:
			case IB_WR_ATOMIC_FETCH_AND_ADD:
				((struct mthca_raddr_seg *) wqe)->raddr =
					cpu_to_be64(wr->wr.atomic.remote_addr);
				((struct mthca_raddr_seg *) wqe)->rkey =
					cpu_to_be32(wr->wr.atomic.rkey);
				((struct mthca_raddr_seg *) wqe)->reserved = 0;

				wqe += sizeof (struct mthca_raddr_seg);

				if (wr->opcode == IB_WR_ATOMIC_CMP_AND_SWP) {
					((struct mthca_atomic_seg *) wqe)->swap_add =
						cpu_to_be64(wr->wr.atomic.swap);
					((struct mthca_atomic_seg *) wqe)->compare =
						cpu_to_be64(wr->wr.atomic.compare_add);
				} else {
					((struct mthca_atomic_seg *) wqe)->swap_add =
						cpu_to_be64(wr->wr.atomic.compare_add);
					((struct mthca_atomic_seg *) wqe)->compare = 0;
				}

				wqe += sizeof (struct mthca_atomic_seg);
				size += sizeof (struct mthca_raddr_seg) / 16 +
					sizeof (struct mthca_atomic_seg);
				break;

			case IB_WR_RDMA_WRITE:
			case IB_WR_RDMA_WRITE_WITH_IMM:
			case IB_WR_RDMA_READ:
				((struct mthca_raddr_seg *) wqe)->raddr =
					cpu_to_be64(wr->wr.rdma.remote_addr);
				((struct mthca_raddr_seg *) wqe)->rkey =
					cpu_to_be32(wr->wr.rdma.rkey);
				((struct mthca_raddr_seg *) wqe)->reserved = 0;
				wqe += sizeof (struct mthca_raddr_seg);
				size += sizeof (struct mthca_raddr_seg) / 16;
				break;

			default:
				/* No extra segments required for sends */
				break;
			}

			break;

		case UD:
			((struct mthca_ud_seg *) wqe)->lkey =
				cpu_to_be32(to_mah(wr->wr.ud.ah)->key);
			((struct mthca_ud_seg *) wqe)->av_addr =
				cpu_to_be64(to_mah(wr->wr.ud.ah)->avdma);
			((struct mthca_ud_seg *) wqe)->dqpn =
				cpu_to_be32(wr->wr.ud.remote_qpn);
			((struct mthca_ud_seg *) wqe)->qkey =
				cpu_to_be32(wr->wr.ud.remote_qkey);

			wqe += sizeof (struct mthca_ud_seg);
			size += sizeof (struct mthca_ud_seg) / 16;
			break;

		case MLX:
			err = build_mlx_header(dev, to_msqp(qp), ind, wr,
					       wqe - sizeof (struct mthca_next_seg),
					       wqe);
			if (err) {
				*bad_wr = wr;
				goto out;
			}
			wqe += sizeof (struct mthca_data_seg);
			size += sizeof (struct mthca_data_seg) / 16;
			break;
		}

		if (wr->num_sge > qp->sq.max_gs) {
			mthca_err(dev, "too many gathers\n");
			err = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		for (i = 0; i < wr->num_sge; ++i) {
			((struct mthca_data_seg *) wqe)->byte_count =
				cpu_to_be32(wr->sg_list[i].length);
			((struct mthca_data_seg *) wqe)->lkey =
				cpu_to_be32(wr->sg_list[i].lkey);
			((struct mthca_data_seg *) wqe)->addr =
				cpu_to_be64(wr->sg_list[i].addr);
			wqe += sizeof (struct mthca_data_seg);
			size += sizeof (struct mthca_data_seg) / 16;
		}

		/* Add one more inline data segment for ICRC */
		if (qp->transport == MLX) {
			((struct mthca_data_seg *) wqe)->byte_count =
				cpu_to_be32((1 << 31) | 4);
			((u32 *) wqe)[1] = 0;
			wqe += sizeof (struct mthca_data_seg);
			size += sizeof (struct mthca_data_seg) / 16;
		}

		qp->wrid[ind + qp->rq.max] = wr->wr_id;

		if (wr->opcode >= ARRAY_SIZE(opcode)) {
			mthca_err(dev, "opcode invalid\n");
			err = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		if (prev_wqe) {
			((struct mthca_next_seg *) prev_wqe)->nda_op =
				cpu_to_be32(((ind << qp->sq.wqe_shift) +
					     qp->send_wqe_offset) |
					    opcode[wr->opcode]);
			smp_wmb();
			((struct mthca_next_seg *) prev_wqe)->ee_nds =
				cpu_to_be32((size0 ? 0 : MTHCA_NEXT_DBD) | size);
		}

		if (!size0) {
			size0 = size;
			op0   = opcode[wr->opcode];
		}

		++ind;
		if (unlikely(ind >= qp->sq.max))
			ind -= qp->sq.max;
	}

out:
	if (nreq) {
		u32 doorbell[2];

		doorbell[0] = cpu_to_be32(((qp->sq.next << qp->sq.wqe_shift) +
					   qp->send_wqe_offset) | f0 | op0);
		doorbell[1] = cpu_to_be32((qp->qpn << 8) | size0);

		wmb();

		mthca_write64(doorbell,
			      dev->kar + MTHCA_SEND_DOORBELL,
			      MTHCA_GET_DOORBELL_LOCK(&dev->doorbell_lock));
	}

	qp->sq.cur += nreq;
	qp->sq.next = ind;

	spin_unlock_irqrestore(&qp->lock, flags);
	return err;
}

int mthca_post_receive(struct ib_qp *ibqp, struct ib_recv_wr *wr,
		       struct ib_recv_wr **bad_wr)
{
	struct mthca_dev *dev = to_mdev(ibqp->device);
	struct mthca_qp *qp = to_mqp(ibqp);
	unsigned long flags;
	int err = 0;
	int nreq;
	int i;
	int size;
	int size0 = 0;
	int ind;
	void *wqe;
	void *prev_wqe;

	spin_lock_irqsave(&qp->lock, flags);

	/* XXX check that state is OK to post receive */

	ind = qp->rq.next;

	for (nreq = 0; wr; ++nreq, wr = wr->next) {
		if (qp->rq.cur + nreq >= qp->rq.max) {
			mthca_err(dev, "RQ %06x full\n", qp->qpn);
			err = -ENOMEM;
			*bad_wr = wr;
			goto out;
		}

		wqe = get_recv_wqe(qp, ind);
		prev_wqe = qp->rq.last;
		qp->rq.last = wqe;

		((struct mthca_next_seg *) wqe)->nda_op = 0;
		((struct mthca_next_seg *) wqe)->ee_nds =
			cpu_to_be32(MTHCA_NEXT_DBD);
		((struct mthca_next_seg *) wqe)->flags =
			(wr->recv_flags & IB_RECV_SIGNALED) ?
			cpu_to_be32(MTHCA_NEXT_CQ_UPDATE) : 0;

		wqe += sizeof (struct mthca_next_seg);
		size = sizeof (struct mthca_next_seg) / 16;

		if (wr->num_sge > qp->rq.max_gs) {
			err = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		for (i = 0; i < wr->num_sge; ++i) {
			((struct mthca_data_seg *) wqe)->byte_count =
				cpu_to_be32(wr->sg_list[i].length);
			((struct mthca_data_seg *) wqe)->lkey =
				cpu_to_be32(wr->sg_list[i].lkey);
			((struct mthca_data_seg *) wqe)->addr =
				cpu_to_be64(wr->sg_list[i].addr);
			wqe += sizeof (struct mthca_data_seg);
			size += sizeof (struct mthca_data_seg) / 16;
		}

		qp->wrid[ind] = wr->wr_id;

		if (prev_wqe) {
			((struct mthca_next_seg *) prev_wqe)->nda_op =
				cpu_to_be32((ind << qp->rq.wqe_shift) | 1);
			smp_wmb();
			((struct mthca_next_seg *) prev_wqe)->ee_nds =
				cpu_to_be32(MTHCA_NEXT_DBD | size);
		}

		if (!size0)
			size0 = size;

		++ind;
		if (unlikely(ind >= qp->rq.max))
			ind -= qp->rq.max;
	}

out:
	if (nreq) {
		u32 doorbell[2];

		doorbell[0] = cpu_to_be32((qp->rq.next << qp->rq.wqe_shift) | size0);
		doorbell[1] = cpu_to_be32((qp->qpn << 8) | nreq);

		wmb();

		mthca_write64(doorbell,
			      dev->kar + MTHCA_RECEIVE_DOORBELL,
			      MTHCA_GET_DOORBELL_LOCK(&dev->doorbell_lock));
	}

	qp->rq.cur += nreq;
	qp->rq.next = ind;

	spin_unlock_irqrestore(&qp->lock, flags);
	return err;
}

int mthca_free_err_wqe(struct mthca_qp *qp, int is_send,
		       int index, int *dbd, u32 *new_wqe)
{
	struct mthca_next_seg *next;

	if (is_send)
		next = get_send_wqe(qp, index);
	else
		next = get_recv_wqe(qp, index);

	*dbd = !!(next->ee_nds & cpu_to_be32(MTHCA_NEXT_DBD));
	if (next->ee_nds & cpu_to_be32(0x3f))
		*new_wqe = (next->nda_op & cpu_to_be32(~0x3f)) |
			(next->ee_nds & cpu_to_be32(0x3f));
	else
		*new_wqe = 0;

	return 0;
}

int __devinit mthca_init_qp_table(struct mthca_dev *dev)
{
	int err;
	u8 status;
	int i;

	spin_lock_init(&dev->qp_table.lock);

	/*
	 * We reserve 2 extra QPs per port for the special QPs.  The
	 * special QP for port 1 has to be even, so round up.
	 */
	dev->qp_table.sqp_start = (dev->limits.reserved_qps + 1) & ~1UL;
	err = mthca_alloc_init(&dev->qp_table.alloc,
			       dev->limits.num_qps,
			       (1 << 24) - 1,
			       dev->qp_table.sqp_start +
			       MTHCA_MAX_PORTS * 2);
	if (err)
		return err;

	err = mthca_array_init(&dev->qp_table.qp,
			       dev->limits.num_qps);
	if (err) {
		mthca_alloc_cleanup(&dev->qp_table.alloc);
		return err;
	}

	for (i = 0; i < 2; ++i) {
		err = mthca_CONF_SPECIAL_QP(dev, i ? IB_QPT_GSI : IB_QPT_SMI,
					    dev->qp_table.sqp_start + i * 2,
					    &status);
		if (err)
			goto err_out;
		if (status) {
			mthca_warn(dev, "CONF_SPECIAL_QP returned "
				   "status %02x, aborting.\n",
				   status);
			err = -EINVAL;
			goto err_out;
		}
	}
	return 0;

 err_out:
	for (i = 0; i < 2; ++i)
		mthca_CONF_SPECIAL_QP(dev, i, 0, &status);

	mthca_array_cleanup(&dev->qp_table.qp, dev->limits.num_qps);
	mthca_alloc_cleanup(&dev->qp_table.alloc);

	return err;
}

void __devexit mthca_cleanup_qp_table(struct mthca_dev *dev)
{
	int i;
	u8 status;

	for (i = 0; i < 2; ++i)
		mthca_CONF_SPECIAL_QP(dev, i, 0, &status);

	mthca_alloc_cleanup(&dev->qp_table.alloc);
}
