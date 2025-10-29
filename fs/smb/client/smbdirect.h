/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *   Copyright (C) 2017, Microsoft Corporation.
 *
 *   Author(s): Long Li <longli@microsoft.com>
 */
#ifndef _SMBDIRECT_H
#define _SMBDIRECT_H

#ifdef CONFIG_CIFS_SMB_DIRECT
#define cifs_rdma_enabled(server)	((server)->rdma)

#include "cifsglob.h"
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/mempool.h>

#include "../common/smbdirect/smbdirect.h"
#include "../common/smbdirect/smbdirect_socket.h"

extern int rdma_readwrite_threshold;
extern int smbd_max_frmr_depth;
extern int smbd_keep_alive_interval;
extern int smbd_max_receive_size;
extern int smbd_max_fragmented_recv_size;
extern int smbd_max_send_size;
extern int smbd_send_credit_target;
extern int smbd_receive_credit_max;

enum keep_alive_status {
	KEEP_ALIVE_NONE,
	KEEP_ALIVE_PENDING,
	KEEP_ALIVE_SENT,
};

/*
 * The context for the SMBDirect transport
 * Everything related to the transport is here. It has several logical parts
 * 1. RDMA related structures
 * 2. SMBDirect connection parameters
 * 3. Memory registrations
 * 4. Receive and reassembly queues for data receive path
 * 5. mempools for allocating packets
 */
struct smbd_connection {
	struct smbdirect_socket socket;

	int ri_rc;
	struct completion ri_done;
	wait_queue_head_t status_wait;

	struct completion negotiate_completion;
	bool negotiate_done;

	struct work_struct disconnect_work;
	struct work_struct post_send_credits_work;

	spinlock_t lock_new_credits_offered;
	int new_credits_offered;

	/* dynamic connection parameters defined in [MS-SMBD] 3.1.1.1 */
	enum keep_alive_status keep_alive_requested;
	int protocol;
	atomic_t send_credits;
	atomic_t receive_credits;
	u16 receive_credit_target;

	/* Memory registrations */
	/* Maximum number of RDMA read/write outstanding on this connection */
	bool legacy_iwarp;
	u8 initiator_depth;
	u8 responder_resources;
	/* Maximum number of pages in a single RDMA write/read on this connection */
	int max_frmr_depth;
	/*
	 * If payload is less than or equal to the threshold,
	 * use RDMA send/recv to send upper layer I/O.
	 * If payload is more than the threshold,
	 * use RDMA read/write through memory registration for I/O.
	 */
	int rdma_readwrite_threshold;
	enum ib_mr_type mr_type;
	struct list_head mr_list;
	spinlock_t mr_list_lock;
	/* The number of available MRs ready for memory registration */
	atomic_t mr_ready_count;
	atomic_t mr_used_count;
	wait_queue_head_t wait_mr;
	struct work_struct mr_recovery_work;
	/* Used by transport to wait until all MRs are returned */
	wait_queue_head_t wait_for_mr_cleanup;

	/* Activity accounting */
	atomic_t send_pending;
	wait_queue_head_t wait_send_pending;
	wait_queue_head_t wait_post_send;

	/* Receive queue */
	int count_receive_queue;
	wait_queue_head_t wait_receive_queues;

	bool send_immediate;

	wait_queue_head_t wait_send_queue;

	struct workqueue_struct *workqueue;
	struct delayed_work idle_timer_work;

	/* for debug purposes */
	unsigned int count_get_receive_buffer;
	unsigned int count_put_receive_buffer;
	unsigned int count_reassembly_queue;
	unsigned int count_enqueue_reassembly_queue;
	unsigned int count_dequeue_reassembly_queue;
	unsigned int count_send_empty;
};

/* Create a SMBDirect session */
struct smbd_connection *smbd_get_connection(
	struct TCP_Server_Info *server, struct sockaddr *dstaddr);

/* Reconnect SMBDirect session */
int smbd_reconnect(struct TCP_Server_Info *server);
/* Destroy SMBDirect session */
void smbd_destroy(struct TCP_Server_Info *server);

/* Interface for carrying upper layer I/O through send/recv */
int smbd_recv(struct smbd_connection *info, struct msghdr *msg);
int smbd_send(struct TCP_Server_Info *server,
	int num_rqst, struct smb_rqst *rqst);

enum mr_state {
	MR_READY,
	MR_REGISTERED,
	MR_INVALIDATED,
	MR_ERROR
};

struct smbd_mr {
	struct smbd_connection	*conn;
	struct list_head	list;
	enum mr_state		state;
	struct ib_mr		*mr;
	struct sg_table		sgt;
	enum dma_data_direction	dir;
	union {
		struct ib_reg_wr	wr;
		struct ib_send_wr	inv_wr;
	};
	struct ib_cqe		cqe;
	bool			need_invalidate;
	struct completion	invalidate_done;
};

/* Interfaces to register and deregister MR for RDMA read/write */
struct smbd_mr *smbd_register_mr(
	struct smbd_connection *info, struct iov_iter *iter,
	bool writing, bool need_invalidate);
int smbd_deregister_mr(struct smbd_mr *mr);

#else
#define cifs_rdma_enabled(server)	0
struct smbd_connection {};
static inline void *smbd_get_connection(
	struct TCP_Server_Info *server, struct sockaddr *dstaddr) {return NULL;}
static inline int smbd_reconnect(struct TCP_Server_Info *server) {return -1; }
static inline void smbd_destroy(struct TCP_Server_Info *server) {}
static inline int smbd_recv(struct smbd_connection *info, struct msghdr *msg) {return -1; }
static inline int smbd_send(struct TCP_Server_Info *server, int num_rqst, struct smb_rqst *rqst) {return -1; }
#endif

#endif
