/*
 *   fs/cifs/readdir.c
 *
 *   Directory search handling
 * 
 *   Copyright (C) International Business Machines  Corp., 2004
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
#include <linux/smp_lock.h>
#include "cifspdu.h"
#include "cifsglob.h"
#include "cifsproto.h"
#include "cifs_unicode.h"
#include "cifs_debug.h"
#include "cifs_fs_sb.h"

extern int CIFSFindFirst2(const int xid, struct cifsTconInfo *tcon,
            const char *searchName, const struct nls_table *nls_codepage,
            __u16 *searchHandle, struct cifs_search_info * psrch_inf);

extern int CIFSFindNext2(const int xid, struct cifsTconInfo *tcon,
            __u16 searchHandle, struct cifs_search_info * psrch_inf);

extern int construct_dentry(struct qstr *qstring, struct file *file,
		 struct inode **ptmp_inode, struct dentry **pnew_dentry);

extern void fill_in_inode(struct inode *tmp_inode,
	      FILE_DIRECTORY_INFO * pfindData, int *pobject_type);

extern void unix_fill_in_inode(struct inode *tmp_inode,
	      FILE_UNIX_INFO * pfindData, int *pobject_type);


/* BB fixme - add debug wrappers around this function to disable it fixme BB */
/* static void dump_cifs_file_struct(struct file * file, char * label)
{
	struct cifsFileInfo * cf;

	if(file) {
		cf = (struct cifsFileInfo *)file->private_data;
		if(cf == NULL) {
			cFYI(1,("empty cifs private file data"));
			return;
		}
		if(cf->invalidHandle) {
			cFYI(1,("invalid handle"));
		}
		if(cf->srch_inf.endOfSearch) {
			cFYI(1,("end of search"));
		}
		if(cf->srch_inf.emptyDir) {
			cFYI(1,("empty dir"));
		}
		
	}
} */

static int initiate_cifs_search(const int xid, struct file * file)
{
	int rc = 0;
	char * full_path;
	struct cifsFileInfo * cifsFile;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;

	if(file->private_data == NULL) {
		file->private_data = 
			kmalloc(sizeof(struct cifsFileInfo),GFP_KERNEL);
	}

	if(file->private_data == NULL) {
		return -ENOMEM;
	} else {
		memset(file->private_data,0,sizeof(struct cifsFileInfo));
	}
	cifsFile = (struct cifsFileInfo *)file->private_data;
	cifsFile->invalidHandle = TRUE;
	cifsFile->srch_inf.endOfSearch = FALSE;

	cifs_sb = CIFS_SB(file->f_dentry->d_sb);
	if(cifs_sb == NULL)
		return -EINVAL;

	pTcon = cifs_sb->tcon;
	if(pTcon == NULL)
		return -EINVAL;

	if(file->f_dentry == NULL)
		return -ENOENT;

	down(&file->f_dentry->d_sb->s_vfs_rename_sem);
	full_path = build_wildcard_path_from_dentry(file->f_dentry);
	up(&file->f_dentry->d_sb->s_vfs_rename_sem);

	if(full_path == NULL) {
		return -ENOMEM;
	}

	cFYI(1, ("Full path: %s start at: %lld ", full_path, file->f_pos));

	/* test for Unix extensions */
	if (pTcon->ses->capabilities & CAP_UNIX) {
		cifsFile->srch_inf.info_level = SMB_FIND_FILE_UNIX;
	} else if (cifs_sb->mnt_cifs_flags & CIFS_MOUNT_SERVER_INUM) {
		cifsFile->srch_inf.info_level = SMB_FIND_FILE_ID_FULL_DIR_INFO;
	} else /* not srvinos - BB fixme add check for backlevel? */ {
		cifsFile->srch_inf.info_level = SMB_FIND_FILE_DIRECTORY_INFO;
	}

	rc = CIFSFindFirst2(xid, pTcon,full_path,cifs_sb->local_nls, 
		&cifsFile->netfid, &cifsFile->srch_inf); 
	if(rc == 0)
		cifsFile->invalidHandle = FALSE;
	if(full_path)
		kfree(full_path);
	return rc;
}

/* return length of unicode string in bytes */
static int cifs_unicode_bytelen(char * str)
{
	int len;
	__le16 * ustr = (__le16 *)str;

	for(len=0;len <= PATH_MAX;len++) {
		if(ustr[len] == 0)
			return len << 1;
	}
	cFYI(1,("Unicode string longer than PATH_MAX found"));
	return len << 1;
}

static char * nxt_dir_entry(char * old_entry, char * end_of_smb)
{
	char * new_entry;
	FILE_DIRECTORY_INFO * pDirInfo = (FILE_DIRECTORY_INFO *)old_entry;

	new_entry = old_entry + le32_to_cpu(pDirInfo->NextEntryOffset);
	cFYI(1,("new entry %p old entry %p",new_entry,old_entry));
	/* validate that new_entry is not past end of SMB */
	if(new_entry >= end_of_smb) {
		cFYI(1,("search entry %p began after end of SMB %p old entry %p",
			new_entry,end_of_smb,old_entry)); 
		return NULL;
	} else
		return new_entry;

}

#define UNICODE_DOT cpu_to_le16(0x2e)

/* return 0 if no match and 1 for . (current directory) and 2 for .. (parent) */
static int cifs_entry_is_dot(char * current_entry, struct cifsFileInfo * cfile)
{
	int rc = 0;
	char * filename = NULL;
	int len = 0; 

	if(cfile->srch_inf.info_level == 0x202) {
		FILE_UNIX_INFO * pFindData = (FILE_UNIX_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		if(cfile->srch_inf.unicode) {
			len = cifs_unicode_bytelen(filename);
		} else {
			/* BB should we make this strnlen of PATH_MAX? */
			len = strnlen(filename, 5);
		}
	} else if(cfile->srch_inf.info_level == 0x101) {
		FILE_DIRECTORY_INFO * pFindData = 
			(FILE_DIRECTORY_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		len = le32_to_cpu(pFindData->FileNameLength);
	} else if(cfile->srch_inf.info_level == 0x102) {
		FILE_FULL_DIRECTORY_INFO * pFindData = 
			(FILE_FULL_DIRECTORY_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		len = le32_to_cpu(pFindData->FileNameLength);
	} else if(cfile->srch_inf.info_level == 0x105) {
		SEARCH_ID_FULL_DIR_INFO * pFindData = 
			(SEARCH_ID_FULL_DIR_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		len = le32_to_cpu(pFindData->FileNameLength);
	} else if(cfile->srch_inf.info_level == 0x104) {
		FILE_BOTH_DIRECTORY_INFO * pFindData = 
			(FILE_BOTH_DIRECTORY_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		len = le32_to_cpu(pFindData->FileNameLength);
	} else {
		cFYI(1,("Unknown findfirst level %d",cfile->srch_inf.info_level));
	}

	if(filename) {
		if(cfile->srch_inf.unicode) {
			__le16 *ufilename = (__le16 *)filename;
			if(len == 2) {
				/* check for . */
				if(ufilename[0] == UNICODE_DOT)
					rc = 1;
			} else if(len == 4) {
				/* check for .. */
				if((ufilename[0] == UNICODE_DOT)
				   &&(ufilename[1] == UNICODE_DOT))
					rc = 2;
			}
		} else /* ASCII */ {
			if(len == 1) {
				if(filename[0] == '.') 
					rc = 1;
			} else if(len == 2) {
				if((filename[0] == '.') && (filename[1] == '.')) 
					rc = 2;
			}
		}
	}

	return rc;
}

/* find the corresponding entry in the search */
/* Note that the SMB server returns search entries for . and .. which
   complicates logic here if we choose to parse for them and we do not
   assume that they are located in the findfirst return buffer.*/
/* We start counting in the buffer with entry 2 and increment for every
   entry (do not increment for . or .. entry) */
static int find_cifs_entry(const int xid, struct cifsTconInfo * pTcon, 
		struct file * file, char ** ppCurrentEntry,int * num_to_ret) 
{
	int rc = 0;
	int pos_in_buf = 0;
	loff_t first_entry_in_buffer;
	loff_t index_to_find = file->f_pos;
	struct cifsFileInfo * cifsFile = (struct cifsFileInfo *)file->private_data;
	/* check if index in the buffer */
	
	if((cifsFile == NULL) || (ppCurrentEntry == NULL) || (num_to_ret == NULL))
		return -ENOENT;
	
	*ppCurrentEntry = NULL;
	first_entry_in_buffer = 
		cifsFile->srch_inf.index_of_last_entry - 
			cifsFile->srch_inf.entries_in_buffer;
/*	dump_cifs_file_struct(file, "In fce ");*/
	if(index_to_find < first_entry_in_buffer) {
		/* close and restart search */
		cFYI(1,("search backing up - close and restart search"));
		cifsFile->invalidHandle = TRUE;
		CIFSFindClose(xid, pTcon, cifsFile->netfid);
		if(cifsFile->search_resume_name) {
			kfree(cifsFile->search_resume_name);
			cifsFile->search_resume_name = NULL;
		}
		if(cifsFile->srch_inf.ntwrk_buf_start) {
			cFYI(1,("freeing SMB ff cache buf on search rewind")); 
			cifs_buf_release(cifsFile->srch_inf.ntwrk_buf_start);
		}
		rc = initiate_cifs_search(xid,file);
		if(rc) {
			cFYI(1,("error %d reinitiating a search on rewind",rc));
			return rc;
		}
	}

	while((index_to_find >= cifsFile->srch_inf.index_of_last_entry) && 
	      (rc == 0) && (cifsFile->srch_inf.endOfSearch == FALSE)){
	 	cFYI(1,("calling findnext2"));
		rc = CIFSFindNext2(xid,pTcon,cifsFile->netfid, &cifsFile->srch_inf);
		if(rc)
			return -ENOENT;
	}
	if(index_to_find < cifsFile->srch_inf.index_of_last_entry) {
		/* we found the buffer that contains the entry */
		/* scan and find it */
		int i;
		char * current_entry;
		char * end_of_smb = cifsFile->srch_inf.ntwrk_buf_start + 
			smbCalcSize((struct smb_hdr *)cifsFile->srch_inf.ntwrk_buf_start);
/*	dump_cifs_file_struct(file,"found entry in fce "); */
		first_entry_in_buffer = cifsFile->srch_inf.index_of_last_entry -
			cifsFile->srch_inf.entries_in_buffer;
		pos_in_buf = index_to_find - first_entry_in_buffer;
		cFYI(1,("found entry - pos_in_buf %d",pos_in_buf)); 
		current_entry = cifsFile->srch_inf.srch_entries_start;
		for(i=0;(i<(pos_in_buf)) && (current_entry != NULL);i++) {
			/* go entry to next entry figuring out which we need to start with */
			/* if( . or ..)
				skip */
			rc = cifs_entry_is_dot(current_entry,cifsFile);
			if(rc == 1) /* is . or .. so skip */ {
				cFYI(1,("Entry is .")); /* BB removeme BB */
				/* continue; */
			} else if (rc == 2 ) {
				cFYI(1,("Entry is ..")); /* BB removeme BB */
				/* continue; */
			}
			current_entry = nxt_dir_entry(current_entry,end_of_smb);
		}
		if((current_entry == NULL) && (i < pos_in_buf)) {
			cERROR(1,("reached end of buf searching for pos in buf %d index to find %lld rc %d",pos_in_buf,index_to_find,rc)); /* BB removeme BB */
		}
		rc = 0;
		*ppCurrentEntry = current_entry;
	} else {
		cFYI(1,("index not in buffer - could not findnext into it"));
		return 0;
	}

	if(pos_in_buf >= cifsFile->srch_inf.entries_in_buffer) {
		cFYI(1,("can not return entries when pos_in_buf beyond last entry"));
		*num_to_ret = 0;
	} else
		*num_to_ret = cifsFile->srch_inf.entries_in_buffer - pos_in_buf;
/*	dump_cifs_file_struct(file, "end fce ");*/

	return rc;
}

/* inode num, inode type and filename returned */
static int cifs_get_name_from_search_buf(struct qstr * pqst,char * current_entry,
			__u16 level,unsigned int unicode,struct nls_table * nlt,
			ino_t * pinum)
{
	int rc = 0;
	unsigned int len = 0;
	char * filename;

	*pinum = 0;

	if(level == SMB_FIND_FILE_UNIX) {
		FILE_UNIX_INFO * pFindData = (FILE_UNIX_INFO *)current_entry;

		filename = &pFindData->FileName[0];
		if(unicode) {
			len = cifs_unicode_bytelen(filename);
		} else {
			/* BB should we make this strnlen of PATH_MAX? */
			len = strnlen(filename, PATH_MAX);
		}

		/* BB fixme - hash low and high 32 bits if not 64 bit arch BB fixme */
		*pinum = pFindData->UniqueId;
	} else if(level == SMB_FIND_FILE_DIRECTORY_INFO) {
		FILE_DIRECTORY_INFO * pFindData = 
			(FILE_DIRECTORY_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		len = le32_to_cpu(pFindData->FileNameLength);
	} else if(level == SMB_FIND_FILE_FULL_DIRECTORY_INFO) {
		FILE_FULL_DIRECTORY_INFO * pFindData = 
			(FILE_FULL_DIRECTORY_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		len = le32_to_cpu(pFindData->FileNameLength);
	} else if(level == SMB_FIND_FILE_ID_FULL_DIR_INFO) {
		SEARCH_ID_FULL_DIR_INFO * pFindData = 
			(SEARCH_ID_FULL_DIR_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		len = le32_to_cpu(pFindData->FileNameLength);
		*pinum = pFindData->UniqueId;
	} else if(level == SMB_FIND_FILE_BOTH_DIRECTORY_INFO) {
		FILE_BOTH_DIRECTORY_INFO * pFindData = 
			(FILE_BOTH_DIRECTORY_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		len = le32_to_cpu(pFindData->FileNameLength);
	} else {
		cFYI(1,("Unknown findfirst level %d",level));
		return -EINVAL;
	}
	if(unicode) {
		/* BB fixme - test with long names */
		/* Note converted filename can be longer than in unicode */
		pqst->len = cifs_strfromUCS_le((char *)pqst->name,(wchar_t *)filename,len/2,nlt);
	} else {
		pqst->name = filename;
		pqst->len = len;
	}
	pqst->hash = full_name_hash(pqst->name,pqst->len);
/*	cFYI(1,("filldir on %s",pqst->name));  */
	return rc;
}


static int
cifs_filldir2(char * pfindEntry, struct file *file, 
			  filldir_t filldir, void *direntry,char * scratch_buf)
{
	int rc = 0;
	struct qstr qstring;
	struct cifsFileInfo * pCifsF;
	unsigned obj_type;
	ino_t  inum;
	struct cifs_sb_info * cifs_sb;
	struct inode *tmp_inode;
	struct dentry *tmp_dentry;

	/* get filename and len into qstring */
	/* get dentry */
	/* decide whether to create and populate ionde */
	if((direntry == NULL) || (file == NULL))
		return -EINVAL;

	pCifsF = file->private_data;
	
	if((scratch_buf == NULL) || (pfindEntry == NULL) || (pCifsF == NULL))
		return -ENOENT;

	if(file->f_dentry == NULL)
		return -ENOENT;

	cifs_sb = CIFS_SB(file->f_dentry->d_sb);

	qstring.name = scratch_buf;
	rc = cifs_get_name_from_search_buf(&qstring,pfindEntry,
			pCifsF->srch_inf.info_level,
			pCifsF->srch_inf.unicode,cifs_sb->local_nls,
			&inum /* returned */);

	if(rc)
		return rc;

	rc = construct_dentry(&qstring,file,&tmp_inode, &tmp_dentry);
	if((tmp_inode == NULL) || (tmp_dentry == NULL))
		return -ENOMEM;

	if(rc) {
		/* inode created, we need to hash it with right inode number */
		if(inum != 0) {
			/* BB fixme - hash the 2 32 quantities bits together if necessary BB */
			tmp_inode->i_ino = inum;
		}
		insert_inode_hash(tmp_inode);
	}

	if(pCifsF->srch_inf.info_level == SMB_FIND_FILE_UNIX) {
		unix_fill_in_inode(tmp_inode,(FILE_UNIX_INFO *)pfindEntry,&obj_type);
	} else {
		fill_in_inode(tmp_inode,(FILE_DIRECTORY_INFO *)pfindEntry,&obj_type);
	}
	
	rc = filldir(direntry,qstring.name,qstring.len,file->f_pos,tmp_inode->i_ino,obj_type);
	if(rc) {
		cFYI(1,("filldir rc = %d",rc));
	}

	dput(tmp_dentry);
	return rc;
}

int cifs_save_resume_key(const char * current_entry,struct cifsFileInfo * cifsFile)
{
	int rc = 0;
	unsigned int len = 0;
	__u16 level;
	char * filename;

	if((cifsFile == NULL) || (current_entry == NULL))
		return -EINVAL;

	level = cifsFile->srch_inf.info_level;

	if(level == SMB_FIND_FILE_UNIX) {
		FILE_UNIX_INFO * pFindData = (FILE_UNIX_INFO *)current_entry;

		filename = &pFindData->FileName[0];
		if(cifsFile->srch_inf.unicode) {
			len = cifs_unicode_bytelen(filename);
		} else {
			/* BB should we make this strnlen of PATH_MAX? */
			len = strnlen(filename, PATH_MAX);
		}
		cifsFile->srch_inf.resume_key = pFindData->ResumeKey;
	} else if(level == SMB_FIND_FILE_DIRECTORY_INFO) {
		FILE_DIRECTORY_INFO * pFindData = 
			(FILE_DIRECTORY_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		len = le32_to_cpu(pFindData->FileNameLength);
		cifsFile->srch_inf.resume_key = pFindData->FileIndex;
	} else if(level == SMB_FIND_FILE_FULL_DIRECTORY_INFO) {
		FILE_FULL_DIRECTORY_INFO * pFindData = 
			(FILE_FULL_DIRECTORY_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		len = le32_to_cpu(pFindData->FileNameLength);
		cifsFile->srch_inf.resume_key = pFindData->FileIndex;
	} else if(level == SMB_FIND_FILE_ID_FULL_DIR_INFO) {
		SEARCH_ID_FULL_DIR_INFO * pFindData = 
			(SEARCH_ID_FULL_DIR_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		len = le32_to_cpu(pFindData->FileNameLength);
		cifsFile->srch_inf.resume_key = pFindData->FileIndex;
	} else if(level == SMB_FIND_FILE_BOTH_DIRECTORY_INFO) {
		FILE_BOTH_DIRECTORY_INFO * pFindData = 
			(FILE_BOTH_DIRECTORY_INFO *)current_entry;
		filename = &pFindData->FileName[0];
		len = le32_to_cpu(pFindData->FileNameLength);
		cifsFile->srch_inf.resume_key = pFindData->FileIndex;
	} else {
		cFYI(1,("Unknown findfirst level %d",level));
		return -EINVAL;
	}
	cifsFile->srch_inf.resume_name_len = len;
	cifsFile->srch_inf.presume_name = filename;
	return rc;
}

int cifs_readdir2(struct file *file, void *direntry, filldir_t filldir)
{
	int rc = 0;
	int xid,i;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	struct cifsFileInfo *cifsFile = NULL;
	char * current_entry;
	int num_to_fill = 0;
	char * tmp_buf = NULL;
	char * end_of_smb;

	xid = GetXid();

	if(file->f_dentry == NULL) {
		FreeXid(xid);
		return -EIO;
	}
/*	dump_cifs_file_struct(file, "Begin rdir "); */

	cifs_sb = CIFS_SB(file->f_dentry->d_sb);
	pTcon = cifs_sb->tcon;
	if(pTcon == NULL)
		return -EINVAL;

/*	cFYI(1,("readdir2 pos: %lld",file->f_pos)); */

	switch ((int) file->f_pos) {
	case 0:
		/*if (filldir(direntry, ".", 1, file->f_pos,
		     file->f_dentry->d_inode->i_ino, DT_DIR) < 0) {
			cERROR(1, ("Filldir for current dir failed "));
			rc = -ENOMEM;
			break;
		}
		file->f_pos++; */
	case 1:
		/* if (filldir(direntry, "..", 2, file->f_pos,
		     file->f_dentry->d_parent->d_inode->i_ino, DT_DIR) < 0) {
			cERROR(1, ("Filldir for parent dir failed "));
			rc = -ENOMEM;
			break;
		}
		file->f_pos++; */
	case 2:
		/* 1) If search is active, 
			is in current search buffer? 
			if it before then restart search
			if after then keep searching till find it */

		if(file->private_data == NULL) {
			rc = initiate_cifs_search(xid,file);
			cFYI(1,("initiate cifs search rc %d",rc));
			if(rc) {
				FreeXid(xid);
				return rc;
			}
		}
	default:
		if(file->private_data == NULL) {
			rc = -EINVAL;
			FreeXid(xid);
			return rc;
		}
		cifsFile = (struct cifsFileInfo *) file->private_data;
		if (cifsFile->srch_inf.endOfSearch) {
			if(cifsFile->srch_inf.emptyDir) {
				cFYI(1, ("End of search, empty dir"));
				rc = 0;
				break;
			}
		} /* else {
			cifsFile->invalidHandle = TRUE;
			CIFSFindClose(xid, pTcon, cifsFile->netfid);
		} 
		if(cifsFile->search_resume_name) {
			kfree(cifsFile->search_resume_name);
			cifsFile->search_resume_name = NULL;
		} */
/* BB account for . and .. in f_pos */
		/* dump_cifs_file_struct(file, "rdir after default ");*/

		rc = find_cifs_entry(xid,pTcon, file,
				&current_entry,&num_to_fill);
		if(rc) {
			cFYI(1,("fce error %d",rc)); 
			goto rddir2_exit;
		} else if (current_entry != NULL) {
			cFYI(1,("entry %lld found",file->f_pos));
		} else {
			cFYI(1,("could not find entry"));
			goto rddir2_exit;
		}
		cFYI(1,("loop through %d times filling dir for net buf %p",
			num_to_fill,cifsFile->srch_inf.ntwrk_buf_start)); 
		end_of_smb = cifsFile->srch_inf.ntwrk_buf_start + 
			smbCalcSize((struct smb_hdr *)cifsFile->srch_inf.ntwrk_buf_start);
		tmp_buf = kmalloc(NAME_MAX+1,GFP_KERNEL);
		for(i=0;(i<num_to_fill) && (rc == 0);i++) {
			if(current_entry == NULL) {
				cERROR(1,("beyond end of smb with num to fill %d i %d",num_to_fill,i)); /* BB removeme BB */
				break;
			}
/*			if((!(cifs_sb->mnt_cifs_flags & CIFS_MOUNT_SERVER_INUM)) || 
			   (cifsFile->srch_inf.info_level != something that supports server inodes)) {
				create dentry
				create inode
				fill in inode new_inode (which makes number locally)
			}
			also create local inode for per reasons unless new mount parm says otherwise */
			rc = cifs_filldir2(current_entry, file, 
					filldir, direntry,tmp_buf);
			file->f_pos++;
			if(file->f_pos == cifsFile->srch_inf.index_of_last_entry) {
				cFYI(1,("last entry in buf at pos %lld %s",file->f_pos,tmp_buf)); /* BB removeme BB */
				cifs_save_resume_key(current_entry,cifsFile);
				break;
			} else 
				current_entry = nxt_dir_entry(current_entry,end_of_smb);
		}
		if(tmp_buf != NULL)
			kfree(tmp_buf);
		break;
	} /* end switch */

rddir2_exit:
	/* dump_cifs_file_struct(file, "end rdir ");  */
	FreeXid(xid);
	return rc;
}

