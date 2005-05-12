#ifndef __M68KNOMMU_MMU_H
#define __M68KNOMMU_MMU_H

/* Copyright (C) 2002, David McCullough <davidm@snapgear.com> */

struct mm_rblock_struct {
	int	size;
	int	refcount;
	void	*kblock;
};

struct mm_tblock_struct {
	struct mm_rblock_struct	*rblock;
	struct mm_tblock_struct	*next;
};

typedef struct {
	struct mm_tblock_struct	tblock;
	unsigned long		end_brk;
} mm_context_t;

#endif /* __M68KNOMMU_MMU_H */
