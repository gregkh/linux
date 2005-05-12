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

#include "mthca_memfree.h"
#include "mthca_dev.h"
#include "mthca_cmd.h"

/*
 * We allocate in as big chunks as we can, up to a maximum of 256 KB
 * per chunk.
 */
enum {
	MTHCA_ICM_ALLOC_SIZE   = 1 << 18,
	MTHCA_TABLE_CHUNK_SIZE = 1 << 18
};

void mthca_free_icm(struct mthca_dev *dev, struct mthca_icm *icm)
{
	struct mthca_icm_chunk *chunk, *tmp;
	int i;

	if (!icm)
		return;

	list_for_each_entry_safe(chunk, tmp, &icm->chunk_list, list) {
		if (chunk->nsg > 0)
			pci_unmap_sg(dev->pdev, chunk->mem, chunk->npages,
				     PCI_DMA_BIDIRECTIONAL);

		for (i = 0; i < chunk->npages; ++i)
			__free_pages(chunk->mem[i].page,
				     get_order(chunk->mem[i].length));

		kfree(chunk);
	}

	kfree(icm);
}

struct mthca_icm *mthca_alloc_icm(struct mthca_dev *dev, int npages,
				  unsigned int gfp_mask)
{
	struct mthca_icm *icm;
	struct mthca_icm_chunk *chunk = NULL;
	int cur_order;

	icm = kmalloc(sizeof *icm, gfp_mask & ~(__GFP_HIGHMEM | __GFP_NOWARN));
	if (!icm)
		return icm;

	INIT_LIST_HEAD(&icm->chunk_list);

	cur_order = get_order(MTHCA_ICM_ALLOC_SIZE);

	while (npages > 0) {
		if (!chunk) {
			chunk = kmalloc(sizeof *chunk,
					gfp_mask & ~(__GFP_HIGHMEM | __GFP_NOWARN));
			if (!chunk)
				goto fail;

			chunk->npages = 0;
			chunk->nsg    = 0;
			list_add_tail(&chunk->list, &icm->chunk_list);
		}

		while (1 << cur_order > npages)
			--cur_order;

		chunk->mem[chunk->npages].page = alloc_pages(gfp_mask, cur_order);
		if (chunk->mem[chunk->npages].page) {
			chunk->mem[chunk->npages].length = PAGE_SIZE << cur_order;
			chunk->mem[chunk->npages].offset = 0;

			if (++chunk->npages == MTHCA_ICM_CHUNK_LEN) {
				chunk->nsg = pci_map_sg(dev->pdev, chunk->mem,
							chunk->npages,
							PCI_DMA_BIDIRECTIONAL);

				if (chunk->nsg <= 0)
					goto fail;

				chunk = NULL;
			}

			npages -= 1 << cur_order;
		} else {
			--cur_order;
			if (cur_order < 0)
				goto fail;
		}
	}

	if (chunk) {
		chunk->nsg = pci_map_sg(dev->pdev, chunk->mem,
					chunk->npages,
					PCI_DMA_BIDIRECTIONAL);

		if (chunk->nsg <= 0)
			goto fail;
	}

	return icm;

fail:
	mthca_free_icm(dev, icm);
	return NULL;
}

struct mthca_icm_table *mthca_alloc_icm_table(struct mthca_dev *dev,
					      u64 virt, unsigned size,
					      unsigned reserved,
					      int use_lowmem)
{
	struct mthca_icm_table *table;
	int num_icm;
	int i;
	u8 status;

	num_icm = size / MTHCA_TABLE_CHUNK_SIZE;

	table = kmalloc(sizeof *table + num_icm * sizeof *table->icm, GFP_KERNEL);
	if (!table)
		return NULL;

	table->virt    = virt;
	table->num_icm = num_icm;
	init_MUTEX(&table->sem);

	for (i = 0; i < num_icm; ++i)
		table->icm[i] = NULL;

	for (i = 0; i < (reserved + MTHCA_TABLE_CHUNK_SIZE - 1) / MTHCA_TABLE_CHUNK_SIZE; ++i) {
		table->icm[i] = mthca_alloc_icm(dev, MTHCA_TABLE_CHUNK_SIZE >> PAGE_SHIFT,
						(use_lowmem ? GFP_KERNEL : GFP_HIGHUSER) |
						__GFP_NOWARN);
		if (!table->icm[i])
			goto err;
		if (mthca_MAP_ICM(dev, table->icm[i], virt + i * MTHCA_TABLE_CHUNK_SIZE,
				  &status) || status) {
			mthca_free_icm(dev, table->icm[i]);
			table->icm[i] = NULL;
			goto err;
		}
	}

	return table;

err:
	for (i = 0; i < num_icm; ++i)
		if (table->icm[i]) {
			mthca_UNMAP_ICM(dev, virt + i * MTHCA_TABLE_CHUNK_SIZE,
					MTHCA_TABLE_CHUNK_SIZE >> 12, &status);
			mthca_free_icm(dev, table->icm[i]);
		}

	kfree(table);

	return NULL;
}

void mthca_free_icm_table(struct mthca_dev *dev, struct mthca_icm_table *table)
{
	int i;
	u8 status;

	for (i = 0; i < table->num_icm; ++i)
		if (table->icm[i]) {
			mthca_UNMAP_ICM(dev, table->virt + i * MTHCA_TABLE_CHUNK_SIZE,
					MTHCA_TABLE_CHUNK_SIZE >> 12, &status);
			mthca_free_icm(dev, table->icm[i]);
		}

	kfree(table);
}
