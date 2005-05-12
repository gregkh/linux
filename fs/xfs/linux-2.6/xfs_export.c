/*
 * Copyright (c) 2004-2005 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Further, this software is distributed without any warranty that it is
 * free of the rightful claim of any third person regarding infringement
 * or the like.  Any license provided herein, whether implied or
 * otherwise, applies only to this software file.  Patent licenses, if
 * any, provided herein do not apply to combinations of this program with
 * other software, or any other product whatsoever.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * Contact information: Silicon Graphics, Inc., 1600 Amphitheatre Pkwy,
 * Mountain View, CA  94043, or:
 *
 * http://www.sgi.com
 *
 * For further information regarding this notice, see:
 *
 * http://oss.sgi.com/projects/GenInfo/SGIGPLNoticeExplan/
 */

#include "xfs.h"


STATIC struct dentry *
linvfs_decode_fh(
	struct super_block	*sb,
	__u32			*fh,
	int			fh_len,
	int			fileid_type,
	int (*acceptable)(
		void		*context,
		struct dentry	*de),
	void			*context)
{
	__u32 parent[2];
	parent[0] = parent[1] = 0;
	
	if (fh_len < 2 || fileid_type > 2)
		return NULL;
	
	if (fileid_type == 2 && fh_len > 2) {
		if (fh_len == 3) {
			printk(KERN_WARNING
			       "XFS: detected filehandle without "
			       "parent inode generation information.");
			return ERR_PTR(-ESTALE);
		}
			
		parent[0] = fh[2];
		parent[1] = fh[3];
	}
	
	return find_exported_dentry(sb, fh, parent, acceptable, context);

}

STATIC struct dentry *
linvfs_get_dentry(
	struct super_block	*sb,
	void			*data)
{
	vnode_t			*vp;
	struct inode		*inode;
	struct dentry		*result;
	xfs_fid2_t		xfid;
	vfs_t			*vfsp = LINVFS_GET_VFS(sb);
	int			error;

	xfid.fid_len = sizeof(xfs_fid2_t) - sizeof(xfid.fid_len);
	xfid.fid_pad = 0;
	xfid.fid_gen = ((__u32 *)data)[1];
	xfid.fid_ino = ((__u32 *)data)[0];

	VFS_VGET(vfsp, &vp, (fid_t *)&xfid, error);
	if (error || vp == NULL)
		return ERR_PTR(-ESTALE) ;

	inode = LINVFS_GET_IP(vp);
	result = d_alloc_anon(inode);
        if (!result) {
		iput(inode);
		return ERR_PTR(-ENOMEM);
	}
	return result;
}

STATIC struct dentry *
linvfs_get_parent(
	struct dentry		*child)
{
	int			error;
	vnode_t			*vp, *cvp;
	struct dentry		*parent;
	struct dentry		dotdot;

	dotdot.d_name.name = "..";
	dotdot.d_name.len = 2;
	dotdot.d_inode = NULL;

	cvp = NULL;
	vp = LINVFS_GET_VP(child->d_inode);
	VOP_LOOKUP(vp, &dotdot, &cvp, 0, NULL, NULL, error);
	if (unlikely(error))
		return ERR_PTR(-error);

	parent = d_alloc_anon(LINVFS_GET_IP(cvp));
	if (unlikely(!parent)) {
		VN_RELE(cvp);
		return ERR_PTR(-ENOMEM);
	}
	return parent;
}

struct export_operations linvfs_export_ops = {
	.decode_fh		= linvfs_decode_fh,
	.get_parent		= linvfs_get_parent,
	.get_dentry		= linvfs_get_dentry,
};
