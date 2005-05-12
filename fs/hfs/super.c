/*
 *  linux/fs/hfs/super.c
 *
 * Copyright (C) 1995-1997  Paul H. Hargrove
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 * This file may be distributed under the terms of the GNU General Public License.
 *
 * This file contains hfs_read_super(), some of the super_ops and
 * init_module() and cleanup_module().	The remaining super_ops are in
 * inode.c since they deal with inodes.
 *
 * Based on the minix file system code, (C) 1991, 1992 by Linus Torvalds
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/vfs.h>

#include "hfs_fs.h"
#include "btree.h"

const char hfs_version[]="0.96";

static kmem_cache_t *hfs_inode_cachep;

MODULE_LICENSE("GPL");

/*
 * hfs_write_super()
 *
 * Description:
 *   This function is called by the VFS only. When the filesystem
 *   is mounted r/w it updates the MDB on disk.
 * Input Variable(s):
 *   struct super_block *sb: Pointer to the hfs superblock
 * Output Variable(s):
 *   NONE
 * Returns:
 *   void
 * Preconditions:
 *   'sb' points to a "valid" (struct super_block).
 * Postconditions:
 *   The MDB is marked 'unsuccessfully unmounted' by clearing bit 8 of drAtrb
 *   (hfs_put_super() must set this flag!). Some MDB fields are updated
 *   and the MDB buffer is written to disk by calling hfs_mdb_commit().
 */
static void hfs_write_super(struct super_block *sb)
{
	sb->s_dirt = 0;
	if (sb->s_flags & MS_RDONLY)
		return;
	/* sync everything to the buffers */
	hfs_mdb_commit(sb);
}

/*
 * hfs_put_super()
 *
 * This is the put_super() entry in the super_operations structure for
 * HFS filesystems.  The purpose is to release the resources
 * associated with the superblock sb.
 */
static void hfs_put_super(struct super_block *sb)
{
	hfs_mdb_close(sb);
	/* release the MDB's resources */
	hfs_mdb_put(sb);
}

/*
 * hfs_statfs()
 *
 * This is the statfs() entry in the super_operations structure for
 * HFS filesystems.  The purpose is to return various data about the
 * filesystem.
 *
 * changed f_files/f_ffree to reflect the fs_ablock/free_ablocks.
 */
static int hfs_statfs(struct super_block *sb, struct kstatfs *buf)
{
	buf->f_type = HFS_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = (u32)HFS_SB(sb)->fs_ablocks * HFS_SB(sb)->fs_div;
	buf->f_bfree = (u32)HFS_SB(sb)->free_ablocks * HFS_SB(sb)->fs_div;
	buf->f_bavail = buf->f_bfree;
	buf->f_files = HFS_SB(sb)->fs_ablocks;
	buf->f_ffree = HFS_SB(sb)->free_ablocks;
	buf->f_namelen = HFS_NAMELEN;

	return 0;
}

int hfs_remount(struct super_block *sb, int *flags, char *data)
{
	*flags |= MS_NODIRATIME;
	if ((*flags & MS_RDONLY) == (sb->s_flags & MS_RDONLY))
		return 0;
	if (!(*flags & MS_RDONLY)) {
		if (!(HFS_SB(sb)->mdb->drAtrb & cpu_to_be16(HFS_SB_ATTRIB_UNMNT))) {
			printk("HFS-fs warning: Filesystem was not cleanly unmounted, "
			       "running fsck.hfs is recommended.  leaving read-only.\n");
			sb->s_flags |= MS_RDONLY;
			*flags |= MS_RDONLY;
		} else if (HFS_SB(sb)->mdb->drAtrb & cpu_to_be16(HFS_SB_ATTRIB_SLOCK)) {
			printk("HFS-fs: Filesystem is marked locked, leaving read-only.\n");
			sb->s_flags |= MS_RDONLY;
			*flags |= MS_RDONLY;
		}
	}
	return 0;
}

static struct inode *hfs_alloc_inode(struct super_block *sb)
{
	struct hfs_inode_info *i;

	i = kmem_cache_alloc(hfs_inode_cachep, SLAB_KERNEL);
	return i ? &i->vfs_inode : NULL;
}

static void hfs_destroy_inode(struct inode *inode)
{
	kmem_cache_free(hfs_inode_cachep, HFS_I(inode));
}

static struct super_operations hfs_super_operations = {
	.alloc_inode	= hfs_alloc_inode,
	.destroy_inode	= hfs_destroy_inode,
	.write_inode	= hfs_write_inode,
	.clear_inode	= hfs_clear_inode,
	.put_super	= hfs_put_super,
	.write_super	= hfs_write_super,
	.statfs		= hfs_statfs,
	.remount_fs     = hfs_remount,
};

/*
 * parse_options()
 *
 * adapted from linux/fs/msdos/inode.c written 1992,93 by Werner Almesberger
 * This function is called by hfs_read_super() to parse the mount options.
 */
static int parse_options(char *options, struct hfs_sb_info *hsb)
{
	char *this_char, *value;

	/* initialize the sb with defaults */
	hsb->s_uid = current->uid;
	hsb->s_gid = current->gid;
	hsb->s_file_umask = 0644;
	hsb->s_dir_umask = 0755;
	hsb->s_type = hsb->s_creator = cpu_to_be32(0x3f3f3f3f);	/* == '????' */
	hsb->s_quiet = 0;
	hsb->part = -1;
	hsb->session = -1;

	if (!options)
		return 1;

	while ((this_char = strsep(&options, ",")) != 0) {
		if (!*this_char)
			continue;
		value = strchr(this_char, '=');
		if (value)
			*value++ = 0;

	/* Numeric-valued options */
		if (!strcmp(this_char, "uid")) {
			if (!value || !*value)
				return 0;
			hsb->s_uid = simple_strtoul(value, &value, 0);
			if (*value)
				return 0;
		} else if (!strcmp(this_char, "gid")) {
			if (!value || !*value)
				return 0;
			hsb->s_gid = simple_strtoul(value, &value, 0);
			if (*value)
				return 0;
		} else if (!strcmp(this_char, "umask")) {
			if (!value || !*value)
				return 0;
			hsb->s_file_umask = simple_strtoul(value, &value, 8);
			hsb->s_dir_umask = hsb->s_file_umask;
			if (*value)
				return 0;
		} else if (!strcmp(this_char, "file_umask")) {
			if (!value || !*value)
				return 0;
			hsb->s_file_umask = simple_strtoul(value, &value, 8);
			if (*value)
				return 0;
		} else if (!strcmp(this_char, "dir_umask")) {
			if (!value || !*value)
				return 0;
			hsb->s_dir_umask = simple_strtoul(value, &value, 8);
			if (*value)
				return 0;
		} else if (!strcmp(this_char, "part")) {
			if (!value || !*value)
				return 0;
			hsb->part = simple_strtoul(value, &value, 0);
			if (*value)
				return 0;
		} else if (!strcmp(this_char, "session")) {
			if (!value || !*value)
				return 0;
			hsb->session = simple_strtoul(value, &value, 0);
			if (*value)
				return 0;
	/* String-valued options */
		} else if (!strcmp(this_char, "type") && value) {
			if (strlen(value) != 4)
				return 0;
			memcpy(&hsb->s_type, value, 4);
		} else if (!strcmp(this_char, "creator") && value) {
			if (strlen(value) != 4)
				return 0;
			memcpy(&hsb->s_creator, value, 4);
	/* Boolean-valued options */
		} else if (!strcmp(this_char, "quiet")) {
			if (value)
				return 0;
			hsb->s_quiet = 1;
		} else
			return 0;
	}

	hsb->s_dir_umask &= 0777;
	hsb->s_file_umask &= 0777;

	return 1;
}

/*
 * hfs_read_super()
 *
 * This is the function that is responsible for mounting an HFS
 * filesystem.	It performs all the tasks necessary to get enough data
 * from the disk to read the root inode.  This includes parsing the
 * mount options, dealing with Macintosh partitions, reading the
 * superblock and the allocation bitmap blocks, calling
 * hfs_btree_init() to get the necessary data about the extents and
 * catalog B-trees and, finally, reading the root inode into memory.
 */
static int hfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct hfs_sb_info *sbi;
	struct hfs_find_data fd;
	hfs_cat_rec rec;
	struct inode *root_inode;
	int res;

	sbi = kmalloc(sizeof(struct hfs_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;
	memset(sbi, 0, sizeof(struct hfs_sb_info));
	INIT_HLIST_HEAD(&sbi->rsrc_inodes);

	res = -EINVAL;
	if (!parse_options((char *)data, sbi)) {
		hfs_warn("hfs_fs: unable to parse mount options.\n");
		goto bail3;
	}

	sb->s_op = &hfs_super_operations;
	sb->s_flags |= MS_NODIRATIME;
	init_MUTEX(&sbi->bitmap_lock);

	res = hfs_mdb_get(sb);
	if (res) {
		if (!silent)
			hfs_warn("VFS: Can't find a HFS filesystem on dev %s.\n",
				hfs_mdb_name(sb));
		res = -EINVAL;
		goto bail2;
	}

	/* try to get the root inode */
	hfs_find_init(HFS_SB(sb)->cat_tree, &fd);
	res = hfs_cat_find_brec(sb, HFS_ROOT_CNID, &fd);
	if (!res)
		hfs_bnode_read(fd.bnode, &rec, fd.entryoffset, fd.entrylength);
	if (res) {
		hfs_find_exit(&fd);
		goto bail_no_root;
	}
	root_inode = hfs_iget(sb, &fd.search_key->cat, &rec);
	hfs_find_exit(&fd);
	if (!root_inode)
		goto bail_no_root;

	sb->s_root = d_alloc_root(root_inode);
	if (!sb->s_root)
		goto bail_iput;

	sb->s_root->d_op = &hfs_dentry_operations;

	/* everything's okay */
	return 0;

bail_iput:
	iput(root_inode);
bail_no_root:
	hfs_warn("hfs_fs: get root inode failed.\n");
	hfs_mdb_put(sb);
bail2:
bail3:
	kfree(sbi);
	return res;
}

static struct super_block *hfs_get_sb(struct file_system_type *fs_type,
				      int flags, const char *dev_name, void *data)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, hfs_fill_super);
}

static struct file_system_type hfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "hfs",
	.get_sb		= hfs_get_sb,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static void hfs_init_once(void *p, kmem_cache_t *cachep, unsigned long flags)
{
	struct hfs_inode_info *i = p;

	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) == SLAB_CTOR_CONSTRUCTOR)
		inode_init_once(&i->vfs_inode);
}

static int __init init_hfs_fs(void)
{
	int err;

	hfs_inode_cachep = kmem_cache_create("hfs_inode_cache",
		sizeof(struct hfs_inode_info), 0, SLAB_HWCACHE_ALIGN,
		hfs_init_once, NULL);
	if (!hfs_inode_cachep)
		return -ENOMEM;
	err = register_filesystem(&hfs_fs_type);
	if (err)
		kmem_cache_destroy(hfs_inode_cachep);
	return err;
}

static void __exit exit_hfs_fs(void)
{
	unregister_filesystem(&hfs_fs_type);
	if (kmem_cache_destroy(hfs_inode_cachep))
		printk(KERN_INFO "hfs_inode_cache: not all structures were freed\n");
}

module_init(init_hfs_fs)
module_exit(exit_hfs_fs)
