// SPDX-License-Identifier: GPL-2.0
#ifndef IOU_KBUF_H
#define IOU_KBUF_H

#include <uapi/linux/io_uring.h>

enum {
	/* ring mapped provided buffers */
	IOBL_BUF_RING	= 1,
	/* ring mapped provided buffers, but mmap'ed by application */
	IOBL_MMAP	= 2,
	/* buffers are consumed incrementally rather than always fully */
	IOBL_INC	= 4,

};

struct io_buffer_list {
	/*
	 * If ->buf_nr_pages is set, then buf_pages/buf_ring are used. If not,
	 * then these are classic provided buffers and ->buf_list is used.
	 */
	union {
		struct list_head buf_list;
		struct {
			struct page **buf_pages;
			struct io_uring_buf_ring *buf_ring;
		};
		struct rcu_head rcu;
	};
	__u16 bgid;

	/* below is for ring provided buffers */
	__u16 buf_nr_pages;
	__u16 nr_entries;
	__u16 head;
	__u16 mask;

	__u16 flags;

	atomic_t refs;

	/*
	 * minimum required amount to be left to reuse an incrementally
	 * consumed buffer. If less than this is left at consumption time,
	 * buffer is done and head is incremented to the next buffer.
	 */
	__u32 min_left_sub_one;
};

struct io_buffer {
	struct list_head list;
	__u64 addr;
	__u32 len;
	__u16 bid;
	__u16 bgid;
};

enum {
	/* can alloc a bigger vec */
	KBUF_MODE_EXPAND	= 1,
	/* if bigger vec allocated, free old one */
	KBUF_MODE_FREE		= 2,
};

struct buf_sel_arg {
	struct iovec *iovs;
	size_t out_len;
	size_t max_len;
	unsigned short nr_iovs;
	unsigned short mode;
	unsigned short partial_map;
};

/*
 * Return value from io_buffer_list selection, to avoid stashing it in
 * struct io_kiocb. For legacy/classic provided buffers, keeping a reference
 * across execution contexts are fine. But for ring provided buffers, the
 * list may go away as soon as ->uring_lock is dropped. As the io_kiocb
 * persists, it's better to just keep the buffer local for those cases.
 */
struct io_br_sel {
	struct io_buffer_list *buf_list;
	/*
	 * Some selection parts return the user address, others return an error.
	 */
	union {
		void __user *addr;
		ssize_t val;
	};
};

struct io_br_sel io_buffer_select(struct io_kiocb *req, size_t *len,
				 unsigned int issue_flags);
int io_buffers_select(struct io_kiocb *req, struct buf_sel_arg *arg,
		      struct io_br_sel *sel, unsigned int issue_flags);
int io_buffers_peek(struct io_kiocb *req, struct buf_sel_arg *arg,
		    struct io_br_sel *sel);
void io_destroy_buffers(struct io_ring_ctx *ctx);

int io_remove_buffers_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_remove_buffers(struct io_kiocb *req, unsigned int issue_flags);

int io_provide_buffers_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_provide_buffers(struct io_kiocb *req, unsigned int issue_flags);

int io_register_pbuf_ring(struct io_ring_ctx *ctx, void __user *arg);
int io_unregister_pbuf_ring(struct io_ring_ctx *ctx, void __user *arg);
int io_register_pbuf_status(struct io_ring_ctx *ctx, void __user *arg);

bool io_kbuf_recycle_legacy(struct io_kiocb *req, unsigned issue_flags);
void io_kbuf_drop_legacy(struct io_kiocb *req);

unsigned int __io_put_kbufs(struct io_kiocb *req, struct io_buffer_list *bl,
			    int len, int nbufs);
bool io_kbuf_commit(struct io_kiocb *req,
		    struct io_buffer_list *bl, int len, int nr);

void io_put_bl(struct io_ring_ctx *ctx, struct io_buffer_list *bl);
struct io_buffer_list *io_pbuf_get_bl(struct io_ring_ctx *ctx,
				      unsigned long bgid);
int io_pbuf_mmap(struct file *file, struct vm_area_struct *vma);

static inline bool io_kbuf_recycle_ring(struct io_kiocb *req,
					struct io_buffer_list *bl)
{
	if (bl) {
		req->buf_index = bl->bgid;
		req->flags &= ~(REQ_F_BUFFER_RING|REQ_F_BUFFERS_COMMIT);
		return true;
	}
	return false;
}

static inline bool io_do_buffer_select(struct io_kiocb *req)
{
	if (!(req->flags & REQ_F_BUFFER_SELECT))
		return false;
	return !(req->flags & (REQ_F_BUFFER_SELECTED|REQ_F_BUFFER_RING));
}

static inline bool io_kbuf_recycle(struct io_kiocb *req, struct io_buffer_list *bl,
				   unsigned issue_flags)
{
	if (req->flags & REQ_F_BL_NO_RECYCLE)
		return false;
	if (req->flags & REQ_F_BUFFER_SELECTED)
		return io_kbuf_recycle_legacy(req, issue_flags);
	if (req->flags & REQ_F_BUFFER_RING)
		return io_kbuf_recycle_ring(req, bl);
	return false;
}

/* Mapped buffer ring, return io_uring_buf from head */
#define io_ring_head_to_buf(br, head, mask)	&(br)->bufs[(head) & (mask)]

static inline unsigned int io_put_kbuf(struct io_kiocb *req, int len,
				       struct io_buffer_list *bl)
{
	if (!(req->flags & (REQ_F_BUFFER_RING | REQ_F_BUFFER_SELECTED)))
		return 0;
	return __io_put_kbufs(req, bl, len, 1);
}

static inline unsigned int io_put_kbufs(struct io_kiocb *req, int len,
					struct io_buffer_list *bl, int nbufs)
{
	if (!(req->flags & (REQ_F_BUFFER_RING | REQ_F_BUFFER_SELECTED)))
		return 0;
	return __io_put_kbufs(req, bl, len, nbufs);
}
#endif
