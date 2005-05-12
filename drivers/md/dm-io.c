/*
 * Copyright (C) 2003 Sistina Software
 *
 * This file is released under the GPL.
 */

#include "dm-io.h"

#include <linux/bio.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>

#define BIO_POOL_SIZE 256


/*-----------------------------------------------------------------
 * Bio set, move this to bio.c
 *---------------------------------------------------------------*/
#define BV_NAME_SIZE 16
struct biovec_pool {
	int nr_vecs;
	char name[BV_NAME_SIZE];
	kmem_cache_t *slab;
	mempool_t *pool;
	atomic_t allocated;	/* FIXME: debug */
};

#define BIOVEC_NR_POOLS 6
struct bio_set {
	char name[BV_NAME_SIZE];
	kmem_cache_t *bio_slab;
	mempool_t *bio_pool;
	struct biovec_pool pools[BIOVEC_NR_POOLS];
};

static void bio_set_exit(struct bio_set *bs)
{
	unsigned i;
	struct biovec_pool *bp;

	if (bs->bio_pool)
		mempool_destroy(bs->bio_pool);

	if (bs->bio_slab)
		kmem_cache_destroy(bs->bio_slab);

	for (i = 0; i < BIOVEC_NR_POOLS; i++) {
		bp = bs->pools + i;
		if (bp->pool)
			mempool_destroy(bp->pool);

		if (bp->slab)
			kmem_cache_destroy(bp->slab);
	}
}

static void mk_name(char *str, size_t len, const char *prefix, unsigned count)
{
	snprintf(str, len, "%s-%u", prefix, count);
}

static int bio_set_init(struct bio_set *bs, const char *slab_prefix,
			 unsigned pool_entries, unsigned scale)
{
	/* FIXME: this must match bvec_index(), why not go the
	 * whole hog and have a pool per power of 2 ? */
	static unsigned _vec_lengths[BIOVEC_NR_POOLS] = {
		1, 4, 16, 64, 128, BIO_MAX_PAGES
	};


	unsigned i, size;
	struct biovec_pool *bp;

	/* zero the bs so we can tear down properly on error */
	memset(bs, 0, sizeof(*bs));

	/*
	 * Set up the bio pool.
	 */
	snprintf(bs->name, sizeof(bs->name), "%s-bio", slab_prefix);

	bs->bio_slab = kmem_cache_create(bs->name, sizeof(struct bio), 0,
					 SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (!bs->bio_slab) {
		DMWARN("can't init bio slab");
		goto bad;
	}

	bs->bio_pool = mempool_create(pool_entries, mempool_alloc_slab,
				      mempool_free_slab, bs->bio_slab);
	if (!bs->bio_pool) {
		DMWARN("can't init bio pool");
		goto bad;
	}

	/*
	 * Set up the biovec pools.
	 */
	for (i = 0; i < BIOVEC_NR_POOLS; i++) {
		bp = bs->pools + i;
		bp->nr_vecs = _vec_lengths[i];
		atomic_set(&bp->allocated, 1); /* FIXME: debug */


		size = bp->nr_vecs * sizeof(struct bio_vec);

		mk_name(bp->name, sizeof(bp->name), slab_prefix, i);
		bp->slab = kmem_cache_create(bp->name, size, 0,
					     SLAB_HWCACHE_ALIGN, NULL, NULL);
		if (!bp->slab) {
			DMWARN("can't init biovec slab cache");
			goto bad;
		}

		if (i >= scale)
			pool_entries >>= 1;

		bp->pool = mempool_create(pool_entries, mempool_alloc_slab,
					  mempool_free_slab, bp->slab);
		if (!bp->pool) {
			DMWARN("can't init biovec mempool");
			goto bad;
		}
	}

	return 0;

 bad:
	bio_set_exit(bs);
	return -ENOMEM;
}

/* FIXME: blech */
static inline unsigned bvec_index(unsigned nr)
{
	switch (nr) {
	case 1:		return 0;
	case 2 ... 4: 	return 1;
	case 5 ... 16:	return 2;
	case 17 ... 64:	return 3;
	case 65 ... 128:return 4;
	case 129 ... BIO_MAX_PAGES: return 5;
	}

	BUG();
	return 0;
}

static unsigned _bio_count = 0;
struct bio *bio_set_alloc(struct bio_set *bs, int gfp_mask, int nr_iovecs)
{
	struct biovec_pool *bp;
	struct bio_vec *bv = NULL;
	unsigned long idx;
	struct bio *bio;

	bio = mempool_alloc(bs->bio_pool, gfp_mask);
	if (unlikely(!bio))
		return NULL;

	bio_init(bio);

	if (likely(nr_iovecs)) {
		idx = bvec_index(nr_iovecs);
		bp = bs->pools + idx;
		bv = mempool_alloc(bp->pool, gfp_mask);
		if (!bv) {
			mempool_free(bio, bs->bio_pool);
			return NULL;
		}

		memset(bv, 0, bp->nr_vecs * sizeof(*bv));
		bio->bi_flags |= idx << BIO_POOL_OFFSET;
		bio->bi_max_vecs = bp->nr_vecs;
		atomic_inc(&bp->allocated);
	}

	bio->bi_io_vec = bv;
	return bio;
}

static void bio_set_free(struct bio_set *bs, struct bio *bio)
{
	struct biovec_pool *bp = bs->pools + BIO_POOL_IDX(bio);

	if (atomic_dec_and_test(&bp->allocated))
		BUG();

	mempool_free(bio->bi_io_vec, bp->pool);
	mempool_free(bio, bs->bio_pool);
}

/*-----------------------------------------------------------------
 * dm-io proper
 *---------------------------------------------------------------*/
static struct bio_set _bios;

/* FIXME: can we shrink this ? */
struct io {
	unsigned long error;
	atomic_t count;
	struct task_struct *sleeper;
	io_notify_fn callback;
	void *context;
};

/*
 * io contexts are only dynamically allocated for asynchronous
 * io.  Since async io is likely to be the majority of io we'll
 * have the same number of io contexts as buffer heads ! (FIXME:
 * must reduce this).
 */
static unsigned _num_ios;
static mempool_t *_io_pool;

static void *alloc_io(int gfp_mask, void *pool_data)
{
	return kmalloc(sizeof(struct io), gfp_mask);
}

static void free_io(void *element, void *pool_data)
{
	kfree(element);
}

static unsigned int pages_to_ios(unsigned int pages)
{
	return 4 * pages;	/* too many ? */
}

static int resize_pool(unsigned int new_ios)
{
	int r = 0;

	if (_io_pool) {
		if (new_ios == 0) {
			/* free off the pool */
			mempool_destroy(_io_pool);
			_io_pool = NULL;
			bio_set_exit(&_bios);

		} else {
			/* resize the pool */
			r = mempool_resize(_io_pool, new_ios, GFP_KERNEL);
		}

	} else {
		/* create new pool */
		_io_pool = mempool_create(new_ios, alloc_io, free_io, NULL);
		if (!_io_pool)
			return -ENOMEM;

		r = bio_set_init(&_bios, "dm-io", 512, 1);
		if (r) {
			mempool_destroy(_io_pool);
			_io_pool = NULL;
		}
	}

	if (!r)
		_num_ios = new_ios;

	return r;
}

int dm_io_get(unsigned int num_pages)
{
	return resize_pool(_num_ios + pages_to_ios(num_pages));
}

void dm_io_put(unsigned int num_pages)
{
	resize_pool(_num_ios - pages_to_ios(num_pages));
}

/*-----------------------------------------------------------------
 * We need to keep track of which region a bio is doing io for.
 * In order to save a memory allocation we store this the last
 * bvec which we know is unused (blech).
 *---------------------------------------------------------------*/
static inline void bio_set_region(struct bio *bio, unsigned region)
{
	bio->bi_io_vec[bio->bi_max_vecs - 1].bv_len = region;
}

static inline unsigned bio_get_region(struct bio *bio)
{
	return bio->bi_io_vec[bio->bi_max_vecs - 1].bv_len;
}

/*-----------------------------------------------------------------
 * We need an io object to keep track of the number of bios that
 * have been dispatched for a particular io.
 *---------------------------------------------------------------*/
static void dec_count(struct io *io, unsigned int region, int error)
{
	if (error)
		set_bit(region, &io->error);

	if (atomic_dec_and_test(&io->count)) {
		if (io->sleeper)
			wake_up_process(io->sleeper);

		else {
			int r = io->error;
			io_notify_fn fn = io->callback;
			void *context = io->context;

			mempool_free(io, _io_pool);
			fn(r, context);
		}
	}
}

/* FIXME Move this to bio.h? */
static void zero_fill_bio(struct bio *bio)
{
	unsigned long flags;
	struct bio_vec *bv;
	int i;

	bio_for_each_segment(bv, bio, i) {
		char *data = bvec_kmap_irq(bv, &flags);
		memset(data, 0, bv->bv_len);
		flush_dcache_page(bv->bv_page);
		bvec_kunmap_irq(data, &flags);
	}
}

static int endio(struct bio *bio, unsigned int done, int error)
{
	struct io *io = (struct io *) bio->bi_private;

	/* keep going until we've finished */
	if (bio->bi_size)
		return 1;

	if (error && bio_data_dir(bio) == READ)
		zero_fill_bio(bio);

	dec_count(io, bio_get_region(bio), error);
	bio_put(bio);

	return 0;
}

static void bio_dtr(struct bio *bio)
{
	_bio_count--;
	bio_set_free(&_bios, bio);
}

/*-----------------------------------------------------------------
 * These little objects provide an abstraction for getting a new
 * destination page for io.
 *---------------------------------------------------------------*/
struct dpages {
	void (*get_page)(struct dpages *dp,
			 struct page **p, unsigned long *len, unsigned *offset);
	void (*next_page)(struct dpages *dp);

	unsigned context_u;
	void *context_ptr;
};

/*
 * Functions for getting the pages from a list.
 */
static void list_get_page(struct dpages *dp,
		  struct page **p, unsigned long *len, unsigned *offset)
{
	unsigned o = dp->context_u;
	struct page_list *pl = (struct page_list *) dp->context_ptr;

	*p = pl->page;
	*len = PAGE_SIZE - o;
	*offset = o;
}

static void list_next_page(struct dpages *dp)
{
	struct page_list *pl = (struct page_list *) dp->context_ptr;
	dp->context_ptr = pl->next;
	dp->context_u = 0;
}

static void list_dp_init(struct dpages *dp, struct page_list *pl, unsigned offset)
{
	dp->get_page = list_get_page;
	dp->next_page = list_next_page;
	dp->context_u = offset;
	dp->context_ptr = pl;
}

/*
 * Functions for getting the pages from a bvec.
 */
static void bvec_get_page(struct dpages *dp,
		  struct page **p, unsigned long *len, unsigned *offset)
{
	struct bio_vec *bvec = (struct bio_vec *) dp->context_ptr;
	*p = bvec->bv_page;
	*len = bvec->bv_len;
	*offset = bvec->bv_offset;
}

static void bvec_next_page(struct dpages *dp)
{
	struct bio_vec *bvec = (struct bio_vec *) dp->context_ptr;
	dp->context_ptr = bvec + 1;
}

static void bvec_dp_init(struct dpages *dp, struct bio_vec *bvec)
{
	dp->get_page = bvec_get_page;
	dp->next_page = bvec_next_page;
	dp->context_ptr = bvec;
}

static void vm_get_page(struct dpages *dp,
		 struct page **p, unsigned long *len, unsigned *offset)
{
	*p = vmalloc_to_page(dp->context_ptr);
	*offset = dp->context_u;
	*len = PAGE_SIZE - dp->context_u;
}

static void vm_next_page(struct dpages *dp)
{
	dp->context_ptr += PAGE_SIZE - dp->context_u;
	dp->context_u = 0;
}

static void vm_dp_init(struct dpages *dp, void *data)
{
	dp->get_page = vm_get_page;
	dp->next_page = vm_next_page;
	dp->context_u = ((unsigned long) data) & (PAGE_SIZE - 1);
	dp->context_ptr = data;
}

/*-----------------------------------------------------------------
 * IO routines that accept a list of pages.
 *---------------------------------------------------------------*/
static void do_region(int rw, unsigned int region, struct io_region *where,
		      struct dpages *dp, struct io *io)
{
	struct bio *bio;
	struct page *page;
	unsigned long len;
	unsigned offset;
	unsigned num_bvecs;
	sector_t remaining = where->count;

	while (remaining) {
		/*
		 * Allocate a suitably sized bio, we add an extra
		 * bvec for bio_get/set_region().
		 */
		num_bvecs = (remaining / (PAGE_SIZE >> 9)) + 2;
		_bio_count++;
		bio = bio_set_alloc(&_bios, GFP_NOIO, num_bvecs);
		bio->bi_sector = where->sector + (where->count - remaining);
		bio->bi_bdev = where->bdev;
		bio->bi_end_io = endio;
		bio->bi_private = io;
		bio->bi_destructor = bio_dtr;
		bio_set_region(bio, region);

		/*
		 * Try and add as many pages as possible.
		 */
		while (remaining) {
			dp->get_page(dp, &page, &len, &offset);
			len = min(len, to_bytes(remaining));
			if (!bio_add_page(bio, page, len, offset))
				break;

			offset = 0;
			remaining -= to_sector(len);
			dp->next_page(dp);
		}

		atomic_inc(&io->count);
		submit_bio(rw, bio);
	}
}

static void dispatch_io(int rw, unsigned int num_regions,
			struct io_region *where, struct dpages *dp,
			struct io *io, int sync)
{
	int i;
	struct dpages old_pages = *dp;

	if (sync)
		rw |= (1 << BIO_RW_SYNC);

	/*
	 * For multiple regions we need to be careful to rewind
	 * the dp object for each call to do_region.
	 */
	for (i = 0; i < num_regions; i++) {
		*dp = old_pages;
		if (where[i].count)
			do_region(rw, i, where + i, dp, io);
	}

	/*
	 * Drop the extra refence that we were holding to avoid
	 * the io being completed too early.
	 */
	dec_count(io, 0, 0);
}

static int sync_io(unsigned int num_regions, struct io_region *where,
	    int rw, struct dpages *dp, unsigned long *error_bits)
{
	struct io io;

	if (num_regions > 1 && rw != WRITE) {
		WARN_ON(1);
		return -EIO;
	}

	io.error = 0;
	atomic_set(&io.count, 1); /* see dispatch_io() */
	io.sleeper = current;

	dispatch_io(rw, num_regions, where, dp, &io, 1);

	while (1) {
		set_current_state(TASK_UNINTERRUPTIBLE);

		if (!atomic_read(&io.count) || signal_pending(current))
			break;

		io_schedule();
	}
	set_current_state(TASK_RUNNING);

	if (atomic_read(&io.count))
		return -EINTR;

	*error_bits = io.error;
	return io.error ? -EIO : 0;
}

static int async_io(unsigned int num_regions, struct io_region *where, int rw,
	     struct dpages *dp, io_notify_fn fn, void *context)
{
	struct io *io;

	if (num_regions > 1 && rw != WRITE) {
		WARN_ON(1);
		fn(1, context);
		return -EIO;
	}

	io = mempool_alloc(_io_pool, GFP_NOIO);
	io->error = 0;
	atomic_set(&io->count, 1); /* see dispatch_io() */
	io->sleeper = NULL;
	io->callback = fn;
	io->context = context;

	dispatch_io(rw, num_regions, where, dp, io, 0);
	return 0;
}

int dm_io_sync(unsigned int num_regions, struct io_region *where, int rw,
	       struct page_list *pl, unsigned int offset,
	       unsigned long *error_bits)
{
	struct dpages dp;
	list_dp_init(&dp, pl, offset);
	return sync_io(num_regions, where, rw, &dp, error_bits);
}

int dm_io_sync_bvec(unsigned int num_regions, struct io_region *where, int rw,
		    struct bio_vec *bvec, unsigned long *error_bits)
{
	struct dpages dp;
	bvec_dp_init(&dp, bvec);
	return sync_io(num_regions, where, rw, &dp, error_bits);
}

int dm_io_sync_vm(unsigned int num_regions, struct io_region *where, int rw,
		  void *data, unsigned long *error_bits)
{
	struct dpages dp;
	vm_dp_init(&dp, data);
	return sync_io(num_regions, where, rw, &dp, error_bits);
}

int dm_io_async(unsigned int num_regions, struct io_region *where, int rw,
		struct page_list *pl, unsigned int offset,
		io_notify_fn fn, void *context)
{
	struct dpages dp;
	list_dp_init(&dp, pl, offset);
	return async_io(num_regions, where, rw, &dp, fn, context);
}

int dm_io_async_bvec(unsigned int num_regions, struct io_region *where, int rw,
		     struct bio_vec *bvec, io_notify_fn fn, void *context)
{
	struct dpages dp;
	bvec_dp_init(&dp, bvec);
	return async_io(num_regions, where, rw, &dp, fn, context);
}

int dm_io_async_vm(unsigned int num_regions, struct io_region *where, int rw,
		   void *data, io_notify_fn fn, void *context)
{
	struct dpages dp;
	vm_dp_init(&dp, data);
	return async_io(num_regions, where, rw, &dp, fn, context);
}

EXPORT_SYMBOL(dm_io_get);
EXPORT_SYMBOL(dm_io_put);
EXPORT_SYMBOL(dm_io_sync);
EXPORT_SYMBOL(dm_io_async);
EXPORT_SYMBOL(dm_io_sync_bvec);
EXPORT_SYMBOL(dm_io_async_bvec);
EXPORT_SYMBOL(dm_io_sync_vm);
EXPORT_SYMBOL(dm_io_async_vm);
