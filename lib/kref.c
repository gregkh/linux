/*
 * kref.c - library routines for handling generic reference counted objects
 *
 * Copyright (C) 2004 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (C) 2004 IBM Corp.
 *
 * based on lib/kobject.c which was:
 * Copyright (C) 2002-2003 Patrick Mochel <mochel@osdl.org>
 *
 * This file is released under the GPLv2.
 *
 */

#include <linux/kref.h>
#include <linux/module.h>

/**
 * kref_init - initialize object.
 * @kref: object in question.
 */
void kref_init(struct kref *kref)
{
	atomic_set(&kref->refcount,1);
}

/**
 * kref_get - increment refcount for object.
 * @kref: object.
 */
void kref_get(struct kref *kref)
{
	WARN_ON(!atomic_read(&kref->refcount));
	atomic_inc(&kref->refcount);
}

/**
 * kref_put - decrement refcount for object.
 * @kref: object.
 * @release: pointer to the function that will clean up the object when the
 *	     last reference to the object is released.
 *	     This pointer is required, and it is not acceptable to pass kfree
 *	     in as this function.
 *
 * Decrement the refcount, and if 0, call release().
 */
void kref_put(struct kref *kref, void (*release) (struct kref *kref))
{
	WARN_ON(release == NULL);
	WARN_ON(release == (void (*)(struct kref *))kfree);

	if (atomic_dec_and_test(&kref->refcount))
		release(kref);
}

EXPORT_SYMBOL(kref_init);
EXPORT_SYMBOL(kref_get);
EXPORT_SYMBOL(kref_put);
