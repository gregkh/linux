// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * zsmalloc memory allocator
 *
 * Copyright (C) 2011  Nitin Gupta
 * Copyright (C) 2012, 2013 Minchan Kim
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the license that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/*
 * lock ordering:
 *	page_lock
 *	pool->lock
 *	class->lock
 *	zspage->lock
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/highmem.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sprintf.h>
#include <linux/shrinker.h>
#include <linux/types.h>
#include <linux/debugfs.h>
#include <linux/zsmalloc.h>
#include <linux/zpool.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include "zpdesc.h"

#define ZSPAGE_MAGIC	0x58

/*
 * This must be power of 2 and greater than or equal to sizeof(link_free).
 * These two conditions ensure that any 'struct link_free' itself doesn't
 * span more than 1 page which avoids complex case of mapping 2 pages simply
 * to restore link_free pointer values.
 */
#define ZS_ALIGN		8

#define ZS_HANDLE_SIZE (sizeof(unsigned long))

/*
 * Object location (<PFN>, <obj_idx>) is encoded as
 * a single (unsigned long) handle value.
 *
 * Note that object index <obj_idx> starts from 0.
 *
 * This is made more complicated by various memory models and PAE.
 */

#ifndef MAX_POSSIBLE_PHYSMEM_BITS
#ifdef MAX_PHYSMEM_BITS
#define MAX_POSSIBLE_PHYSMEM_BITS MAX_PHYSMEM_BITS
#else
/*
 * If this definition of MAX_PHYSMEM_BITS is used, OBJ_INDEX_BITS will just
 * be PAGE_SHIFT
 */
#define MAX_POSSIBLE_PHYSMEM_BITS BITS_PER_LONG
#endif
#endif

#define _PFN_BITS		(MAX_POSSIBLE_PHYSMEM_BITS - PAGE_SHIFT)

/*
 * Head in allocated object should have OBJ_ALLOCATED_TAG
 * to identify the object was allocated or not.
 * It's okay to add the status bit in the least bit because
 * header keeps handle which is 4byte-aligned address so we
 * have room for two bit at least.
 */
#define OBJ_ALLOCATED_TAG 1

#define OBJ_TAG_BITS	1
#define OBJ_TAG_MASK	OBJ_ALLOCATED_TAG

#define OBJ_INDEX_BITS	(BITS_PER_LONG - _PFN_BITS)
#define OBJ_INDEX_MASK	((_AC(1, UL) << OBJ_INDEX_BITS) - 1)

#define HUGE_BITS	1
#define FULLNESS_BITS	4
#define CLASS_BITS	8
#define MAGIC_VAL_BITS	8

#define ZS_MAX_PAGES_PER_ZSPAGE	(_AC(CONFIG_ZSMALLOC_CHAIN_SIZE, UL))

/* ZS_MIN_ALLOC_SIZE must be multiple of ZS_ALIGN */
#define ZS_MIN_ALLOC_SIZE \
	MAX(32, (ZS_MAX_PAGES_PER_ZSPAGE << PAGE_SHIFT >> OBJ_INDEX_BITS))
/* each chunk includes extra space to keep handle */
#define ZS_MAX_ALLOC_SIZE	PAGE_SIZE

/*
 * On systems with 4K page size, this gives 255 size classes! There is a
 * trader-off here:
 *  - Large number of size classes is potentially wasteful as free page are
 *    spread across these classes
 *  - Small number of size classes causes large internal fragmentation
 *  - Probably its better to use specific size classes (empirically
 *    determined). NOTE: all those class sizes must be set as multiple of
 *    ZS_ALIGN to make sure link_free itself never has to span 2 pages.
 *
 *  ZS_MIN_ALLOC_SIZE and ZS_SIZE_CLASS_DELTA must be multiple of ZS_ALIGN
 *  (reason above)
 */
#define ZS_SIZE_CLASS_DELTA	(PAGE_SIZE >> CLASS_BITS)
#define ZS_SIZE_CLASSES	(DIV_ROUND_UP(ZS_MAX_ALLOC_SIZE - ZS_MIN_ALLOC_SIZE, \
				      ZS_SIZE_CLASS_DELTA) + 1)

/*
 * Pages are distinguished by the ratio of used memory (that is the ratio
 * of ->inuse objects to all objects that page can store). For example,
 * INUSE_RATIO_10 means that the ratio of used objects is > 0% and <= 10%.
 *
 * The number of fullness groups is not random. It allows us to keep
 * difference between the least busy page in the group (minimum permitted
 * number of ->inuse objects) and the most busy page (maximum permitted
 * number of ->inuse objects) at a reasonable value.
 */
enum fullness_group {
	ZS_INUSE_RATIO_0,
	ZS_INUSE_RATIO_10,
	/* NOTE: 8 more fullness groups here */
	ZS_INUSE_RATIO_99       = 10,
	ZS_INUSE_RATIO_100,
	NR_FULLNESS_GROUPS,
};

enum class_stat_type {
	/* NOTE: stats for 12 fullness groups here: from inuse 0 to 100 */
	ZS_OBJS_ALLOCATED       = NR_FULLNESS_GROUPS,
	ZS_OBJS_INUSE,
	NR_CLASS_STAT_TYPES,
};

struct zs_size_stat {
	unsigned long objs[NR_CLASS_STAT_TYPES];
};

#ifdef CONFIG_ZSMALLOC_STAT
static struct dentry *zs_stat_root;
#endif

static size_t huge_class_size;

struct size_class {
	spinlock_t lock;
	struct list_head fullness_list[NR_FULLNESS_GROUPS];
	/*
	 * Size of objects stored in this class. Must be multiple
	 * of ZS_ALIGN.
	 */
	int size;
	int objs_per_zspage;
	/* Number of PAGE_SIZE sized pages to combine to form a 'zspage' */
	int pages_per_zspage;

	unsigned int index;
	struct zs_size_stat stats;
};

/*
 * Placed within free objects to form a singly linked list.
 * For every zspage, zspage->freeobj gives head of this list.
 *
 * This must be power of 2 and less than or equal to ZS_ALIGN
 */
struct link_free {
	union {
		/*
		 * Free object index;
		 * It's valid for non-allocated object
		 */
		unsigned long next;
		/*
		 * Handle of allocated object.
		 */
		unsigned long handle;
	};
};

struct zs_pool {
	const char *name;

	struct size_class *size_class[ZS_SIZE_CLASSES];
	struct kmem_cache *handle_cachep;
	struct kmem_cache *zspage_cachep;

	atomic_long_t pages_allocated;

	struct zs_pool_stats stats;

	/* Compact classes */
	struct shrinker *shrinker;

#ifdef CONFIG_ZSMALLOC_STAT
	struct dentry *stat_dentry;
#endif
#ifdef CONFIG_COMPACTION
	struct work_struct free_work;
#endif
	/* protect zspage migration/compaction */
	rwlock_t lock;
	atomic_t compaction_in_progress;
};

static inline void zpdesc_set_first(struct zpdesc *zpdesc)
{
	SetPagePrivate(zpdesc_page(zpdesc));
}

static inline void zpdesc_inc_zone_page_state(struct zpdesc *zpdesc)
{
	inc_zone_page_state(zpdesc_page(zpdesc), NR_ZSPAGES);
}

static inline void zpdesc_dec_zone_page_state(struct zpdesc *zpdesc)
{
	dec_zone_page_state(zpdesc_page(zpdesc), NR_ZSPAGES);
}

static inline struct zpdesc *alloc_zpdesc(gfp_t gfp, const int nid)
{
	struct page *page = alloc_pages_node(nid, gfp, 0);

	return page_zpdesc(page);
}

static inline void free_zpdesc(struct zpdesc *zpdesc)
{
	struct page *page = zpdesc_page(zpdesc);

	/* PageZsmalloc is sticky until the page is freed to the buddy. */
	__free_page(page);
}

#define ZS_PAGE_UNLOCKED	0
#define ZS_PAGE_WRLOCKED	-1

struct zspage_lock {
	spinlock_t lock;
	int cnt;
	struct lockdep_map dep_map;
};

struct zspage {
	struct {
		unsigned int huge:HUGE_BITS;
		unsigned int fullness:FULLNESS_BITS;
		unsigned int class:CLASS_BITS + 1;
		unsigned int magic:MAGIC_VAL_BITS;
	};
	unsigned int inuse;
	unsigned int freeobj;
	struct zpdesc *first_zpdesc;
	struct list_head list; /* fullness list */
	struct zs_pool *pool;
	struct zspage_lock zsl;
};

static void zspage_lock_init(struct zspage *zspage)
{
	static struct lock_class_key __key;
	struct zspage_lock *zsl = &zspage->zsl;

	lockdep_init_map(&zsl->dep_map, "zspage->lock", &__key, 0);
	spin_lock_init(&zsl->lock);
	zsl->cnt = ZS_PAGE_UNLOCKED;
}

/*
 * The zspage lock can be held from atomic contexts, but it needs to remain
 * preemptible when held for reading because it remains held outside of those
 * atomic contexts, otherwise we unnecessarily lose preemptibility.
 *
 * To achieve this, the following rules are enforced on readers and writers:
 *
 * - Writers are blocked by both writers and readers, while readers are only
 *   blocked by writers (i.e. normal rwlock semantics).
 *
 * - Writers are always atomic (to allow readers to spin waiting for them).
 *
 * - Writers always use trylock (as the lock may be held be sleeping readers).
 *
 * - Readers may spin on the lock (as they can only wait for atomic writers).
 *
 * - Readers may sleep while holding the lock (as writes only use trylock).
 */
static void zspage_read_lock(struct zspage *zspage)
{
	struct zspage_lock *zsl = &zspage->zsl;

	rwsem_acquire_read(&zsl->dep_map, 0, 0, _RET_IP_);

	spin_lock(&zsl->lock);
	zsl->cnt++;
	spin_unlock(&zsl->lock);

	lock_acquired(&zsl->dep_map, _RET_IP_);
}

static void zspage_read_unlock(struct zspage *zspage)
{
	struct zspage_lock *zsl = &zspage->zsl;

	rwsem_release(&zsl->dep_map, _RET_IP_);

	spin_lock(&zsl->lock);
	zsl->cnt--;
	spin_unlock(&zsl->lock);
}

static __must_check bool zspage_write_trylock(struct zspage *zspage)
{
	struct zspage_lock *zsl = &zspage->zsl;

	spin_lock(&zsl->lock);
	if (zsl->cnt == ZS_PAGE_UNLOCKED) {
		zsl->cnt = ZS_PAGE_WRLOCKED;
		rwsem_acquire(&zsl->dep_map, 0, 1, _RET_IP_);
		lock_acquired(&zsl->dep_map, _RET_IP_);
		return true;
	}

	spin_unlock(&zsl->lock);
	return false;
}

static void zspage_write_unlock(struct zspage *zspage)
{
	struct zspage_lock *zsl = &zspage->zsl;

	rwsem_release(&zsl->dep_map, _RET_IP_);

	zsl->cnt = ZS_PAGE_UNLOCKED;
	spin_unlock(&zsl->lock);
}

/* huge object: pages_per_zspage == 1 && maxobj_per_zspage == 1 */
static void SetZsHugePage(struct zspage *zspage)
{
	zspage->huge = 1;
}

static bool ZsHugePage(struct zspage *zspage)
{
	return zspage->huge;
}

#ifdef CONFIG_COMPACTION
static void kick_deferred_free(struct zs_pool *pool);
static void init_deferred_free(struct zs_pool *pool);
static void SetZsPageMovable(struct zs_pool *pool, struct zspage *zspage);
#else
static void kick_deferred_free(struct zs_pool *pool) {}
static void init_deferred_free(struct zs_pool *pool) {}
static void SetZsPageMovable(struct zs_pool *pool, struct zspage *zspage) {}
#endif

static int create_cache(struct zs_pool *pool)
{
	char *name;

	name = kasprintf(GFP_KERNEL, "zs_handle-%s", pool->name);
	if (!name)
		return -ENOMEM;
	pool->handle_cachep = kmem_cache_create(name, ZS_HANDLE_SIZE,
						0, 0, NULL);
	kfree(name);
	if (!pool->handle_cachep)
		return -EINVAL;

	name = kasprintf(GFP_KERNEL, "zspage-%s", pool->name);
	if (!name)
		return -ENOMEM;
	pool->zspage_cachep = kmem_cache_create(name, sizeof(struct zspage),
						0, 0, NULL);
	kfree(name);
	if (!pool->zspage_cachep) {
		kmem_cache_destroy(pool->handle_cachep);
		pool->handle_cachep = NULL;
		return -EINVAL;
	}

	return 0;
}

static void destroy_cache(struct zs_pool *pool)
{
	kmem_cache_destroy(pool->handle_cachep);
	kmem_cache_destroy(pool->zspage_cachep);
}

static unsigned long cache_alloc_handle(struct zs_pool *pool, gfp_t gfp)
{
	return (unsigned long)kmem_cache_alloc(pool->handle_cachep,
			gfp & ~(__GFP_HIGHMEM|__GFP_MOVABLE));
}

static void cache_free_handle(struct zs_pool *pool, unsigned long handle)
{
	kmem_cache_free(pool->handle_cachep, (void *)handle);
}

static struct zspage *cache_alloc_zspage(struct zs_pool *pool, gfp_t flags)
{
	return kmem_cache_zalloc(pool->zspage_cachep,
			flags & ~(__GFP_HIGHMEM|__GFP_MOVABLE));
}

static void cache_free_zspage(struct zs_pool *pool, struct zspage *zspage)
{
	kmem_cache_free(pool->zspage_cachep, zspage);
}

/* class->lock(which owns the handle) synchronizes races */
static void record_obj(unsigned long handle, unsigned long obj)
{
	*(unsigned long *)handle = obj;
}

/* zpool driver */

#ifdef CONFIG_ZPOOL

static void *zs_zpool_create(const char *name, gfp_t gfp)
{
	/*
	 * Ignore global gfp flags: zs_malloc() may be invoked from
	 * different contexts and its caller must provide a valid
	 * gfp mask.
	 */
	return zs_create_pool(name);
}

static void zs_zpool_destroy(void *pool)
{
	zs_destroy_pool(pool);
}

static int zs_zpool_malloc(void *pool, size_t size, gfp_t gfp,
			   unsigned long *handle, const int nid)
{
	*handle = zs_malloc(pool, size, gfp, nid);

	if (IS_ERR_VALUE(*handle))
		return PTR_ERR((void *)*handle);
	return 0;
}
static void zs_zpool_free(void *pool, unsigned long handle)
{
	zs_free(pool, handle);
}

static void *zs_zpool_obj_read_begin(void *pool, unsigned long handle,
				     void *local_copy)
{
	return zs_obj_read_begin(pool, handle, local_copy);
}

static void zs_zpool_obj_read_end(void *pool, unsigned long handle,
				  void *handle_mem)
{
	zs_obj_read_end(pool, handle, handle_mem);
}

static void zs_zpool_obj_write(void *pool, unsigned long handle,
			       void *handle_mem, size_t mem_len)
{
	zs_obj_write(pool, handle, handle_mem, mem_len);
}

static u64 zs_zpool_total_pages(void *pool)
{
	return zs_get_total_pages(pool);
}

static struct zpool_driver zs_zpool_driver = {
	.type =			  "zsmalloc",
	.owner =		  THIS_MODULE,
	.create =		  zs_zpool_create,
	.destroy =		  zs_zpool_destroy,
	.malloc =		  zs_zpool_malloc,
	.free =			  zs_zpool_free,
	.obj_read_begin =	  zs_zpool_obj_read_begin,
	.obj_read_end  =	  zs_zpool_obj_read_end,
	.obj_write =		  zs_zpool_obj_write,
	.total_pages =		  zs_zpool_total_pages,
};

MODULE_ALIAS("zpool-zsmalloc");
#endif /* CONFIG_ZPOOL */

static inline bool __maybe_unused is_first_zpdesc(struct zpdesc *zpdesc)
{
	return PagePrivate(zpdesc_page(zpdesc));
}

/* Protected by class->lock */
static inline int get_zspage_inuse(struct zspage *zspage)
{
	return zspage->inuse;
}

static inline void mod_zspage_inuse(struct zspage *zspage, int val)
{
	zspage->inuse += val;
}

static struct zpdesc *get_first_zpdesc(struct zspage *zspage)
{
	struct zpdesc *first_zpdesc = zspage->first_zpdesc;

	VM_BUG_ON_PAGE(!is_first_zpdesc(first_zpdesc), zpdesc_page(first_zpdesc));
	return first_zpdesc;
}

#define FIRST_OBJ_PAGE_TYPE_MASK	0xffffff

static inline unsigned int get_first_obj_offset(struct zpdesc *zpdesc)
{
	VM_WARN_ON_ONCE(!PageZsmalloc(zpdesc_page(zpdesc)));
	return zpdesc->first_obj_offset & FIRST_OBJ_PAGE_TYPE_MASK;
}

static inline void set_first_obj_offset(struct zpdesc *zpdesc, unsigned int offset)
{
	/* With 24 bits available, we can support offsets into 16 MiB pages. */
	BUILD_BUG_ON(PAGE_SIZE > SZ_16M);
	VM_WARN_ON_ONCE(!PageZsmalloc(zpdesc_page(zpdesc)));
	VM_WARN_ON_ONCE(offset & ~FIRST_OBJ_PAGE_TYPE_MASK);
	zpdesc->first_obj_offset &= ~FIRST_OBJ_PAGE_TYPE_MASK;
	zpdesc->first_obj_offset |= offset & FIRST_OBJ_PAGE_TYPE_MASK;
}

static inline unsigned int get_freeobj(struct zspage *zspage)
{
	return zspage->freeobj;
}

static inline void set_freeobj(struct zspage *zspage, unsigned int obj)
{
	zspage->freeobj = obj;
}

static struct size_class *zspage_class(struct zs_pool *pool,
				       struct zspage *zspage)
{
	return pool->size_class[zspage->class];
}

/*
 * zsmalloc divides the pool into various size classes where each
 * class maintains a list of zspages where each zspage is divided
 * into equal sized chunks. Each allocation falls into one of these
 * classes depending on its size. This function returns index of the
 * size class which has chunk size big enough to hold the given size.
 */
static int get_size_class_index(int size)
{
	int idx = 0;

	if (likely(size > ZS_MIN_ALLOC_SIZE))
		idx = DIV_ROUND_UP(size - ZS_MIN_ALLOC_SIZE,
				ZS_SIZE_CLASS_DELTA);

	return min_t(int, ZS_SIZE_CLASSES - 1, idx);
}

static inline void class_stat_add(struct size_class *class, int type,
				  unsigned long cnt)
{
	class->stats.objs[type] += cnt;
}

static inline void class_stat_sub(struct size_class *class, int type,
				  unsigned long cnt)
{
	class->stats.objs[type] -= cnt;
}

static inline unsigned long class_stat_read(struct size_class *class, int type)
{
	return class->stats.objs[type];
}

#ifdef CONFIG_ZSMALLOC_STAT

static void __init zs_stat_init(void)
{
	if (!debugfs_initialized()) {
		pr_warn("debugfs not available, stat dir not created\n");
		return;
	}

	zs_stat_root = debugfs_create_dir("zsmalloc", NULL);
}

static void __exit zs_stat_exit(void)
{
	debugfs_remove_recursive(zs_stat_root);
}

static unsigned long zs_can_compact(struct size_class *class);

static int zs_stats_size_show(struct seq_file *s, void *v)
{
	int i, fg;
	struct zs_pool *pool = s->private;
	struct size_class *class;
	int objs_per_zspage;
	unsigned long obj_allocated, obj_used, pages_used, freeable;
	unsigned long total_objs = 0, total_used_objs = 0, total_pages = 0;
	unsigned long total_freeable = 0;
	unsigned long inuse_totals[NR_FULLNESS_GROUPS] = {0, };

	seq_printf(s, " %5s %5s %9s %9s %9s %9s %9s %9s %9s %9s %9s %9s %9s %13s %10s %10s %16s %8s\n",
			"class", "size", "10%", "20%", "30%", "40%",
			"50%", "60%", "70%", "80%", "90%", "99%", "100%",
			"obj_allocated", "obj_used", "pages_used",
			"pages_per_zspage", "freeable");

	for (i = 0; i < ZS_SIZE_CLASSES; i++) {

		class = pool->size_class[i];

		if (class->index != i)
			continue;

		spin_lock(&class->lock);

		seq_printf(s, " %5u %5u ", i, class->size);
		for (fg = ZS_INUSE_RATIO_10; fg < NR_FULLNESS_GROUPS; fg++) {
			inuse_totals[fg] += class_stat_read(class, fg);
			seq_printf(s, "%9lu ", class_stat_read(class, fg));
		}

		obj_allocated = class_stat_read(class, ZS_OBJS_ALLOCATED);
		obj_used = class_stat_read(class, ZS_OBJS_INUSE);
		freeable = zs_can_compact(class);
		spin_unlock(&class->lock);

		objs_per_zspage = class->objs_per_zspage;
		pages_used = obj_allocated / objs_per_zspage *
				class->pages_per_zspage;

		seq_printf(s, "%13lu %10lu %10lu %16d %8lu\n",
			   obj_allocated, obj_used, pages_used,
			   class->pages_per_zspage, freeable);

		total_objs += obj_allocated;
		total_used_objs += obj_used;
		total_pages += pages_used;
		total_freeable += freeable;
	}

	seq_puts(s, "\n");
	seq_printf(s, " %5s %5s ", "Total", "");

	for (fg = ZS_INUSE_RATIO_10; fg < NR_FULLNESS_GROUPS; fg++)
		seq_printf(s, "%9lu ", inuse_totals[fg]);

	seq_printf(s, "%13lu %10lu %10lu %16s %8lu\n",
		   total_objs, total_used_objs, total_pages, "",
		   total_freeable);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(zs_stats_size);

static void zs_pool_stat_create(struct zs_pool *pool, const char *name)
{
	if (!zs_stat_root) {
		pr_warn("no root stat dir, not creating <%s> stat dir\n", name);
		return;
	}

	pool->stat_dentry = debugfs_create_dir(name, zs_stat_root);

	debugfs_create_file("classes", S_IFREG | 0444, pool->stat_dentry, pool,
			    &zs_stats_size_fops);
}

static void zs_pool_stat_destroy(struct zs_pool *pool)
{
	debugfs_remove_recursive(pool->stat_dentry);
}

#else /* CONFIG_ZSMALLOC_STAT */
static void __init zs_stat_init(void)
{
}

static void __exit zs_stat_exit(void)
{
}

static inline void zs_pool_stat_create(struct zs_pool *pool, const char *name)
{
}

static inline void zs_pool_stat_destroy(struct zs_pool *pool)
{
}
#endif


/*
 * For each size class, zspages are divided into different groups
 * depending on their usage ratio. This function returns fullness
 * status of the given page.
 */
static int get_fullness_group(struct size_class *class, struct zspage *zspage)
{
	int inuse, objs_per_zspage, ratio;

	inuse = get_zspage_inuse(zspage);
	objs_per_zspage = class->objs_per_zspage;

	if (inuse == 0)
		return ZS_INUSE_RATIO_0;
	if (inuse == objs_per_zspage)
		return ZS_INUSE_RATIO_100;

	ratio = 100 * inuse / objs_per_zspage;
	/*
	 * Take integer division into consideration: a page with one inuse
	 * object out of 127 possible, will end up having 0 usage ratio,
	 * which is wrong as it belongs in ZS_INUSE_RATIO_10 fullness group.
	 */
	return ratio / 10 + 1;
}

/*
 * Each size class maintains various freelists and zspages are assigned
 * to one of these freelists based on the number of live objects they
 * have. This functions inserts the given zspage into the freelist
 * identified by <class, fullness_group>.
 */
static void insert_zspage(struct size_class *class,
				struct zspage *zspage,
				int fullness)
{
	class_stat_add(class, fullness, 1);
	list_add(&zspage->list, &class->fullness_list[fullness]);
	zspage->fullness = fullness;
}

/*
 * This function removes the given zspage from the freelist identified
 * by <class, fullness_group>.
 */
static void remove_zspage(struct size_class *class, struct zspage *zspage)
{
	int fullness = zspage->fullness;

	VM_BUG_ON(list_empty(&class->fullness_list[fullness]));

	list_del_init(&zspage->list);
	class_stat_sub(class, fullness, 1);
}

/*
 * Each size class maintains zspages in different fullness groups depending
 * on the number of live objects they contain. When allocating or freeing
 * objects, the fullness status of the page can change, for instance, from
 * INUSE_RATIO_80 to INUSE_RATIO_70 when freeing an object. This function
 * checks if such a status change has occurred for the given page and
 * accordingly moves the page from the list of the old fullness group to that
 * of the new fullness group.
 */
static int fix_fullness_group(struct size_class *class, struct zspage *zspage)
{
	int newfg;

	newfg = get_fullness_group(class, zspage);
	if (newfg == zspage->fullness)
		goto out;

	remove_zspage(class, zspage);
	insert_zspage(class, zspage, newfg);
out:
	return newfg;
}

static struct zspage *get_zspage(struct zpdesc *zpdesc)
{
	struct zspage *zspage = zpdesc->zspage;

	BUG_ON(zspage->magic != ZSPAGE_MAGIC);
	return zspage;
}

static struct zpdesc *get_next_zpdesc(struct zpdesc *zpdesc)
{
	struct zspage *zspage = get_zspage(zpdesc);

	if (unlikely(ZsHugePage(zspage)))
		return NULL;

	return zpdesc->next;
}

/**
 * obj_to_location - get (<zpdesc>, <obj_idx>) from encoded object value
 * @obj: the encoded object value
 * @zpdesc: zpdesc object resides in zspage
 * @obj_idx: object index
 */
static void obj_to_location(unsigned long obj, struct zpdesc **zpdesc,
				unsigned int *obj_idx)
{
	*zpdesc = pfn_zpdesc(obj >> OBJ_INDEX_BITS);
	*obj_idx = (obj & OBJ_INDEX_MASK);
}

static void obj_to_zpdesc(unsigned long obj, struct zpdesc **zpdesc)
{
	*zpdesc = pfn_zpdesc(obj >> OBJ_INDEX_BITS);
}

/**
 * location_to_obj - get obj value encoded from (<zpdesc>, <obj_idx>)
 * @zpdesc: zpdesc object resides in zspage
 * @obj_idx: object index
 */
static unsigned long location_to_obj(struct zpdesc *zpdesc, unsigned int obj_idx)
{
	unsigned long obj;

	obj = zpdesc_pfn(zpdesc) << OBJ_INDEX_BITS;
	obj |= obj_idx & OBJ_INDEX_MASK;

	return obj;
}

static unsigned long handle_to_obj(unsigned long handle)
{
	return *(unsigned long *)handle;
}

static inline bool obj_allocated(struct zpdesc *zpdesc, void *obj,
				 unsigned long *phandle)
{
	unsigned long handle;
	struct zspage *zspage = get_zspage(zpdesc);

	if (unlikely(ZsHugePage(zspage))) {
		VM_BUG_ON_PAGE(!is_first_zpdesc(zpdesc), zpdesc_page(zpdesc));
		handle = zpdesc->handle;
	} else
		handle = *(unsigned long *)obj;

	if (!(handle & OBJ_ALLOCATED_TAG))
		return false;

	/* Clear all tags before returning the handle */
	*phandle = handle & ~OBJ_TAG_MASK;
	return true;
}

static void reset_zpdesc(struct zpdesc *zpdesc)
{
	struct page *page = zpdesc_page(zpdesc);

	ClearPagePrivate(page);
	zpdesc->zspage = NULL;
	zpdesc->next = NULL;
	/* PageZsmalloc is sticky until the page is freed to the buddy. */
}

static int trylock_zspage(struct zspage *zspage)
{
	struct zpdesc *cursor, *fail;

	for (cursor = get_first_zpdesc(zspage); cursor != NULL; cursor =
					get_next_zpdesc(cursor)) {
		if (!zpdesc_trylock(cursor)) {
			fail = cursor;
			goto unlock;
		}
	}

	return 1;
unlock:
	for (cursor = get_first_zpdesc(zspage); cursor != fail; cursor =
					get_next_zpdesc(cursor))
		zpdesc_unlock(cursor);

	return 0;
}

static void __free_zspage(struct zs_pool *pool, struct size_class *class,
				struct zspage *zspage)
{
	struct zpdesc *zpdesc, *next;

	assert_spin_locked(&class->lock);

	VM_BUG_ON(get_zspage_inuse(zspage));
	VM_BUG_ON(zspage->fullness != ZS_INUSE_RATIO_0);

	next = zpdesc = get_first_zpdesc(zspage);
	do {
		VM_BUG_ON_PAGE(!zpdesc_is_locked(zpdesc), zpdesc_page(zpdesc));
		next = get_next_zpdesc(zpdesc);
		reset_zpdesc(zpdesc);
		zpdesc_unlock(zpdesc);
		zpdesc_dec_zone_page_state(zpdesc);
		zpdesc_put(zpdesc);
		zpdesc = next;
	} while (zpdesc != NULL);

	cache_free_zspage(pool, zspage);

	class_stat_sub(class, ZS_OBJS_ALLOCATED, class->objs_per_zspage);
	atomic_long_sub(class->pages_per_zspage, &pool->pages_allocated);
}

static void free_zspage(struct zs_pool *pool, struct size_class *class,
				struct zspage *zspage)
{
	VM_BUG_ON(get_zspage_inuse(zspage));
	VM_BUG_ON(list_empty(&zspage->list));

	/*
	 * Since zs_free couldn't be sleepable, this function cannot call
	 * lock_page. The page locks trylock_zspage got will be released
	 * by __free_zspage.
	 */
	if (!trylock_zspage(zspage)) {
		kick_deferred_free(pool);
		return;
	}

	remove_zspage(class, zspage);
	__free_zspage(pool, class, zspage);
}

/* Initialize a newly allocated zspage */
static void init_zspage(struct size_class *class, struct zspage *zspage)
{
	unsigned int freeobj = 1;
	unsigned long off = 0;
	struct zpdesc *zpdesc = get_first_zpdesc(zspage);

	while (zpdesc) {
		struct zpdesc *next_zpdesc;
		struct link_free *link;
		void *vaddr;

		set_first_obj_offset(zpdesc, off);

		vaddr = kmap_local_zpdesc(zpdesc);
		link = (struct link_free *)vaddr + off / sizeof(*link);

		while ((off += class->size) < PAGE_SIZE) {
			link->next = freeobj++ << OBJ_TAG_BITS;
			link += class->size / sizeof(*link);
		}

		/*
		 * We now come to the last (full or partial) object on this
		 * page, which must point to the first object on the next
		 * page (if present)
		 */
		next_zpdesc = get_next_zpdesc(zpdesc);
		if (next_zpdesc) {
			link->next = freeobj++ << OBJ_TAG_BITS;
		} else {
			/*
			 * Reset OBJ_TAG_BITS bit to last link to tell
			 * whether it's allocated object or not.
			 */
			link->next = -1UL << OBJ_TAG_BITS;
		}
		kunmap_local(vaddr);
		zpdesc = next_zpdesc;
		off %= PAGE_SIZE;
	}

	set_freeobj(zspage, 0);
}

static void create_page_chain(struct size_class *class, struct zspage *zspage,
				struct zpdesc *zpdescs[])
{
	int i;
	struct zpdesc *zpdesc;
	struct zpdesc *prev_zpdesc = NULL;
	int nr_zpdescs = class->pages_per_zspage;

	/*
	 * Allocate individual pages and link them together as:
	 * 1. all pages are linked together using zpdesc->next
	 * 2. each sub-page point to zspage using zpdesc->zspage
	 *
	 * we set PG_private to identify the first zpdesc (i.e. no other zpdesc
	 * has this flag set).
	 */
	for (i = 0; i < nr_zpdescs; i++) {
		zpdesc = zpdescs[i];
		zpdesc->zspage = zspage;
		zpdesc->next = NULL;
		if (i == 0) {
			zspage->first_zpdesc = zpdesc;
			zpdesc_set_first(zpdesc);
			if (unlikely(class->objs_per_zspage == 1 &&
					class->pages_per_zspage == 1))
				SetZsHugePage(zspage);
		} else {
			prev_zpdesc->next = zpdesc;
		}
		prev_zpdesc = zpdesc;
	}
}

/*
 * Allocate a zspage for the given size class
 */
static struct zspage *alloc_zspage(struct zs_pool *pool,
				   struct size_class *class,
				   gfp_t gfp, const int nid)
{
	int i;
	struct zpdesc *zpdescs[ZS_MAX_PAGES_PER_ZSPAGE];
	struct zspage *zspage = cache_alloc_zspage(pool, gfp);

	if (!zspage)
		return NULL;

	if (!IS_ENABLED(CONFIG_COMPACTION))
		gfp &= ~__GFP_MOVABLE;

	zspage->magic = ZSPAGE_MAGIC;
	zspage->pool = pool;
	zspage->class = class->index;
	zspage_lock_init(zspage);

	for (i = 0; i < class->pages_per_zspage; i++) {
		struct zpdesc *zpdesc;

		zpdesc = alloc_zpdesc(gfp, nid);
		if (!zpdesc) {
			while (--i >= 0) {
				zpdesc_dec_zone_page_state(zpdescs[i]);
				free_zpdesc(zpdescs[i]);
			}
			cache_free_zspage(pool, zspage);
			return NULL;
		}
		__zpdesc_set_zsmalloc(zpdesc);

		zpdesc_inc_zone_page_state(zpdesc);
		zpdescs[i] = zpdesc;
	}

	create_page_chain(class, zspage, zpdescs);
	init_zspage(class, zspage);

	return zspage;
}

static struct zspage *find_get_zspage(struct size_class *class)
{
	int i;
	struct zspage *zspage;

	for (i = ZS_INUSE_RATIO_99; i >= ZS_INUSE_RATIO_0; i--) {
		zspage = list_first_entry_or_null(&class->fullness_list[i],
						  struct zspage, list);
		if (zspage)
			break;
	}

	return zspage;
}

static bool can_merge(struct size_class *prev, int pages_per_zspage,
					int objs_per_zspage)
{
	if (prev->pages_per_zspage == pages_per_zspage &&
		prev->objs_per_zspage == objs_per_zspage)
		return true;

	return false;
}

static bool zspage_full(struct size_class *class, struct zspage *zspage)
{
	return get_zspage_inuse(zspage) == class->objs_per_zspage;
}

static bool zspage_empty(struct zspage *zspage)
{
	return get_zspage_inuse(zspage) == 0;
}

/**
 * zs_lookup_class_index() - Returns index of the zsmalloc &size_class
 * that hold objects of the provided size.
 * @pool: zsmalloc pool to use
 * @size: object size
 *
 * Context: Any context.
 *
 * Return: the index of the zsmalloc &size_class that hold objects of the
 * provided size.
 */
unsigned int zs_lookup_class_index(struct zs_pool *pool, unsigned int size)
{
	struct size_class *class;

	class = pool->size_class[get_size_class_index(size)];

	return class->index;
}
EXPORT_SYMBOL_GPL(zs_lookup_class_index);

unsigned long zs_get_total_pages(struct zs_pool *pool)
{
	return atomic_long_read(&pool->pages_allocated);
}
EXPORT_SYMBOL_GPL(zs_get_total_pages);

void *zs_obj_read_begin(struct zs_pool *pool, unsigned long handle,
			void *local_copy)
{
	struct zspage *zspage;
	struct zpdesc *zpdesc;
	unsigned long obj, off;
	unsigned int obj_idx;
	struct size_class *class;
	void *addr;

	/* Guarantee we can get zspage from handle safely */
	read_lock(&pool->lock);
	obj = handle_to_obj(handle);
	obj_to_location(obj, &zpdesc, &obj_idx);
	zspage = get_zspage(zpdesc);

	/* Make sure migration doesn't move any pages in this zspage */
	zspage_read_lock(zspage);
	read_unlock(&pool->lock);

	class = zspage_class(pool, zspage);
	off = offset_in_page(class->size * obj_idx);

	if (off + class->size <= PAGE_SIZE) {
		/* this object is contained entirely within a page */
		addr = kmap_local_zpdesc(zpdesc);
		addr += off;
	} else {
		size_t sizes[2];

		/* this object spans two pages */
		sizes[0] = PAGE_SIZE - off;
		sizes[1] = class->size - sizes[0];
		addr = local_copy;

		memcpy_from_page(addr, zpdesc_page(zpdesc),
				 off, sizes[0]);
		zpdesc = get_next_zpdesc(zpdesc);
		memcpy_from_page(addr + sizes[0],
				 zpdesc_page(zpdesc),
				 0, sizes[1]);
	}

	if (!ZsHugePage(zspage))
		addr += ZS_HANDLE_SIZE;

	return addr;
}
EXPORT_SYMBOL_GPL(zs_obj_read_begin);

void zs_obj_read_end(struct zs_pool *pool, unsigned long handle,
		     void *handle_mem)
{
	struct zspage *zspage;
	struct zpdesc *zpdesc;
	unsigned long obj, off;
	unsigned int obj_idx;
	struct size_class *class;

	obj = handle_to_obj(handle);
	obj_to_location(obj, &zpdesc, &obj_idx);
	zspage = get_zspage(zpdesc);
	class = zspage_class(pool, zspage);
	off = offset_in_page(class->size * obj_idx);

	if (off + class->size <= PAGE_SIZE) {
		if (!ZsHugePage(zspage))
			off += ZS_HANDLE_SIZE;
		handle_mem -= off;
		kunmap_local(handle_mem);
	}

	zspage_read_unlock(zspage);
}
EXPORT_SYMBOL_GPL(zs_obj_read_end);

void zs_obj_write(struct zs_pool *pool, unsigned long handle,
		  void *handle_mem, size_t mem_len)
{
	struct zspage *zspage;
	struct zpdesc *zpdesc;
	unsigned long obj, off;
	unsigned int obj_idx;
	struct size_class *class;

	/* Guarantee we can get zspage from handle safely */
	read_lock(&pool->lock);
	obj = handle_to_obj(handle);
	obj_to_location(obj, &zpdesc, &obj_idx);
	zspage = get_zspage(zpdesc);

	/* Make sure migration doesn't move any pages in this zspage */
	zspage_read_lock(zspage);
	read_unlock(&pool->lock);

	class = zspage_class(pool, zspage);
	off = offset_in_page(class->size * obj_idx);

	if (!ZsHugePage(zspage))
		off += ZS_HANDLE_SIZE;

	if (off + mem_len <= PAGE_SIZE) {
		/* this object is contained entirely within a page */
		void *dst = kmap_local_zpdesc(zpdesc);

		memcpy(dst + off, handle_mem, mem_len);
		kunmap_local(dst);
	} else {
		/* this object spans two pages */
		size_t sizes[2];

		sizes[0] = PAGE_SIZE - off;
		sizes[1] = mem_len - sizes[0];

		memcpy_to_page(zpdesc_page(zpdesc), off,
			       handle_mem, sizes[0]);
		zpdesc = get_next_zpdesc(zpdesc);
		memcpy_to_page(zpdesc_page(zpdesc), 0,
			       handle_mem + sizes[0], sizes[1]);
	}

	zspage_read_unlock(zspage);
}
EXPORT_SYMBOL_GPL(zs_obj_write);

/**
 * zs_huge_class_size() - Returns the size (in bytes) of the first huge
 *                        zsmalloc &size_class.
 * @pool: zsmalloc pool to use
 *
 * The function returns the size of the first huge class - any object of equal
 * or bigger size will be stored in zspage consisting of a single physical
 * page.
 *
 * Context: Any context.
 *
 * Return: the size (in bytes) of the first huge zsmalloc &size_class.
 */
size_t zs_huge_class_size(struct zs_pool *pool)
{
	return huge_class_size;
}
EXPORT_SYMBOL_GPL(zs_huge_class_size);

static unsigned long obj_malloc(struct zs_pool *pool,
				struct zspage *zspage, unsigned long handle)
{
	int i, nr_zpdesc, offset;
	unsigned long obj;
	struct link_free *link;
	struct size_class *class;

	struct zpdesc *m_zpdesc;
	unsigned long m_offset;
	void *vaddr;

	class = pool->size_class[zspage->class];
	obj = get_freeobj(zspage);

	offset = obj * class->size;
	nr_zpdesc = offset >> PAGE_SHIFT;
	m_offset = offset_in_page(offset);
	m_zpdesc = get_first_zpdesc(zspage);

	for (i = 0; i < nr_zpdesc; i++)
		m_zpdesc = get_next_zpdesc(m_zpdesc);

	vaddr = kmap_local_zpdesc(m_zpdesc);
	link = (struct link_free *)vaddr + m_offset / sizeof(*link);
	set_freeobj(zspage, link->next >> OBJ_TAG_BITS);
	if (likely(!ZsHugePage(zspage)))
		/* record handle in the header of allocated chunk */
		link->handle = handle | OBJ_ALLOCATED_TAG;
	else
		zspage->first_zpdesc->handle = handle | OBJ_ALLOCATED_TAG;

	kunmap_local(vaddr);
	mod_zspage_inuse(zspage, 1);

	obj = location_to_obj(m_zpdesc, obj);
	record_obj(handle, obj);

	return obj;
}


/**
 * zs_malloc - Allocate block of given size from pool.
 * @pool: pool to allocate from
 * @size: size of block to allocate
 * @gfp: gfp flags when allocating object
 * @nid: The preferred node id to allocate new zspage (if needed)
 *
 * On success, handle to the allocated object is returned,
 * otherwise an ERR_PTR().
 * Allocation requests with size > ZS_MAX_ALLOC_SIZE will fail.
 */
unsigned long zs_malloc(struct zs_pool *pool, size_t size, gfp_t gfp,
			const int nid)
{
	unsigned long handle;
	struct size_class *class;
	int newfg;
	struct zspage *zspage;

	if (unlikely(!size))
		return (unsigned long)ERR_PTR(-EINVAL);

	if (unlikely(size > ZS_MAX_ALLOC_SIZE))
		return (unsigned long)ERR_PTR(-ENOSPC);

	handle = cache_alloc_handle(pool, gfp);
	if (!handle)
		return (unsigned long)ERR_PTR(-ENOMEM);

	/* extra space in chunk to keep the handle */
	size += ZS_HANDLE_SIZE;
	class = pool->size_class[get_size_class_index(size)];

	/* class->lock effectively protects the zpage migration */
	spin_lock(&class->lock);
	zspage = find_get_zspage(class);
	if (likely(zspage)) {
		obj_malloc(pool, zspage, handle);
		/* Now move the zspage to another fullness group, if required */
		fix_fullness_group(class, zspage);
		class_stat_add(class, ZS_OBJS_INUSE, 1);

		goto out;
	}

	spin_unlock(&class->lock);

	zspage = alloc_zspage(pool, class, gfp, nid);
	if (!zspage) {
		cache_free_handle(pool, handle);
		return (unsigned long)ERR_PTR(-ENOMEM);
	}

	spin_lock(&class->lock);
	obj_malloc(pool, zspage, handle);
	newfg = get_fullness_group(class, zspage);
	insert_zspage(class, zspage, newfg);
	atomic_long_add(class->pages_per_zspage, &pool->pages_allocated);
	class_stat_add(class, ZS_OBJS_ALLOCATED, class->objs_per_zspage);
	class_stat_add(class, ZS_OBJS_INUSE, 1);

	/* We completely set up zspage so mark them as movable */
	SetZsPageMovable(pool, zspage);
out:
	spin_unlock(&class->lock);

	return handle;
}
EXPORT_SYMBOL_GPL(zs_malloc);

static void obj_free(int class_size, unsigned long obj)
{
	struct link_free *link;
	struct zspage *zspage;
	struct zpdesc *f_zpdesc;
	unsigned long f_offset;
	unsigned int f_objidx;
	void *vaddr;


	obj_to_location(obj, &f_zpdesc, &f_objidx);
	f_offset = offset_in_page(class_size * f_objidx);
	zspage = get_zspage(f_zpdesc);

	vaddr = kmap_local_zpdesc(f_zpdesc);
	link = (struct link_free *)(vaddr + f_offset);

	/* Insert this object in containing zspage's freelist */
	if (likely(!ZsHugePage(zspage)))
		link->next = get_freeobj(zspage) << OBJ_TAG_BITS;
	else
		f_zpdesc->handle = 0;
	set_freeobj(zspage, f_objidx);

	kunmap_local(vaddr);
	mod_zspage_inuse(zspage, -1);
}

void zs_free(struct zs_pool *pool, unsigned long handle)
{
	struct zspage *zspage;
	struct zpdesc *f_zpdesc;
	unsigned long obj;
	struct size_class *class;
	int fullness;

	if (IS_ERR_OR_NULL((void *)handle))
		return;

	/*
	 * The pool->lock protects the race with zpage's migration
	 * so it's safe to get the page from handle.
	 */
	read_lock(&pool->lock);
	obj = handle_to_obj(handle);
	obj_to_zpdesc(obj, &f_zpdesc);
	zspage = get_zspage(f_zpdesc);
	class = zspage_class(pool, zspage);
	spin_lock(&class->lock);
	read_unlock(&pool->lock);

	class_stat_sub(class, ZS_OBJS_INUSE, 1);
	obj_free(class->size, obj);

	fullness = fix_fullness_group(class, zspage);
	if (fullness == ZS_INUSE_RATIO_0)
		free_zspage(pool, class, zspage);

	spin_unlock(&class->lock);
	cache_free_handle(pool, handle);
}
EXPORT_SYMBOL_GPL(zs_free);

static void zs_object_copy(struct size_class *class, unsigned long dst,
				unsigned long src)
{
	struct zpdesc *s_zpdesc, *d_zpdesc;
	unsigned int s_objidx, d_objidx;
	unsigned long s_off, d_off;
	void *s_addr, *d_addr;
	int s_size, d_size, size;
	int written = 0;

	s_size = d_size = class->size;

	obj_to_location(src, &s_zpdesc, &s_objidx);
	obj_to_location(dst, &d_zpdesc, &d_objidx);

	s_off = offset_in_page(class->size * s_objidx);
	d_off = offset_in_page(class->size * d_objidx);

	if (s_off + class->size > PAGE_SIZE)
		s_size = PAGE_SIZE - s_off;

	if (d_off + class->size > PAGE_SIZE)
		d_size = PAGE_SIZE - d_off;

	s_addr = kmap_local_zpdesc(s_zpdesc);
	d_addr = kmap_local_zpdesc(d_zpdesc);

	while (1) {
		size = min(s_size, d_size);
		memcpy(d_addr + d_off, s_addr + s_off, size);
		written += size;

		if (written == class->size)
			break;

		s_off += size;
		s_size -= size;
		d_off += size;
		d_size -= size;

		/*
		 * Calling kunmap_local(d_addr) is necessary. kunmap_local()
		 * calls must occurs in reverse order of calls to kmap_local_page().
		 * So, to call kunmap_local(s_addr) we should first call
		 * kunmap_local(d_addr). For more details see
		 * Documentation/mm/highmem.rst.
		 */
		if (s_off >= PAGE_SIZE) {
			kunmap_local(d_addr);
			kunmap_local(s_addr);
			s_zpdesc = get_next_zpdesc(s_zpdesc);
			s_addr = kmap_local_zpdesc(s_zpdesc);
			d_addr = kmap_local_zpdesc(d_zpdesc);
			s_size = class->size - written;
			s_off = 0;
		}

		if (d_off >= PAGE_SIZE) {
			kunmap_local(d_addr);
			d_zpdesc = get_next_zpdesc(d_zpdesc);
			d_addr = kmap_local_zpdesc(d_zpdesc);
			d_size = class->size - written;
			d_off = 0;
		}
	}

	kunmap_local(d_addr);
	kunmap_local(s_addr);
}

/*
 * Find alloced object in zspage from index object and
 * return handle.
 */
static unsigned long find_alloced_obj(struct size_class *class,
				      struct zpdesc *zpdesc, int *obj_idx)
{
	unsigned int offset;
	int index = *obj_idx;
	unsigned long handle = 0;
	void *addr = kmap_local_zpdesc(zpdesc);

	offset = get_first_obj_offset(zpdesc);
	offset += class->size * index;

	while (offset < PAGE_SIZE) {
		if (obj_allocated(zpdesc, addr + offset, &handle))
			break;

		offset += class->size;
		index++;
	}

	kunmap_local(addr);

	*obj_idx = index;

	return handle;
}

static void migrate_zspage(struct zs_pool *pool, struct zspage *src_zspage,
			   struct zspage *dst_zspage)
{
	unsigned long used_obj, free_obj;
	unsigned long handle;
	int obj_idx = 0;
	struct zpdesc *s_zpdesc = get_first_zpdesc(src_zspage);
	struct size_class *class = pool->size_class[src_zspage->class];

	while (1) {
		handle = find_alloced_obj(class, s_zpdesc, &obj_idx);
		if (!handle) {
			s_zpdesc = get_next_zpdesc(s_zpdesc);
			if (!s_zpdesc)
				break;
			obj_idx = 0;
			continue;
		}

		used_obj = handle_to_obj(handle);
		free_obj = obj_malloc(pool, dst_zspage, handle);
		zs_object_copy(class, free_obj, used_obj);
		obj_idx++;
		obj_free(class->size, used_obj);

		/* Stop if there is no more space */
		if (zspage_full(class, dst_zspage))
			break;

		/* Stop if there are no more objects to migrate */
		if (zspage_empty(src_zspage))
			break;
	}
}

static struct zspage *isolate_src_zspage(struct size_class *class)
{
	struct zspage *zspage;
	int fg;

	for (fg = ZS_INUSE_RATIO_10; fg <= ZS_INUSE_RATIO_99; fg++) {
		zspage = list_first_entry_or_null(&class->fullness_list[fg],
						  struct zspage, list);
		if (zspage) {
			remove_zspage(class, zspage);
			return zspage;
		}
	}

	return zspage;
}

static struct zspage *isolate_dst_zspage(struct size_class *class)
{
	struct zspage *zspage;
	int fg;

	for (fg = ZS_INUSE_RATIO_99; fg >= ZS_INUSE_RATIO_10; fg--) {
		zspage = list_first_entry_or_null(&class->fullness_list[fg],
						  struct zspage, list);
		if (zspage) {
			remove_zspage(class, zspage);
			return zspage;
		}
	}

	return zspage;
}

/*
 * putback_zspage - add @zspage into right class's fullness list
 * @class: destination class
 * @zspage: target page
 *
 * Return @zspage's fullness status
 */
static int putback_zspage(struct size_class *class, struct zspage *zspage)
{
	int fullness;

	fullness = get_fullness_group(class, zspage);
	insert_zspage(class, zspage, fullness);

	return fullness;
}

#ifdef CONFIG_COMPACTION
/*
 * To prevent zspage destroy during migration, zspage freeing should
 * hold locks of all pages in the zspage.
 */
static void lock_zspage(struct zspage *zspage)
{
	struct zpdesc *curr_zpdesc, *zpdesc;

	/*
	 * Pages we haven't locked yet can be migrated off the list while we're
	 * trying to lock them, so we need to be careful and only attempt to
	 * lock each page under zspage_read_lock(). Otherwise, the page we lock
	 * may no longer belong to the zspage. This means that we may wait for
	 * the wrong page to unlock, so we must take a reference to the page
	 * prior to waiting for it to unlock outside zspage_read_lock().
	 */
	while (1) {
		zspage_read_lock(zspage);
		zpdesc = get_first_zpdesc(zspage);
		if (zpdesc_trylock(zpdesc))
			break;
		zpdesc_get(zpdesc);
		zspage_read_unlock(zspage);
		zpdesc_wait_locked(zpdesc);
		zpdesc_put(zpdesc);
	}

	curr_zpdesc = zpdesc;
	while ((zpdesc = get_next_zpdesc(curr_zpdesc))) {
		if (zpdesc_trylock(zpdesc)) {
			curr_zpdesc = zpdesc;
		} else {
			zpdesc_get(zpdesc);
			zspage_read_unlock(zspage);
			zpdesc_wait_locked(zpdesc);
			zpdesc_put(zpdesc);
			zspage_read_lock(zspage);
		}
	}
	zspage_read_unlock(zspage);
}
#endif /* CONFIG_COMPACTION */

#ifdef CONFIG_COMPACTION

static void replace_sub_page(struct size_class *class, struct zspage *zspage,
				struct zpdesc *newzpdesc, struct zpdesc *oldzpdesc)
{
	struct zpdesc *zpdesc;
	struct zpdesc *zpdescs[ZS_MAX_PAGES_PER_ZSPAGE] = {NULL, };
	unsigned int first_obj_offset;
	int idx = 0;

	zpdesc = get_first_zpdesc(zspage);
	do {
		if (zpdesc == oldzpdesc)
			zpdescs[idx] = newzpdesc;
		else
			zpdescs[idx] = zpdesc;
		idx++;
	} while ((zpdesc = get_next_zpdesc(zpdesc)) != NULL);

	create_page_chain(class, zspage, zpdescs);
	first_obj_offset = get_first_obj_offset(oldzpdesc);
	set_first_obj_offset(newzpdesc, first_obj_offset);
	if (unlikely(ZsHugePage(zspage)))
		newzpdesc->handle = oldzpdesc->handle;
	__zpdesc_set_movable(newzpdesc);
}

static bool zs_page_isolate(struct page *page, isolate_mode_t mode)
{
	/*
	 * Page is locked so zspage can't be destroyed concurrently
	 * (see free_zspage()). But if the page was already destroyed
	 * (see reset_zpdesc()), refuse isolation here.
	 */
	return page_zpdesc(page)->zspage;
}

static int zs_page_migrate(struct page *newpage, struct page *page,
		enum migrate_mode mode)
{
	struct zs_pool *pool;
	struct size_class *class;
	struct zspage *zspage;
	struct zpdesc *dummy;
	struct zpdesc *newzpdesc = page_zpdesc(newpage);
	struct zpdesc *zpdesc = page_zpdesc(page);
	void *s_addr, *d_addr, *addr;
	unsigned int offset;
	unsigned long handle;
	unsigned long old_obj, new_obj;
	unsigned int obj_idx;

	/*
	 * TODO: nothing prevents a zspage from getting destroyed while
	 * it is isolated for migration, as the page lock is temporarily
	 * dropped after zs_page_isolate() succeeded: we should rework that
	 * and defer destroying such pages once they are un-isolated (putback)
	 * instead.
	 */
	if (!zpdesc->zspage)
		return 0;

	/* The page is locked, so this pointer must remain valid */
	zspage = get_zspage(zpdesc);
	pool = zspage->pool;

	/*
	 * The pool migrate_lock protects the race between zpage migration
	 * and zs_free.
	 */
	write_lock(&pool->lock);
	class = zspage_class(pool, zspage);

	/*
	 * the class lock protects zpage alloc/free in the zspage.
	 */
	spin_lock(&class->lock);
	/* the zspage write_lock protects zpage access via zs_obj_read/write() */
	if (!zspage_write_trylock(zspage)) {
		spin_unlock(&class->lock);
		write_unlock(&pool->lock);
		return -EINVAL;
	}

	/* We're committed, tell the world that this is a Zsmalloc page. */
	__zpdesc_set_zsmalloc(newzpdesc);

	offset = get_first_obj_offset(zpdesc);
	s_addr = kmap_local_zpdesc(zpdesc);

	/*
	 * Here, any user cannot access all objects in the zspage so let's move.
	 */
	d_addr = kmap_local_zpdesc(newzpdesc);
	copy_page(d_addr, s_addr);
	kunmap_local(d_addr);

	for (addr = s_addr + offset; addr < s_addr + PAGE_SIZE;
					addr += class->size) {
		if (obj_allocated(zpdesc, addr, &handle)) {

			old_obj = handle_to_obj(handle);
			obj_to_location(old_obj, &dummy, &obj_idx);
			new_obj = (unsigned long)location_to_obj(newzpdesc, obj_idx);
			record_obj(handle, new_obj);
		}
	}
	kunmap_local(s_addr);

	replace_sub_page(class, zspage, newzpdesc, zpdesc);
	/*
	 * Since we complete the data copy and set up new zspage structure,
	 * it's okay to release migration_lock.
	 */
	write_unlock(&pool->lock);
	spin_unlock(&class->lock);
	zspage_write_unlock(zspage);

	zpdesc_get(newzpdesc);
	if (zpdesc_zone(newzpdesc) != zpdesc_zone(zpdesc)) {
		zpdesc_dec_zone_page_state(zpdesc);
		zpdesc_inc_zone_page_state(newzpdesc);
	}

	reset_zpdesc(zpdesc);
	zpdesc_put(zpdesc);

	return 0;
}

static void zs_page_putback(struct page *page)
{
}

const struct movable_operations zsmalloc_mops = {
	.isolate_page = zs_page_isolate,
	.migrate_page = zs_page_migrate,
	.putback_page = zs_page_putback,
};

/*
 * Caller should hold page_lock of all pages in the zspage
 * In here, we cannot use zspage meta data.
 */
static void async_free_zspage(struct work_struct *work)
{
	int i;
	struct size_class *class;
	struct zspage *zspage, *tmp;
	LIST_HEAD(free_pages);
	struct zs_pool *pool = container_of(work, struct zs_pool,
					free_work);

	for (i = 0; i < ZS_SIZE_CLASSES; i++) {
		class = pool->size_class[i];
		if (class->index != i)
			continue;

		spin_lock(&class->lock);
		list_splice_init(&class->fullness_list[ZS_INUSE_RATIO_0],
				 &free_pages);
		spin_unlock(&class->lock);
	}

	list_for_each_entry_safe(zspage, tmp, &free_pages, list) {
		list_del(&zspage->list);
		lock_zspage(zspage);

		class = zspage_class(pool, zspage);
		spin_lock(&class->lock);
		class_stat_sub(class, ZS_INUSE_RATIO_0, 1);
		__free_zspage(pool, class, zspage);
		spin_unlock(&class->lock);
	}
};

static void kick_deferred_free(struct zs_pool *pool)
{
	schedule_work(&pool->free_work);
}

static void zs_flush_migration(struct zs_pool *pool)
{
	flush_work(&pool->free_work);
}

static void init_deferred_free(struct zs_pool *pool)
{
	INIT_WORK(&pool->free_work, async_free_zspage);
}

static void SetZsPageMovable(struct zs_pool *pool, struct zspage *zspage)
{
	struct zpdesc *zpdesc = get_first_zpdesc(zspage);

	do {
		WARN_ON(!zpdesc_trylock(zpdesc));
		__zpdesc_set_movable(zpdesc);
		zpdesc_unlock(zpdesc);
	} while ((zpdesc = get_next_zpdesc(zpdesc)) != NULL);
}
#else
static inline void zs_flush_migration(struct zs_pool *pool) { }
#endif

/*
 *
 * Based on the number of unused allocated objects calculate
 * and return the number of pages that we can free.
 */
static unsigned long zs_can_compact(struct size_class *class)
{
	unsigned long obj_wasted;
	unsigned long obj_allocated = class_stat_read(class, ZS_OBJS_ALLOCATED);
	unsigned long obj_used = class_stat_read(class, ZS_OBJS_INUSE);

	if (obj_allocated <= obj_used)
		return 0;

	obj_wasted = obj_allocated - obj_used;
	obj_wasted /= class->objs_per_zspage;

	return obj_wasted * class->pages_per_zspage;
}

static unsigned long __zs_compact(struct zs_pool *pool,
				  struct size_class *class)
{
	struct zspage *src_zspage = NULL;
	struct zspage *dst_zspage = NULL;
	unsigned long pages_freed = 0;

	/*
	 * protect the race between zpage migration and zs_free
	 * as well as zpage allocation/free
	 */
	write_lock(&pool->lock);
	spin_lock(&class->lock);
	while (zs_can_compact(class)) {
		int fg;

		if (!dst_zspage) {
			dst_zspage = isolate_dst_zspage(class);
			if (!dst_zspage)
				break;
		}

		src_zspage = isolate_src_zspage(class);
		if (!src_zspage)
			break;

		if (!zspage_write_trylock(src_zspage))
			break;

		migrate_zspage(pool, src_zspage, dst_zspage);
		zspage_write_unlock(src_zspage);

		fg = putback_zspage(class, src_zspage);
		if (fg == ZS_INUSE_RATIO_0) {
			free_zspage(pool, class, src_zspage);
			pages_freed += class->pages_per_zspage;
		}
		src_zspage = NULL;

		if (get_fullness_group(class, dst_zspage) == ZS_INUSE_RATIO_100
		    || rwlock_is_contended(&pool->lock)) {
			putback_zspage(class, dst_zspage);
			dst_zspage = NULL;

			spin_unlock(&class->lock);
			write_unlock(&pool->lock);
			cond_resched();
			write_lock(&pool->lock);
			spin_lock(&class->lock);
		}
	}

	if (src_zspage)
		putback_zspage(class, src_zspage);

	if (dst_zspage)
		putback_zspage(class, dst_zspage);

	spin_unlock(&class->lock);
	write_unlock(&pool->lock);

	return pages_freed;
}

unsigned long zs_compact(struct zs_pool *pool)
{
	int i;
	struct size_class *class;
	unsigned long pages_freed = 0;

	/*
	 * Pool compaction is performed under pool->lock so it is basically
	 * single-threaded. Having more than one thread in __zs_compact()
	 * will increase pool->lock contention, which will impact other
	 * zsmalloc operations that need pool->lock.
	 */
	if (atomic_xchg(&pool->compaction_in_progress, 1))
		return 0;

	for (i = ZS_SIZE_CLASSES - 1; i >= 0; i--) {
		class = pool->size_class[i];
		if (class->index != i)
			continue;
		pages_freed += __zs_compact(pool, class);
	}
	atomic_long_add(pages_freed, &pool->stats.pages_compacted);
	atomic_set(&pool->compaction_in_progress, 0);

	return pages_freed;
}
EXPORT_SYMBOL_GPL(zs_compact);

void zs_pool_stats(struct zs_pool *pool, struct zs_pool_stats *stats)
{
	memcpy(stats, &pool->stats, sizeof(struct zs_pool_stats));
}
EXPORT_SYMBOL_GPL(zs_pool_stats);

static unsigned long zs_shrinker_scan(struct shrinker *shrinker,
		struct shrink_control *sc)
{
	unsigned long pages_freed;
	struct zs_pool *pool = shrinker->private_data;

	/*
	 * Compact classes and calculate compaction delta.
	 * Can run concurrently with a manually triggered
	 * (by user) compaction.
	 */
	pages_freed = zs_compact(pool);

	return pages_freed ? pages_freed : SHRINK_STOP;
}

static unsigned long zs_shrinker_count(struct shrinker *shrinker,
		struct shrink_control *sc)
{
	int i;
	struct size_class *class;
	unsigned long pages_to_free = 0;
	struct zs_pool *pool = shrinker->private_data;

	for (i = ZS_SIZE_CLASSES - 1; i >= 0; i--) {
		class = pool->size_class[i];
		if (class->index != i)
			continue;

		pages_to_free += zs_can_compact(class);
	}

	return pages_to_free;
}

static void zs_unregister_shrinker(struct zs_pool *pool)
{
	shrinker_free(pool->shrinker);
}

static int zs_register_shrinker(struct zs_pool *pool)
{
	pool->shrinker = shrinker_alloc(0, "mm-zspool:%s", pool->name);
	if (!pool->shrinker)
		return -ENOMEM;

	pool->shrinker->scan_objects = zs_shrinker_scan;
	pool->shrinker->count_objects = zs_shrinker_count;
	pool->shrinker->batch = 0;
	pool->shrinker->private_data = pool;

	shrinker_register(pool->shrinker);

	return 0;
}

static int calculate_zspage_chain_size(int class_size)
{
	int i, min_waste = INT_MAX;
	int chain_size = 1;

	if (is_power_of_2(class_size))
		return chain_size;

	for (i = 1; i <= ZS_MAX_PAGES_PER_ZSPAGE; i++) {
		int waste;

		waste = (i * PAGE_SIZE) % class_size;
		if (waste < min_waste) {
			min_waste = waste;
			chain_size = i;
		}
	}

	return chain_size;
}

/**
 * zs_create_pool - Creates an allocation pool to work from.
 * @name: pool name to be created
 *
 * This function must be called before anything when using
 * the zsmalloc allocator.
 *
 * On success, a pointer to the newly created pool is returned,
 * otherwise NULL.
 */
struct zs_pool *zs_create_pool(const char *name)
{
	int i;
	struct zs_pool *pool;
	struct size_class *prev_class = NULL;

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return NULL;

	init_deferred_free(pool);
	rwlock_init(&pool->lock);
	atomic_set(&pool->compaction_in_progress, 0);

	pool->name = kstrdup(name, GFP_KERNEL);
	if (!pool->name)
		goto err;

	if (create_cache(pool))
		goto err;

	/*
	 * Iterate reversely, because, size of size_class that we want to use
	 * for merging should be larger or equal to current size.
	 */
	for (i = ZS_SIZE_CLASSES - 1; i >= 0; i--) {
		int size;
		int pages_per_zspage;
		int objs_per_zspage;
		struct size_class *class;
		int fullness;

		size = ZS_MIN_ALLOC_SIZE + i * ZS_SIZE_CLASS_DELTA;
		if (size > ZS_MAX_ALLOC_SIZE)
			size = ZS_MAX_ALLOC_SIZE;
		pages_per_zspage = calculate_zspage_chain_size(size);
		objs_per_zspage = pages_per_zspage * PAGE_SIZE / size;

		/*
		 * We iterate from biggest down to smallest classes,
		 * so huge_class_size holds the size of the first huge
		 * class. Any object bigger than or equal to that will
		 * endup in the huge class.
		 */
		if (pages_per_zspage != 1 && objs_per_zspage != 1 &&
				!huge_class_size) {
			huge_class_size = size;
			/*
			 * The object uses ZS_HANDLE_SIZE bytes to store the
			 * handle. We need to subtract it, because zs_malloc()
			 * unconditionally adds handle size before it performs
			 * size class search - so object may be smaller than
			 * huge class size, yet it still can end up in the huge
			 * class because it grows by ZS_HANDLE_SIZE extra bytes
			 * right before class lookup.
			 */
			huge_class_size -= (ZS_HANDLE_SIZE - 1);
		}

		/*
		 * size_class is used for normal zsmalloc operation such
		 * as alloc/free for that size. Although it is natural that we
		 * have one size_class for each size, there is a chance that we
		 * can get more memory utilization if we use one size_class for
		 * many different sizes whose size_class have same
		 * characteristics. So, we makes size_class point to
		 * previous size_class if possible.
		 */
		if (prev_class) {
			if (can_merge(prev_class, pages_per_zspage, objs_per_zspage)) {
				pool->size_class[i] = prev_class;
				continue;
			}
		}

		class = kzalloc(sizeof(struct size_class), GFP_KERNEL);
		if (!class)
			goto err;

		class->size = size;
		class->index = i;
		class->pages_per_zspage = pages_per_zspage;
		class->objs_per_zspage = objs_per_zspage;
		spin_lock_init(&class->lock);
		pool->size_class[i] = class;

		fullness = ZS_INUSE_RATIO_0;
		while (fullness < NR_FULLNESS_GROUPS) {
			INIT_LIST_HEAD(&class->fullness_list[fullness]);
			fullness++;
		}

		prev_class = class;
	}

	/* debug only, don't abort if it fails */
	zs_pool_stat_create(pool, name);

	/*
	 * Not critical since shrinker is only used to trigger internal
	 * defragmentation of the pool which is pretty optional thing.  If
	 * registration fails we still can use the pool normally and user can
	 * trigger compaction manually. Thus, ignore return code.
	 */
	zs_register_shrinker(pool);

	return pool;

err:
	zs_destroy_pool(pool);
	return NULL;
}
EXPORT_SYMBOL_GPL(zs_create_pool);

void zs_destroy_pool(struct zs_pool *pool)
{
	int i;

	zs_unregister_shrinker(pool);
	zs_flush_migration(pool);
	zs_pool_stat_destroy(pool);

	for (i = 0; i < ZS_SIZE_CLASSES; i++) {
		int fg;
		struct size_class *class = pool->size_class[i];

		if (!class)
			continue;

		if (class->index != i)
			continue;

		for (fg = ZS_INUSE_RATIO_0; fg < NR_FULLNESS_GROUPS; fg++) {
			if (list_empty(&class->fullness_list[fg]))
				continue;

			pr_err("Class-%d fullness group %d is not empty\n",
			       class->size, fg);
		}
		kfree(class);
	}

	destroy_cache(pool);
	kfree(pool->name);
	kfree(pool);
}
EXPORT_SYMBOL_GPL(zs_destroy_pool);

static int __init zs_init(void)
{
	int rc __maybe_unused;

#ifdef CONFIG_ZPOOL
	zpool_register_driver(&zs_zpool_driver);
#endif
#ifdef CONFIG_COMPACTION
	rc = set_movable_ops(&zsmalloc_mops, PGTY_zsmalloc);
	if (rc)
		return rc;
#endif
	zs_stat_init();
	return 0;
}

static void __exit zs_exit(void)
{
#ifdef CONFIG_ZPOOL
	zpool_unregister_driver(&zs_zpool_driver);
#endif
#ifdef CONFIG_COMPACTION
	set_movable_ops(NULL, PGTY_zsmalloc);
#endif
	zs_stat_exit();
}

module_init(zs_init);
module_exit(zs_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Nitin Gupta <ngupta@vflare.org>");
MODULE_DESCRIPTION("zsmalloc memory allocator");
