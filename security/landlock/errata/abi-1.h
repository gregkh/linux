/* SPDX-License-Identifier: GPL-2.0-only */

/**
 * DOC: erratum_4
 *
 * Erratum 4: Creation of whiteout objects
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This fix changes the access rights required for the creation of whiteout
 * objects through :manpage:`mknod(2)`, :manpage:`renameat2(2)`, or
 * :manpage:`link(2)`.  Creating whiteout objects is now guarded by
 * ``LANDLOCK_ACCESS_FS_MAKE_REG`` instead of ``LANDLOCK_ACCESS_FS_MAKE_CHAR``.
 *
 * Whiteout objects are used in OverlayFS to mark the absence of a file in an
 * upper file system.  Despite being created with ``S_IFCHR``, whiteout objects
 * do not count as character devices.
 *
 * Impact:
 *
 * Sandboxed programs that create OverlayFS whiteouts (such as fuse-overlayfs)
 * now require ``LANDLOCK_ACCESS_FS_MAKE_REG`` instead of
 * ``LANDLOCK_ACCESS_FS_MAKE_CHAR``.
 */
LANDLOCK_ERRATUM(4)
