/*
 * Copyright (c) 2004, 2005 Topspin Communications.  All rights reserved.
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
 * $Id$
 */

#ifndef MTHCA_MEMFREE_H
#define MTHCA_MEMFREE_H

#include <linux/list.h>
#include <linux/pci.h>

#include <asm/semaphore.h>

#define MTHCA_ICM_CHUNK_LEN \
	((256 - sizeof (struct list_head) - 2 * sizeof (int)) /		\
	 (sizeof (struct scatterlist)))

struct mthca_icm_chunk {
	struct list_head   list;
	int                npages;
	int                nsg;
	struct scatterlist mem[MTHCA_ICM_CHUNK_LEN];
};

struct mthca_icm {
	struct list_head chunk_list;
};

struct mthca_icm_table {
	u64               virt;
	int               num_icm;
	struct semaphore  sem;
	struct mthca_icm *icm[0];
};

struct mthca_icm_iter {
	struct mthca_icm       *icm;
	struct mthca_icm_chunk *chunk;
	int                     page_idx;
};

struct mthca_dev;

struct mthca_icm *mthca_alloc_icm(struct mthca_dev *dev, int npages,
				  unsigned int gfp_mask);
void mthca_free_icm(struct mthca_dev *dev, struct mthca_icm *icm);

struct mthca_icm_table *mthca_alloc_icm_table(struct mthca_dev *dev,
					      u64 virt, unsigned size,
					      unsigned reserved,
					      int use_lowmem);
void mthca_free_icm_table(struct mthca_dev *dev, struct mthca_icm_table *table);

static inline void mthca_icm_first(struct mthca_icm *icm,
				   struct mthca_icm_iter *iter)
{
	iter->icm      = icm;
	iter->chunk    = list_empty(&icm->chunk_list) ?
		NULL : list_entry(icm->chunk_list.next,
				  struct mthca_icm_chunk, list);
	iter->page_idx = 0;
}

static inline int mthca_icm_last(struct mthca_icm_iter *iter)
{
	return !iter->chunk;
}

static inline void mthca_icm_next(struct mthca_icm_iter *iter)
{
	if (++iter->page_idx >= iter->chunk->nsg) {
		if (iter->chunk->list.next == &iter->icm->chunk_list) {
			iter->chunk = NULL;
			return;
		}

		iter->chunk = list_entry(iter->chunk->list.next,
					 struct mthca_icm_chunk, list);
		iter->page_idx = 0;
	}
}

static inline dma_addr_t mthca_icm_addr(struct mthca_icm_iter *iter)
{
	return sg_dma_address(&iter->chunk->mem[iter->page_idx]);
}

static inline unsigned long mthca_icm_size(struct mthca_icm_iter *iter)
{
	return sg_dma_len(&iter->chunk->mem[iter->page_idx]);
}

#endif /* MTHCA_MEMFREE_H */
