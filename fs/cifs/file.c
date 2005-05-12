/*
 *   fs/cifs/file.c
 *
 *   vfs operations that deal with files
 * 
 *   Copyright (C) International Business Machines  Corp., 2002,2003
 *   Author(s): Steve French (sfrench@us.ibm.com)
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published
 *   by the Free Software Foundation; either version 2.1 of the License, or
 *   (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/smp_lock.h>
#include <asm/div64.h>
#include "cifsfs.h"
#include "cifspdu.h"
#include "cifsglob.h"
#include "cifsproto.h"
#include "cifs_unicode.h"
#include "cifs_debug.h"
#include "cifs_fs_sb.h"

extern int cifs_readdir2(struct file *file, void *direntry, filldir_t filldir); /* BB removeme BB */

int
cifs_open(struct inode *inode, struct file *file)
{
	int rc = -EACCES;
	int xid, oplock;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	struct cifsFileInfo *pCifsFile;
	struct cifsInodeInfo *pCifsInode;
	struct list_head * tmp;
	char *full_path = NULL;
	int desiredAccess = 0x20197;
	int disposition;
	__u16 netfid;
	FILE_ALL_INFO * buf = NULL;

	xid = GetXid();

	cifs_sb = CIFS_SB(inode->i_sb);
	pTcon = cifs_sb->tcon;

	if (file->f_flags & O_CREAT) {
		/* search inode for this file and fill in file->private_data = */
		pCifsInode = CIFS_I(file->f_dentry->d_inode);
		read_lock(&GlobalSMBSeslock);
		list_for_each(tmp, &pCifsInode->openFileList) {            
			pCifsFile = list_entry(tmp,struct cifsFileInfo, flist);           
			if((pCifsFile->pfile == NULL)&& (pCifsFile->pid == current->tgid)){
			/* mode set in cifs_create */
				pCifsFile->pfile = file; /* needed for writepage */
				file->private_data = pCifsFile;
				break;
			}
		}
		read_unlock(&GlobalSMBSeslock);
		if(file->private_data != NULL) {
			rc = 0;
			FreeXid(xid);
			return rc;
		} else {
			if(file->f_flags & O_EXCL)
				cERROR(1,("could not find file instance for new file %p ",file));
		}
	}

	down(&inode->i_sb->s_vfs_rename_sem);
	full_path = build_path_from_dentry(file->f_dentry);
	up(&inode->i_sb->s_vfs_rename_sem);
	if(full_path == NULL) {
		FreeXid(xid);
		return -ENOMEM;
	}

	cFYI(1, (" inode = 0x%p file flags are 0x%x for %s", inode, file->f_flags,full_path));
	if ((file->f_flags & O_ACCMODE) == O_RDONLY)
		desiredAccess = GENERIC_READ;
	else if ((file->f_flags & O_ACCMODE) == O_WRONLY)
		desiredAccess = GENERIC_WRITE;
	else if ((file->f_flags & O_ACCMODE) == O_RDWR) {
		/* GENERIC_ALL is too much permission to request */
		/* can cause unnecessary access denied on create */
		/* desiredAccess = GENERIC_ALL; */
		desiredAccess = GENERIC_READ | GENERIC_WRITE;
	}

/*********************************************************************
 *  open flag mapping table:
 *  
 *	POSIX Flag            CIFS Disposition
 *	----------            ---------------- 
 *	O_CREAT               FILE_OPEN_IF
 *	O_CREAT | O_EXCL      FILE_CREATE
 *	O_CREAT | O_TRUNC     FILE_OVERWRITE_IF
 *	O_TRUNC               FILE_OVERWRITE
 *	none of the above     FILE_OPEN
 *
 *	Note that there is not a direct match between disposition
 *	FILE_SUPERSEDE (ie create whether or not file exists although 
 *	O_CREAT | O_TRUNC is similar but truncates the existing
 *	file rather than creating a new file as FILE_SUPERSEDE does
 *	(which uses the attributes / metadata passed in on open call)
 *?
 *?  O_SYNC is a reasonable match to CIFS writethrough flag  
 *?  and the read write flags match reasonably.  O_LARGEFILE
 *?  is irrelevant because largefile support is always used
 *?  by this client. Flags O_APPEND, O_DIRECT, O_DIRECTORY,
 *	 O_FASYNC, O_NOFOLLOW, O_NONBLOCK need further investigation
 *********************************************************************/

	if((file->f_flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
		disposition = FILE_CREATE;
	else if((file->f_flags & (O_CREAT | O_TRUNC)) == (O_CREAT | O_TRUNC))
		disposition = FILE_OVERWRITE_IF;
	else if((file->f_flags & O_CREAT) == O_CREAT)
		disposition = FILE_OPEN_IF;
	else
		disposition = FILE_OPEN;

	if (oplockEnabled)
		oplock = REQ_OPLOCK;
	else
		oplock = FALSE;

	/* BB pass O_SYNC flag through on file attributes .. BB */

	/* Also refresh inode by passing in file_info buf returned by SMBOpen 
	   and calling get_inode_info with returned buf (at least 
	   helps non-Unix server case */

	/* BB we can not do this if this is the second open of a file 
	and the first handle has writebehind data, we might be 
	able to simply do a filemap_fdatawrite/filemap_fdatawait first */
	buf = kmalloc(sizeof(FILE_ALL_INFO),GFP_KERNEL);
	if(buf== NULL) {
		if (full_path)
			kfree(full_path);
		FreeXid(xid);
		return -ENOMEM;
	}
	rc = CIFSSMBOpen(xid, pTcon, full_path, disposition, desiredAccess,
			CREATE_NOT_DIR, &netfid, &oplock, buf, cifs_sb->local_nls);
	if (rc) {
		cFYI(1, ("cifs_open returned 0x%x ", rc));
		cFYI(1, ("oplock: %d ", oplock));	
	} else {
		file->private_data =
			kmalloc(sizeof (struct cifsFileInfo), GFP_KERNEL);
		if (file->private_data) {
			memset(file->private_data, 0, sizeof(struct cifsFileInfo));
			pCifsFile = (struct cifsFileInfo *) file->private_data;
			pCifsFile->netfid = netfid;
			pCifsFile->pid = current->tgid;
			init_MUTEX(&pCifsFile->fh_sem);
			pCifsFile->pfile = file; /* needed for writepage */
			pCifsFile->pInode = inode;
			pCifsFile->invalidHandle = FALSE;
			pCifsFile->closePend     = FALSE;
			write_lock(&file->f_owner.lock);
			write_lock(&GlobalSMBSeslock);
			list_add(&pCifsFile->tlist,&pTcon->openFileList);
			pCifsInode = CIFS_I(file->f_dentry->d_inode);
			if(pCifsInode) {
				/* want handles we can use to read with first */
				/* in the list so we do not have to walk the */
				/* list to search for one in prepare_write */
				if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
					list_add_tail(&pCifsFile->flist,&pCifsInode->openFileList);
				} else {
					list_add(&pCifsFile->flist,&pCifsInode->openFileList);
				}
				write_unlock(&GlobalSMBSeslock);
				write_unlock(&file->f_owner.lock);
				if(pCifsInode->clientCanCacheRead) {
					/* we have the inode open somewhere else
					   no need to discard cache data */
				} else {
					if(buf) {
					/* BB need same check in cifs_create too? */

					/* if not oplocked, invalidate inode pages if mtime 
					   or file size changed */
						struct timespec temp;
						temp = cifs_NTtimeToUnix(le64_to_cpu(buf->LastWriteTime));
						if(timespec_equal(&file->f_dentry->d_inode->i_mtime,&temp) && 
							(file->f_dentry->d_inode->i_size == (loff_t)le64_to_cpu(buf->EndOfFile))) {
							cFYI(1,("inode unchanged on server"));
						} else {
							if(file->f_dentry->d_inode->i_mapping) {
							/* BB no need to lock inode until after invalidate*/
							/* since namei code should already have it locked?*/
								filemap_fdatawrite(file->f_dentry->d_inode->i_mapping);
								filemap_fdatawait(file->f_dentry->d_inode->i_mapping);
							}
							cFYI(1,("invalidating remote inode since open detected it changed"));
							invalidate_remote_inode(file->f_dentry->d_inode);
						}
					}
				}
				if (pTcon->ses->capabilities & CAP_UNIX)
					rc = cifs_get_inode_info_unix(&file->f_dentry->d_inode,
						full_path, inode->i_sb,xid);
				else
					rc = cifs_get_inode_info(&file->f_dentry->d_inode,
						full_path, buf, inode->i_sb,xid);

				if((oplock & 0xF) == OPLOCK_EXCLUSIVE) {
					pCifsInode->clientCanCacheAll = TRUE;
					pCifsInode->clientCanCacheRead = TRUE;
					cFYI(1,("Exclusive Oplock granted on inode %p",file->f_dentry->d_inode));
				} else if((oplock & 0xF) == OPLOCK_READ)
					pCifsInode->clientCanCacheRead = TRUE;
			} else {
				write_unlock(&GlobalSMBSeslock);
				write_unlock(&file->f_owner.lock);
			}
			if(oplock & CIFS_CREATE_ACTION) {           
				/* time to set mode which we can not set earlier due
				 to problems creating new read-only files */
				if (cifs_sb->tcon->ses->capabilities & CAP_UNIX)                
					CIFSSMBUnixSetPerms(xid, pTcon, full_path, inode->i_mode,
						(__u64)-1, 
						(__u64)-1,
						0 /* dev */,
						cifs_sb->local_nls);
				else {/* BB implement via Windows security descriptors */
			/* eg CIFSSMBWinSetPerms(xid,pTcon,full_path,mode,-1,-1,local_nls);*/
			/* in the meantime could set r/o dos attribute when perms are eg:
					mode & 0222 == 0 */
				}
			}
		}
	}

	if (buf)
		kfree(buf);
	if (full_path)
		kfree(full_path);
	FreeXid(xid);
	return rc;
}

/* Try to reaquire byte range locks that were released when session */
/* to server was lost */
static int cifs_relock_file(struct cifsFileInfo * cifsFile)
{
	int rc = 0;

/* BB list all locks open on this file and relock */

	return rc;
}

static int cifs_reopen_file(struct inode *inode, struct file *file, int can_flush)
{
	int rc = -EACCES;
	int xid, oplock;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	struct cifsFileInfo *pCifsFile;
	struct cifsInodeInfo *pCifsInode;
	char *full_path = NULL;
	int desiredAccess = 0x20197;
	int disposition = FILE_OPEN;
	__u16 netfid;

	if(inode == NULL)
		return -EBADF;
	if (file->private_data) {
		pCifsFile = (struct cifsFileInfo *) file->private_data;
	} else
		return -EBADF;

	xid = GetXid();
	down(&pCifsFile->fh_sem);
	if(pCifsFile->invalidHandle == FALSE) {
		up(&pCifsFile->fh_sem);
		FreeXid(xid);
		return 0;
	}

	if(file->f_dentry == NULL) {
		up(&pCifsFile->fh_sem);
		cFYI(1,("failed file reopen, no valid name if dentry freed"));
		FreeXid(xid);
		return -EBADF;
	}
	cifs_sb = CIFS_SB(inode->i_sb);
	pTcon = cifs_sb->tcon;
/* can not grab rename sem here because various ops, including
those that already have the rename sem can end up causing writepage
to get called and if the server was down that means we end up here,
and we can never tell if the caller already has the rename_sem */
	full_path = build_path_from_dentry(file->f_dentry);
	if(full_path == NULL) {
		up(&pCifsFile->fh_sem);
		FreeXid(xid);
		return -ENOMEM;
	}

	cFYI(1, (" inode = 0x%p file flags are 0x%x for %s", inode, file->f_flags,full_path));
	if ((file->f_flags & O_ACCMODE) == O_RDONLY)
		desiredAccess = GENERIC_READ;
	else if ((file->f_flags & O_ACCMODE) == O_WRONLY)
		desiredAccess = GENERIC_WRITE;
	else if ((file->f_flags & O_ACCMODE) == O_RDWR) {
		/* GENERIC_ALL is too much permission to request */
		/* can cause unnecessary access denied on create */
		/* desiredAccess = GENERIC_ALL; */
		desiredAccess = GENERIC_READ | GENERIC_WRITE;
	}

	if (oplockEnabled)
		oplock = REQ_OPLOCK;
	else
		oplock = FALSE;

	
	/* Can not refresh inode by passing in file_info buf to be returned
	 by SMBOpen and then calling get_inode_info with returned buf 
	 since file might have write behind data that needs to be flushed 
	 and server version of file size can be stale. If we 
	 knew for sure that inode was not dirty locally we could do this */

/*	buf = kmalloc(sizeof(FILE_ALL_INFO),GFP_KERNEL);
	if(buf==0) {
		up(&pCifsFile->fh_sem);
		if (full_path)
			kfree(full_path);
		FreeXid(xid);
		return -ENOMEM;
	}*/
	rc = CIFSSMBOpen(xid, pTcon, full_path, disposition, desiredAccess,
				CREATE_NOT_DIR, &netfid, &oplock, NULL, cifs_sb->local_nls);
	if (rc) {
		up(&pCifsFile->fh_sem);
		cFYI(1, ("cifs_open returned 0x%x ", rc));
		cFYI(1, ("oplock: %d ", oplock));
	} else {
		pCifsFile->netfid = netfid;
		pCifsFile->invalidHandle = FALSE;
		up(&pCifsFile->fh_sem);
		pCifsInode = CIFS_I(inode);
		if(pCifsInode) {
			if(can_flush) {
				filemap_fdatawrite(inode->i_mapping);
				filemap_fdatawait(inode->i_mapping);
			/* temporarily disable caching while we
			go to server to get inode info */
				pCifsInode->clientCanCacheAll = FALSE;
				pCifsInode->clientCanCacheRead = FALSE;
				if (pTcon->ses->capabilities & CAP_UNIX)
					rc = cifs_get_inode_info_unix(&inode,
						full_path, inode->i_sb,xid);
				else
					rc = cifs_get_inode_info(&inode,
						full_path, NULL, inode->i_sb,xid);
			} /* else we are writing out data to server already
			and could deadlock if we tried to flush data, and 
			since we do not know if we have data that would
			invalidate the current end of file on the server
			we can not go to the server to get the new
			inod info */
			if((oplock & 0xF) == OPLOCK_EXCLUSIVE) {
				pCifsInode->clientCanCacheAll =  TRUE;
				pCifsInode->clientCanCacheRead = TRUE;
				cFYI(1,("Exclusive Oplock granted on inode %p",file->f_dentry->d_inode));
			} else if((oplock & 0xF) == OPLOCK_READ) {
				pCifsInode->clientCanCacheRead = TRUE;
				pCifsInode->clientCanCacheAll =  FALSE;
			} else {
				pCifsInode->clientCanCacheRead = FALSE;
				pCifsInode->clientCanCacheAll =  FALSE;
			}
			cifs_relock_file(pCifsFile);
		}
	}

	if (full_path)
		kfree(full_path);
	FreeXid(xid);
	return rc;
}

int
cifs_close(struct inode *inode, struct file *file)
{
	int rc = 0;
	int xid;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	struct cifsFileInfo *pSMBFile =
		(struct cifsFileInfo *) file->private_data;

	xid = GetXid();

	cifs_sb = CIFS_SB(inode->i_sb);
	pTcon = cifs_sb->tcon;
	if (pSMBFile) {
		pSMBFile->closePend    = TRUE;
		write_lock(&file->f_owner.lock);
		if(pTcon) {
			/* no sense reconnecting to close a file that is
				already closed */
			if (pTcon->tidStatus != CifsNeedReconnect) {
				write_unlock(&file->f_owner.lock);
				rc = CIFSSMBClose(xid,pTcon,pSMBFile->netfid);
				write_lock(&file->f_owner.lock);
			}
		}
		list_del(&pSMBFile->flist);
		list_del(&pSMBFile->tlist);
		write_unlock(&file->f_owner.lock);
		if(pSMBFile->search_resume_name)
			kfree(pSMBFile->search_resume_name);
		kfree(file->private_data);
		file->private_data = NULL;
	} else
		rc = -EBADF;

	if(list_empty(&(CIFS_I(inode)->openFileList))) {
		cFYI(1,("closing last open instance for inode %p",inode));
		/* if the file is not open we do not know if we can cache
		info on this inode, much less write behind and read ahead */
		CIFS_I(inode)->clientCanCacheRead = FALSE;
		CIFS_I(inode)->clientCanCacheAll  = FALSE;
	}
	if((rc ==0) && CIFS_I(inode)->write_behind_rc)
		rc = CIFS_I(inode)->write_behind_rc;
	FreeXid(xid);
	return rc;
}

int
cifs_closedir(struct inode *inode, struct file *file)
{
	int rc = 0;
	int xid;
	struct cifsFileInfo *pCFileStruct =
	    (struct cifsFileInfo *) file->private_data;
	char * ptmp;

	cFYI(1, ("Closedir inode = 0x%p with ", inode));

	xid = GetXid();

	if (pCFileStruct) {
		struct cifsTconInfo *pTcon;
		struct cifs_sb_info * cifs_sb = CIFS_SB(file->f_dentry->d_sb);

		pTcon = cifs_sb->tcon;

		cFYI(1, ("Freeing private data in close dir"));
		if(pCFileStruct->srch_inf.endOfSearch == FALSE) {
			pCFileStruct->invalidHandle = TRUE;
			rc = CIFSFindClose(xid, pTcon, pCFileStruct->netfid);
			cFYI(1,("Closing uncompleted readdir with rc %d",rc));
			/* not much we can do if it fails anywway, ignore rc */
			rc = 0;
		}
		ptmp = pCFileStruct->srch_inf.ntwrk_buf_start;
		if(ptmp) {
			cFYI(1,("freeing smb buf in srch struct in closedir")); /* BB removeme BB */
			pCFileStruct->srch_inf.ntwrk_buf_start = NULL;
			cifs_buf_release(ptmp);
		}
		ptmp = pCFileStruct->search_resume_name;
		if(ptmp) {
			cFYI(1,("freeing resume name in closedir")); /* BB removeme BB */
			pCFileStruct->search_resume_name = NULL;
			kfree(ptmp);
		}
		kfree(file->private_data);
		file->private_data = NULL;
	}
	/* BB can we lock the filestruct while this is going on? */
	FreeXid(xid);
	return rc;
}

int
cifs_lock(struct file *file, int cmd, struct file_lock *pfLock)
{
	int rc, xid;
	__u32 lockType = LOCKING_ANDX_LARGE_FILES;
	__u32 numLock = 0;
	__u32 numUnlock = 0;
	__u64 length;
	int wait_flag = FALSE;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	length = 1 + pfLock->fl_end - pfLock->fl_start;

	rc = -EACCES;

	xid = GetXid();

	cFYI(1,
	     ("Lock parm: 0x%x flockflags: 0x%x flocktype: 0x%x start: %lld end: %lld",
	      cmd, pfLock->fl_flags, pfLock->fl_type, pfLock->fl_start,
	      pfLock->fl_end));

	if (pfLock->fl_flags & FL_POSIX)
		cFYI(1, ("Posix "));
	if (pfLock->fl_flags & FL_FLOCK)
		cFYI(1, ("Flock "));
	if (pfLock->fl_flags & FL_SLEEP) {
		cFYI(1, ("Blocking lock "));
		wait_flag = TRUE;
	}
	if (pfLock->fl_flags & FL_ACCESS)
		cFYI(1, ("Process suspended by mandatory locking - not implemented yet "));
	if (pfLock->fl_flags & FL_LEASE)
		cFYI(1, ("Lease on file - not implemented yet"));
	if (pfLock->fl_flags & (~(FL_POSIX | FL_FLOCK | FL_SLEEP | FL_ACCESS | FL_LEASE)))
		cFYI(1, ("Unknown lock flags 0x%x",pfLock->fl_flags));

	if (pfLock->fl_type == F_WRLCK) {
		cFYI(1, ("F_WRLCK "));
		numLock = 1;
	} else if (pfLock->fl_type == F_UNLCK) {
		cFYI(1, ("F_UNLCK "));
		numUnlock = 1;
	} else if (pfLock->fl_type == F_RDLCK) {
		cFYI(1, ("F_RDLCK "));
		lockType |= LOCKING_ANDX_SHARED_LOCK;
		numLock = 1;
	} else if (pfLock->fl_type == F_EXLCK) {
		cFYI(1, ("F_EXLCK "));
		numLock = 1;
	} else if (pfLock->fl_type == F_SHLCK) {
		cFYI(1, ("F_SHLCK "));
		lockType |= LOCKING_ANDX_SHARED_LOCK;
		numLock = 1;
	} else
		cFYI(1, ("Unknown type of lock "));

	cifs_sb = CIFS_SB(file->f_dentry->d_sb);
	pTcon = cifs_sb->tcon;

	if (file->private_data == NULL) {
		FreeXid(xid);
		return -EBADF;
	}

	if (IS_GETLK(cmd)) {
		rc = CIFSSMBLock(xid, pTcon,
				 ((struct cifsFileInfo *) file->
				  private_data)->netfid,
				 length,
				 pfLock->fl_start, 0, 1, lockType,
				 0 /* wait flag */ );
		if (rc == 0) {
			rc = CIFSSMBLock(xid, pTcon,
					 ((struct cifsFileInfo *) file->
					  private_data)->netfid,
					 length,
					 pfLock->fl_start, 1 /* numUnlock */ ,
					 0 /* numLock */ , lockType,
					 0 /* wait flag */ );
			pfLock->fl_type = F_UNLCK;
			if (rc != 0)
				cERROR(1,
					("Error unlocking previously locked range %d during test of lock ",
					rc));
			rc = 0;

		} else {
			/* if rc == ERR_SHARING_VIOLATION ? */
			rc = 0;	/* do not change lock type to unlock since range in use */
		}

		FreeXid(xid);
		return rc;
	}

	rc = CIFSSMBLock(xid, pTcon,
			 ((struct cifsFileInfo *) file->private_data)->
			 netfid, length,
			 pfLock->fl_start, numUnlock, numLock, lockType,
			 wait_flag);
	if (rc == 0 && (pfLock->fl_flags & FL_POSIX))
		posix_lock_file_wait(file, pfLock);
	FreeXid(xid);
	return rc;
}

ssize_t
cifs_user_write(struct file * file, const char __user * write_data,
	   size_t write_size, loff_t * poffset)
{
	int rc = 0;
	unsigned int bytes_written = 0;
	unsigned int total_written;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	int xid, long_op;
	struct cifsFileInfo * open_file;

	if(file->f_dentry == NULL)
		return -EBADF;

	cifs_sb = CIFS_SB(file->f_dentry->d_sb);
	if(cifs_sb == NULL) {
		return -EBADF;
	}
	pTcon = cifs_sb->tcon;

	/*cFYI(1,
	   (" write %d bytes to offset %lld of %s", write_size,
	   *poffset, file->f_dentry->d_name.name)); */

	if (file->private_data == NULL) {
		return -EBADF;
	} else {
		open_file = (struct cifsFileInfo *) file->private_data;
	}
	
	xid = GetXid();
	if(file->f_dentry->d_inode == NULL) {
		FreeXid(xid);
		return -EBADF;
	}

	if (*poffset > file->f_dentry->d_inode->i_size)
		long_op = 2;  /* writes past end of file can take a long time */
	else
		long_op = 1;

	for (total_written = 0; write_size > total_written;
	     total_written += bytes_written) {
		rc = -EAGAIN;
		while(rc == -EAGAIN) {
			if(file->private_data == NULL) {
				/* file has been closed on us */
				FreeXid(xid);
			/* if we have gotten here we have written some data
			and blocked, and the file has been freed on us
			while we blocked so return what we managed to write */
				return total_written;
			} 
			if(open_file->closePend) {
				FreeXid(xid);
				if(total_written)
					return total_written;
				else
					return -EBADF;
			}
			if (open_file->invalidHandle) {
				if((file->f_dentry == NULL) ||
				   (file->f_dentry->d_inode == NULL)) {
					FreeXid(xid);
					return total_written;
				}
				/* we could deadlock if we called
				 filemap_fdatawait from here so tell
				reopen_file not to flush data to server now */
				rc = cifs_reopen_file(file->f_dentry->d_inode,
					file,FALSE);
				if(rc != 0)
					break;
			}

			rc = CIFSSMBWrite(xid, pTcon,
				  open_file->netfid,
				  write_size - total_written, *poffset,
				  &bytes_written,
				  NULL, write_data + total_written, long_op);
		}
		if (rc || (bytes_written == 0)) {
			if (total_written)
				break;
			else {
				FreeXid(xid);
				return rc;
			}
		} else
			*poffset += bytes_written;
		long_op = FALSE; /* subsequent writes fast - 15 seconds is plenty */
	}

#ifdef CONFIG_CIFS_STATS
	if(total_written > 0) {
		atomic_inc(&pTcon->num_writes);
		spin_lock(&pTcon->stat_lock);
		pTcon->bytes_written += total_written;
		spin_unlock(&pTcon->stat_lock);
	}
#endif		

	/* since the write may have blocked check these pointers again */
	if(file->f_dentry) {
		if(file->f_dentry->d_inode) {
			struct inode *inode = file->f_dentry->d_inode;
			inode->i_ctime = inode->i_mtime =
				current_fs_time(inode->i_sb);
			if (total_written > 0) {
				if (*poffset > file->f_dentry->d_inode->i_size)
					i_size_write(file->f_dentry->d_inode, *poffset);
			}
			mark_inode_dirty_sync(file->f_dentry->d_inode);
		}
	}
	FreeXid(xid);
	return total_written;
}

static ssize_t
cifs_write(struct file * file, const char *write_data,
	   size_t write_size, loff_t * poffset)
{
	int rc = 0;
	unsigned int bytes_written = 0;
	unsigned int total_written;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	int xid, long_op;
	struct cifsFileInfo * open_file;

	if(file->f_dentry == NULL)
		return -EBADF;

	cifs_sb = CIFS_SB(file->f_dentry->d_sb);
	if(cifs_sb == NULL) {
		return -EBADF;
	}
	pTcon = cifs_sb->tcon;

	/*cFYI(1,
	   (" write %d bytes to offset %lld of %s", write_size,
	   *poffset, file->f_dentry->d_name.name)); */

	if (file->private_data == NULL) {
		return -EBADF;
	} else {
		open_file = (struct cifsFileInfo *) file->private_data;
	}
	
	xid = GetXid();
	if(file->f_dentry->d_inode == NULL) {
		FreeXid(xid);
		return -EBADF;
	}

	if (*poffset > file->f_dentry->d_inode->i_size)
		long_op = 2;  /* writes past end of file can take a long time */
	else
		long_op = 1;

	for (total_written = 0; write_size > total_written;
	     total_written += bytes_written) {
		rc = -EAGAIN;
		while(rc == -EAGAIN) {
			if(file->private_data == NULL) {
				/* file has been closed on us */
				FreeXid(xid);
			/* if we have gotten here we have written some data
			and blocked, and the file has been freed on us
			while we blocked so return what we managed to write */
				return total_written;
			} 
			if(open_file->closePend) {
				FreeXid(xid);
				if(total_written)
					return total_written;
				else
					return -EBADF;
			}
			if (open_file->invalidHandle) {
				if((file->f_dentry == NULL) ||
				   (file->f_dentry->d_inode == NULL)) {
					FreeXid(xid);
					return total_written;
				}
				/* we could deadlock if we called
				 filemap_fdatawait from here so tell
				reopen_file not to flush data to server now */
				rc = cifs_reopen_file(file->f_dentry->d_inode,
					file,FALSE);
				if(rc != 0)
					break;
			}

			rc = CIFSSMBWrite(xid, pTcon,
				  open_file->netfid,
				  write_size - total_written, *poffset,
				  &bytes_written,
				  write_data + total_written, NULL, long_op);
		}
		if (rc || (bytes_written == 0)) {
			if (total_written)
				break;
			else {
				FreeXid(xid);
				return rc;
			}
		} else
			*poffset += bytes_written;
		long_op = FALSE; /* subsequent writes fast - 15 seconds is plenty */
	}

#ifdef CONFIG_CIFS_STATS
	if(total_written > 0) {
		atomic_inc(&pTcon->num_writes);
		spin_lock(&pTcon->stat_lock);
		pTcon->bytes_written += total_written;
		spin_unlock(&pTcon->stat_lock);
	}
#endif		

	/* since the write may have blocked check these pointers again */
	if(file->f_dentry) {
		if(file->f_dentry->d_inode) {
			file->f_dentry->d_inode->i_ctime = file->f_dentry->d_inode->i_mtime =
				CURRENT_TIME;
			if (total_written > 0) {
				if (*poffset > file->f_dentry->d_inode->i_size)
					i_size_write(file->f_dentry->d_inode, *poffset);
			}
			mark_inode_dirty_sync(file->f_dentry->d_inode);
		}
	}
	FreeXid(xid);
	return total_written;
}

static int
cifs_partialpagewrite(struct page *page,unsigned from, unsigned to)
{
	struct address_space *mapping = page->mapping;
	loff_t offset = (loff_t)page->index << PAGE_CACHE_SHIFT;
	char * write_data;
	int rc = -EFAULT;
	int bytes_written = 0;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	struct inode *inode;
	struct cifsInodeInfo *cifsInode;
	struct cifsFileInfo *open_file = NULL;
	struct list_head *tmp;
	struct list_head *tmp1;

	if (!mapping) {
		return -EFAULT;
	} else if(!mapping->host) {
		return -EFAULT;
	}

	inode = page->mapping->host;
	cifs_sb = CIFS_SB(inode->i_sb);
	pTcon = cifs_sb->tcon;

	offset += (loff_t)from;
	write_data = kmap(page);
	write_data += from;

	if((to > PAGE_CACHE_SIZE) || (from > to)) {
		kunmap(page);
		return -EIO;
	}

	/* racing with truncate? */
	if(offset > mapping->host->i_size) {
		kunmap(page);
		return 0; /* don't care */
	}

	/* check to make sure that we are not extending the file */
	if(mapping->host->i_size - offset < (loff_t)to)
		to = (unsigned)(mapping->host->i_size - offset); 
		

	cifsInode = CIFS_I(mapping->host);
	read_lock(&GlobalSMBSeslock); 
	/* BB we should start at the end */
	list_for_each_safe(tmp, tmp1, &cifsInode->openFileList) {            
		open_file = list_entry(tmp,struct cifsFileInfo, flist);
		if(open_file->closePend)
			continue;
		/* We check if file is open for writing first */
		if((open_file->pfile) && 
		   ((open_file->pfile->f_flags & O_RDWR) || 
			(open_file->pfile->f_flags & O_WRONLY))) {
			read_unlock(&GlobalSMBSeslock);
			bytes_written = cifs_write(open_file->pfile, write_data,
					to-from, &offset);
			read_lock(&GlobalSMBSeslock);
		/* Does mm or vfs already set times? */
			inode->i_atime = inode->i_mtime = current_fs_time(inode->i_sb);
			if ((bytes_written > 0) && (offset)) {
				rc = 0;
			} else if(bytes_written < 0) {
				if(rc == -EBADF) {
				/* have seen a case in which
				kernel seemed to have closed/freed a file
				even with writes active so we might as well
				see if there are other file structs to try
				for the same inode before giving up */
					continue;
				} else
					rc = bytes_written;
			}
			break;  /* now that we found a valid file handle
				and tried to write to it we are done, no
				sense continuing to loop looking for another */
		}
		if(tmp->next == NULL) {
			cFYI(1,("File instance %p removed",tmp));
			break;
		}
	}
	read_unlock(&GlobalSMBSeslock);
	if(open_file == NULL) {
		cFYI(1,("No writeable filehandles for inode"));
		rc = -EIO;
	}

	kunmap(page);
	return rc;
}

#if 0
static int
cifs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	int rc = -EFAULT;
	int xid;

	xid = GetXid();
/* call 16K write then Setpageuptodate */
	FreeXid(xid);
	return rc;
}
#endif

static int
cifs_writepage(struct page* page, struct writeback_control *wbc)
{
	int rc = -EFAULT;
	int xid;

	xid = GetXid();
/* BB add check for wbc flags */
	page_cache_get(page);
        if (!PageUptodate(page)) {
		cFYI(1,("ppw - page not up to date"));
	}
	
	rc = cifs_partialpagewrite(page,0,PAGE_CACHE_SIZE);
	SetPageUptodate(page); /* BB add check for error and Clearuptodate? */
	unlock_page(page);
	page_cache_release(page);	
	FreeXid(xid);
	return rc;
}

static int
cifs_commit_write(struct file *file, struct page *page, unsigned offset,
		  unsigned to)
{
	int xid;
	int rc = 0;
	struct inode *inode = page->mapping->host;
	loff_t position = ((loff_t)page->index << PAGE_CACHE_SHIFT) + to;
	char * page_data;

	xid = GetXid();
	cFYI(1,("commit write for page %p up to position %lld for %d",page,position,to));
	if (position > inode->i_size){
		i_size_write(inode, position);
		/*if (file->private_data == NULL) {
			rc = -EBADF;
		} else {
			open_file = (struct cifsFileInfo *)file->private_data;
			cifs_sb = CIFS_SB(inode->i_sb);
			rc = -EAGAIN;
			while(rc == -EAGAIN) {
				if((open_file->invalidHandle) && 
				  (!open_file->closePend)) {
					rc = cifs_reopen_file(file->f_dentry->d_inode,file);
					if(rc != 0)
						break;
				}
				if(!open_file->closePend) {
					rc = CIFSSMBSetFileSize(xid, cifs_sb->tcon, 
						position, open_file->netfid,
						open_file->pid,FALSE);
				} else {
					rc = -EBADF;
					break;
				}
			}
			cFYI(1,(" SetEOF (commit write) rc = %d",rc));
		}*/
	}
	if (!PageUptodate(page)) {
		position =  ((loff_t)page->index << PAGE_CACHE_SHIFT) + offset;
		/* can not rely on (or let) writepage write this data */
		if(to < offset) {
			cFYI(1,("Illegal offsets, can not copy from %d to %d",
				offset,to));
			FreeXid(xid);
			return rc;
		}
		/* this is probably better than directly calling 
		partialpage_write since in this function
		the file handle is known which we might as well
		leverage */
		/* BB check if anything else missing out of ppw */
		/* such as updating last write time */
		page_data = kmap(page);
		rc = cifs_write(file, page_data+offset,to-offset,
                                        &position);
		if(rc > 0)
			rc = 0;
		/* else if rc < 0 should we set writebehind rc? */
		kunmap(page);
	} else {	
		set_page_dirty(page);
	}

	FreeXid(xid);
	return rc;
}

int
cifs_fsync(struct file *file, struct dentry *dentry, int datasync)
{
	int xid;
	int rc = 0;
	struct inode * inode = file->f_dentry->d_inode;

	xid = GetXid();

	cFYI(1, ("Sync file - name: %s datasync: 0x%x ", 
		dentry->d_name.name, datasync));
	
	rc = filemap_fdatawrite(inode->i_mapping);
	if(rc == 0)
		CIFS_I(inode)->write_behind_rc = 0;
	FreeXid(xid);
	return rc;
}

/* static int
cifs_sync_page(struct page *page)
{
	struct address_space *mapping;
	struct inode *inode;
	unsigned long index = page->index;
	unsigned int rpages = 0;
	int rc = 0;

	cFYI(1,("sync page %p",page));
	mapping = page->mapping;
	if (!mapping)
		return 0;
	inode = mapping->host;
	if (!inode)
		return 0;*/

/*	fill in rpages then 
    result = cifs_pagein_inode(inode, index, rpages); *//* BB finish */

/*   cFYI(1, ("rpages is %d for sync page of Index %ld ", rpages, index));

	if (rc < 0)
		return rc;
	return 0;
} */

/*
 * As file closes, flush all cached write data for this inode checking
 * for write behind errors.
 *
 */
int cifs_flush(struct file *file)
{
	struct inode * inode = file->f_dentry->d_inode;
	int rc = 0;

	/* Rather than do the steps manually: */
	/* lock the inode for writing */
	/* loop through pages looking for write behind data (dirty pages) */
	/* coalesce into contiguous 16K (or smaller) chunks to write to server */
	/* send to server (prefer in parallel) */
	/* deal with writebehind errors */
	/* unlock inode for writing */
	/* filemapfdatawrite appears easier for the time being */

	rc = filemap_fdatawrite(inode->i_mapping);
	if(rc == 0) /* reset wb rc if we were able to write out dirty pages */
		CIFS_I(inode)->write_behind_rc = 0;
		
	cFYI(1,("Flush inode %p file %p rc %d",inode,file,rc));

	return rc;
}


ssize_t
cifs_user_read(struct file * file, char __user *read_data, size_t read_size,
	  loff_t * poffset)
{
	int rc = -EACCES;
	unsigned int bytes_read = 0;
	unsigned int total_read = 0;
	unsigned int current_read_size;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	int xid;
	struct cifsFileInfo * open_file;
	char * smb_read_data;
	char __user * current_offset;
	struct smb_com_read_rsp * pSMBr;

	xid = GetXid();
	cifs_sb = CIFS_SB(file->f_dentry->d_sb);
	pTcon = cifs_sb->tcon;

	if (file->private_data == NULL) {
		FreeXid(xid);
		return -EBADF;
	}
	open_file = (struct cifsFileInfo *)file->private_data;

	if((file->f_flags & O_ACCMODE) == O_WRONLY) {
		cFYI(1,("attempting read on write only file instance"));
	}

	for (total_read = 0,current_offset=read_data; read_size > total_read;
				total_read += bytes_read,current_offset+=bytes_read) {
		current_read_size = min_t(const int,read_size - total_read,cifs_sb->rsize);
		rc = -EAGAIN;
		smb_read_data = NULL;
		while(rc == -EAGAIN) {
			if ((open_file->invalidHandle) && (!open_file->closePend)) {
				rc = cifs_reopen_file(file->f_dentry->d_inode,
					file,TRUE);
				if(rc != 0)
					break;
			}

			rc = CIFSSMBRead(xid, pTcon,
				 open_file->netfid,
				 current_read_size, *poffset,
				 &bytes_read, &smb_read_data);

			pSMBr = (struct smb_com_read_rsp *)smb_read_data;
			copy_to_user(current_offset,smb_read_data + 4/* RFC1001 hdr*/
				+ le16_to_cpu(pSMBr->DataOffset), bytes_read);
			if(smb_read_data) {
				cifs_buf_release(smb_read_data);
				smb_read_data = NULL;
			}
		}
		if (rc || (bytes_read == 0)) {
			if (total_read) {
				break;
			} else {
				FreeXid(xid);
				return rc;
			}
		} else {
#ifdef CONFIG_CIFS_STATS
			atomic_inc(&pTcon->num_reads);
			spin_lock(&pTcon->stat_lock);
			pTcon->bytes_read += total_read;
			spin_unlock(&pTcon->stat_lock);
#endif
			*poffset += bytes_read;
		}
	}
	FreeXid(xid);
	return total_read;
}

static ssize_t
cifs_read(struct file * file, char *read_data, size_t read_size,
	  loff_t * poffset)
{
	int rc = -EACCES;
	unsigned int bytes_read = 0;
	unsigned int total_read;
	unsigned int current_read_size;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	int xid;
	char * current_offset;
	struct cifsFileInfo * open_file;

	xid = GetXid();
	cifs_sb = CIFS_SB(file->f_dentry->d_sb);
	pTcon = cifs_sb->tcon;

	if (file->private_data == NULL) {
		FreeXid(xid);
		return -EBADF;
	}
	open_file = (struct cifsFileInfo *)file->private_data;

	if((file->f_flags & O_ACCMODE) == O_WRONLY) {
		cFYI(1,("attempting read on write only file instance"));
	}

	for (total_read = 0,current_offset=read_data; read_size > total_read;
				total_read += bytes_read,current_offset+=bytes_read) {
		current_read_size = min_t(const int,read_size - total_read,cifs_sb->rsize);
		rc = -EAGAIN;
		while(rc == -EAGAIN) {
			if ((open_file->invalidHandle) && (!open_file->closePend)) {
				rc = cifs_reopen_file(file->f_dentry->d_inode,
					file,TRUE);
				if(rc != 0)
					break;
			}

			rc = CIFSSMBRead(xid, pTcon,
				 open_file->netfid,
				 current_read_size, *poffset,
				 &bytes_read, &current_offset);
		}
		if (rc || (bytes_read == 0)) {
			if (total_read) {
				break;
			} else {
				FreeXid(xid);
				return rc;
			}
		} else {
#ifdef CONFIG_CIFS_STATS
			atomic_inc(&pTcon->num_reads);
			spin_lock(&pTcon->stat_lock);
			pTcon->bytes_read += total_read;
			spin_unlock(&pTcon->stat_lock);
#endif
			*poffset += bytes_read;
		}
	}
	FreeXid(xid);
	return total_read;
}

int cifs_file_mmap(struct file * file, struct vm_area_struct * vma)
{
	struct dentry * dentry = file->f_dentry;
	int	rc, xid;

#ifdef CIFS_EXPERIMENTAL   /* BB fixme reenable when cifs_read_wrapper fixed */
	if(dentry->d_sb) {
		struct cifs_sb_info *cifs_sb;
		cifs_sb = CIFS_SB(sb);
		if(cifs_sb != NULL) {
			if(cifs_sb->mnt_cifs_flags & CIFS_MOUNT_DIRECT_IO)
				return -ENODEV
		}
	}
#endif /* CIFS_EXPERIMENTAL */

	xid = GetXid();
	rc = cifs_revalidate(dentry);
	if (rc) {
		cFYI(1,("Validation prior to mmap failed, error=%d", rc));
		FreeXid(xid);
		return rc;
	}
	rc = generic_file_mmap(file, vma);
	FreeXid(xid);
	return rc;
}

static void cifs_copy_cache_pages(struct address_space *mapping, 
		struct list_head *pages, int bytes_read, 
		char *data,struct pagevec * plru_pvec)
{
	struct page *page;
	char * target;

	while (bytes_read > 0) {
		if(list_empty(pages))
			break;

		page = list_entry(pages->prev, struct page, lru);
		list_del(&page->lru);

		if (add_to_page_cache(page, mapping, page->index, GFP_KERNEL)) {
			page_cache_release(page);
			cFYI(1,("Add page cache failed"));
			continue;
		}

		target = kmap_atomic(page,KM_USER0);

		if(PAGE_CACHE_SIZE > bytes_read) {
			memcpy(target,data,bytes_read);
			/* zero the tail end of this partial page */
			memset(target+bytes_read,0,PAGE_CACHE_SIZE-bytes_read);
			bytes_read = 0;
		} else {
			memcpy(target,data,PAGE_CACHE_SIZE);
			bytes_read -= PAGE_CACHE_SIZE;
		}
		kunmap_atomic(target,KM_USER0);

		flush_dcache_page(page);
		SetPageUptodate(page);
		unlock_page(page);
		if (!pagevec_add(plru_pvec, page))
			__pagevec_lru_add(plru_pvec);
		data += PAGE_CACHE_SIZE;
	}
	return;
}


static int
cifs_readpages(struct file *file, struct address_space *mapping,
		struct list_head *page_list, unsigned num_pages)
{
	int rc = -EACCES;
	int xid;
	loff_t offset;
	struct page * page;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	int bytes_read = 0;
	unsigned int read_size,i;
	char * smb_read_data = NULL;
	struct smb_com_read_rsp * pSMBr;
	struct pagevec lru_pvec;
	struct cifsFileInfo * open_file;

	xid = GetXid();
	if (file->private_data == NULL) {
		FreeXid(xid);
		return -EBADF;
	}
	open_file = (struct cifsFileInfo *)file->private_data;
	cifs_sb = CIFS_SB(file->f_dentry->d_sb);
	pTcon = cifs_sb->tcon;

	pagevec_init(&lru_pvec, 0);

	for(i = 0;i<num_pages;) {
		unsigned contig_pages;
		struct page * tmp_page;
		unsigned long expected_index;

		if(list_empty(page_list)) {
			break;
		}
		page = list_entry(page_list->prev, struct page, lru);
		offset = (loff_t)page->index << PAGE_CACHE_SHIFT;

		/* count adjacent pages that we will read into */
		contig_pages = 0;
		expected_index = list_entry(page_list->prev,struct page,lru)->index;
		list_for_each_entry_reverse(tmp_page,page_list,lru) {
			if(tmp_page->index == expected_index) {
				contig_pages++;
				expected_index++;
			} else {
				break; 
			}
		}
		if(contig_pages + i >  num_pages) {
			contig_pages = num_pages - i;
		}

		/* for reads over a certain size could initiate async read ahead */

		read_size = contig_pages * PAGE_CACHE_SIZE;
		/* Read size needs to be in multiples of one page */
		read_size = min_t(const unsigned int,read_size,cifs_sb->rsize & PAGE_CACHE_MASK);

		rc = -EAGAIN;
		while(rc == -EAGAIN) {
			if ((open_file->invalidHandle) && (!open_file->closePend)) {
				rc = cifs_reopen_file(file->f_dentry->d_inode,
					file, TRUE);
				if(rc != 0)
					break;
			}

			rc = CIFSSMBRead(xid, pTcon,
				open_file->netfid,
				read_size, offset,
				&bytes_read, &smb_read_data);
			/* BB need to check return code here */
			if(rc== -EAGAIN) {
				if(smb_read_data) {
					cifs_buf_release(smb_read_data);
					smb_read_data = NULL;
				}
			}
		}
		if ((rc < 0) || (smb_read_data == NULL)) {
			cFYI(1,("Read error in readpages: %d",rc));
			/* clean up remaing pages off list */
			while (!list_empty(page_list) && (i < num_pages)) {
				page = list_entry(page_list->prev, struct page, lru);
				list_del(&page->lru);
				page_cache_release(page);
			}
			break;
		} else if (bytes_read > 0) {
			pSMBr = (struct smb_com_read_rsp *)smb_read_data;
			cifs_copy_cache_pages(mapping, page_list, bytes_read,
				smb_read_data + 4 /* RFC1001 hdr */ +
				le16_to_cpu(pSMBr->DataOffset), &lru_pvec);

			i +=  bytes_read >> PAGE_CACHE_SHIFT;
#ifdef CONFIG_CIFS_STATS
			atomic_inc(&pTcon->num_reads);
			spin_lock(&pTcon->stat_lock);
			pTcon->bytes_read += bytes_read;
			spin_unlock(&pTcon->stat_lock);
#endif
			if((int)(bytes_read & PAGE_CACHE_MASK) != bytes_read) {
				i++; /* account for partial page */

				/* server copy of file can have smaller size than client */
				/* BB do we need to verify this common case ? this case is ok - 
				if we are at server EOF we will hit it on next read */

			/* while(!list_empty(page_list) && (i < num_pages)) {
					page = list_entry(page_list->prev,struct page, list);
					list_del(&page->list);
					page_cache_release(page);
				}
				break; */
			}
		} else {
			cFYI(1,("No bytes read (%d) at offset %lld . Cleaning remaining pages from readahead list",bytes_read,offset)); 
			/* BB turn off caching and do new lookup on file size at server? */
			while (!list_empty(page_list) && (i < num_pages)) {
				page = list_entry(page_list->prev, struct page, lru);
				list_del(&page->lru);
				page_cache_release(page); /* BB removeme - replace with zero of page? */
			}
			break;
		}
		if(smb_read_data) {
			cifs_buf_release(smb_read_data);
			smb_read_data = NULL;
		}
		bytes_read = 0;
	}

	pagevec_lru_add(&lru_pvec);

/* need to free smb_read_data buf before exit */
	if(smb_read_data) {
		cifs_buf_release(smb_read_data);
		smb_read_data = NULL;
	} 

	FreeXid(xid);
	return rc;
}

static int cifs_readpage_worker(struct file *file, struct page *page, loff_t * poffset)
{
	char * read_data;
	int rc;

	page_cache_get(page);
	read_data = kmap(page);
	/* for reads over a certain size could initiate async read ahead */
                                                                                                                           
	rc = cifs_read(file, read_data, PAGE_CACHE_SIZE, poffset);
                                                                                                                           
	if (rc < 0)
		goto io_error;
	else {
		cFYI(1,("Bytes read %d ",rc));
	}
                                                                                                                           
	file->f_dentry->d_inode->i_atime =
		current_fs_time(file->f_dentry->d_inode->i_sb);
                                                                                                                           
	if(PAGE_CACHE_SIZE > rc) {
		memset(read_data+rc, 0, PAGE_CACHE_SIZE - rc);
	}
	flush_dcache_page(page);
	SetPageUptodate(page);
	rc = 0;
                                                                                                                           
io_error:
        kunmap(page);
	page_cache_release(page);
	return rc;
}

static int
cifs_readpage(struct file *file, struct page *page)
{
	loff_t offset = (loff_t)page->index << PAGE_CACHE_SHIFT;
	int rc = -EACCES;
	int xid;

	xid = GetXid();

	if (file->private_data == NULL) {
		FreeXid(xid);
		return -EBADF;
	}

	cFYI(1,("readpage %p at offset %d 0x%x\n",page,(int)offset,(int)offset));

	rc = cifs_readpage_worker(file,page,&offset);

	unlock_page(page);

	FreeXid(xid);
	return rc;
}

/* We do not want to update the file size from server for inodes
   open for write - to avoid races with writepage extending
   the file - in the future we could consider allowing
   refreshing the inode only on increases in the file size 
   but this is tricky to do without racing with writebehind
   page caching in the current Linux kernel design */
   
int is_size_safe_to_change(struct cifsInodeInfo * cifsInode)
{
	struct list_head *tmp;
	struct list_head *tmp1;
	struct cifsFileInfo *open_file = NULL;
	int rc = TRUE;

	if(cifsInode == NULL)
		return rc;

	read_lock(&GlobalSMBSeslock); 
	list_for_each_safe(tmp, tmp1, &cifsInode->openFileList) {            
		open_file = list_entry(tmp,struct cifsFileInfo, flist);
		if(open_file == NULL)
			break;
		if(open_file->closePend)
			continue;
	/* We check if file is open for writing,   
	BB we could supplement this with a check to see if file size
	changes have been flushed to server - ie inode metadata dirty */
		if((open_file->pfile) && 
	   ((open_file->pfile->f_flags & O_RDWR) || 
		(open_file->pfile->f_flags & O_WRONLY))) {
			 rc = FALSE;
			 break;
		}
		if(tmp->next == NULL) {
			cFYI(1,("File instance %p removed",tmp));
			break;
		}
	}
	read_unlock(&GlobalSMBSeslock);
	return rc;
}


void
fill_in_inode(struct inode *tmp_inode,
	      FILE_DIRECTORY_INFO * pfindData, int *pobject_type)
{
	struct cifsInodeInfo *cifsInfo = CIFS_I(tmp_inode);
	struct cifs_sb_info *cifs_sb = CIFS_SB(tmp_inode->i_sb);
	__u32 attr = le32_to_cpu(pfindData->ExtFileAttributes);
	__u64 allocation_size = le64_to_cpu(pfindData->AllocationSize);
	__u64 end_of_file = le64_to_cpu(pfindData->EndOfFile);

	cifsInfo->cifsAttrs = attr;
	cifsInfo->time = jiffies;

	/* Linux can not store file creation time unfortunately so ignore it */
	tmp_inode->i_atime =
	    cifs_NTtimeToUnix(le64_to_cpu(pfindData->LastAccessTime));
	tmp_inode->i_mtime =
	    cifs_NTtimeToUnix(le64_to_cpu(pfindData->LastWriteTime));
	tmp_inode->i_ctime =
	    cifs_NTtimeToUnix(le64_to_cpu(pfindData->ChangeTime));
	/* treat dos attribute of read-only as read-only mode bit e.g. 555? */
	/* 2767 perms - indicate mandatory locking */
		/* BB fill in uid and gid here? with help from winbind? 
			or retrieve from NTFS stream extended attribute */
	if(atomic_read(&cifsInfo->inUse) == 0) {
		tmp_inode->i_uid = cifs_sb->mnt_uid;
		tmp_inode->i_gid = cifs_sb->mnt_gid;
		/* set default mode. will override for dirs below */
		tmp_inode->i_mode = cifs_sb->mnt_file_mode;
	}

	cFYI(0,
	     ("CIFS FFIRST: Attributes came in as 0x%x",
	      attr));
	if (attr & ATTR_DIRECTORY) {
		*pobject_type = DT_DIR;
		/* override default perms since we do not lock dirs */
		if(atomic_read(&cifsInfo->inUse) == 0) {
			tmp_inode->i_mode = cifs_sb->mnt_dir_mode;
		}
		tmp_inode->i_mode |= S_IFDIR;
/* we no longer mark these because we could not follow them */
/*        } else if (attr & ATTR_REPARSE) {
                *pobject_type = DT_LNK;
                tmp_inode->i_mode |= S_IFLNK;*/
	} else {
		*pobject_type = DT_REG;
		tmp_inode->i_mode |= S_IFREG;
		if(attr & ATTR_READONLY)
			tmp_inode->i_mode &= ~(S_IWUGO);
	}/* could add code here - to validate if device or weird share type? */

	/* can not fill in nlink here as in qpathinfo version and Unx search */
	if(atomic_read(&cifsInfo->inUse) == 0) {
		atomic_set(&cifsInfo->inUse,1);
	}

	if(is_size_safe_to_change(cifsInfo)) {
		/* can not safely change the file size here if the 
		client is writing to it due to potential races */
		i_size_write(tmp_inode,end_of_file);

	/* 512 bytes (2**9) is the fake blocksize that must be used */
	/* for this calculation, even though the reported blocksize is larger */
		tmp_inode->i_blocks = (512 - 1 + allocation_size) >> 9;
	}

	if (allocation_size < end_of_file)
		cFYI(1, ("Possible sparse file: allocation size less than end of file "));
	cFYI(1,
	     ("File Size %ld and blocks %ld and blocksize %ld",
	      (unsigned long) tmp_inode->i_size, tmp_inode->i_blocks,
	      tmp_inode->i_blksize));
	if (S_ISREG(tmp_inode->i_mode)) {
		cFYI(1, (" File inode "));
		tmp_inode->i_op = &cifs_file_inode_ops;
		tmp_inode->i_fop = &cifs_file_ops;
		tmp_inode->i_data.a_ops = &cifs_addr_ops;
	} else if (S_ISDIR(tmp_inode->i_mode)) {
		cFYI(1, (" Directory inode"));
		tmp_inode->i_op = &cifs_dir_inode_ops;
		tmp_inode->i_fop = &cifs_dir_ops;
	} else if (S_ISLNK(tmp_inode->i_mode)) {
		cFYI(1, (" Symbolic Link inode "));
		tmp_inode->i_op = &cifs_symlink_inode_ops;
	} else {
		cFYI(1, (" Init special inode "));
		init_special_inode(tmp_inode, tmp_inode->i_mode,
				   tmp_inode->i_rdev);
	}
}

void
unix_fill_in_inode(struct inode *tmp_inode,
		   FILE_UNIX_INFO * pfindData, int *pobject_type)
{
	struct cifsInodeInfo *cifsInfo = CIFS_I(tmp_inode);
	__u32 type = le32_to_cpu(pfindData->Type);
	__u64 num_of_bytes = le64_to_cpu(pfindData->NumOfBytes);
	__u64 end_of_file = le64_to_cpu(pfindData->EndOfFile);
	cifsInfo->time = jiffies;
	atomic_inc(&cifsInfo->inUse);

	tmp_inode->i_atime =
	    cifs_NTtimeToUnix(le64_to_cpu(pfindData->LastAccessTime));
	tmp_inode->i_mtime =
	    cifs_NTtimeToUnix(le64_to_cpu(pfindData->LastModificationTime));
	tmp_inode->i_ctime =
	    cifs_NTtimeToUnix(le64_to_cpu(pfindData->LastStatusChange));

	tmp_inode->i_mode = le64_to_cpu(pfindData->Permissions);
	if (type == UNIX_FILE) {
		*pobject_type = DT_REG;
		tmp_inode->i_mode |= S_IFREG;
	} else if (type == UNIX_SYMLINK) {
		*pobject_type = DT_LNK;
		tmp_inode->i_mode |= S_IFLNK;
	} else if (type == UNIX_DIR) {
		*pobject_type = DT_DIR;
		tmp_inode->i_mode |= S_IFDIR;
	} else if (type == UNIX_CHARDEV) {
		*pobject_type = DT_CHR;
		tmp_inode->i_mode |= S_IFCHR;
		tmp_inode->i_rdev = MKDEV(le64_to_cpu(pfindData->DevMajor),
				le64_to_cpu(pfindData->DevMinor) & MINORMASK);
	} else if (type == UNIX_BLOCKDEV) {
		*pobject_type = DT_BLK;
		tmp_inode->i_mode |= S_IFBLK;
		tmp_inode->i_rdev = MKDEV(le64_to_cpu(pfindData->DevMajor),
				le64_to_cpu(pfindData->DevMinor) & MINORMASK);
	} else if (type == UNIX_FIFO) {
		*pobject_type = DT_FIFO;
		tmp_inode->i_mode |= S_IFIFO;
	} else if (type == UNIX_SOCKET) {
		*pobject_type = DT_SOCK;
		tmp_inode->i_mode |= S_IFSOCK;
	}

	tmp_inode->i_uid = le64_to_cpu(pfindData->Uid);
	tmp_inode->i_gid = le64_to_cpu(pfindData->Gid);
	tmp_inode->i_nlink = le64_to_cpu(pfindData->Nlinks);


	if(is_size_safe_to_change(cifsInfo)) {
		/* can not safely change the file size here if the 
		client is writing to it due to potential races */
		i_size_write(tmp_inode,end_of_file);

	/* 512 bytes (2**9) is the fake blocksize that must be used */
	/* for this calculation, not the real blocksize */
		tmp_inode->i_blocks = (512 - 1 + num_of_bytes) >> 9;
	}

	if (S_ISREG(tmp_inode->i_mode)) {
		cFYI(1, ("File inode"));
		tmp_inode->i_op = &cifs_file_inode_ops;
		tmp_inode->i_fop = &cifs_file_ops;
		tmp_inode->i_data.a_ops = &cifs_addr_ops;
	} else if (S_ISDIR(tmp_inode->i_mode)) {
		cFYI(1, ("Directory inode"));
		tmp_inode->i_op = &cifs_dir_inode_ops;
		tmp_inode->i_fop = &cifs_dir_ops;
	} else if (S_ISLNK(tmp_inode->i_mode)) {
		cFYI(1, ("Symbolic Link inode"));
		tmp_inode->i_op = &cifs_symlink_inode_ops;
/* tmp_inode->i_fop = *//* do not need to set to anything */
	} else {
		cFYI(1, ("Special inode")); 
		init_special_inode(tmp_inode, tmp_inode->i_mode,
				   tmp_inode->i_rdev);
	}
}

/* Returns one if new inode created (which therefore needs to be hashed) */
/* Might check in the future if inode number changed so we can rehash inode */
int
construct_dentry(struct qstr *qstring, struct file *file,
		 struct inode **ptmp_inode, struct dentry **pnew_dentry)
{
	struct dentry *tmp_dentry;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	int rc = 0;

	cFYI(1, ("For %s ", qstring->name));
	cifs_sb = CIFS_SB(file->f_dentry->d_sb);
	pTcon = cifs_sb->tcon;

	qstring->hash = full_name_hash(qstring->name, qstring->len);
	tmp_dentry = d_lookup(file->f_dentry, qstring);
	if (tmp_dentry) {
		cFYI(0, (" existing dentry with inode 0x%p", tmp_dentry->d_inode));
		*ptmp_inode = tmp_dentry->d_inode;
		/* BB overwrite the old name? i.e. tmp_dentry->d_name and tmp_dentry->d_name.len ?? */
		if(*ptmp_inode == NULL) {
	                *ptmp_inode = new_inode(file->f_dentry->d_sb);
			if(*ptmp_inode == NULL)
				return rc;
			rc = 1;
			d_instantiate(tmp_dentry, *ptmp_inode);
		}
	} else {
		tmp_dentry = d_alloc(file->f_dentry, qstring);
		if(tmp_dentry == NULL) {
			cERROR(1,("Failed allocating dentry"));
			*ptmp_inode = NULL;
			return rc;
		}
			
		*ptmp_inode = new_inode(file->f_dentry->d_sb);
		tmp_dentry->d_op = &cifs_dentry_ops;
		if(*ptmp_inode == NULL)
			return rc;
		rc = 1;
		d_instantiate(tmp_dentry, *ptmp_inode);
		d_rehash(tmp_dentry);
	}

	tmp_dentry->d_time = jiffies;
	*pnew_dentry = tmp_dentry;
	return rc; 
}

static void reset_resume_key(struct file * dir_file, 
				unsigned char * filename, 
				unsigned int len,int Unicode,struct nls_table * nls_tab) {
	struct cifsFileInfo *cifsFile;

	cifsFile = (struct cifsFileInfo *)dir_file->private_data;
	if(cifsFile == NULL)
		return;
	if(cifsFile->search_resume_name) {
		kfree(cifsFile->search_resume_name);
	}

	if(Unicode) 
		len *= 2;
	cifsFile->resume_name_length = len;

	cifsFile->search_resume_name = 
		kmalloc(cifsFile->resume_name_length, GFP_KERNEL);

	if(cifsFile->search_resume_name == NULL) {
		cERROR(1,("failed new resume key allocate, length %d",
				  cifsFile->resume_name_length));
		return;
	}
	if(Unicode)
		cifs_strtoUCS((wchar_t *) cifsFile->search_resume_name,
			filename, len, nls_tab);
	else
		memcpy(cifsFile->search_resume_name, filename, 
		   cifsFile->resume_name_length);
	cFYI(1,("Reset resume key to: %s with len %d",filename,len));
	return;
}



static int
cifs_filldir(struct qstr *pqstring, FILE_DIRECTORY_INFO * pfindData,
	     struct file *file, filldir_t filldir, void *direntry)
{
	struct inode *tmp_inode;
	struct dentry *tmp_dentry;
	int object_type,rc;

	pqstring->name = pfindData->FileName;
	/* pqstring->len is already set by caller */

	rc = construct_dentry(pqstring, file, &tmp_inode, &tmp_dentry);
	if((tmp_inode == NULL) || (tmp_dentry == NULL)) {
		return -ENOMEM;
	}
	fill_in_inode(tmp_inode, pfindData, &object_type);
	if(rc) {
		/* We have no reliable way to get inode numbers
		from servers w/o Unix extensions yet so we can not set
		i_ino from pfindData yet */

		/* new inode created, let us hash it */
		insert_inode_hash(tmp_inode);
	} /* else if inode number changed do we rehash it? */
	rc = filldir(direntry, pfindData->FileName, pqstring->len, file->f_pos,
		tmp_inode->i_ino, object_type);
	if(rc) {
		/* due to readdir error we need to recalculate resume 
		key so next readdir will restart on right entry */
		cFYI(1,("Error %d on filldir of %s",rc ,pfindData->FileName));
	}
	dput(tmp_dentry);
	return rc;
}

static int
cifs_filldir_unix(struct qstr *pqstring,
		  FILE_UNIX_INFO * pUnixFindData, struct file *file,
		  filldir_t filldir, void *direntry)
{
	struct inode *tmp_inode;
	struct dentry *tmp_dentry;
	int object_type, rc;

	pqstring->name = pUnixFindData->FileName;
	pqstring->len = strnlen(pUnixFindData->FileName, MAX_PATHCONF);

	rc = construct_dentry(pqstring, file, &tmp_inode, &tmp_dentry);
	if((tmp_inode == NULL) || (tmp_dentry == NULL)) {
		return -ENOMEM;
	} 
	if(rc) {
		struct cifs_sb_info *cifs_sb = CIFS_SB(tmp_inode->i_sb);

		if(cifs_sb->mnt_cifs_flags & CIFS_MOUNT_SERVER_INUM) {
			tmp_inode->i_ino = 
				(unsigned long)pUnixFindData->UniqueId;
		}
		insert_inode_hash(tmp_inode);
	} /* else if i_ino has changed should we rehash it? */
	unix_fill_in_inode(tmp_inode, pUnixFindData, &object_type);
	rc = filldir(direntry, pUnixFindData->FileName, pqstring->len,
		file->f_pos, tmp_inode->i_ino, object_type);
	if(rc) {
		/* due to readdir error we need to recalculate resume 
			key so next readdir will restart on right entry */
		cFYI(1,("Error %d on filldir of %s",rc ,pUnixFindData->FileName));
	}
	dput(tmp_dentry);
	return rc;
}

int
cifs_readdir(struct file *file, void *direntry, filldir_t filldir)
{
	int rc = 0;
	int xid;
	int Unicode = FALSE;
	int UnixSearch = FALSE;
	unsigned int bufsize, i;
	__u16 searchHandle;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	struct cifsFileInfo *cifsFile = NULL;
	char *full_path = NULL;
	char *data;
	struct qstr qstring;
	T2_FFIRST_RSP_PARMS findParms;
	T2_FNEXT_RSP_PARMS findNextParms;
	FILE_DIRECTORY_INFO *pfindData;
	FILE_DIRECTORY_INFO *lastFindData;
	FILE_UNIX_INFO *pfindDataUnix;


    /* BB removeme begin */
	if(!experimEnabled)
		return cifs_readdir2(file,direntry,filldir);
    /* BB removeme end */


	xid = GetXid();

	if(file->f_dentry == NULL) {
		rc = -EIO;
		FreeXid(xid);
		return rc;
	}
	cifs_sb = CIFS_SB(file->f_dentry->d_sb);
	pTcon = cifs_sb->tcon;
	bufsize = pTcon->ses->server->maxBuf - MAX_CIFS_HDR_SIZE;
	if(bufsize > CIFSMaxBufSize) {
		rc = -EIO;
		FreeXid(xid);
		return rc;
	}
	data = kmalloc(bufsize, GFP_KERNEL);
	pfindData = (FILE_DIRECTORY_INFO *) data;
	if(data == NULL) {
		rc = -ENOMEM;
		FreeXid(xid);
		return rc;
	}
	down(&file->f_dentry->d_sb->s_vfs_rename_sem);
	full_path = build_wildcard_path_from_dentry(file->f_dentry);
	up(&file->f_dentry->d_sb->s_vfs_rename_sem);

	if(full_path == NULL) {
		kfree(data);
		FreeXid(xid);
		return -ENOMEM;
	}
	cFYI(1, ("Full path: %s start at: %lld ", full_path, file->f_pos));

	switch ((int) file->f_pos) {
	case 0:
		if (filldir(direntry, ".", 1, file->f_pos,
		     file->f_dentry->d_inode->i_ino, DT_DIR) < 0) {
			cERROR(1, ("Filldir for current dir failed "));
			break;
		}
		file->f_pos++;
		/* fallthrough */
	case 1:
		if (filldir(direntry, "..", 2, file->f_pos,
		     file->f_dentry->d_parent->d_inode->i_ino, DT_DIR) < 0) {
			cERROR(1, ("Filldir for parent dir failed "));
			break;
		}
		file->f_pos++;
		/* fallthrough */
	case 2:
		if (file->private_data != NULL) {
			cifsFile =
				(struct cifsFileInfo *) file->private_data;
			if (cifsFile->srch_inf.endOfSearch) {
				if(cifsFile->srch_inf.emptyDir) {
					cFYI(1, ("End of search, empty dir"));
					rc = 0;
					break;
				}
			} else {
				cifsFile->invalidHandle = TRUE;
				CIFSFindClose(xid, pTcon, cifsFile->netfid);
			}
			if(cifsFile->search_resume_name) {
				kfree(cifsFile->search_resume_name);
				cifsFile->search_resume_name = NULL;
			}
		}
		rc = CIFSFindFirst(xid, pTcon, full_path, pfindData,
				&findParms, cifs_sb->local_nls,
				&Unicode, &UnixSearch);
		cFYI(1, ("Count: %d  End: %d ",
			le16_to_cpu(findParms.SearchCount),
			le16_to_cpu(findParms.EndofSearch)));
 
		if (rc == 0) {
			__u16 count = le16_to_cpu(findParms.SearchCount);
			searchHandle = findParms.SearchHandle;
			if(file->private_data == NULL)
				file->private_data =
					kmalloc(sizeof(struct cifsFileInfo),GFP_KERNEL);
			if (file->private_data) {
				memset(file->private_data, 0,
				       sizeof (struct cifsFileInfo));
				cifsFile =
				    (struct cifsFileInfo *) file->private_data;
				cifsFile->netfid = searchHandle;
				cifsFile->invalidHandle = FALSE;
				init_MUTEX(&cifsFile->fh_sem);
			} else {
				rc = -ENOMEM;
				break;
			}

			renew_parental_timestamps(file->f_dentry);
			lastFindData = 
				(FILE_DIRECTORY_INFO *) ((char *) pfindData + 
					le16_to_cpu(findParms.LastNameOffset));
			if((char *)lastFindData > (char *)pfindData + bufsize) {
				cFYI(1,("last search entry past end of packet"));
				rc = -EIO;
				break;
			}
			/* Offset of resume key same for levels 257 and 514 */
			cifsFile->srch_inf.resume_key = lastFindData->FileIndex;
			if(UnixSearch == FALSE) {
				cifsFile->resume_name_length = 
					le32_to_cpu(lastFindData->FileNameLength);
				if(cifsFile->resume_name_length > bufsize - 64) {
					cFYI(1,("Illegal resume file name length %d",
						cifsFile->resume_name_length));
					rc = -ENOMEM;
					break;
				}
				cifsFile->search_resume_name = 
					kmalloc(cifsFile->resume_name_length, GFP_KERNEL);
				cFYI(1,("Last file: %s with name %d bytes long",
					lastFindData->FileName,
					cifsFile->resume_name_length));
				if(cifsFile->search_resume_name == NULL) {
					rc = -ENOMEM;
					break;
				}
				memcpy(cifsFile->search_resume_name,
					lastFindData->FileName, 
					cifsFile->resume_name_length);
			} else {
				pfindDataUnix = (FILE_UNIX_INFO *)lastFindData;
				if (Unicode == TRUE) {
					for(i=0;(pfindDataUnix->FileName[i] 
						    | pfindDataUnix->FileName[i+1]);
						i+=2) {
						if(i > bufsize-64)
							break;
					}
					cifsFile->resume_name_length = i + 2;
				} else {
					cifsFile->resume_name_length = 
						strnlen(pfindDataUnix->FileName,
							bufsize-63);
				}
				if(cifsFile->resume_name_length > bufsize - 64) {
					cFYI(1,("Illegal resume file name length %d",
						cifsFile->resume_name_length));
					rc = -ENOMEM;
					break;
				}
				cifsFile->search_resume_name = 
					kmalloc(cifsFile->resume_name_length, GFP_KERNEL);
				cFYI(1,("Last file: %s with name %d bytes long",
					pfindDataUnix->FileName,
					cifsFile->resume_name_length));
				if(cifsFile->search_resume_name == NULL) {
					rc = -ENOMEM;
					break;
				}
				memcpy(cifsFile->search_resume_name,
					pfindDataUnix->FileName, 
					cifsFile->resume_name_length);
			}
			for (i = 2; i < count + 2; i++) {
				if (UnixSearch == FALSE) {
					__u32 len = le32_to_cpu(pfindData->FileNameLength);
					if (Unicode == TRUE)
						len =
						    cifs_strfromUCS_le
						    (pfindData->FileName,
						     (wchar_t *)
						     pfindData->FileName,
						     len / 2,
						     cifs_sb->local_nls);
					qstring.len = len;
					if (((len != 1)
					     || (pfindData->FileName[0] != '.'))
					    && ((len != 2)
						|| (pfindData->
						    FileName[0] != '.')
						|| (pfindData->
						    FileName[1] != '.'))) {
						if(cifs_filldir(&qstring,
							     pfindData,
							     file, filldir,
							     direntry)) {
							/* do not end search if
								kernel not ready to take
								remaining entries yet */
							reset_resume_key(file, pfindData->FileName,qstring.len,
								Unicode, cifs_sb->local_nls);
							findParms.EndofSearch = 0;
							break;
						}
						file->f_pos++;
					}
				} else {	/* UnixSearch */
					pfindDataUnix =
					    (FILE_UNIX_INFO *) pfindData;
					if (Unicode == TRUE)
						qstring.len =
							cifs_strfromUCS_le
							(pfindDataUnix->FileName,
							(wchar_t *)
							pfindDataUnix->FileName,
							MAX_PATHCONF,
							cifs_sb->local_nls);
					else
						qstring.len =
							strnlen(pfindDataUnix->
							  FileName,
							  MAX_PATHCONF);
					if (((qstring.len != 1)
					     || (pfindDataUnix->
						 FileName[0] != '.'))
					    && ((qstring.len != 2)
						|| (pfindDataUnix->
						    FileName[0] != '.')
						|| (pfindDataUnix->
						    FileName[1] != '.'))) {
						if(cifs_filldir_unix(&qstring,
								  pfindDataUnix,
								  file,
								  filldir,
								  direntry)) {
							/* do not end search if
								kernel not ready to take
								remaining entries yet */
							findParms.EndofSearch = 0;
							reset_resume_key(file, pfindDataUnix->FileName,
								qstring.len,Unicode,cifs_sb->local_nls);
							break;
						}
						file->f_pos++;
					}
				}
				/* works also for Unix ff struct since first field of both */
				pfindData = 
					(FILE_DIRECTORY_INFO *) ((char *) pfindData
						 + le32_to_cpu(pfindData->NextEntryOffset));
				/* BB also should check to make sure that pointer is not beyond the end of the SMB */
				/* if(pfindData > lastFindData) rc = -EIO; break; */
			}	/* end for loop */
			if ((findParms.EndofSearch != 0) && cifsFile) {
				cifsFile->srch_inf.endOfSearch = TRUE;
				if(findParms.SearchCount == cpu_to_le16(2))
					cifsFile->srch_inf.emptyDir = TRUE;
			}
		} else {
			if (cifsFile)
				cifsFile->srch_inf.endOfSearch = TRUE;
			/* unless parent directory gone do not return error */
			rc = 0;
		}
		break;
	default:
		if (file->private_data == NULL) {
			rc = -EBADF;
			cFYI(1,
			     ("Readdir on closed srch, pos = %lld",
			      file->f_pos));
		} else {
			cifsFile = (struct cifsFileInfo *) file->private_data;
			if (cifsFile->srch_inf.endOfSearch) {
				rc = 0;
				cFYI(1, ("End of search "));
				break;
			}
			searchHandle = cifsFile->netfid;
			rc = CIFSFindNext(xid, pTcon, pfindData,
				&findNextParms, searchHandle, 
				cifsFile->search_resume_name,
				cifsFile->resume_name_length,
				cifsFile->srch_inf.resume_key,
				&Unicode, &UnixSearch);
			cFYI(1,("Count: %d  End: %d ",
			      le16_to_cpu(findNextParms.SearchCount),
			      le16_to_cpu(findNextParms.EndofSearch)));
			if ((rc == 0) && (findNextParms.SearchCount != 0)) {
			/* BB save off resume key, key name and name length  */
				__u16 count = le16_to_cpu(findNextParms.SearchCount);
				lastFindData = 
					(FILE_DIRECTORY_INFO *) ((char *) pfindData 
					+ le16_to_cpu(findNextParms.LastNameOffset));
				if((char *)lastFindData > (char *)pfindData + bufsize) {
					cFYI(1,("last search entry past end of packet"));
					rc = -EIO;
					break;
				}
				/* Offset of resume key same for levels 257 and 514 */
				cifsFile->srch_inf.resume_key = lastFindData->FileIndex;

				if(UnixSearch == FALSE) {
					cifsFile->resume_name_length = 
						le32_to_cpu(lastFindData->FileNameLength);
					if(cifsFile->resume_name_length > bufsize - 64) {
						cFYI(1,("Illegal resume file name length %d",
							cifsFile->resume_name_length));
						rc = -ENOMEM;
						break;
					}
					/* Free the memory allocated by previous findfirst 
					or findnext call - we can not reuse the memory since
					the resume name may not be same string length */
					if(cifsFile->search_resume_name)
						kfree(cifsFile->search_resume_name);
					cifsFile->search_resume_name = 
						kmalloc(cifsFile->resume_name_length, GFP_KERNEL);
					cFYI(1,("Last file: %s with name %d bytes long",
						lastFindData->FileName,
						cifsFile->resume_name_length));
					if(cifsFile->search_resume_name == NULL) {
						rc = -ENOMEM;
						break;
					}
					
					memcpy(cifsFile->search_resume_name,
						lastFindData->FileName, 
						cifsFile->resume_name_length);
				} else {
					pfindDataUnix = (FILE_UNIX_INFO *)lastFindData;
					if (Unicode == TRUE) {
						for(i=0;(pfindDataUnix->FileName[i] 
								| pfindDataUnix->FileName[i+1]);
							i+=2) {
							if(i > bufsize-64)
								break;
						}
						cifsFile->resume_name_length = i + 2;
					} else {
						cifsFile->resume_name_length = 
							strnlen(pfindDataUnix->
							 FileName,
							 MAX_PATHCONF);
					}
					if(cifsFile->resume_name_length > bufsize - 64) {
						cFYI(1,("Illegal resume file name length %d",
								cifsFile->resume_name_length));
						rc = -ENOMEM;
						break;
					}
					/* Free the memory allocated by previous findfirst 
					or findnext call - we can not reuse the memory since
					the resume name may not be same string length */
					if(cifsFile->search_resume_name)
						kfree(cifsFile->search_resume_name);
					cifsFile->search_resume_name = 
						kmalloc(cifsFile->resume_name_length, GFP_KERNEL);
					cFYI(1,("fnext last file: %s with name %d bytes long",
						pfindDataUnix->FileName,
						cifsFile->resume_name_length));
					if(cifsFile->search_resume_name == NULL) {
						rc = -ENOMEM;
						break;
					}
					memcpy(cifsFile->search_resume_name,
						pfindDataUnix->FileName, 
						cifsFile->resume_name_length);
				}

				for (i = 0; i < count; i++) {
					__u32 len = le32_to_cpu(pfindData->
							FileNameLength);
					if (UnixSearch == FALSE) {
						if (Unicode == TRUE)
							len =
							  cifs_strfromUCS_le
							  (pfindData->FileName,
							  (wchar_t *)
							  pfindData->FileName,
							  len / 2,
							  cifs_sb->local_nls);
						qstring.len = len;
						if (((len != 1)
						    || (pfindData->FileName[0] != '.'))
						    && ((len != 2)
							|| (pfindData->FileName[0] != '.')
							|| (pfindData->FileName[1] !=
							    '.'))) {
							if(cifs_filldir
							    (&qstring,
							     pfindData,
							     file, filldir,
							     direntry)) {
							/* do not end search if
								kernel not ready to take
								remaining entries yet */
								findNextParms.EndofSearch = 0;
								reset_resume_key(file, pfindData->FileName,qstring.len,
									Unicode,cifs_sb->local_nls);
								break;
							}
							file->f_pos++;
						}
					} else {	/* UnixSearch */
						pfindDataUnix =
						    (FILE_UNIX_INFO *)
						    pfindData;
						if (Unicode == TRUE)
							qstring.len =
							  cifs_strfromUCS_le
							  (pfindDataUnix->FileName,
							  (wchar_t *)
							  pfindDataUnix->FileName,
							  MAX_PATHCONF,
							  cifs_sb->local_nls);
						else
							qstring.len =
							  strnlen
							  (pfindDataUnix->
							  FileName,
							  MAX_PATHCONF);
						if (((qstring.len != 1)
						     || (pfindDataUnix->
							 FileName[0] != '.'))
						    && ((qstring.len != 2)
							|| (pfindDataUnix->
							    FileName[0] != '.')
							|| (pfindDataUnix->
							    FileName[1] !=
							    '.'))) {
							if(cifs_filldir_unix
							    (&qstring,
							     pfindDataUnix,
							     file, filldir,
							     direntry)) {
								/* do not end search if
								kernel not ready to take
								remaining entries yet */
								findNextParms.EndofSearch = 0;
								reset_resume_key(file, pfindDataUnix->FileName,qstring.len,
									Unicode,cifs_sb->local_nls);
								break;
							}
							file->f_pos++;
						}
					}
					pfindData = (FILE_DIRECTORY_INFO *) ((char *) pfindData + 
						le32_to_cpu(pfindData->NextEntryOffset));
	/* works also for Unix find struct since first field of both */
	/* BB also should check to ensure pointer not beyond end of SMB */
				} /* end for loop */
				if (findNextParms.EndofSearch != 0) {
					cifsFile->srch_inf.endOfSearch = TRUE;
				}
			} else {
				cifsFile->srch_inf.endOfSearch = TRUE;
				rc = 0;	/* unless parent directory disappeared - do not
				return error here (eg Access Denied or no more files) */
			}
		}
	} /* end switch */
	if (data)
		kfree(data);
	if (full_path)
		kfree(full_path);
	FreeXid(xid);

	return rc;
}
int cifs_prepare_write(struct file *file, struct page *page,
			unsigned from, unsigned to)
{
	int rc = 0;
        loff_t offset = (loff_t)page->index << PAGE_CACHE_SHIFT;
	cFYI(1,("prepare write for page %p from %d to %d",page,from,to));
	if (!PageUptodate(page)) {
	/*	if (to - from != PAGE_CACHE_SIZE) {
			void *kaddr = kmap_atomic(page, KM_USER0);
			memset(kaddr, 0, from);
			memset(kaddr + to, 0, PAGE_CACHE_SIZE - to);
			flush_dcache_page(page);
			kunmap_atomic(kaddr, KM_USER0);
		} */
		/* If we are writing a full page it will be up to date,
		no need to read from the server */
		if((to==PAGE_CACHE_SIZE) && (from == 0))
			SetPageUptodate(page);

		/* might as well read a page, it is fast enough */
		if((file->f_flags & O_ACCMODE) != O_WRONLY) {
			rc = cifs_readpage_worker(file,page,&offset);
		} else {
		/* should we try using another
		file handle if there is one - how would we lock it
		to prevent close of that handle racing with this read? */
		/* In any case this will be written out by commit_write */
		}
	}

	/* BB should we pass any errors back? e.g. if we do not have read access to the file */
	return 0;
}


struct address_space_operations cifs_addr_ops = {
	.readpage = cifs_readpage,
	.readpages = cifs_readpages,
	.writepage = cifs_writepage,
	.prepare_write = cifs_prepare_write, 
	.commit_write = cifs_commit_write,
	.set_page_dirty = __set_page_dirty_nobuffers,
   /* .sync_page = cifs_sync_page, */
	/*.direct_IO = */
};
