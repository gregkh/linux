/*
 *  linux/fs/fat/misc.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *  22/11/2000 - Fixed fat_date_unix2dos for dates earlier than 01/01/1980
 *		 and date_dos2unix for date==0 by Igor Zhbanov(bsg@uniyar.ac.ru)
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/msdos_fs.h>
#include <linux/buffer_head.h>

/*
 * fat_fs_panic reports a severe file system problem and sets the file system
 * read-only. The file system can be made writable again by remounting it.
 */
void fat_fs_panic(struct super_block *s, const char *fmt, ...)
{
	va_list args;

	printk(KERN_ERR "FAT: Filesystem panic (dev %s)\n", s->s_id);

	printk(KERN_ERR "    ");
	va_start(args, fmt);
	vprintk(fmt, args);
	va_end(args);
	printk("\n");

	if (!(s->s_flags & MS_RDONLY)) {
		s->s_flags |= MS_RDONLY;
		printk(KERN_ERR "    File system has been set read-only\n");
	}
}

void lock_fat(struct super_block *sb)
{
	down(&(MSDOS_SB(sb)->fat_lock));
}

void unlock_fat(struct super_block *sb)
{
	up(&(MSDOS_SB(sb)->fat_lock));
}

/* Flushes the number of free clusters on FAT32 */
/* XXX: Need to write one per FSINFO block.  Currently only writes 1 */
void fat_clusters_flush(struct super_block *sb)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct buffer_head *bh;
	struct fat_boot_fsinfo *fsinfo;

	if (sbi->fat_bits != 32)
		return;

	bh = sb_bread(sb, sbi->fsinfo_sector);
	if (bh == NULL) {
		printk(KERN_ERR "FAT bread failed in fat_clusters_flush\n");
		return;
	}

	fsinfo = (struct fat_boot_fsinfo *)bh->b_data;
	/* Sanity check */
	if (!IS_FSINFO(fsinfo)) {
		printk(KERN_ERR "FAT: Did not find valid FSINFO signature.\n"
		       "     Found signature1 0x%08x signature2 0x%08x"
		       " (sector = %lu)\n",
		       le32_to_cpu(fsinfo->signature1),
		       le32_to_cpu(fsinfo->signature2),
		       sbi->fsinfo_sector);
	} else {
		if (sbi->free_clusters != -1)
			fsinfo->free_clusters = cpu_to_le32(sbi->free_clusters);
		if (sbi->prev_free != -1)
			fsinfo->next_cluster = cpu_to_le32(sbi->prev_free);
		mark_buffer_dirty(bh);
	}
	brelse(bh);
}

/*
 * fat_add_cluster tries to allocate a new cluster and adds it to the
 * file represented by inode.
 */
int fat_add_cluster(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	int ret, count, limit, new_dclus, new_fclus, last;
	int cluster_bits = sbi->cluster_bits;

	/*
	 * We must locate the last cluster of the file to add this new
	 * one (new_dclus) to the end of the link list (the FAT).
	 *
	 * In order to confirm that the cluster chain is valid, we
	 * find out EOF first.
	 */
	last = new_fclus = 0;
	if (MSDOS_I(inode)->i_start) {
		int ret, fclus, dclus;

		ret = fat_get_cluster(inode, FAT_ENT_EOF, &fclus, &dclus);
		if (ret < 0)
			return ret;
		new_fclus = fclus + 1;
		last = dclus;
	}

	/* find free FAT entry */
	lock_fat(sb);

	if (sbi->free_clusters == 0) {
		unlock_fat(sb);
		return -ENOSPC;
	}

	limit = sbi->max_cluster;
	new_dclus = sbi->prev_free + 1;
	for (count = FAT_START_ENT; count < limit; count++, new_dclus++) {
		new_dclus = new_dclus % limit;
		if (new_dclus < FAT_START_ENT)
			new_dclus = FAT_START_ENT;

		ret = fat_access(sb, new_dclus, -1);
		if (ret < 0) {
			unlock_fat(sb);
			return ret;
		} else if (ret == FAT_ENT_FREE)
			break;
	}
	if (count >= limit) {
		sbi->free_clusters = 0;
		unlock_fat(sb);
		return -ENOSPC;
	}

	ret = fat_access(sb, new_dclus, FAT_ENT_EOF);
	if (ret < 0) {
		unlock_fat(sb);
		return ret;
	}

	sbi->prev_free = new_dclus;
	if (sbi->free_clusters != -1)
		sbi->free_clusters--;
	fat_clusters_flush(sb);

	unlock_fat(sb);

	/* add new one to the last of the cluster chain */
	if (last) {
		ret = fat_access(sb, last, new_dclus);
		if (ret < 0)
			return ret;
//		fat_cache_add(inode, new_fclus, new_dclus);
	} else {
		MSDOS_I(inode)->i_start = new_dclus;
		MSDOS_I(inode)->i_logstart = new_dclus;
		mark_inode_dirty(inode);
	}
	if (new_fclus != (inode->i_blocks >> (cluster_bits - 9))) {
		fat_fs_panic(sb, "clusters badly computed (%d != %lu)",
			new_fclus, inode->i_blocks >> (cluster_bits - 9));
		fat_cache_inval_inode(inode);
	}
	inode->i_blocks += sbi->cluster_size >> 9;

	return new_dclus;
}

extern struct timezone sys_tz;

/* Linear day numbers of the respective 1sts in non-leap years. */
static int day_n[] = {
   /* Jan  Feb  Mar  Apr   May  Jun  Jul  Aug  Sep  Oct  Nov  Dec */
	0,  31,  59,  90,  120, 151, 181, 212, 243, 273, 304, 334, 0, 0, 0, 0
};

/* Convert a MS-DOS time/date pair to a UNIX date (seconds since 1 1 70). */
int date_dos2unix(unsigned short time, unsigned short date)
{
	int month, year, secs;

	/*
	 * first subtract and mask after that... Otherwise, if
	 * date == 0, bad things happen
	 */
	month = ((date >> 5) - 1) & 15;
	year = date >> 9;
	secs = (time & 31)*2+60*((time >> 5) & 63)+(time >> 11)*3600+86400*
	    ((date & 31)-1+day_n[month]+(year/4)+year*365-((year & 3) == 0 &&
	    month < 2 ? 1 : 0)+3653);
			/* days since 1.1.70 plus 80's leap day */
	secs += sys_tz.tz_minuteswest*60;
	return secs;
}

/* Convert linear UNIX date to a MS-DOS time/date pair. */
void fat_date_unix2dos(int unix_date, __le16 *time, __le16 *date)
{
	int day, year, nl_day, month;

	unix_date -= sys_tz.tz_minuteswest*60;

	/* Jan 1 GMT 00:00:00 1980. But what about another time zone? */
	if (unix_date < 315532800)
		unix_date = 315532800;

	*time = cpu_to_le16((unix_date % 60)/2+(((unix_date/60) % 60) << 5)+
	    (((unix_date/3600) % 24) << 11));
	day = unix_date/86400-3652;
	year = day/365;
	if ((year+3)/4+365*year > day)
		year--;
	day -= (year+3)/4+365*year;
	if (day == 59 && !(year & 3)) {
		nl_day = day;
		month = 2;
	} else {
		nl_day = (year & 3) || day <= 59 ? day : day-1;
		for (month = 0; month < 12; month++) {
			if (day_n[month] > nl_day)
				break;
		}
	}
	*date = cpu_to_le16(nl_day-day_n[month-1]+1+(month << 5)+(year << 9));
}

EXPORT_SYMBOL(fat_date_unix2dos);

/* Returns the inode number of the directory entry at offset pos. If bh is
   non-NULL, it is brelse'd before. Pos is incremented. The buffer header is
   returned in bh.
   AV. Most often we do it item-by-item. Makes sense to optimize.
   AV. OK, there we go: if both bh and de are non-NULL we assume that we just
   AV. want the next entry (took one explicit de=NULL in vfat/namei.c).
   AV. It's done in fat_get_entry() (inlined), here the slow case lives.
   AV. Additionally, when we return -1 (i.e. reached the end of directory)
   AV. we make bh NULL.
 */

int fat__get_entry(struct inode *dir, loff_t *pos, struct buffer_head **bh,
		   struct msdos_dir_entry **de, loff_t *i_pos)
{
	struct super_block *sb = dir->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	sector_t phys, iblock;
	loff_t offset;
	int err;

next:
	offset = *pos;
	if (*bh)
		brelse(*bh);

	*bh = NULL;
	iblock = *pos >> sb->s_blocksize_bits;
	err = fat_bmap(dir, iblock, &phys);
	if (err || !phys)
		return -1;	/* beyond EOF or error */

	*bh = sb_bread(sb, phys);
	if (*bh == NULL) {
		printk(KERN_ERR "FAT: Directory bread(block %llu) failed\n",
		       (unsigned long long)phys);
		/* skip this block */
		*pos = (iblock + 1) << sb->s_blocksize_bits;
		goto next;
	}

	offset &= sb->s_blocksize - 1;
	*pos += sizeof(struct msdos_dir_entry);
	*de = (struct msdos_dir_entry *)((*bh)->b_data + offset);
	*i_pos = ((loff_t)phys << sbi->dir_per_block_bits) + (offset >> MSDOS_DIR_BITS);

	return 0;
}

EXPORT_SYMBOL(fat__get_entry);
