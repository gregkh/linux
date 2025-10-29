// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2017, Microsoft Corporation.
 *
 *   Author(s): Long Li <longli@microsoft.com>
 */
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/folio_queue.h>
#include "../common/smbdirect/smbdirect_pdu.h"
#include "smbdirect.h"
#include "cifs_debug.h"
#include "cifsproto.h"
#include "smb2proto.h"

static struct smbdirect_recv_io *get_receive_buffer(
		struct smbd_connection *info);
static void put_receive_buffer(
		struct smbd_connection *info,
		struct smbdirect_recv_io *response);
static int allocate_receive_buffers(struct smbd_connection *info, int num_buf);
static void destroy_receive_buffers(struct smbd_connection *info);

static void enqueue_reassembly(
		struct smbd_connection *info,
		struct smbdirect_recv_io *response, int data_length);
static struct smbdirect_recv_io *_get_first_reassembly(
		struct smbd_connection *info);

static int smbd_post_recv(
		struct smbd_connection *info,
		struct smbdirect_recv_io *response);

static int smbd_post_send_empty(struct smbd_connection *info);

static void destroy_mr_list(struct smbd_connection *info);
static int allocate_mr_list(struct smbd_connection *info);

struct smb_extract_to_rdma {
	struct ib_sge		*sge;
	unsigned int		nr_sge;
	unsigned int		max_sge;
	struct ib_device	*device;
	u32			local_dma_lkey;
	enum dma_data_direction	direction;
};
static ssize_t smb_extract_iter_to_rdma(struct iov_iter *iter, size_t len,
					struct smb_extract_to_rdma *rdma);

/* Port numbers for SMBD transport */
#define SMB_PORT	445
#define SMBD_PORT	5445

/* Address lookup and resolve timeout in ms */
#define RDMA_RESOLVE_TIMEOUT	5000

/* SMBD negotiation timeout in seconds */
#define SMBD_NEGOTIATE_TIMEOUT	120

/* SMBD minimum receive size and fragmented sized defined in [MS-SMBD] */
#define SMBD_MIN_RECEIVE_SIZE		128
#define SMBD_MIN_FRAGMENTED_SIZE	131072

/*
 * Default maximum number of RDMA read/write outstanding on this connection
 * This value is possibly decreased during QP creation on hardware limit
 */
#define SMBD_CM_RESPONDER_RESOURCES	32

/* Maximum number of retries on data transfer operations */
#define SMBD_CM_RETRY			6
/* No need to retry on Receiver Not Ready since SMBD manages credits */
#define SMBD_CM_RNR_RETRY		0

/*
 * User configurable initial values per SMBD transport connection
 * as defined in [MS-SMBD] 3.1.1.1
 * Those may change after a SMBD negotiation
 */
/* The local peer's maximum number of credits to grant to the peer */
int smbd_receive_credit_max = 255;

/* The remote peer's credit request of local peer */
int smbd_send_credit_target = 255;

/* The maximum single message size can be sent to remote peer */
int smbd_max_send_size = 1364;

/*  The maximum fragmented upper-layer payload receive size supported */
int smbd_max_fragmented_recv_size = 1024 * 1024;

/*  The maximum single-message size which can be received */
int smbd_max_receive_size = 1364;

/* The timeout to initiate send of a keepalive message on idle */
int smbd_keep_alive_interval = 120;

/*
 * User configurable initial values for RDMA transport
 * The actual values used may be lower and are limited to hardware capabilities
 */
/* Default maximum number of pages in a single RDMA write/read */
int smbd_max_frmr_depth = 2048;

/* If payload is less than this byte, use RDMA send/recv not read/write */
int rdma_readwrite_threshold = 4096;

/* Transport logging functions
 * Logging are defined as classes. They can be OR'ed to define the actual
 * logging level via module parameter smbd_logging_class
 * e.g. cifs.smbd_logging_class=0xa0 will log all log_rdma_recv() and
 * log_rdma_event()
 */
#define LOG_OUTGOING			0x1
#define LOG_INCOMING			0x2
#define LOG_READ			0x4
#define LOG_WRITE			0x8
#define LOG_RDMA_SEND			0x10
#define LOG_RDMA_RECV			0x20
#define LOG_KEEP_ALIVE			0x40
#define LOG_RDMA_EVENT			0x80
#define LOG_RDMA_MR			0x100
static unsigned int smbd_logging_class;
module_param(smbd_logging_class, uint, 0644);
MODULE_PARM_DESC(smbd_logging_class,
	"Logging class for SMBD transport 0x0 to 0x100");

#define ERR		0x0
#define INFO		0x1
static unsigned int smbd_logging_level = ERR;
module_param(smbd_logging_level, uint, 0644);
MODULE_PARM_DESC(smbd_logging_level,
	"Logging level for SMBD transport, 0 (default): error, 1: info");

#define log_rdma(level, class, fmt, args...)				\
do {									\
	if (level <= smbd_logging_level || class & smbd_logging_class)	\
		cifs_dbg(VFS, "%s:%d " fmt, __func__, __LINE__, ##args);\
} while (0)

#define log_outgoing(level, fmt, args...) \
		log_rdma(level, LOG_OUTGOING, fmt, ##args)
#define log_incoming(level, fmt, args...) \
		log_rdma(level, LOG_INCOMING, fmt, ##args)
#define log_read(level, fmt, args...)	log_rdma(level, LOG_READ, fmt, ##args)
#define log_write(level, fmt, args...)	log_rdma(level, LOG_WRITE, fmt, ##args)
#define log_rdma_send(level, fmt, args...) \
		log_rdma(level, LOG_RDMA_SEND, fmt, ##args)
#define log_rdma_recv(level, fmt, args...) \
		log_rdma(level, LOG_RDMA_RECV, fmt, ##args)
#define log_keep_alive(level, fmt, args...) \
		log_rdma(level, LOG_KEEP_ALIVE, fmt, ##args)
#define log_rdma_event(level, fmt, args...) \
		log_rdma(level, LOG_RDMA_EVENT, fmt, ##args)
#define log_rdma_mr(level, fmt, args...) \
		log_rdma(level, LOG_RDMA_MR, fmt, ##args)

static void smbd_disconnect_rdma_work(struct work_struct *work)
{
	struct smbd_connection *info =
		container_of(work, struct smbd_connection, disconnect_work);
	struct smbdirect_socket *sc = &info->socket;

	if (sc->status == SMBDIRECT_SOCKET_CONNECTED) {
		sc->status = SMBDIRECT_SOCKET_DISCONNECTING;
		rdma_disconnect(sc->rdma.cm_id);
	}
}

static void smbd_disconnect_rdma_connection(struct smbd_connection *info)
{
	queue_work(info->workqueue, &info->disconnect_work);
}

/* Upcall from RDMA CM */
static int smbd_conn_upcall(
		struct rdma_cm_id *id, struct rdma_cm_event *event)
{
	struct smbd_connection *info = id->context;
	struct smbdirect_socket *sc = &info->socket;
	const char *event_name = rdma_event_msg(event->event);
	u8 peer_initiator_depth;
	u8 peer_responder_resources;

	log_rdma_event(INFO, "event=%s status=%d\n",
		event_name, event->status);

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		info->ri_rc = 0;
		complete(&info->ri_done);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
		log_rdma_event(ERR, "connecting failed event=%s\n", event_name);
		info->ri_rc = -EHOSTUNREACH;
		complete(&info->ri_done);
		break;

	case RDMA_CM_EVENT_ROUTE_ERROR:
		log_rdma_event(ERR, "connecting failed event=%s\n", event_name);
		info->ri_rc = -ENETUNREACH;
		complete(&info->ri_done);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		log_rdma_event(INFO, "connected event=%s\n", event_name);

		/*
		 * Here we work around an inconsistency between
		 * iWarp and other devices (at least rxe and irdma using RoCEv2)
		 */
		if (rdma_protocol_iwarp(id->device, id->port_num)) {
			/*
			 * iWarp devices report the peer's values
			 * with the perspective of the peer here.
			 * Tested with siw and irdma (in iwarp mode)
			 * We need to change to our perspective here,
			 * so we need to switch the values.
			 */
			peer_initiator_depth = event->param.conn.responder_resources;
			peer_responder_resources = event->param.conn.initiator_depth;
		} else {
			/*
			 * Non iWarp devices report the peer's values
			 * already changed to our perspective here.
			 * Tested with rxe and irdma (in roce mode).
			 */
			peer_initiator_depth = event->param.conn.initiator_depth;
			peer_responder_resources = event->param.conn.responder_resources;
		}
		if (rdma_protocol_iwarp(id->device, id->port_num) &&
		    event->param.conn.private_data_len == 8) {
			/*
			 * Legacy clients with only iWarp MPA v1 support
			 * need a private blob in order to negotiate
			 * the IRD/ORD values.
			 */
			const __be32 *ird_ord_hdr = event->param.conn.private_data;
			u32 ird32 = be32_to_cpu(ird_ord_hdr[0]);
			u32 ord32 = be32_to_cpu(ird_ord_hdr[1]);

			/*
			 * cifs.ko sends the legacy IRD/ORD negotiation
			 * event if iWarp MPA v2 was used.
			 *
			 * Here we check that the values match and only
			 * mark the client as legacy if they don't match.
			 */
			if ((u32)event->param.conn.initiator_depth != ird32 ||
			    (u32)event->param.conn.responder_resources != ord32) {
				/*
				 * There are broken clients (old cifs.ko)
				 * using little endian and also
				 * struct rdma_conn_param only uses u8
				 * for initiator_depth and responder_resources,
				 * so we truncate the value to U8_MAX.
				 *
				 * smb_direct_accept_client() will then
				 * do the real negotiation in order to
				 * select the minimum between client and
				 * server.
				 */
				ird32 = min_t(u32, ird32, U8_MAX);
				ord32 = min_t(u32, ord32, U8_MAX);

				info->legacy_iwarp = true;
				peer_initiator_depth = (u8)ird32;
				peer_responder_resources = (u8)ord32;
			}
		}

		/*
		 * negotiate the value by using the minimum
		 * between client and server if the client provided
		 * non 0 values.
		 */
		if (peer_initiator_depth != 0)
			info->initiator_depth =
					min_t(u8, info->initiator_depth,
					      peer_initiator_depth);
		if (peer_responder_resources != 0)
			info->responder_resources =
					min_t(u8, info->responder_resources,
					      peer_responder_resources);

		sc->status = SMBDIRECT_SOCKET_CONNECTED;
		wake_up_interruptible(&info->status_wait);
		break;

	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		log_rdma_event(ERR, "connecting failed event=%s\n", event_name);
		sc->status = SMBDIRECT_SOCKET_DISCONNECTED;
		wake_up_interruptible(&info->status_wait);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
	case RDMA_CM_EVENT_DISCONNECTED:
		/* This happens when we fail the negotiation */
		if (sc->status == SMBDIRECT_SOCKET_NEGOTIATE_FAILED) {
			log_rdma_event(ERR, "event=%s during negotiation\n", event_name);
			sc->status = SMBDIRECT_SOCKET_DISCONNECTED;
			wake_up(&info->status_wait);
			break;
		}

		sc->status = SMBDIRECT_SOCKET_DISCONNECTED;
		wake_up_interruptible(&info->status_wait);
		wake_up_interruptible(&sc->recv_io.reassembly.wait_queue);
		wake_up_interruptible_all(&info->wait_send_queue);
		break;

	default:
		log_rdma_event(ERR, "unexpected event=%s status=%d\n",
			       event_name, event->status);
		break;
	}

	return 0;
}

/* Upcall from RDMA QP */
static void
smbd_qp_async_error_upcall(struct ib_event *event, void *context)
{
	struct smbd_connection *info = context;

	log_rdma_event(ERR, "%s on device %s info %p\n",
		ib_event_msg(event->event), event->device->name, info);

	switch (event->event) {
	case IB_EVENT_CQ_ERR:
	case IB_EVENT_QP_FATAL:
		smbd_disconnect_rdma_connection(info);
		break;

	default:
		break;
	}
}

static inline void *smbdirect_send_io_payload(struct smbdirect_send_io *request)
{
	return (void *)request->packet;
}

static inline void *smbdirect_recv_io_payload(struct smbdirect_recv_io *response)
{
	return (void *)response->packet;
}

/* Called when a RDMA send is done */
static void send_done(struct ib_cq *cq, struct ib_wc *wc)
{
	int i;
	struct smbdirect_send_io *request =
		container_of(wc->wr_cqe, struct smbdirect_send_io, cqe);
	struct smbdirect_socket *sc = request->socket;
	struct smbd_connection *info =
		container_of(sc, struct smbd_connection, socket);

	log_rdma_send(INFO, "smbdirect_send_io 0x%p completed wc->status=%s\n",
		request, ib_wc_status_msg(wc->status));

	for (i = 0; i < request->num_sge; i++)
		ib_dma_unmap_single(sc->ib.dev,
			request->sge[i].addr,
			request->sge[i].length,
			DMA_TO_DEVICE);

	if (wc->status != IB_WC_SUCCESS || wc->opcode != IB_WC_SEND) {
		if (wc->status != IB_WC_WR_FLUSH_ERR)
			log_rdma_send(ERR, "wc->status=%s wc->opcode=%d\n",
				ib_wc_status_msg(wc->status), wc->opcode);
		mempool_free(request, sc->send_io.mem.pool);
		smbd_disconnect_rdma_connection(info);
		return;
	}

	if (atomic_dec_and_test(&info->send_pending))
		wake_up(&info->wait_send_pending);

	wake_up(&info->wait_post_send);

	mempool_free(request, sc->send_io.mem.pool);
}

static void dump_smbdirect_negotiate_resp(struct smbdirect_negotiate_resp *resp)
{
	log_rdma_event(INFO, "resp message min_version %u max_version %u negotiated_version %u credits_requested %u credits_granted %u status %u max_readwrite_size %u preferred_send_size %u max_receive_size %u max_fragmented_size %u\n",
		       resp->min_version, resp->max_version,
		       resp->negotiated_version, resp->credits_requested,
		       resp->credits_granted, resp->status,
		       resp->max_readwrite_size, resp->preferred_send_size,
		       resp->max_receive_size, resp->max_fragmented_size);
}

/*
 * Process a negotiation response message, according to [MS-SMBD]3.1.5.7
 * response, packet_length: the negotiation response message
 * return value: true if negotiation is a success, false if failed
 */
static bool process_negotiation_response(
		struct smbdirect_recv_io *response, int packet_length)
{
	struct smbdirect_socket *sc = response->socket;
	struct smbd_connection *info =
		container_of(sc, struct smbd_connection, socket);
	struct smbdirect_socket_parameters *sp = &sc->parameters;
	struct smbdirect_negotiate_resp *packet = smbdirect_recv_io_payload(response);

	if (packet_length < sizeof(struct smbdirect_negotiate_resp)) {
		log_rdma_event(ERR,
			"error: packet_length=%d\n", packet_length);
		return false;
	}

	if (le16_to_cpu(packet->negotiated_version) != SMBDIRECT_V1) {
		log_rdma_event(ERR, "error: negotiated_version=%x\n",
			le16_to_cpu(packet->negotiated_version));
		return false;
	}
	info->protocol = le16_to_cpu(packet->negotiated_version);

	if (packet->credits_requested == 0) {
		log_rdma_event(ERR, "error: credits_requested==0\n");
		return false;
	}
	info->receive_credit_target = le16_to_cpu(packet->credits_requested);
	info->receive_credit_target = min_t(u16, info->receive_credit_target, sp->recv_credit_max);

	if (packet->credits_granted == 0) {
		log_rdma_event(ERR, "error: credits_granted==0\n");
		return false;
	}
	atomic_set(&info->send_credits, le16_to_cpu(packet->credits_granted));

	atomic_set(&info->receive_credits, 0);

	if (le32_to_cpu(packet->preferred_send_size) > sp->max_recv_size) {
		log_rdma_event(ERR, "error: preferred_send_size=%d\n",
			le32_to_cpu(packet->preferred_send_size));
		return false;
	}
	sp->max_recv_size = le32_to_cpu(packet->preferred_send_size);

	if (le32_to_cpu(packet->max_receive_size) < SMBD_MIN_RECEIVE_SIZE) {
		log_rdma_event(ERR, "error: max_receive_size=%d\n",
			le32_to_cpu(packet->max_receive_size));
		return false;
	}
	sp->max_send_size = min_t(u32, sp->max_send_size,
				  le32_to_cpu(packet->max_receive_size));

	if (le32_to_cpu(packet->max_fragmented_size) <
			SMBD_MIN_FRAGMENTED_SIZE) {
		log_rdma_event(ERR, "error: max_fragmented_size=%d\n",
			le32_to_cpu(packet->max_fragmented_size));
		return false;
	}
	sp->max_fragmented_send_size =
		le32_to_cpu(packet->max_fragmented_size);
	info->rdma_readwrite_threshold =
		rdma_readwrite_threshold > sp->max_fragmented_send_size ?
		sp->max_fragmented_send_size :
		rdma_readwrite_threshold;


	sp->max_read_write_size = min_t(u32,
			le32_to_cpu(packet->max_readwrite_size),
			info->max_frmr_depth * PAGE_SIZE);
	info->max_frmr_depth = sp->max_read_write_size / PAGE_SIZE;

	sc->recv_io.expected = SMBDIRECT_EXPECT_DATA_TRANSFER;
	return true;
}

static void smbd_post_send_credits(struct work_struct *work)
{
	int ret = 0;
	int rc;
	struct smbdirect_recv_io *response;
	struct smbd_connection *info =
		container_of(work, struct smbd_connection,
			post_send_credits_work);
	struct smbdirect_socket *sc = &info->socket;

	if (sc->status != SMBDIRECT_SOCKET_CONNECTED) {
		wake_up(&info->wait_receive_queues);
		return;
	}

	if (info->receive_credit_target >
		atomic_read(&info->receive_credits)) {
		while (true) {
			response = get_receive_buffer(info);
			if (!response)
				break;

			response->first_segment = false;
			rc = smbd_post_recv(info, response);
			if (rc) {
				log_rdma_recv(ERR,
					"post_recv failed rc=%d\n", rc);
				put_receive_buffer(info, response);
				break;
			}

			ret++;
		}
	}

	spin_lock(&info->lock_new_credits_offered);
	info->new_credits_offered += ret;
	spin_unlock(&info->lock_new_credits_offered);

	/* Promptly send an immediate packet as defined in [MS-SMBD] 3.1.1.1 */
	info->send_immediate = true;
	if (atomic_read(&info->receive_credits) <
		info->receive_credit_target - 1) {
		if (info->keep_alive_requested == KEEP_ALIVE_PENDING ||
		    info->send_immediate) {
			log_keep_alive(INFO, "send an empty message\n");
			smbd_post_send_empty(info);
		}
	}
}

/* Called from softirq, when recv is done */
static void recv_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct smbdirect_data_transfer *data_transfer;
	struct smbdirect_recv_io *response =
		container_of(wc->wr_cqe, struct smbdirect_recv_io, cqe);
	struct smbdirect_socket *sc = response->socket;
	struct smbdirect_socket_parameters *sp = &sc->parameters;
	struct smbd_connection *info =
		container_of(sc, struct smbd_connection, socket);
	u16 old_recv_credit_target;
	u32 data_offset = 0;
	u32 data_length = 0;
	u32 remaining_data_length = 0;

	log_rdma_recv(INFO,
		      "response=0x%p type=%d wc status=%s wc opcode %d byte_len=%d pkey_index=%u\n",
		      response, sc->recv_io.expected,
		      ib_wc_status_msg(wc->status), wc->opcode,
		      wc->byte_len, wc->pkey_index);

	if (wc->status != IB_WC_SUCCESS || wc->opcode != IB_WC_RECV) {
		if (wc->status != IB_WC_WR_FLUSH_ERR)
			log_rdma_recv(ERR, "wc->status=%s opcode=%d\n",
				ib_wc_status_msg(wc->status), wc->opcode);
		goto error;
	}

	ib_dma_sync_single_for_cpu(
		wc->qp->device,
		response->sge.addr,
		response->sge.length,
		DMA_FROM_DEVICE);

	switch (sc->recv_io.expected) {
	/* SMBD negotiation response */
	case SMBDIRECT_EXPECT_NEGOTIATE_REP:
		dump_smbdirect_negotiate_resp(smbdirect_recv_io_payload(response));
		sc->recv_io.reassembly.full_packet_received = true;
		info->negotiate_done =
			process_negotiation_response(response, wc->byte_len);
		put_receive_buffer(info, response);
		complete(&info->negotiate_completion);
		return;

	/* SMBD data transfer packet */
	case SMBDIRECT_EXPECT_DATA_TRANSFER:
		data_transfer = smbdirect_recv_io_payload(response);

		if (wc->byte_len <
		    offsetof(struct smbdirect_data_transfer, padding))
			goto error;

		remaining_data_length = le32_to_cpu(data_transfer->remaining_data_length);
		data_offset = le32_to_cpu(data_transfer->data_offset);
		data_length = le32_to_cpu(data_transfer->data_length);
		if (wc->byte_len < data_offset ||
		    (u64)wc->byte_len < (u64)data_offset + data_length)
			goto error;

		if (remaining_data_length > sp->max_fragmented_recv_size ||
		    data_length > sp->max_fragmented_recv_size ||
		    (u64)remaining_data_length + (u64)data_length > (u64)sp->max_fragmented_recv_size)
			goto error;

		if (data_length) {
			if (sc->recv_io.reassembly.full_packet_received)
				response->first_segment = true;

			if (le32_to_cpu(data_transfer->remaining_data_length))
				sc->recv_io.reassembly.full_packet_received = false;
			else
				sc->recv_io.reassembly.full_packet_received = true;
		}

		atomic_dec(&info->receive_credits);
		old_recv_credit_target = info->receive_credit_target;
		info->receive_credit_target =
			le16_to_cpu(data_transfer->credits_requested);
		info->receive_credit_target =
			min_t(u16, info->receive_credit_target, sp->recv_credit_max);
		info->receive_credit_target =
			max_t(u16, info->receive_credit_target, 1);
		if (le16_to_cpu(data_transfer->credits_granted)) {
			atomic_add(le16_to_cpu(data_transfer->credits_granted),
				&info->send_credits);
			/*
			 * We have new send credits granted from remote peer
			 * If any sender is waiting for credits, unblock it
			 */
			wake_up_interruptible(&info->wait_send_queue);
		}

		log_incoming(INFO, "data flags %d data_offset %d data_length %d remaining_data_length %d\n",
			     le16_to_cpu(data_transfer->flags),
			     le32_to_cpu(data_transfer->data_offset),
			     le32_to_cpu(data_transfer->data_length),
			     le32_to_cpu(data_transfer->remaining_data_length));

		/* Send a KEEP_ALIVE response right away if requested */
		info->keep_alive_requested = KEEP_ALIVE_NONE;
		if (le16_to_cpu(data_transfer->flags) &
				SMBDIRECT_FLAG_RESPONSE_REQUESTED) {
			info->keep_alive_requested = KEEP_ALIVE_PENDING;
		}

		/*
		 * If this is a packet with data playload place the data in
		 * reassembly queue and wake up the reading thread
		 */
		if (data_length) {
			if (info->receive_credit_target > old_recv_credit_target)
				queue_work(info->workqueue, &info->post_send_credits_work);

			enqueue_reassembly(info, response, data_length);
			wake_up_interruptible(&sc->recv_io.reassembly.wait_queue);
		} else
			put_receive_buffer(info, response);

		return;

	case SMBDIRECT_EXPECT_NEGOTIATE_REQ:
		/* Only server... */
		break;
	}

	/*
	 * This is an internal error!
	 */
	log_rdma_recv(ERR, "unexpected response type=%d\n", sc->recv_io.expected);
	WARN_ON_ONCE(sc->recv_io.expected != SMBDIRECT_EXPECT_DATA_TRANSFER);
error:
	put_receive_buffer(info, response);
	smbd_disconnect_rdma_connection(info);
}

static struct rdma_cm_id *smbd_create_id(
		struct smbd_connection *info,
		struct sockaddr *dstaddr, int port)
{
	struct rdma_cm_id *id;
	int rc;
	__be16 *sport;

	id = rdma_create_id(&init_net, smbd_conn_upcall, info,
		RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(id)) {
		rc = PTR_ERR(id);
		log_rdma_event(ERR, "rdma_create_id() failed %i\n", rc);
		return id;
	}

	if (dstaddr->sa_family == AF_INET6)
		sport = &((struct sockaddr_in6 *)dstaddr)->sin6_port;
	else
		sport = &((struct sockaddr_in *)dstaddr)->sin_port;

	*sport = htons(port);

	init_completion(&info->ri_done);
	info->ri_rc = -ETIMEDOUT;

	rc = rdma_resolve_addr(id, NULL, (struct sockaddr *)dstaddr,
		RDMA_RESOLVE_TIMEOUT);
	if (rc) {
		log_rdma_event(ERR, "rdma_resolve_addr() failed %i\n", rc);
		goto out;
	}
	rc = wait_for_completion_interruptible_timeout(
		&info->ri_done, msecs_to_jiffies(RDMA_RESOLVE_TIMEOUT));
	/* e.g. if interrupted returns -ERESTARTSYS */
	if (rc < 0) {
		log_rdma_event(ERR, "rdma_resolve_addr timeout rc: %i\n", rc);
		goto out;
	}
	rc = info->ri_rc;
	if (rc) {
		log_rdma_event(ERR, "rdma_resolve_addr() completed %i\n", rc);
		goto out;
	}

	info->ri_rc = -ETIMEDOUT;
	rc = rdma_resolve_route(id, RDMA_RESOLVE_TIMEOUT);
	if (rc) {
		log_rdma_event(ERR, "rdma_resolve_route() failed %i\n", rc);
		goto out;
	}
	rc = wait_for_completion_interruptible_timeout(
		&info->ri_done, msecs_to_jiffies(RDMA_RESOLVE_TIMEOUT));
	/* e.g. if interrupted returns -ERESTARTSYS */
	if (rc < 0)  {
		log_rdma_event(ERR, "rdma_resolve_addr timeout rc: %i\n", rc);
		goto out;
	}
	rc = info->ri_rc;
	if (rc) {
		log_rdma_event(ERR, "rdma_resolve_route() completed %i\n", rc);
		goto out;
	}

	return id;

out:
	rdma_destroy_id(id);
	return ERR_PTR(rc);
}

/*
 * Test if FRWR (Fast Registration Work Requests) is supported on the device
 * This implementation requires FRWR on RDMA read/write
 * return value: true if it is supported
 */
static bool frwr_is_supported(struct ib_device_attr *attrs)
{
	if (!(attrs->device_cap_flags & IB_DEVICE_MEM_MGT_EXTENSIONS))
		return false;
	if (attrs->max_fast_reg_page_list_len == 0)
		return false;
	return true;
}

static int smbd_ia_open(
		struct smbd_connection *info,
		struct sockaddr *dstaddr, int port)
{
	struct smbdirect_socket *sc = &info->socket;
	int rc;

	sc->rdma.cm_id = smbd_create_id(info, dstaddr, port);
	if (IS_ERR(sc->rdma.cm_id)) {
		rc = PTR_ERR(sc->rdma.cm_id);
		goto out1;
	}
	sc->ib.dev = sc->rdma.cm_id->device;

	if (!frwr_is_supported(&sc->ib.dev->attrs)) {
		log_rdma_event(ERR, "Fast Registration Work Requests (FRWR) is not supported\n");
		log_rdma_event(ERR, "Device capability flags = %llx max_fast_reg_page_list_len = %u\n",
			       sc->ib.dev->attrs.device_cap_flags,
			       sc->ib.dev->attrs.max_fast_reg_page_list_len);
		rc = -EPROTONOSUPPORT;
		goto out2;
	}
	info->max_frmr_depth = min_t(int,
		smbd_max_frmr_depth,
		sc->ib.dev->attrs.max_fast_reg_page_list_len);
	info->mr_type = IB_MR_TYPE_MEM_REG;
	if (sc->ib.dev->attrs.kernel_cap_flags & IBK_SG_GAPS_REG)
		info->mr_type = IB_MR_TYPE_SG_GAPS;

	sc->ib.pd = ib_alloc_pd(sc->ib.dev, 0);
	if (IS_ERR(sc->ib.pd)) {
		rc = PTR_ERR(sc->ib.pd);
		log_rdma_event(ERR, "ib_alloc_pd() returned %d\n", rc);
		goto out2;
	}

	return 0;

out2:
	rdma_destroy_id(sc->rdma.cm_id);
	sc->rdma.cm_id = NULL;

out1:
	return rc;
}

/*
 * Send a negotiation request message to the peer
 * The negotiation procedure is in [MS-SMBD] 3.1.5.2 and 3.1.5.3
 * After negotiation, the transport is connected and ready for
 * carrying upper layer SMB payload
 */
static int smbd_post_send_negotiate_req(struct smbd_connection *info)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbdirect_socket_parameters *sp = &sc->parameters;
	struct ib_send_wr send_wr;
	int rc = -ENOMEM;
	struct smbdirect_send_io *request;
	struct smbdirect_negotiate_req *packet;

	request = mempool_alloc(sc->send_io.mem.pool, GFP_KERNEL);
	if (!request)
		return rc;

	request->socket = sc;

	packet = smbdirect_send_io_payload(request);
	packet->min_version = cpu_to_le16(SMBDIRECT_V1);
	packet->max_version = cpu_to_le16(SMBDIRECT_V1);
	packet->reserved = 0;
	packet->credits_requested = cpu_to_le16(sp->send_credit_target);
	packet->preferred_send_size = cpu_to_le32(sp->max_send_size);
	packet->max_receive_size = cpu_to_le32(sp->max_recv_size);
	packet->max_fragmented_size =
		cpu_to_le32(sp->max_fragmented_recv_size);

	request->num_sge = 1;
	request->sge[0].addr = ib_dma_map_single(
				sc->ib.dev, (void *)packet,
				sizeof(*packet), DMA_TO_DEVICE);
	if (ib_dma_mapping_error(sc->ib.dev, request->sge[0].addr)) {
		rc = -EIO;
		goto dma_mapping_failed;
	}

	request->sge[0].length = sizeof(*packet);
	request->sge[0].lkey = sc->ib.pd->local_dma_lkey;

	ib_dma_sync_single_for_device(
		sc->ib.dev, request->sge[0].addr,
		request->sge[0].length, DMA_TO_DEVICE);

	request->cqe.done = send_done;

	send_wr.next = NULL;
	send_wr.wr_cqe = &request->cqe;
	send_wr.sg_list = request->sge;
	send_wr.num_sge = request->num_sge;
	send_wr.opcode = IB_WR_SEND;
	send_wr.send_flags = IB_SEND_SIGNALED;

	log_rdma_send(INFO, "sge addr=0x%llx length=%u lkey=0x%x\n",
		request->sge[0].addr,
		request->sge[0].length, request->sge[0].lkey);

	atomic_inc(&info->send_pending);
	rc = ib_post_send(sc->ib.qp, &send_wr, NULL);
	if (!rc)
		return 0;

	/* if we reach here, post send failed */
	log_rdma_send(ERR, "ib_post_send failed rc=%d\n", rc);
	atomic_dec(&info->send_pending);
	ib_dma_unmap_single(sc->ib.dev, request->sge[0].addr,
		request->sge[0].length, DMA_TO_DEVICE);

	smbd_disconnect_rdma_connection(info);

dma_mapping_failed:
	mempool_free(request, sc->send_io.mem.pool);
	return rc;
}

/*
 * Extend the credits to remote peer
 * This implements [MS-SMBD] 3.1.5.9
 * The idea is that we should extend credits to remote peer as quickly as
 * it's allowed, to maintain data flow. We allocate as much receive
 * buffer as possible, and extend the receive credits to remote peer
 * return value: the new credtis being granted.
 */
static int manage_credits_prior_sending(struct smbd_connection *info)
{
	int new_credits;

	spin_lock(&info->lock_new_credits_offered);
	new_credits = info->new_credits_offered;
	info->new_credits_offered = 0;
	spin_unlock(&info->lock_new_credits_offered);

	return new_credits;
}

/*
 * Check if we need to send a KEEP_ALIVE message
 * The idle connection timer triggers a KEEP_ALIVE message when expires
 * SMBDIRECT_FLAG_RESPONSE_REQUESTED is set in the message flag to have peer send
 * back a response.
 * return value:
 * 1 if SMBDIRECT_FLAG_RESPONSE_REQUESTED needs to be set
 * 0: otherwise
 */
static int manage_keep_alive_before_sending(struct smbd_connection *info)
{
	if (info->keep_alive_requested == KEEP_ALIVE_PENDING) {
		info->keep_alive_requested = KEEP_ALIVE_SENT;
		return 1;
	}
	return 0;
}

/* Post the send request */
static int smbd_post_send(struct smbd_connection *info,
		struct smbdirect_send_io *request)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbdirect_socket_parameters *sp = &sc->parameters;
	struct ib_send_wr send_wr;
	int rc, i;

	for (i = 0; i < request->num_sge; i++) {
		log_rdma_send(INFO,
			"rdma_request sge[%d] addr=0x%llx length=%u\n",
			i, request->sge[i].addr, request->sge[i].length);
		ib_dma_sync_single_for_device(
			sc->ib.dev,
			request->sge[i].addr,
			request->sge[i].length,
			DMA_TO_DEVICE);
	}

	request->cqe.done = send_done;

	send_wr.next = NULL;
	send_wr.wr_cqe = &request->cqe;
	send_wr.sg_list = request->sge;
	send_wr.num_sge = request->num_sge;
	send_wr.opcode = IB_WR_SEND;
	send_wr.send_flags = IB_SEND_SIGNALED;

	rc = ib_post_send(sc->ib.qp, &send_wr, NULL);
	if (rc) {
		log_rdma_send(ERR, "ib_post_send failed rc=%d\n", rc);
		smbd_disconnect_rdma_connection(info);
		rc = -EAGAIN;
	} else
		/* Reset timer for idle connection after packet is sent */
		mod_delayed_work(info->workqueue, &info->idle_timer_work,
			msecs_to_jiffies(sp->keepalive_interval_msec));

	return rc;
}

static int smbd_post_send_iter(struct smbd_connection *info,
			       struct iov_iter *iter,
			       int *_remaining_data_length)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbdirect_socket_parameters *sp = &sc->parameters;
	int i, rc;
	int header_length;
	int data_length;
	struct smbdirect_send_io *request;
	struct smbdirect_data_transfer *packet;
	int new_credits = 0;

wait_credit:
	/* Wait for send credits. A SMBD packet needs one credit */
	rc = wait_event_interruptible(info->wait_send_queue,
		atomic_read(&info->send_credits) > 0 ||
		sc->status != SMBDIRECT_SOCKET_CONNECTED);
	if (rc)
		goto err_wait_credit;

	if (sc->status != SMBDIRECT_SOCKET_CONNECTED) {
		log_outgoing(ERR, "disconnected not sending on wait_credit\n");
		rc = -EAGAIN;
		goto err_wait_credit;
	}
	if (unlikely(atomic_dec_return(&info->send_credits) < 0)) {
		atomic_inc(&info->send_credits);
		goto wait_credit;
	}

wait_send_queue:
	wait_event(info->wait_post_send,
		atomic_read(&info->send_pending) < sp->send_credit_target ||
		sc->status != SMBDIRECT_SOCKET_CONNECTED);

	if (sc->status != SMBDIRECT_SOCKET_CONNECTED) {
		log_outgoing(ERR, "disconnected not sending on wait_send_queue\n");
		rc = -EAGAIN;
		goto err_wait_send_queue;
	}

	if (unlikely(atomic_inc_return(&info->send_pending) >
				sp->send_credit_target)) {
		atomic_dec(&info->send_pending);
		goto wait_send_queue;
	}

	request = mempool_alloc(sc->send_io.mem.pool, GFP_KERNEL);
	if (!request) {
		rc = -ENOMEM;
		goto err_alloc;
	}

	request->socket = sc;
	memset(request->sge, 0, sizeof(request->sge));

	/* Fill in the data payload to find out how much data we can add */
	if (iter) {
		struct smb_extract_to_rdma extract = {
			.nr_sge		= 1,
			.max_sge	= SMBDIRECT_SEND_IO_MAX_SGE,
			.sge		= request->sge,
			.device		= sc->ib.dev,
			.local_dma_lkey	= sc->ib.pd->local_dma_lkey,
			.direction	= DMA_TO_DEVICE,
		};
		size_t payload_len = umin(*_remaining_data_length,
					  sp->max_send_size - sizeof(*packet));

		rc = smb_extract_iter_to_rdma(iter, payload_len,
					      &extract);
		if (rc < 0)
			goto err_dma;
		data_length = rc;
		request->num_sge = extract.nr_sge;
		*_remaining_data_length -= data_length;
	} else {
		data_length = 0;
		request->num_sge = 1;
	}

	/* Fill in the packet header */
	packet = smbdirect_send_io_payload(request);
	packet->credits_requested = cpu_to_le16(sp->send_credit_target);

	new_credits = manage_credits_prior_sending(info);
	atomic_add(new_credits, &info->receive_credits);
	packet->credits_granted = cpu_to_le16(new_credits);

	info->send_immediate = false;

	packet->flags = 0;
	if (manage_keep_alive_before_sending(info))
		packet->flags |= cpu_to_le16(SMBDIRECT_FLAG_RESPONSE_REQUESTED);

	packet->reserved = 0;
	if (!data_length)
		packet->data_offset = 0;
	else
		packet->data_offset = cpu_to_le32(24);
	packet->data_length = cpu_to_le32(data_length);
	packet->remaining_data_length = cpu_to_le32(*_remaining_data_length);
	packet->padding = 0;

	log_outgoing(INFO, "credits_requested=%d credits_granted=%d data_offset=%d data_length=%d remaining_data_length=%d\n",
		     le16_to_cpu(packet->credits_requested),
		     le16_to_cpu(packet->credits_granted),
		     le32_to_cpu(packet->data_offset),
		     le32_to_cpu(packet->data_length),
		     le32_to_cpu(packet->remaining_data_length));

	/* Map the packet to DMA */
	header_length = sizeof(struct smbdirect_data_transfer);
	/* If this is a packet without payload, don't send padding */
	if (!data_length)
		header_length = offsetof(struct smbdirect_data_transfer, padding);

	request->sge[0].addr = ib_dma_map_single(sc->ib.dev,
						 (void *)packet,
						 header_length,
						 DMA_TO_DEVICE);
	if (ib_dma_mapping_error(sc->ib.dev, request->sge[0].addr)) {
		rc = -EIO;
		request->sge[0].addr = 0;
		goto err_dma;
	}

	request->sge[0].length = header_length;
	request->sge[0].lkey = sc->ib.pd->local_dma_lkey;

	rc = smbd_post_send(info, request);
	if (!rc)
		return 0;

err_dma:
	for (i = 0; i < request->num_sge; i++)
		if (request->sge[i].addr)
			ib_dma_unmap_single(sc->ib.dev,
					    request->sge[i].addr,
					    request->sge[i].length,
					    DMA_TO_DEVICE);
	mempool_free(request, sc->send_io.mem.pool);

	/* roll back receive credits and credits to be offered */
	spin_lock(&info->lock_new_credits_offered);
	info->new_credits_offered += new_credits;
	spin_unlock(&info->lock_new_credits_offered);
	atomic_sub(new_credits, &info->receive_credits);

err_alloc:
	if (atomic_dec_and_test(&info->send_pending))
		wake_up(&info->wait_send_pending);

err_wait_send_queue:
	/* roll back send credits and pending */
	atomic_inc(&info->send_credits);

err_wait_credit:
	return rc;
}

/*
 * Send an empty message
 * Empty message is used to extend credits to peer to for keep live
 * while there is no upper layer payload to send at the time
 */
static int smbd_post_send_empty(struct smbd_connection *info)
{
	int remaining_data_length = 0;

	info->count_send_empty++;
	return smbd_post_send_iter(info, NULL, &remaining_data_length);
}

static int smbd_post_send_full_iter(struct smbd_connection *info,
				    struct iov_iter *iter,
				    int *_remaining_data_length)
{
	int rc = 0;

	/*
	 * smbd_post_send_iter() respects the
	 * negotiated max_send_size, so we need to
	 * loop until the full iter is posted
	 */

	while (iov_iter_count(iter) > 0) {
		rc = smbd_post_send_iter(info, iter, _remaining_data_length);
		if (rc < 0)
			break;
	}

	return rc;
}

/*
 * Post a receive request to the transport
 * The remote peer can only send data when a receive request is posted
 * The interaction is controlled by send/receive credit system
 */
static int smbd_post_recv(
		struct smbd_connection *info, struct smbdirect_recv_io *response)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbdirect_socket_parameters *sp = &sc->parameters;
	struct ib_recv_wr recv_wr;
	int rc = -EIO;

	response->sge.addr = ib_dma_map_single(
				sc->ib.dev, response->packet,
				sp->max_recv_size, DMA_FROM_DEVICE);
	if (ib_dma_mapping_error(sc->ib.dev, response->sge.addr))
		return rc;

	response->sge.length = sp->max_recv_size;
	response->sge.lkey = sc->ib.pd->local_dma_lkey;

	response->cqe.done = recv_done;

	recv_wr.wr_cqe = &response->cqe;
	recv_wr.next = NULL;
	recv_wr.sg_list = &response->sge;
	recv_wr.num_sge = 1;

	rc = ib_post_recv(sc->ib.qp, &recv_wr, NULL);
	if (rc) {
		ib_dma_unmap_single(sc->ib.dev, response->sge.addr,
				    response->sge.length, DMA_FROM_DEVICE);
		response->sge.length = 0;
		smbd_disconnect_rdma_connection(info);
		log_rdma_recv(ERR, "ib_post_recv failed rc=%d\n", rc);
	}

	return rc;
}

/* Perform SMBD negotiate according to [MS-SMBD] 3.1.5.2 */
static int smbd_negotiate(struct smbd_connection *info)
{
	struct smbdirect_socket *sc = &info->socket;
	int rc;
	struct smbdirect_recv_io *response = get_receive_buffer(info);

	sc->recv_io.expected = SMBDIRECT_EXPECT_NEGOTIATE_REP;
	rc = smbd_post_recv(info, response);
	log_rdma_event(INFO, "smbd_post_recv rc=%d iov.addr=0x%llx iov.length=%u iov.lkey=0x%x\n",
		       rc, response->sge.addr,
		       response->sge.length, response->sge.lkey);
	if (rc) {
		put_receive_buffer(info, response);
		return rc;
	}

	init_completion(&info->negotiate_completion);
	info->negotiate_done = false;
	rc = smbd_post_send_negotiate_req(info);
	if (rc)
		return rc;

	rc = wait_for_completion_interruptible_timeout(
		&info->negotiate_completion, SMBD_NEGOTIATE_TIMEOUT * HZ);
	log_rdma_event(INFO, "wait_for_completion_timeout rc=%d\n", rc);

	if (info->negotiate_done)
		return 0;

	if (rc == 0)
		rc = -ETIMEDOUT;
	else if (rc == -ERESTARTSYS)
		rc = -EINTR;
	else
		rc = -ENOTCONN;

	return rc;
}

/*
 * Implement Connection.FragmentReassemblyBuffer defined in [MS-SMBD] 3.1.1.1
 * This is a queue for reassembling upper layer payload and present to upper
 * layer. All the inncoming payload go to the reassembly queue, regardless of
 * if reassembly is required. The uuper layer code reads from the queue for all
 * incoming payloads.
 * Put a received packet to the reassembly queue
 * response: the packet received
 * data_length: the size of payload in this packet
 */
static void enqueue_reassembly(
	struct smbd_connection *info,
	struct smbdirect_recv_io *response,
	int data_length)
{
	struct smbdirect_socket *sc = &info->socket;

	spin_lock(&sc->recv_io.reassembly.lock);
	list_add_tail(&response->list, &sc->recv_io.reassembly.list);
	sc->recv_io.reassembly.queue_length++;
	/*
	 * Make sure reassembly_data_length is updated after list and
	 * reassembly_queue_length are updated. On the dequeue side
	 * reassembly_data_length is checked without a lock to determine
	 * if reassembly_queue_length and list is up to date
	 */
	virt_wmb();
	sc->recv_io.reassembly.data_length += data_length;
	spin_unlock(&sc->recv_io.reassembly.lock);
	info->count_reassembly_queue++;
	info->count_enqueue_reassembly_queue++;
}

/*
 * Get the first entry at the front of reassembly queue
 * Caller is responsible for locking
 * return value: the first entry if any, NULL if queue is empty
 */
static struct smbdirect_recv_io *_get_first_reassembly(struct smbd_connection *info)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbdirect_recv_io *ret = NULL;

	if (!list_empty(&sc->recv_io.reassembly.list)) {
		ret = list_first_entry(
			&sc->recv_io.reassembly.list,
			struct smbdirect_recv_io, list);
	}
	return ret;
}

/*
 * Get a receive buffer
 * For each remote send, we need to post a receive. The receive buffers are
 * pre-allocated in advance.
 * return value: the receive buffer, NULL if none is available
 */
static struct smbdirect_recv_io *get_receive_buffer(struct smbd_connection *info)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbdirect_recv_io *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&sc->recv_io.free.lock, flags);
	if (!list_empty(&sc->recv_io.free.list)) {
		ret = list_first_entry(
			&sc->recv_io.free.list,
			struct smbdirect_recv_io, list);
		list_del(&ret->list);
		info->count_receive_queue--;
		info->count_get_receive_buffer++;
	}
	spin_unlock_irqrestore(&sc->recv_io.free.lock, flags);

	return ret;
}

/*
 * Return a receive buffer
 * Upon returning of a receive buffer, we can post new receive and extend
 * more receive credits to remote peer. This is done immediately after a
 * receive buffer is returned.
 */
static void put_receive_buffer(
	struct smbd_connection *info, struct smbdirect_recv_io *response)
{
	struct smbdirect_socket *sc = &info->socket;
	unsigned long flags;

	if (likely(response->sge.length != 0)) {
		ib_dma_unmap_single(sc->ib.dev,
				    response->sge.addr,
				    response->sge.length,
				    DMA_FROM_DEVICE);
		response->sge.length = 0;
	}

	spin_lock_irqsave(&sc->recv_io.free.lock, flags);
	list_add_tail(&response->list, &sc->recv_io.free.list);
	info->count_receive_queue++;
	info->count_put_receive_buffer++;
	spin_unlock_irqrestore(&sc->recv_io.free.lock, flags);

	queue_work(info->workqueue, &info->post_send_credits_work);
}

/* Preallocate all receive buffer on transport establishment */
static int allocate_receive_buffers(struct smbd_connection *info, int num_buf)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbdirect_recv_io *response;
	int i;

	INIT_LIST_HEAD(&sc->recv_io.reassembly.list);
	spin_lock_init(&sc->recv_io.reassembly.lock);
	sc->recv_io.reassembly.data_length = 0;
	sc->recv_io.reassembly.queue_length = 0;

	INIT_LIST_HEAD(&sc->recv_io.free.list);
	spin_lock_init(&sc->recv_io.free.lock);
	info->count_receive_queue = 0;

	init_waitqueue_head(&info->wait_receive_queues);

	for (i = 0; i < num_buf; i++) {
		response = mempool_alloc(sc->recv_io.mem.pool, GFP_KERNEL);
		if (!response)
			goto allocate_failed;

		response->socket = sc;
		response->sge.length = 0;
		list_add_tail(&response->list, &sc->recv_io.free.list);
		info->count_receive_queue++;
	}

	return 0;

allocate_failed:
	while (!list_empty(&sc->recv_io.free.list)) {
		response = list_first_entry(
				&sc->recv_io.free.list,
				struct smbdirect_recv_io, list);
		list_del(&response->list);
		info->count_receive_queue--;

		mempool_free(response, sc->recv_io.mem.pool);
	}
	return -ENOMEM;
}

static void destroy_receive_buffers(struct smbd_connection *info)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbdirect_recv_io *response;

	while ((response = get_receive_buffer(info)))
		mempool_free(response, sc->recv_io.mem.pool);
}

/* Implement idle connection timer [MS-SMBD] 3.1.6.2 */
static void idle_connection_timer(struct work_struct *work)
{
	struct smbd_connection *info = container_of(
					work, struct smbd_connection,
					idle_timer_work.work);
	struct smbdirect_socket *sc = &info->socket;
	struct smbdirect_socket_parameters *sp = &sc->parameters;

	if (info->keep_alive_requested != KEEP_ALIVE_NONE) {
		log_keep_alive(ERR,
			"error status info->keep_alive_requested=%d\n",
			info->keep_alive_requested);
		smbd_disconnect_rdma_connection(info);
		return;
	}

	log_keep_alive(INFO, "about to send an empty idle message\n");
	smbd_post_send_empty(info);

	/* Setup the next idle timeout work */
	queue_delayed_work(info->workqueue, &info->idle_timer_work,
			msecs_to_jiffies(sp->keepalive_interval_msec));
}

/*
 * Destroy the transport and related RDMA and memory resources
 * Need to go through all the pending counters and make sure on one is using
 * the transport while it is destroyed
 */
void smbd_destroy(struct TCP_Server_Info *server)
{
	struct smbd_connection *info = server->smbd_conn;
	struct smbdirect_socket *sc;
	struct smbdirect_socket_parameters *sp;
	struct smbdirect_recv_io *response;
	unsigned long flags;

	if (!info) {
		log_rdma_event(INFO, "rdma session already destroyed\n");
		return;
	}
	sc = &info->socket;
	sp = &sc->parameters;

	log_rdma_event(INFO, "destroying rdma session\n");
	if (sc->status != SMBDIRECT_SOCKET_DISCONNECTED) {
		rdma_disconnect(sc->rdma.cm_id);
		log_rdma_event(INFO, "wait for transport being disconnected\n");
		wait_event_interruptible(
			info->status_wait,
			sc->status == SMBDIRECT_SOCKET_DISCONNECTED);
	}

	log_rdma_event(INFO, "cancelling post_send_credits_work\n");
	disable_work_sync(&info->post_send_credits_work);

	log_rdma_event(INFO, "destroying qp\n");
	ib_drain_qp(sc->ib.qp);
	rdma_destroy_qp(sc->rdma.cm_id);
	sc->ib.qp = NULL;

	log_rdma_event(INFO, "cancelling idle timer\n");
	disable_delayed_work_sync(&info->idle_timer_work);

	/* It's not possible for upper layer to get to reassembly */
	log_rdma_event(INFO, "drain the reassembly queue\n");
	do {
		spin_lock_irqsave(&sc->recv_io.reassembly.lock, flags);
		response = _get_first_reassembly(info);
		if (response) {
			list_del(&response->list);
			spin_unlock_irqrestore(
				&sc->recv_io.reassembly.lock, flags);
			put_receive_buffer(info, response);
		} else
			spin_unlock_irqrestore(
				&sc->recv_io.reassembly.lock, flags);
	} while (response);
	sc->recv_io.reassembly.data_length = 0;

	log_rdma_event(INFO, "free receive buffers\n");
	wait_event(info->wait_receive_queues,
		info->count_receive_queue == sp->recv_credit_max);
	destroy_receive_buffers(info);

	/*
	 * For performance reasons, memory registration and deregistration
	 * are not locked by srv_mutex. It is possible some processes are
	 * blocked on transport srv_mutex while holding memory registration.
	 * Release the transport srv_mutex to allow them to hit the failure
	 * path when sending data, and then release memory registrations.
	 */
	log_rdma_event(INFO, "freeing mr list\n");
	wake_up_interruptible_all(&info->wait_mr);
	while (atomic_read(&info->mr_used_count)) {
		cifs_server_unlock(server);
		msleep(1000);
		cifs_server_lock(server);
	}
	destroy_mr_list(info);

	ib_free_cq(sc->ib.send_cq);
	ib_free_cq(sc->ib.recv_cq);
	ib_dealloc_pd(sc->ib.pd);
	rdma_destroy_id(sc->rdma.cm_id);

	/* free mempools */
	mempool_destroy(sc->send_io.mem.pool);
	kmem_cache_destroy(sc->send_io.mem.cache);

	mempool_destroy(sc->recv_io.mem.pool);
	kmem_cache_destroy(sc->recv_io.mem.cache);

	sc->status = SMBDIRECT_SOCKET_DESTROYED;

	destroy_workqueue(info->workqueue);
	log_rdma_event(INFO,  "rdma session destroyed\n");
	kfree(info);
	server->smbd_conn = NULL;
}

/*
 * Reconnect this SMBD connection, called from upper layer
 * return value: 0 on success, or actual error code
 */
int smbd_reconnect(struct TCP_Server_Info *server)
{
	log_rdma_event(INFO, "reconnecting rdma session\n");

	if (!server->smbd_conn) {
		log_rdma_event(INFO, "rdma session already destroyed\n");
		goto create_conn;
	}

	/*
	 * This is possible if transport is disconnected and we haven't received
	 * notification from RDMA, but upper layer has detected timeout
	 */
	if (server->smbd_conn->socket.status == SMBDIRECT_SOCKET_CONNECTED) {
		log_rdma_event(INFO, "disconnecting transport\n");
		smbd_destroy(server);
	}

create_conn:
	log_rdma_event(INFO, "creating rdma session\n");
	server->smbd_conn = smbd_get_connection(
		server, (struct sockaddr *) &server->dstaddr);

	if (server->smbd_conn) {
		cifs_dbg(VFS, "RDMA transport re-established\n");
		trace_smb3_smbd_connect_done(server->hostname, server->conn_id, &server->dstaddr);
		return 0;
	}
	trace_smb3_smbd_connect_err(server->hostname, server->conn_id, &server->dstaddr);
	return -ENOENT;
}

static void destroy_caches_and_workqueue(struct smbd_connection *info)
{
	struct smbdirect_socket *sc = &info->socket;

	destroy_receive_buffers(info);
	destroy_workqueue(info->workqueue);
	mempool_destroy(sc->recv_io.mem.pool);
	kmem_cache_destroy(sc->recv_io.mem.cache);
	mempool_destroy(sc->send_io.mem.pool);
	kmem_cache_destroy(sc->send_io.mem.cache);
}

#define MAX_NAME_LEN	80
static int allocate_caches_and_workqueue(struct smbd_connection *info)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbdirect_socket_parameters *sp = &sc->parameters;
	char name[MAX_NAME_LEN];
	int rc;

	if (WARN_ON_ONCE(sp->max_recv_size < sizeof(struct smbdirect_data_transfer)))
		return -ENOMEM;

	scnprintf(name, MAX_NAME_LEN, "smbdirect_send_io_%p", info);
	sc->send_io.mem.cache =
		kmem_cache_create(
			name,
			sizeof(struct smbdirect_send_io) +
				sizeof(struct smbdirect_data_transfer),
			0, SLAB_HWCACHE_ALIGN, NULL);
	if (!sc->send_io.mem.cache)
		return -ENOMEM;

	sc->send_io.mem.pool =
		mempool_create(sp->send_credit_target, mempool_alloc_slab,
			mempool_free_slab, sc->send_io.mem.cache);
	if (!sc->send_io.mem.pool)
		goto out1;

	scnprintf(name, MAX_NAME_LEN, "smbdirect_recv_io_%p", info);

	struct kmem_cache_args response_args = {
		.align		= __alignof__(struct smbdirect_recv_io),
		.useroffset	= (offsetof(struct smbdirect_recv_io, packet) +
				   sizeof(struct smbdirect_data_transfer)),
		.usersize	= sp->max_recv_size - sizeof(struct smbdirect_data_transfer),
	};
	sc->recv_io.mem.cache =
		kmem_cache_create(name,
				  sizeof(struct smbdirect_recv_io) + sp->max_recv_size,
				  &response_args, SLAB_HWCACHE_ALIGN);
	if (!sc->recv_io.mem.cache)
		goto out2;

	sc->recv_io.mem.pool =
		mempool_create(sp->recv_credit_max, mempool_alloc_slab,
		       mempool_free_slab, sc->recv_io.mem.cache);
	if (!sc->recv_io.mem.pool)
		goto out3;

	scnprintf(name, MAX_NAME_LEN, "smbd_%p", info);
	info->workqueue = create_workqueue(name);
	if (!info->workqueue)
		goto out4;

	rc = allocate_receive_buffers(info, sp->recv_credit_max);
	if (rc) {
		log_rdma_event(ERR, "failed to allocate receive buffers\n");
		goto out5;
	}

	return 0;

out5:
	destroy_workqueue(info->workqueue);
out4:
	mempool_destroy(sc->recv_io.mem.pool);
out3:
	kmem_cache_destroy(sc->recv_io.mem.cache);
out2:
	mempool_destroy(sc->send_io.mem.pool);
out1:
	kmem_cache_destroy(sc->send_io.mem.cache);
	return -ENOMEM;
}

/* Create a SMBD connection, called by upper layer */
static struct smbd_connection *_smbd_get_connection(
	struct TCP_Server_Info *server, struct sockaddr *dstaddr, int port)
{
	int rc;
	struct smbd_connection *info;
	struct smbdirect_socket *sc;
	struct smbdirect_socket_parameters *sp;
	struct rdma_conn_param conn_param;
	struct ib_qp_init_attr qp_attr;
	struct sockaddr_in *addr_in = (struct sockaddr_in *) dstaddr;
	struct ib_port_immutable port_immutable;
	__be32 ird_ord_hdr[2];

	info = kzalloc(sizeof(struct smbd_connection), GFP_KERNEL);
	if (!info)
		return NULL;
	sc = &info->socket;
	sp = &sc->parameters;

	info->initiator_depth = 1;
	info->responder_resources = SMBD_CM_RESPONDER_RESOURCES;

	sc->status = SMBDIRECT_SOCKET_CONNECTING;
	rc = smbd_ia_open(info, dstaddr, port);
	if (rc) {
		log_rdma_event(INFO, "smbd_ia_open rc=%d\n", rc);
		goto create_id_failed;
	}

	if (smbd_send_credit_target > sc->ib.dev->attrs.max_cqe ||
	    smbd_send_credit_target > sc->ib.dev->attrs.max_qp_wr) {
		log_rdma_event(ERR, "consider lowering send_credit_target = %d. Possible CQE overrun, device reporting max_cqe %d max_qp_wr %d\n",
			       smbd_send_credit_target,
			       sc->ib.dev->attrs.max_cqe,
			       sc->ib.dev->attrs.max_qp_wr);
		goto config_failed;
	}

	if (smbd_receive_credit_max > sc->ib.dev->attrs.max_cqe ||
	    smbd_receive_credit_max > sc->ib.dev->attrs.max_qp_wr) {
		log_rdma_event(ERR, "consider lowering receive_credit_max = %d. Possible CQE overrun, device reporting max_cqe %d max_qp_wr %d\n",
			       smbd_receive_credit_max,
			       sc->ib.dev->attrs.max_cqe,
			       sc->ib.dev->attrs.max_qp_wr);
		goto config_failed;
	}

	sp->recv_credit_max = smbd_receive_credit_max;
	sp->send_credit_target = smbd_send_credit_target;
	sp->max_send_size = smbd_max_send_size;
	sp->max_fragmented_recv_size = smbd_max_fragmented_recv_size;
	sp->max_recv_size = smbd_max_receive_size;
	sp->keepalive_interval_msec = smbd_keep_alive_interval * 1000;

	if (sc->ib.dev->attrs.max_send_sge < SMBDIRECT_SEND_IO_MAX_SGE ||
	    sc->ib.dev->attrs.max_recv_sge < SMBDIRECT_RECV_IO_MAX_SGE) {
		log_rdma_event(ERR,
			"device %.*s max_send_sge/max_recv_sge = %d/%d too small\n",
			IB_DEVICE_NAME_MAX,
			sc->ib.dev->name,
			sc->ib.dev->attrs.max_send_sge,
			sc->ib.dev->attrs.max_recv_sge);
		goto config_failed;
	}

	sc->ib.send_cq =
		ib_alloc_cq_any(sc->ib.dev, info,
				sp->send_credit_target, IB_POLL_SOFTIRQ);
	if (IS_ERR(sc->ib.send_cq)) {
		sc->ib.send_cq = NULL;
		goto alloc_cq_failed;
	}

	sc->ib.recv_cq =
		ib_alloc_cq_any(sc->ib.dev, info,
				sp->recv_credit_max, IB_POLL_SOFTIRQ);
	if (IS_ERR(sc->ib.recv_cq)) {
		sc->ib.recv_cq = NULL;
		goto alloc_cq_failed;
	}

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.event_handler = smbd_qp_async_error_upcall;
	qp_attr.qp_context = info;
	qp_attr.cap.max_send_wr = sp->send_credit_target;
	qp_attr.cap.max_recv_wr = sp->recv_credit_max;
	qp_attr.cap.max_send_sge = SMBDIRECT_SEND_IO_MAX_SGE;
	qp_attr.cap.max_recv_sge = SMBDIRECT_RECV_IO_MAX_SGE;
	qp_attr.cap.max_inline_data = 0;
	qp_attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	qp_attr.qp_type = IB_QPT_RC;
	qp_attr.send_cq = sc->ib.send_cq;
	qp_attr.recv_cq = sc->ib.recv_cq;
	qp_attr.port_num = ~0;

	rc = rdma_create_qp(sc->rdma.cm_id, sc->ib.pd, &qp_attr);
	if (rc) {
		log_rdma_event(ERR, "rdma_create_qp failed %i\n", rc);
		goto create_qp_failed;
	}
	sc->ib.qp = sc->rdma.cm_id->qp;

	info->responder_resources =
		min_t(u8, info->responder_resources,
		      sc->ib.dev->attrs.max_qp_rd_atom);
	log_rdma_mr(INFO, "responder_resources=%d\n",
		info->responder_resources);

	memset(&conn_param, 0, sizeof(conn_param));
	conn_param.initiator_depth = info->initiator_depth;
	conn_param.responder_resources = info->responder_resources;

	/* Need to send IRD/ORD in private data for iWARP */
	sc->ib.dev->ops.get_port_immutable(
		sc->ib.dev, sc->rdma.cm_id->port_num, &port_immutable);
	if (port_immutable.core_cap_flags & RDMA_CORE_PORT_IWARP) {
		ird_ord_hdr[0] = cpu_to_be32(conn_param.responder_resources);
		ird_ord_hdr[1] = cpu_to_be32(conn_param.initiator_depth);
		conn_param.private_data = ird_ord_hdr;
		conn_param.private_data_len = sizeof(ird_ord_hdr);
	} else {
		conn_param.private_data = NULL;
		conn_param.private_data_len = 0;
	}

	conn_param.retry_count = SMBD_CM_RETRY;
	conn_param.rnr_retry_count = SMBD_CM_RNR_RETRY;
	conn_param.flow_control = 0;

	log_rdma_event(INFO, "connecting to IP %pI4 port %d\n",
		&addr_in->sin_addr, port);

	init_waitqueue_head(&info->status_wait);
	init_waitqueue_head(&sc->recv_io.reassembly.wait_queue);
	rc = rdma_connect(sc->rdma.cm_id, &conn_param);
	if (rc) {
		log_rdma_event(ERR, "rdma_connect() failed with %i\n", rc);
		goto rdma_connect_failed;
	}

	wait_event_interruptible_timeout(
		info->status_wait,
		sc->status != SMBDIRECT_SOCKET_CONNECTING,
		msecs_to_jiffies(RDMA_RESOLVE_TIMEOUT));

	if (sc->status != SMBDIRECT_SOCKET_CONNECTED) {
		log_rdma_event(ERR, "rdma_connect failed port=%d\n", port);
		goto rdma_connect_failed;
	}

	log_rdma_event(INFO, "rdma_connect connected\n");

	rc = allocate_caches_and_workqueue(info);
	if (rc) {
		log_rdma_event(ERR, "cache allocation failed\n");
		goto allocate_cache_failed;
	}

	init_waitqueue_head(&info->wait_send_queue);
	INIT_DELAYED_WORK(&info->idle_timer_work, idle_connection_timer);
	queue_delayed_work(info->workqueue, &info->idle_timer_work,
		msecs_to_jiffies(sp->keepalive_interval_msec));

	init_waitqueue_head(&info->wait_send_pending);
	atomic_set(&info->send_pending, 0);

	init_waitqueue_head(&info->wait_post_send);

	INIT_WORK(&info->disconnect_work, smbd_disconnect_rdma_work);
	INIT_WORK(&info->post_send_credits_work, smbd_post_send_credits);
	info->new_credits_offered = 0;
	spin_lock_init(&info->lock_new_credits_offered);

	rc = smbd_negotiate(info);
	if (rc) {
		log_rdma_event(ERR, "smbd_negotiate rc=%d\n", rc);
		goto negotiation_failed;
	}

	rc = allocate_mr_list(info);
	if (rc) {
		log_rdma_mr(ERR, "memory registration allocation failed\n");
		goto allocate_mr_failed;
	}

	return info;

allocate_mr_failed:
	/* At this point, need to a full transport shutdown */
	server->smbd_conn = info;
	smbd_destroy(server);
	return NULL;

negotiation_failed:
	disable_delayed_work_sync(&info->idle_timer_work);
	destroy_caches_and_workqueue(info);
	sc->status = SMBDIRECT_SOCKET_NEGOTIATE_FAILED;
	rdma_disconnect(sc->rdma.cm_id);
	wait_event(info->status_wait,
		sc->status == SMBDIRECT_SOCKET_DISCONNECTED);

allocate_cache_failed:
rdma_connect_failed:
	rdma_destroy_qp(sc->rdma.cm_id);

create_qp_failed:
alloc_cq_failed:
	if (sc->ib.send_cq)
		ib_free_cq(sc->ib.send_cq);
	if (sc->ib.recv_cq)
		ib_free_cq(sc->ib.recv_cq);

config_failed:
	ib_dealloc_pd(sc->ib.pd);
	rdma_destroy_id(sc->rdma.cm_id);

create_id_failed:
	kfree(info);
	return NULL;
}

struct smbd_connection *smbd_get_connection(
	struct TCP_Server_Info *server, struct sockaddr *dstaddr)
{
	struct smbd_connection *ret;
	int port = SMBD_PORT;

try_again:
	ret = _smbd_get_connection(server, dstaddr, port);

	/* Try SMB_PORT if SMBD_PORT doesn't work */
	if (!ret && port == SMBD_PORT) {
		port = SMB_PORT;
		goto try_again;
	}
	return ret;
}

/*
 * Receive data from the transport's receive reassembly queue
 * All the incoming data packets are placed in reassembly queue
 * iter: the buffer to read data into
 * size: the length of data to read
 * return value: actual data read
 *
 * Note: this implementation copies the data from reassembly queue to receive
 * buffers used by upper layer. This is not the optimal code path. A better way
 * to do it is to not have upper layer allocate its receive buffers but rather
 * borrow the buffer from reassembly queue, and return it after data is
 * consumed. But this will require more changes to upper layer code, and also
 * need to consider packet boundaries while they still being reassembled.
 */
int smbd_recv(struct smbd_connection *info, struct msghdr *msg)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbdirect_recv_io *response;
	struct smbdirect_data_transfer *data_transfer;
	size_t size = iov_iter_count(&msg->msg_iter);
	int to_copy, to_read, data_read, offset;
	u32 data_length, remaining_data_length, data_offset;
	int rc;

	if (WARN_ON_ONCE(iov_iter_rw(&msg->msg_iter) == WRITE))
		return -EINVAL; /* It's a bug in upper layer to get there */

again:
	/*
	 * No need to hold the reassembly queue lock all the time as we are
	 * the only one reading from the front of the queue. The transport
	 * may add more entries to the back of the queue at the same time
	 */
	log_read(INFO, "size=%zd sc->recv_io.reassembly.data_length=%d\n", size,
		sc->recv_io.reassembly.data_length);
	if (sc->recv_io.reassembly.data_length >= size) {
		int queue_length;
		int queue_removed = 0;

		/*
		 * Need to make sure reassembly_data_length is read before
		 * reading reassembly_queue_length and calling
		 * _get_first_reassembly. This call is lock free
		 * as we never read at the end of the queue which are being
		 * updated in SOFTIRQ as more data is received
		 */
		virt_rmb();
		queue_length = sc->recv_io.reassembly.queue_length;
		data_read = 0;
		to_read = size;
		offset = sc->recv_io.reassembly.first_entry_offset;
		while (data_read < size) {
			response = _get_first_reassembly(info);
			data_transfer = smbdirect_recv_io_payload(response);
			data_length = le32_to_cpu(data_transfer->data_length);
			remaining_data_length =
				le32_to_cpu(
					data_transfer->remaining_data_length);
			data_offset = le32_to_cpu(data_transfer->data_offset);

			/*
			 * The upper layer expects RFC1002 length at the
			 * beginning of the payload. Return it to indicate
			 * the total length of the packet. This minimize the
			 * change to upper layer packet processing logic. This
			 * will be eventually remove when an intermediate
			 * transport layer is added
			 */
			if (response->first_segment && size == 4) {
				unsigned int rfc1002_len =
					data_length + remaining_data_length;
				__be32 rfc1002_hdr = cpu_to_be32(rfc1002_len);
				if (copy_to_iter(&rfc1002_hdr, sizeof(rfc1002_hdr),
						 &msg->msg_iter) != sizeof(rfc1002_hdr))
					return -EFAULT;
				data_read = 4;
				response->first_segment = false;
				log_read(INFO, "returning rfc1002 length %d\n",
					rfc1002_len);
				goto read_rfc1002_done;
			}

			to_copy = min_t(int, data_length - offset, to_read);
			if (copy_to_iter((char *)data_transfer + data_offset + offset,
					 to_copy, &msg->msg_iter) != to_copy)
				return -EFAULT;

			/* move on to the next buffer? */
			if (to_copy == data_length - offset) {
				queue_length--;
				/*
				 * No need to lock if we are not at the
				 * end of the queue
				 */
				if (queue_length)
					list_del(&response->list);
				else {
					spin_lock_irq(
						&sc->recv_io.reassembly.lock);
					list_del(&response->list);
					spin_unlock_irq(
						&sc->recv_io.reassembly.lock);
				}
				queue_removed++;
				info->count_reassembly_queue--;
				info->count_dequeue_reassembly_queue++;
				put_receive_buffer(info, response);
				offset = 0;
				log_read(INFO, "put_receive_buffer offset=0\n");
			} else
				offset += to_copy;

			to_read -= to_copy;
			data_read += to_copy;

			log_read(INFO, "_get_first_reassembly memcpy %d bytes data_transfer_length-offset=%d after that to_read=%d data_read=%d offset=%d\n",
				 to_copy, data_length - offset,
				 to_read, data_read, offset);
		}

		spin_lock_irq(&sc->recv_io.reassembly.lock);
		sc->recv_io.reassembly.data_length -= data_read;
		sc->recv_io.reassembly.queue_length -= queue_removed;
		spin_unlock_irq(&sc->recv_io.reassembly.lock);

		sc->recv_io.reassembly.first_entry_offset = offset;
		log_read(INFO, "returning to thread data_read=%d reassembly_data_length=%d first_entry_offset=%d\n",
			 data_read, sc->recv_io.reassembly.data_length,
			 sc->recv_io.reassembly.first_entry_offset);
read_rfc1002_done:
		return data_read;
	}

	log_read(INFO, "wait_event on more data\n");
	rc = wait_event_interruptible(
		sc->recv_io.reassembly.wait_queue,
		sc->recv_io.reassembly.data_length >= size ||
			sc->status != SMBDIRECT_SOCKET_CONNECTED);
	/* Don't return any data if interrupted */
	if (rc)
		return rc;

	if (sc->status != SMBDIRECT_SOCKET_CONNECTED) {
		log_read(ERR, "disconnected\n");
		return -ECONNABORTED;
	}

	goto again;
}

/*
 * Send data to transport
 * Each rqst is transported as a SMBDirect payload
 * rqst: the data to write
 * return value: 0 if successfully write, otherwise error code
 */
int smbd_send(struct TCP_Server_Info *server,
	int num_rqst, struct smb_rqst *rqst_array)
{
	struct smbd_connection *info = server->smbd_conn;
	struct smbdirect_socket *sc = &info->socket;
	struct smbdirect_socket_parameters *sp = &sc->parameters;
	struct smb_rqst *rqst;
	struct iov_iter iter;
	unsigned int remaining_data_length, klen;
	int rc, i, rqst_idx;

	if (sc->status != SMBDIRECT_SOCKET_CONNECTED)
		return -EAGAIN;

	/*
	 * Add in the page array if there is one. The caller needs to set
	 * rq_tailsz to PAGE_SIZE when the buffer has multiple pages and
	 * ends at page boundary
	 */
	remaining_data_length = 0;
	for (i = 0; i < num_rqst; i++)
		remaining_data_length += smb_rqst_len(server, &rqst_array[i]);

	if (unlikely(remaining_data_length > sp->max_fragmented_send_size)) {
		/* assertion: payload never exceeds negotiated maximum */
		log_write(ERR, "payload size %d > max size %d\n",
			remaining_data_length, sp->max_fragmented_send_size);
		return -EINVAL;
	}

	log_write(INFO, "num_rqst=%d total length=%u\n",
			num_rqst, remaining_data_length);

	rqst_idx = 0;
	do {
		rqst = &rqst_array[rqst_idx];

		cifs_dbg(FYI, "Sending smb (RDMA): idx=%d smb_len=%lu\n",
			 rqst_idx, smb_rqst_len(server, rqst));
		for (i = 0; i < rqst->rq_nvec; i++)
			dump_smb(rqst->rq_iov[i].iov_base, rqst->rq_iov[i].iov_len);

		log_write(INFO, "RDMA-WR[%u] nvec=%d len=%u iter=%zu rqlen=%lu\n",
			  rqst_idx, rqst->rq_nvec, remaining_data_length,
			  iov_iter_count(&rqst->rq_iter), smb_rqst_len(server, rqst));

		/* Send the metadata pages. */
		klen = 0;
		for (i = 0; i < rqst->rq_nvec; i++)
			klen += rqst->rq_iov[i].iov_len;
		iov_iter_kvec(&iter, ITER_SOURCE, rqst->rq_iov, rqst->rq_nvec, klen);

		rc = smbd_post_send_full_iter(info, &iter, &remaining_data_length);
		if (rc < 0)
			break;

		if (iov_iter_count(&rqst->rq_iter) > 0) {
			/* And then the data pages if there are any */
			rc = smbd_post_send_full_iter(info, &rqst->rq_iter,
						      &remaining_data_length);
			if (rc < 0)
				break;
		}

	} while (++rqst_idx < num_rqst);

	/*
	 * As an optimization, we don't wait for individual I/O to finish
	 * before sending the next one.
	 * Send them all and wait for pending send count to get to 0
	 * that means all the I/Os have been out and we are good to return
	 */

	wait_event(info->wait_send_pending,
		atomic_read(&info->send_pending) == 0 ||
		sc->status != SMBDIRECT_SOCKET_CONNECTED);

	if (sc->status != SMBDIRECT_SOCKET_CONNECTED && rc == 0)
		rc = -EAGAIN;

	return rc;
}

static void register_mr_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct smbd_mr *mr;
	struct ib_cqe *cqe;

	if (wc->status) {
		log_rdma_mr(ERR, "status=%d\n", wc->status);
		cqe = wc->wr_cqe;
		mr = container_of(cqe, struct smbd_mr, cqe);
		smbd_disconnect_rdma_connection(mr->conn);
	}
}

/*
 * The work queue function that recovers MRs
 * We need to call ib_dereg_mr() and ib_alloc_mr() before this MR can be used
 * again. Both calls are slow, so finish them in a workqueue. This will not
 * block I/O path.
 * There is one workqueue that recovers MRs, there is no need to lock as the
 * I/O requests calling smbd_register_mr will never update the links in the
 * mr_list.
 */
static void smbd_mr_recovery_work(struct work_struct *work)
{
	struct smbd_connection *info =
		container_of(work, struct smbd_connection, mr_recovery_work);
	struct smbdirect_socket *sc = &info->socket;
	struct smbd_mr *smbdirect_mr;
	int rc;

	list_for_each_entry(smbdirect_mr, &info->mr_list, list) {
		if (smbdirect_mr->state == MR_ERROR) {

			/* recover this MR entry */
			rc = ib_dereg_mr(smbdirect_mr->mr);
			if (rc) {
				log_rdma_mr(ERR,
					"ib_dereg_mr failed rc=%x\n",
					rc);
				smbd_disconnect_rdma_connection(info);
				continue;
			}

			smbdirect_mr->mr = ib_alloc_mr(
				sc->ib.pd, info->mr_type,
				info->max_frmr_depth);
			if (IS_ERR(smbdirect_mr->mr)) {
				log_rdma_mr(ERR, "ib_alloc_mr failed mr_type=%x max_frmr_depth=%x\n",
					    info->mr_type,
					    info->max_frmr_depth);
				smbd_disconnect_rdma_connection(info);
				continue;
			}
		} else
			/* This MR is being used, don't recover it */
			continue;

		smbdirect_mr->state = MR_READY;

		/* smbdirect_mr->state is updated by this function
		 * and is read and updated by I/O issuing CPUs trying
		 * to get a MR, the call to atomic_inc_return
		 * implicates a memory barrier and guarantees this
		 * value is updated before waking up any calls to
		 * get_mr() from the I/O issuing CPUs
		 */
		if (atomic_inc_return(&info->mr_ready_count) == 1)
			wake_up_interruptible(&info->wait_mr);
	}
}

static void destroy_mr_list(struct smbd_connection *info)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbd_mr *mr, *tmp;

	disable_work_sync(&info->mr_recovery_work);
	list_for_each_entry_safe(mr, tmp, &info->mr_list, list) {
		if (mr->state == MR_INVALIDATED)
			ib_dma_unmap_sg(sc->ib.dev, mr->sgt.sgl,
				mr->sgt.nents, mr->dir);
		ib_dereg_mr(mr->mr);
		kfree(mr->sgt.sgl);
		kfree(mr);
	}
}

/*
 * Allocate MRs used for RDMA read/write
 * The number of MRs will not exceed hardware capability in responder_resources
 * All MRs are kept in mr_list. The MR can be recovered after it's used
 * Recovery is done in smbd_mr_recovery_work. The content of list entry changes
 * as MRs are used and recovered for I/O, but the list links will not change
 */
static int allocate_mr_list(struct smbd_connection *info)
{
	struct smbdirect_socket *sc = &info->socket;
	int i;
	struct smbd_mr *smbdirect_mr, *tmp;

	INIT_LIST_HEAD(&info->mr_list);
	init_waitqueue_head(&info->wait_mr);
	spin_lock_init(&info->mr_list_lock);
	atomic_set(&info->mr_ready_count, 0);
	atomic_set(&info->mr_used_count, 0);
	init_waitqueue_head(&info->wait_for_mr_cleanup);
	INIT_WORK(&info->mr_recovery_work, smbd_mr_recovery_work);

	if (info->responder_resources == 0) {
		log_rdma_mr(ERR, "responder_resources negotiated as 0\n");
		return -EINVAL;
	}

	/* Allocate more MRs (2x) than hardware responder_resources */
	for (i = 0; i < info->responder_resources * 2; i++) {
		smbdirect_mr = kzalloc(sizeof(*smbdirect_mr), GFP_KERNEL);
		if (!smbdirect_mr)
			goto cleanup_entries;
		smbdirect_mr->mr = ib_alloc_mr(sc->ib.pd, info->mr_type,
					info->max_frmr_depth);
		if (IS_ERR(smbdirect_mr->mr)) {
			log_rdma_mr(ERR, "ib_alloc_mr failed mr_type=%x max_frmr_depth=%x\n",
				    info->mr_type, info->max_frmr_depth);
			goto out;
		}
		smbdirect_mr->sgt.sgl = kcalloc(info->max_frmr_depth,
						sizeof(struct scatterlist),
						GFP_KERNEL);
		if (!smbdirect_mr->sgt.sgl) {
			log_rdma_mr(ERR, "failed to allocate sgl\n");
			ib_dereg_mr(smbdirect_mr->mr);
			goto out;
		}
		smbdirect_mr->state = MR_READY;
		smbdirect_mr->conn = info;

		list_add_tail(&smbdirect_mr->list, &info->mr_list);
		atomic_inc(&info->mr_ready_count);
	}
	return 0;

out:
	kfree(smbdirect_mr);
cleanup_entries:
	list_for_each_entry_safe(smbdirect_mr, tmp, &info->mr_list, list) {
		list_del(&smbdirect_mr->list);
		ib_dereg_mr(smbdirect_mr->mr);
		kfree(smbdirect_mr->sgt.sgl);
		kfree(smbdirect_mr);
	}
	return -ENOMEM;
}

/*
 * Get a MR from mr_list. This function waits until there is at least one
 * MR available in the list. It may access the list while the
 * smbd_mr_recovery_work is recovering the MR list. This doesn't need a lock
 * as they never modify the same places. However, there may be several CPUs
 * issuing I/O trying to get MR at the same time, mr_list_lock is used to
 * protect this situation.
 */
static struct smbd_mr *get_mr(struct smbd_connection *info)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbd_mr *ret;
	int rc;
again:
	rc = wait_event_interruptible(info->wait_mr,
		atomic_read(&info->mr_ready_count) ||
		sc->status != SMBDIRECT_SOCKET_CONNECTED);
	if (rc) {
		log_rdma_mr(ERR, "wait_event_interruptible rc=%x\n", rc);
		return NULL;
	}

	if (sc->status != SMBDIRECT_SOCKET_CONNECTED) {
		log_rdma_mr(ERR, "sc->status=%x\n", sc->status);
		return NULL;
	}

	spin_lock(&info->mr_list_lock);
	list_for_each_entry(ret, &info->mr_list, list) {
		if (ret->state == MR_READY) {
			ret->state = MR_REGISTERED;
			spin_unlock(&info->mr_list_lock);
			atomic_dec(&info->mr_ready_count);
			atomic_inc(&info->mr_used_count);
			return ret;
		}
	}

	spin_unlock(&info->mr_list_lock);
	/*
	 * It is possible that we could fail to get MR because other processes may
	 * try to acquire a MR at the same time. If this is the case, retry it.
	 */
	goto again;
}

/*
 * Transcribe the pages from an iterator into an MR scatterlist.
 */
static int smbd_iter_to_mr(struct smbd_connection *info,
			   struct iov_iter *iter,
			   struct sg_table *sgt,
			   unsigned int max_sg)
{
	int ret;

	memset(sgt->sgl, 0, max_sg * sizeof(struct scatterlist));

	ret = extract_iter_to_sg(iter, iov_iter_count(iter), sgt, max_sg, 0);
	WARN_ON(ret < 0);
	if (sgt->nents > 0)
		sg_mark_end(&sgt->sgl[sgt->nents - 1]);
	return ret;
}

/*
 * Register memory for RDMA read/write
 * iter: the buffer to register memory with
 * writing: true if this is a RDMA write (SMB read), false for RDMA read
 * need_invalidate: true if this MR needs to be locally invalidated after I/O
 * return value: the MR registered, NULL if failed.
 */
struct smbd_mr *smbd_register_mr(struct smbd_connection *info,
				 struct iov_iter *iter,
				 bool writing, bool need_invalidate)
{
	struct smbdirect_socket *sc = &info->socket;
	struct smbd_mr *smbdirect_mr;
	int rc, num_pages;
	enum dma_data_direction dir;
	struct ib_reg_wr *reg_wr;

	num_pages = iov_iter_npages(iter, info->max_frmr_depth + 1);
	if (num_pages > info->max_frmr_depth) {
		log_rdma_mr(ERR, "num_pages=%d max_frmr_depth=%d\n",
			num_pages, info->max_frmr_depth);
		WARN_ON_ONCE(1);
		return NULL;
	}

	smbdirect_mr = get_mr(info);
	if (!smbdirect_mr) {
		log_rdma_mr(ERR, "get_mr returning NULL\n");
		return NULL;
	}

	dir = writing ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	smbdirect_mr->dir = dir;
	smbdirect_mr->need_invalidate = need_invalidate;
	smbdirect_mr->sgt.nents = 0;
	smbdirect_mr->sgt.orig_nents = 0;

	log_rdma_mr(INFO, "num_pages=0x%x count=0x%zx depth=%u\n",
		    num_pages, iov_iter_count(iter), info->max_frmr_depth);
	smbd_iter_to_mr(info, iter, &smbdirect_mr->sgt, info->max_frmr_depth);

	rc = ib_dma_map_sg(sc->ib.dev, smbdirect_mr->sgt.sgl,
			   smbdirect_mr->sgt.nents, dir);
	if (!rc) {
		log_rdma_mr(ERR, "ib_dma_map_sg num_pages=%x dir=%x rc=%x\n",
			num_pages, dir, rc);
		goto dma_map_error;
	}

	rc = ib_map_mr_sg(smbdirect_mr->mr, smbdirect_mr->sgt.sgl,
			  smbdirect_mr->sgt.nents, NULL, PAGE_SIZE);
	if (rc != smbdirect_mr->sgt.nents) {
		log_rdma_mr(ERR,
			"ib_map_mr_sg failed rc = %d nents = %x\n",
			rc, smbdirect_mr->sgt.nents);
		goto map_mr_error;
	}

	ib_update_fast_reg_key(smbdirect_mr->mr,
		ib_inc_rkey(smbdirect_mr->mr->rkey));
	reg_wr = &smbdirect_mr->wr;
	reg_wr->wr.opcode = IB_WR_REG_MR;
	smbdirect_mr->cqe.done = register_mr_done;
	reg_wr->wr.wr_cqe = &smbdirect_mr->cqe;
	reg_wr->wr.num_sge = 0;
	reg_wr->wr.send_flags = IB_SEND_SIGNALED;
	reg_wr->mr = smbdirect_mr->mr;
	reg_wr->key = smbdirect_mr->mr->rkey;
	reg_wr->access = writing ?
			IB_ACCESS_REMOTE_WRITE | IB_ACCESS_LOCAL_WRITE :
			IB_ACCESS_REMOTE_READ;

	/*
	 * There is no need for waiting for complemtion on ib_post_send
	 * on IB_WR_REG_MR. Hardware enforces a barrier and order of execution
	 * on the next ib_post_send when we actually send I/O to remote peer
	 */
	rc = ib_post_send(sc->ib.qp, &reg_wr->wr, NULL);
	if (!rc)
		return smbdirect_mr;

	log_rdma_mr(ERR, "ib_post_send failed rc=%x reg_wr->key=%x\n",
		rc, reg_wr->key);

	/* If all failed, attempt to recover this MR by setting it MR_ERROR*/
map_mr_error:
	ib_dma_unmap_sg(sc->ib.dev, smbdirect_mr->sgt.sgl,
			smbdirect_mr->sgt.nents, smbdirect_mr->dir);

dma_map_error:
	smbdirect_mr->state = MR_ERROR;
	if (atomic_dec_and_test(&info->mr_used_count))
		wake_up(&info->wait_for_mr_cleanup);

	smbd_disconnect_rdma_connection(info);

	return NULL;
}

static void local_inv_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct smbd_mr *smbdirect_mr;
	struct ib_cqe *cqe;

	cqe = wc->wr_cqe;
	smbdirect_mr = container_of(cqe, struct smbd_mr, cqe);
	smbdirect_mr->state = MR_INVALIDATED;
	if (wc->status != IB_WC_SUCCESS) {
		log_rdma_mr(ERR, "invalidate failed status=%x\n", wc->status);
		smbdirect_mr->state = MR_ERROR;
	}
	complete(&smbdirect_mr->invalidate_done);
}

/*
 * Deregister a MR after I/O is done
 * This function may wait if remote invalidation is not used
 * and we have to locally invalidate the buffer to prevent data is being
 * modified by remote peer after upper layer consumes it
 */
int smbd_deregister_mr(struct smbd_mr *smbdirect_mr)
{
	struct ib_send_wr *wr;
	struct smbd_connection *info = smbdirect_mr->conn;
	struct smbdirect_socket *sc = &info->socket;
	int rc = 0;

	if (smbdirect_mr->need_invalidate) {
		/* Need to finish local invalidation before returning */
		wr = &smbdirect_mr->inv_wr;
		wr->opcode = IB_WR_LOCAL_INV;
		smbdirect_mr->cqe.done = local_inv_done;
		wr->wr_cqe = &smbdirect_mr->cqe;
		wr->num_sge = 0;
		wr->ex.invalidate_rkey = smbdirect_mr->mr->rkey;
		wr->send_flags = IB_SEND_SIGNALED;

		init_completion(&smbdirect_mr->invalidate_done);
		rc = ib_post_send(sc->ib.qp, wr, NULL);
		if (rc) {
			log_rdma_mr(ERR, "ib_post_send failed rc=%x\n", rc);
			smbd_disconnect_rdma_connection(info);
			goto done;
		}
		wait_for_completion(&smbdirect_mr->invalidate_done);
		smbdirect_mr->need_invalidate = false;
	} else
		/*
		 * For remote invalidation, just set it to MR_INVALIDATED
		 * and defer to mr_recovery_work to recover the MR for next use
		 */
		smbdirect_mr->state = MR_INVALIDATED;

	if (smbdirect_mr->state == MR_INVALIDATED) {
		ib_dma_unmap_sg(
			sc->ib.dev, smbdirect_mr->sgt.sgl,
			smbdirect_mr->sgt.nents,
			smbdirect_mr->dir);
		smbdirect_mr->state = MR_READY;
		if (atomic_inc_return(&info->mr_ready_count) == 1)
			wake_up_interruptible(&info->wait_mr);
	} else
		/*
		 * Schedule the work to do MR recovery for future I/Os MR
		 * recovery is slow and don't want it to block current I/O
		 */
		queue_work(info->workqueue, &info->mr_recovery_work);

done:
	if (atomic_dec_and_test(&info->mr_used_count))
		wake_up(&info->wait_for_mr_cleanup);

	return rc;
}

static bool smb_set_sge(struct smb_extract_to_rdma *rdma,
			struct page *lowest_page, size_t off, size_t len)
{
	struct ib_sge *sge = &rdma->sge[rdma->nr_sge];
	u64 addr;

	addr = ib_dma_map_page(rdma->device, lowest_page,
			       off, len, rdma->direction);
	if (ib_dma_mapping_error(rdma->device, addr))
		return false;

	sge->addr   = addr;
	sge->length = len;
	sge->lkey   = rdma->local_dma_lkey;
	rdma->nr_sge++;
	return true;
}

/*
 * Extract page fragments from a BVEC-class iterator and add them to an RDMA
 * element list.  The pages are not pinned.
 */
static ssize_t smb_extract_bvec_to_rdma(struct iov_iter *iter,
					struct smb_extract_to_rdma *rdma,
					ssize_t maxsize)
{
	const struct bio_vec *bv = iter->bvec;
	unsigned long start = iter->iov_offset;
	unsigned int i;
	ssize_t ret = 0;

	for (i = 0; i < iter->nr_segs; i++) {
		size_t off, len;

		len = bv[i].bv_len;
		if (start >= len) {
			start -= len;
			continue;
		}

		len = min_t(size_t, maxsize, len - start);
		off = bv[i].bv_offset + start;

		if (!smb_set_sge(rdma, bv[i].bv_page, off, len))
			return -EIO;

		ret += len;
		maxsize -= len;
		if (rdma->nr_sge >= rdma->max_sge || maxsize <= 0)
			break;
		start = 0;
	}

	if (ret > 0)
		iov_iter_advance(iter, ret);
	return ret;
}

/*
 * Extract fragments from a KVEC-class iterator and add them to an RDMA list.
 * This can deal with vmalloc'd buffers as well as kmalloc'd or static buffers.
 * The pages are not pinned.
 */
static ssize_t smb_extract_kvec_to_rdma(struct iov_iter *iter,
					struct smb_extract_to_rdma *rdma,
					ssize_t maxsize)
{
	const struct kvec *kv = iter->kvec;
	unsigned long start = iter->iov_offset;
	unsigned int i;
	ssize_t ret = 0;

	for (i = 0; i < iter->nr_segs; i++) {
		struct page *page;
		unsigned long kaddr;
		size_t off, len, seg;

		len = kv[i].iov_len;
		if (start >= len) {
			start -= len;
			continue;
		}

		kaddr = (unsigned long)kv[i].iov_base + start;
		off = kaddr & ~PAGE_MASK;
		len = min_t(size_t, maxsize, len - start);
		kaddr &= PAGE_MASK;

		maxsize -= len;
		do {
			seg = min_t(size_t, len, PAGE_SIZE - off);

			if (is_vmalloc_or_module_addr((void *)kaddr))
				page = vmalloc_to_page((void *)kaddr);
			else
				page = virt_to_page((void *)kaddr);

			if (!smb_set_sge(rdma, page, off, seg))
				return -EIO;

			ret += seg;
			len -= seg;
			kaddr += PAGE_SIZE;
			off = 0;
		} while (len > 0 && rdma->nr_sge < rdma->max_sge);

		if (rdma->nr_sge >= rdma->max_sge || maxsize <= 0)
			break;
		start = 0;
	}

	if (ret > 0)
		iov_iter_advance(iter, ret);
	return ret;
}

/*
 * Extract folio fragments from a FOLIOQ-class iterator and add them to an RDMA
 * list.  The folios are not pinned.
 */
static ssize_t smb_extract_folioq_to_rdma(struct iov_iter *iter,
					  struct smb_extract_to_rdma *rdma,
					  ssize_t maxsize)
{
	const struct folio_queue *folioq = iter->folioq;
	unsigned int slot = iter->folioq_slot;
	ssize_t ret = 0;
	size_t offset = iter->iov_offset;

	BUG_ON(!folioq);

	if (slot >= folioq_nr_slots(folioq)) {
		folioq = folioq->next;
		if (WARN_ON_ONCE(!folioq))
			return -EIO;
		slot = 0;
	}

	do {
		struct folio *folio = folioq_folio(folioq, slot);
		size_t fsize = folioq_folio_size(folioq, slot);

		if (offset < fsize) {
			size_t part = umin(maxsize, fsize - offset);

			if (!smb_set_sge(rdma, folio_page(folio, 0), offset, part))
				return -EIO;

			offset += part;
			ret += part;
			maxsize -= part;
		}

		if (offset >= fsize) {
			offset = 0;
			slot++;
			if (slot >= folioq_nr_slots(folioq)) {
				if (!folioq->next) {
					WARN_ON_ONCE(ret < iter->count);
					break;
				}
				folioq = folioq->next;
				slot = 0;
			}
		}
	} while (rdma->nr_sge < rdma->max_sge && maxsize > 0);

	iter->folioq = folioq;
	iter->folioq_slot = slot;
	iter->iov_offset = offset;
	iter->count -= ret;
	return ret;
}

/*
 * Extract page fragments from up to the given amount of the source iterator
 * and build up an RDMA list that refers to all of those bits.  The RDMA list
 * is appended to, up to the maximum number of elements set in the parameter
 * block.
 *
 * The extracted page fragments are not pinned or ref'd in any way; if an
 * IOVEC/UBUF-type iterator is to be used, it should be converted to a
 * BVEC-type iterator and the pages pinned, ref'd or otherwise held in some
 * way.
 */
static ssize_t smb_extract_iter_to_rdma(struct iov_iter *iter, size_t len,
					struct smb_extract_to_rdma *rdma)
{
	ssize_t ret;
	int before = rdma->nr_sge;

	switch (iov_iter_type(iter)) {
	case ITER_BVEC:
		ret = smb_extract_bvec_to_rdma(iter, rdma, len);
		break;
	case ITER_KVEC:
		ret = smb_extract_kvec_to_rdma(iter, rdma, len);
		break;
	case ITER_FOLIOQ:
		ret = smb_extract_folioq_to_rdma(iter, rdma, len);
		break;
	default:
		WARN_ON_ONCE(1);
		return -EIO;
	}

	if (ret < 0) {
		while (rdma->nr_sge > before) {
			struct ib_sge *sge = &rdma->sge[rdma->nr_sge--];

			ib_dma_unmap_single(rdma->device, sge->addr, sge->length,
					    rdma->direction);
			sge->addr = 0;
		}
	}

	return ret;
}
