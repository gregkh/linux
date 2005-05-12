/*
 *  linux/fs/vfat/namei.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *
 *  Windows95/Windows NT compatible extended MSDOS filesystem
 *    by Gordon Chaffee Copyright (C) 1995.  Send bug reports for the
 *    VFAT filesystem to <chaffee@cs.berkeley.edu>.  Specify
 *    what file operation caused you trouble and if you can duplicate
 *    the problem, send a script that demonstrates it.
 *
 *  Short name translation 1999, 2001 by Wolfram Pienkoss <wp@bszh.de>
 *
 *  Support Multibyte characters and cleanup by
 *				OGAWA Hirofumi <hirofumi@mail.parknet.co.jp>
 */

#include <linux/module.h>

#include <linux/jiffies.h>
#include <linux/msdos_fs.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/buffer_head.h>
#include <linux/namei.h>

static int vfat_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	int ret = 1;

	if (!dentry->d_inode &&
	    nd && !(nd->flags & LOOKUP_CONTINUE) && (nd->flags & LOOKUP_CREATE))
		/*
		 * negative dentry is dropped, in order to make sure
		 * to use the name which a user desires if this is
		 * create path.
		 */
		ret = 0;
	else {
		spin_lock(&dentry->d_lock);
		if (dentry->d_time != dentry->d_parent->d_inode->i_version)
			ret = 0;
		spin_unlock(&dentry->d_lock);
	}
	return ret;
}

/* returns the length of a struct qstr, ignoring trailing dots */
static unsigned int vfat_striptail_len(struct qstr *qstr)
{
	unsigned int len = qstr->len;

	while (len && qstr->name[len - 1] == '.')
		len--;
	return len;
}

/*
 * Compute the hash for the vfat name corresponding to the dentry.
 * Note: if the name is invalid, we leave the hash code unchanged so
 * that the existing dentry can be used. The vfat fs routines will
 * return ENOENT or EINVAL as appropriate.
 */
static int vfat_hash(struct dentry *dentry, struct qstr *qstr)
{
	qstr->hash = full_name_hash(qstr->name, vfat_striptail_len(qstr));
	return 0;
}

/*
 * Compute the hash for the vfat name corresponding to the dentry.
 * Note: if the name is invalid, we leave the hash code unchanged so
 * that the existing dentry can be used. The vfat fs routines will
 * return ENOENT or EINVAL as appropriate.
 */
static int vfat_hashi(struct dentry *dentry, struct qstr *qstr)
{
	struct nls_table *t = MSDOS_SB(dentry->d_inode->i_sb)->nls_io;
	const unsigned char *name;
	unsigned int len;
	unsigned long hash;

	name = qstr->name;
	len = vfat_striptail_len(qstr);

	hash = init_name_hash();
	while (len--)
		hash = partial_name_hash(nls_tolower(t, *name++), hash);
	qstr->hash = end_name_hash(hash);

	return 0;
}

/*
 * Case insensitive compare of two vfat names.
 */
static int vfat_cmpi(struct dentry *dentry, struct qstr *a, struct qstr *b)
{
	struct nls_table *t = MSDOS_SB(dentry->d_inode->i_sb)->nls_io;
	unsigned int alen, blen;

	/* A filename cannot end in '.' or we treat it like it has none */
	alen = vfat_striptail_len(a);
	blen = vfat_striptail_len(b);
	if (alen == blen) {
		if (nls_strnicmp(t, a->name, b->name, alen) == 0)
			return 0;
	}
	return 1;
}

/*
 * Case sensitive compare of two vfat names.
 */
static int vfat_cmp(struct dentry *dentry, struct qstr *a, struct qstr *b)
{
	unsigned int alen, blen;

	/* A filename cannot end in '.' or we treat it like it has none */
	alen = vfat_striptail_len(a);
	blen = vfat_striptail_len(b);
	if (alen == blen) {
		if (strncmp(a->name, b->name, alen) == 0)
			return 0;
	}
	return 1;
}

static struct dentry_operations vfat_dentry_ops[4] = {
	{
		.d_hash		= vfat_hashi,
		.d_compare	= vfat_cmpi,
	},
	{
		.d_revalidate	= vfat_revalidate,
		.d_hash		= vfat_hashi,
		.d_compare	= vfat_cmpi,
	},
	{
		.d_hash		= vfat_hash,
		.d_compare	= vfat_cmp,
	},
	{
		.d_revalidate	= vfat_revalidate,
		.d_hash		= vfat_hash,
		.d_compare	= vfat_cmp,
	}
};

/* Characters that are undesirable in an MS-DOS file name */

static inline wchar_t vfat_bad_char(wchar_t w)
{
	return (w < 0x0020)
	    || (w == '*') || (w == '?') || (w == '<') || (w == '>')
	    || (w == '|') || (w == '"') || (w == ':') || (w == '/')
	    || (w == '\\');
}

static inline wchar_t vfat_replace_char(wchar_t w)
{
	return (w == '[') || (w == ']') || (w == ';') || (w == ',')
	    || (w == '+') || (w == '=');
}

static wchar_t vfat_skip_char(wchar_t w)
{
	return (w == '.') || (w == ' ');
}

static inline int vfat_is_used_badchars(const wchar_t *s, int len)
{
	int i;

	for (i = 0; i < len; i++)
		if (vfat_bad_char(s[i]))
			return -EINVAL;
	return 0;
}

static int vfat_valid_longname(const unsigned char *name, unsigned int len)
{
	if (name[len - 1] == ' ')
		return -EINVAL;
	if (len >= 256)
		return -ENAMETOOLONG;

	/* MS-DOS "device special files" */
	if (len == 3 || (len > 3 && name[3] == '.')) {	/* basename == 3 */
		if (!strnicmp(name, "aux", 3) ||
		    !strnicmp(name, "con", 3) ||
		    !strnicmp(name, "nul", 3) ||
		    !strnicmp(name, "prn", 3))
			return -EINVAL;
	}
	if (len == 4 || (len > 4 && name[4] == '.')) {	/* basename == 4 */
		/* "com1", "com2", ... */
		if ('1' <= name[3] && name[3] <= '9') {
			if (!strnicmp(name, "com", 3) ||
			    !strnicmp(name, "lpt", 3))
				return -EINVAL;
		}
	}

	return 0;
}

static int vfat_find_form(struct inode *dir, unsigned char *name)
{
	struct msdos_dir_entry *de;
	struct buffer_head *bh = NULL;
	loff_t i_pos;
	int res;

	res = fat_scan(dir, name, &bh, &de, &i_pos);
	brelse(bh);
	if (res < 0)
		return -ENOENT;
	return 0;
}

/*
 * 1) Valid characters for the 8.3 format alias are any combination of
 * letters, uppercase alphabets, digits, any of the
 * following special characters:
 *     $ % ' ` - @ { } ~ ! # ( ) & _ ^
 * In this case Longfilename is not stored in disk.
 *
 * WinNT's Extension:
 * File name and extension name is contain uppercase/lowercase
 * only. And it is expressed by CASE_LOWER_BASE and CASE_LOWER_EXT.
 *
 * 2) File name is 8.3 format, but it contain the uppercase and
 * lowercase char, muliti bytes char, etc. In this case numtail is not
 * added, but Longfilename is stored.
 *
 * 3) When the one except for the above, or the following special
 * character are contained:
 *        .   [ ] ; , + =
 * numtail is added, and Longfilename must be stored in disk .
 */
struct shortname_info {
	unsigned char lower:1,
		      upper:1,
		      valid:1;
};
#define INIT_SHORTNAME_INFO(x)	do {		\
	(x)->lower = 1;				\
	(x)->upper = 1;				\
	(x)->valid = 1;				\
} while (0)

static inline int to_shortname_char(struct nls_table *nls,
				    unsigned char *buf, int buf_size,
				    wchar_t *src, struct shortname_info *info)
{
	int len;

	if (vfat_skip_char(*src)) {
		info->valid = 0;
		return 0;
	}
	if (vfat_replace_char(*src)) {
		info->valid = 0;
		buf[0] = '_';
		return 1;
	}

	len = nls->uni2char(*src, buf, buf_size);
	if (len <= 0) {
		info->valid = 0;
		buf[0] = '_';
		len = 1;
	} else if (len == 1) {
		unsigned char prev = buf[0];

		if (buf[0] >= 0x7F) {
			info->lower = 0;
			info->upper = 0;
		}

		buf[0] = nls_toupper(nls, buf[0]);
		if (isalpha(buf[0])) {
			if (buf[0] == prev)
				info->lower = 0;
			else
				info->upper = 0;
		}
	} else {
		info->lower = 0;
		info->upper = 0;
	}

	return len;
}

/*
 * Given a valid longname, create a unique shortname.  Make sure the
 * shortname does not exist
 * Returns negative number on error, 0 for a normal
 * return, and 1 for valid shortname
 */
static int vfat_create_shortname(struct inode *dir, struct nls_table *nls,
				 wchar_t *uname, int ulen,
				 unsigned char *name_res, unsigned char *lcase)
{
	wchar_t *ip, *ext_start, *end, *name_start;
	unsigned char base[9], ext[4], buf[8], *p;
	unsigned char charbuf[NLS_MAX_CHARSET_SIZE];
	int chl, chi;
	int sz = 0, extlen, baselen, i, numtail_baselen, numtail2_baselen;
	int is_shortname;
	struct shortname_info base_info, ext_info;
	unsigned short opt_shortname = MSDOS_SB(dir->i_sb)->options.shortname;

	is_shortname = 1;
	INIT_SHORTNAME_INFO(&base_info);
	INIT_SHORTNAME_INFO(&ext_info);

	/* Now, we need to create a shortname from the long name */
	ext_start = end = &uname[ulen];
	while (--ext_start >= uname) {
		if (*ext_start == 0x002E) {	/* is `.' */
			if (ext_start == end - 1) {
				sz = ulen;
				ext_start = NULL;
			}
			break;
		}
	}

	if (ext_start == uname - 1) {
		sz = ulen;
		ext_start = NULL;
	} else if (ext_start) {
		/*
		 * Names which start with a dot could be just
		 * an extension eg. "...test".  In this case Win95
		 * uses the extension as the name and sets no extension.
		 */
		name_start = &uname[0];
		while (name_start < ext_start) {
			if (!vfat_skip_char(*name_start))
				break;
			name_start++;
		}
		if (name_start != ext_start) {
			sz = ext_start - uname;
			ext_start++;
		} else {
			sz = ulen;
			ext_start = NULL;
		}
	}

	numtail_baselen = 6;
	numtail2_baselen = 2;
	for (baselen = i = 0, p = base, ip = uname; i < sz; i++, ip++) {
		chl = to_shortname_char(nls, charbuf, sizeof(charbuf),
					ip, &base_info);
		if (chl == 0)
			continue;

		if (baselen < 2 && (baselen + chl) > 2)
			numtail2_baselen = baselen;
		if (baselen < 6 && (baselen + chl) > 6)
			numtail_baselen = baselen;
		for (chi = 0; chi < chl; chi++) {
			*p++ = charbuf[chi];
			baselen++;
			if (baselen >= 8)
				break;
		}
		if (baselen >= 8) {
			if ((chi < chl - 1) || (ip + 1) - uname < sz)
				is_shortname = 0;
			break;
		}
	}
	if (baselen == 0) {
		return -EINVAL;
	}

	extlen = 0;
	if (ext_start) {
		for (p = ext, ip = ext_start; extlen < 3 && ip < end; ip++) {
			chl = to_shortname_char(nls, charbuf, sizeof(charbuf),
						ip, &ext_info);
			if (chl == 0)
				continue;

			if ((extlen + chl) > 3) {
				is_shortname = 0;
				break;
			}
			for (chi = 0; chi < chl; chi++) {
				*p++ = charbuf[chi];
				extlen++;
			}
			if (extlen >= 3) {
				if (ip + 1 != end)
					is_shortname = 0;
				break;
			}
		}
	}
	ext[extlen] = '\0';
	base[baselen] = '\0';

	/* Yes, it can happen. ".\xe5" would do it. */
	if (base[0] == DELETED_FLAG)
		base[0] = 0x05;

	/* OK, at this point we know that base is not longer than 8 symbols,
	 * ext is not longer than 3, base is nonempty, both don't contain
	 * any bad symbols (lowercase transformed to uppercase).
	 */

	memset(name_res, ' ', MSDOS_NAME);
	memcpy(name_res, base, baselen);
	memcpy(name_res + 8, ext, extlen);
	*lcase = 0;
	if (is_shortname && base_info.valid && ext_info.valid) {
		if (vfat_find_form(dir, name_res) == 0)
			return -EEXIST;

		if (opt_shortname & VFAT_SFN_CREATE_WIN95) {
			return (base_info.upper && ext_info.upper);
		} else if (opt_shortname & VFAT_SFN_CREATE_WINNT) {
			if ((base_info.upper || base_info.lower) &&
			    (ext_info.upper || ext_info.lower)) {
				if (!base_info.upper && base_info.lower)
					*lcase |= CASE_LOWER_BASE;
				if (!ext_info.upper && ext_info.lower)
					*lcase |= CASE_LOWER_EXT;
				return 1;
			}
			return 0;
		} else {
			BUG();
		}
	}

	if (MSDOS_SB(dir->i_sb)->options.numtail == 0)
		if (vfat_find_form(dir, name_res) < 0)
			return 0;

	/*
	 * Try to find a unique extension.  This used to
	 * iterate through all possibilities sequentially,
	 * but that gave extremely bad performance.  Windows
	 * only tries a few cases before using random
	 * values for part of the base.
	 */

	if (baselen > 6) {
		baselen = numtail_baselen;
		name_res[7] = ' ';
	}
	name_res[baselen] = '~';
	for (i = 1; i < 10; i++) {
		name_res[baselen + 1] = i + '0';
		if (vfat_find_form(dir, name_res) < 0)
			return 0;
	}

	i = jiffies & 0xffff;
	sz = (jiffies >> 16) & 0x7;
	if (baselen > 2) {
		baselen = numtail2_baselen;
		name_res[7] = ' ';
	}
	name_res[baselen + 4] = '~';
	name_res[baselen + 5] = '1' + sz;
	while (1) {
		sprintf(buf, "%04X", i);
		memcpy(&name_res[baselen], buf, 4);
		if (vfat_find_form(dir, name_res) < 0)
			break;
		i -= 11;
	}
	return 0;
}

/* Translate a string, including coded sequences into Unicode */
static int
xlate_to_uni(const unsigned char *name, int len, unsigned char *outname,
	     int *longlen, int *outlen, int escape, int utf8,
	     struct nls_table *nls)
{
	const unsigned char *ip;
	unsigned char nc;
	unsigned char *op;
	unsigned int ec;
	int i, k, fill;
	int charlen;

	if (utf8) {
		int name_len = strlen(name);

		*outlen = utf8_mbstowcs((wchar_t *)outname, name, PAGE_SIZE);

		/*
		 * We stripped '.'s before and set len appropriately,
		 * but utf8_mbstowcs doesn't care about len
		 */
		*outlen -= (name_len - len);

		op = &outname[*outlen * sizeof(wchar_t)];
	} else {
		if (nls) {
			for (i = 0, ip = name, op = outname, *outlen = 0;
			     i < len && *outlen <= 260;
			     *outlen += 1)
			{
				if (escape && (*ip == ':')) {
					if (i > len - 5)
						return -EINVAL;
					ec = 0;
					for (k = 1; k < 5; k++) {
						nc = ip[k];
						ec <<= 4;
						if (nc >= '0' && nc <= '9') {
							ec |= nc - '0';
							continue;
						}
						if (nc >= 'a' && nc <= 'f') {
							ec |= nc - ('a' - 10);
							continue;
						}
						if (nc >= 'A' && nc <= 'F') {
							ec |= nc - ('A' - 10);
							continue;
						}
						return -EINVAL;
					}
					*op++ = ec & 0xFF;
					*op++ = ec >> 8;
					ip += 5;
					i += 5;
				} else {
					if ((charlen = nls->char2uni(ip, len - i, (wchar_t *)op)) < 0)
						return -EINVAL;
					ip += charlen;
					i += charlen;
					op += 2;
				}
			}
		} else {
			for (i = 0, ip = name, op = outname, *outlen = 0;
			     i < len && *outlen <= 260;
			     i++, *outlen += 1)
			{
				*op++ = *ip++;
				*op++ = 0;
			}
		}
	}
	if (*outlen > 260)
		return -ENAMETOOLONG;

	*longlen = *outlen;
	if (*outlen % 13) {
		*op++ = 0;
		*op++ = 0;
		*outlen += 1;
		if (*outlen % 13) {
			fill = 13 - (*outlen % 13);
			for (i = 0; i < fill; i++) {
				*op++ = 0xff;
				*op++ = 0xff;
			}
			*outlen += fill;
		}
	}

	return 0;
}

static int vfat_build_slots(struct inode *dir, const unsigned char *name,
			    int len, struct msdos_dir_slot *ds,
			    int *slots, int is_dir)
{
	struct msdos_sb_info *sbi = MSDOS_SB(dir->i_sb);
	struct fat_mount_options *opts = &sbi->options;
	struct msdos_dir_slot *ps;
	struct msdos_dir_entry *de;
	unsigned long page;
	unsigned char cksum, lcase;
	unsigned char msdos_name[MSDOS_NAME];
	wchar_t *uname;
	int res, slot, ulen, usize, i;
	loff_t offset;

	*slots = 0;
	res = vfat_valid_longname(name, len);
	if (res)
		return res;

	page = __get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	uname = (wchar_t *)page;
	res = xlate_to_uni(name, len, (unsigned char *)uname, &ulen, &usize,
			   opts->unicode_xlate, opts->utf8, sbi->nls_io);
	if (res < 0)
		goto out_free;

	res = vfat_is_used_badchars(uname, ulen);
	if (res < 0)
		goto out_free;

	res = vfat_create_shortname(dir, sbi->nls_disk, uname, ulen,
				    msdos_name, &lcase);
	if (res < 0)
		goto out_free;
	else if (res == 1) {
		de = (struct msdos_dir_entry *)ds;
		res = 0;
		goto shortname;
	}

	/* build the entry of long file name */
	*slots = usize / 13;
	for (cksum = i = 0; i < 11; i++)
		cksum = (((cksum&1)<<7)|((cksum&0xfe)>>1)) + msdos_name[i];

	for (ps = ds, slot = *slots; slot > 0; slot--, ps++) {
		ps->id = slot;
		ps->attr = ATTR_EXT;
		ps->reserved = 0;
		ps->alias_checksum = cksum;
		ps->start = 0;
		offset = (slot - 1) * 13;
		fatwchar_to16(ps->name0_4, uname + offset, 5);
		fatwchar_to16(ps->name5_10, uname + offset + 5, 6);
		fatwchar_to16(ps->name11_12, uname + offset + 11, 2);
	}
	ds[0].id |= 0x40;
	de = (struct msdos_dir_entry *)ps;

shortname:
	/* build the entry of 8.3 alias name */
	(*slots)++;
	memcpy(de->name, msdos_name, MSDOS_NAME);
	de->attr = is_dir ? ATTR_DIR : ATTR_ARCH;
	de->lcase = lcase;
	de->adate = de->cdate = de->date = 0;
	de->ctime = de->time = 0;
	de->ctime_ms = 0;
	de->start = 0;
	de->starthi = 0;
	de->size = 0;

out_free:
	free_page(page);
	return res;
}

static int vfat_add_entry(struct inode *dir, struct qstr *qname,
			  int is_dir, struct vfat_slot_info *sinfo_out,
			  struct buffer_head **bh, struct msdos_dir_entry **de)
{
	struct msdos_dir_slot *dir_slots;
	loff_t offset;
	int res, slots, slot;
	unsigned int len;
	struct msdos_dir_entry *dummy_de;
	struct buffer_head *dummy_bh;
	loff_t dummy_i_pos;

	len = vfat_striptail_len(qname);
	if (len == 0)
		return -ENOENT;

	dir_slots = kmalloc(sizeof(*dir_slots) * MSDOS_SLOTS, GFP_KERNEL);
	if (dir_slots == NULL)
		return -ENOMEM;

	res = vfat_build_slots(dir, qname->name, len,
			       dir_slots, &slots, is_dir);
	if (res < 0)
		goto cleanup;

	/* build the empty directory entry of number of slots */
	offset =
	    fat_add_entries(dir, slots, &dummy_bh, &dummy_de, &dummy_i_pos);
	if (offset < 0) {
		res = offset;
		goto cleanup;
	}
	brelse(dummy_bh);

	/* Now create the new entry */
	*bh = NULL;
	for (slot = 0; slot < slots; slot++) {
		if (fat_get_entry(dir, &offset, bh, de, &sinfo_out->i_pos) < 0) {
			res = -EIO;
			goto cleanup;
		}
		memcpy(*de, dir_slots + slot, sizeof(struct msdos_dir_slot));
		mark_buffer_dirty(*bh);
	}

	res = 0;
	/* update timestamp */
	dir->i_ctime = dir->i_mtime = dir->i_atime = CURRENT_TIME_SEC;
	mark_inode_dirty(dir);

	fat_date_unix2dos(dir->i_mtime.tv_sec, &(*de)->time, &(*de)->date);
	dir->i_mtime.tv_nsec = 0;
	(*de)->ctime = (*de)->time;
	(*de)->adate = (*de)->cdate = (*de)->date;

	mark_buffer_dirty(*bh);

	/* slots can't be less than 1 */
	sinfo_out->long_slots = slots - 1;
	sinfo_out->longname_offset =
		offset - sizeof(struct msdos_dir_slot) * slots;

cleanup:
	kfree(dir_slots);
	return res;
}

static int vfat_find(struct inode *dir, struct qstr *qname,
		     struct vfat_slot_info *sinfo, struct buffer_head **last_bh,
		     struct msdos_dir_entry **last_de)
{
	struct super_block *sb = dir->i_sb;
	loff_t offset;
	unsigned int len;
	int res;

	len = vfat_striptail_len(qname);
	if (len == 0)
		return -ENOENT;

	res = fat_search_long(dir, qname->name, len,
			      (MSDOS_SB(sb)->options.name_check != 's'),
			      &offset, &sinfo->longname_offset);
	if (res > 0) {
		sinfo->long_slots = res - 1;
		if (fat_get_entry(dir, &offset, last_bh, last_de, &sinfo->i_pos) >= 0)
			return 0;
		res = -EIO;
	}
	return res ? res : -ENOENT;
}

static struct dentry *vfat_lookup(struct inode *dir, struct dentry *dentry,
				  struct nameidata *nd)
{
	int res;
	struct vfat_slot_info sinfo;
	struct inode *inode;
	struct dentry *alias;
	struct buffer_head *bh = NULL;
	struct msdos_dir_entry *de;
	int table;

	lock_kernel();
	table = (MSDOS_SB(dir->i_sb)->options.name_check == 's') ? 2 : 0;
	dentry->d_op = &vfat_dentry_ops[table];

	inode = NULL;
	res = vfat_find(dir, &dentry->d_name, &sinfo, &bh, &de);
	if (res < 0) {
		table++;
		goto error;
	}
	inode = fat_build_inode(dir->i_sb, de, sinfo.i_pos, &res);
	brelse(bh);
	if (res) {
		unlock_kernel();
		return ERR_PTR(res);
	}
	alias = d_find_alias(inode);
	if (alias) {
		if (d_invalidate(alias) == 0)
			dput(alias);
		else {
			iput(inode);
			unlock_kernel();
			return alias;
		}

	}
error:
	unlock_kernel();
	dentry->d_op = &vfat_dentry_ops[table];
	dentry->d_time = dentry->d_parent->d_inode->i_version;
	dentry = d_splice_alias(inode, dentry);
	if (dentry) {
		dentry->d_op = &vfat_dentry_ops[table];
		dentry->d_time = dentry->d_parent->d_inode->i_version;
	}
	return dentry;
}

static int vfat_create(struct inode *dir, struct dentry *dentry, int mode,
		       struct nameidata *nd)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = NULL;
	struct buffer_head *bh = NULL;
	struct msdos_dir_entry *de;
	struct vfat_slot_info sinfo;
	int res;

	lock_kernel();
	res = vfat_add_entry(dir, &dentry->d_name, 0, &sinfo, &bh, &de);
	if (res < 0)
		goto out;
	inode = fat_build_inode(sb, de, sinfo.i_pos, &res);
	brelse(bh);
	if (!inode)
		goto out;
	res = 0;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode);
	inode->i_version++;
	dir->i_version++;
	dentry->d_time = dentry->d_parent->d_inode->i_version;
	d_instantiate(dentry, inode);
out:
	unlock_kernel();
	return res;
}

static void vfat_remove_entry(struct inode *dir, struct vfat_slot_info *sinfo,
			      struct buffer_head *bh,
			      struct msdos_dir_entry *de)
{
	loff_t offset, i_pos;
	int i;

	/* remove the shortname */
	dir->i_mtime = dir->i_atime = CURRENT_TIME_SEC;
	dir->i_version++;
	mark_inode_dirty(dir);
	de->name[0] = DELETED_FLAG;
	mark_buffer_dirty(bh);
	/* remove the longname */
	offset = sinfo->longname_offset;
	de = NULL;
	for (i = sinfo->long_slots; i > 0; --i) {
		if (fat_get_entry(dir, &offset, &bh, &de, &i_pos) < 0)
			continue;
		de->name[0] = DELETED_FLAG;
		de->attr = ATTR_NONE;
		mark_buffer_dirty(bh);
	}
	brelse(bh);
}

static int vfat_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct vfat_slot_info sinfo;
	struct buffer_head *bh = NULL;
	struct msdos_dir_entry *de;
	int res;

	lock_kernel();
	res = fat_dir_empty(inode);
	if (res)
		goto out;

	res = vfat_find(dir, &dentry->d_name, &sinfo, &bh, &de);
	if (res < 0)
		goto out;

	res = 0;
	inode->i_nlink = 0;
	inode->i_mtime = inode->i_atime = CURRENT_TIME_SEC;
	fat_detach(inode);
	mark_inode_dirty(inode);
	/* releases bh */
	vfat_remove_entry(dir, &sinfo, bh, de);
	dir->i_nlink--;
out:
	unlock_kernel();
	return res;
}

static int vfat_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct vfat_slot_info sinfo;
	struct buffer_head *bh = NULL;
	struct msdos_dir_entry *de;
	int res;

	lock_kernel();
	res = vfat_find(dir, &dentry->d_name, &sinfo, &bh, &de);
	if (res < 0)
		goto out;
	inode->i_nlink = 0;
	inode->i_mtime = inode->i_atime = CURRENT_TIME_SEC;
	fat_detach(inode);
	mark_inode_dirty(inode);
	/* releases bh */
	vfat_remove_entry(dir, &sinfo, bh, de);
out:
	unlock_kernel();

	return res;
}

static int vfat_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = NULL;
	struct vfat_slot_info sinfo;
	struct buffer_head *bh = NULL;
	struct msdos_dir_entry *de;
	int res;

	lock_kernel();
	res = vfat_add_entry(dir, &dentry->d_name, 1, &sinfo, &bh, &de);
	if (res < 0)
		goto out;
	inode = fat_build_inode(sb, de, sinfo.i_pos, &res);
	if (!inode)
		goto out_brelse;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode);
	inode->i_version++;
	dir->i_version++;
	dir->i_nlink++;
	inode->i_nlink = 2;	/* no need to mark them dirty */
	res = fat_new_dir(inode, dir, 1);
	if (res < 0)
		goto mkdir_failed;
	dentry->d_time = dentry->d_parent->d_inode->i_version;
	d_instantiate(dentry, inode);
out_brelse:
	brelse(bh);
out:
	unlock_kernel();
	return res;

mkdir_failed:
	inode->i_nlink = 0;
	inode->i_mtime = inode->i_atime = CURRENT_TIME_SEC;
	fat_detach(inode);
	mark_inode_dirty(inode);
	/* releases bh */
	vfat_remove_entry(dir, &sinfo, bh, de);
	iput(inode);
	dir->i_nlink--;
	goto out;
}

static int vfat_rename(struct inode *old_dir, struct dentry *old_dentry,
		       struct inode *new_dir, struct dentry *new_dentry)
{
	struct buffer_head *old_bh, *new_bh, *dotdot_bh;
	struct msdos_dir_entry *old_de, *new_de, *dotdot_de;
	loff_t dotdot_i_pos;
	struct inode *old_inode, *new_inode;
	int res, is_dir;
	struct vfat_slot_info old_sinfo, sinfo;

	old_bh = new_bh = dotdot_bh = NULL;
	old_inode = old_dentry->d_inode;
	new_inode = new_dentry->d_inode;
	lock_kernel();
	res = vfat_find(old_dir, &old_dentry->d_name, &old_sinfo, &old_bh,
			&old_de);
	if (res < 0)
		goto rename_done;

	is_dir = S_ISDIR(old_inode->i_mode);

	if (is_dir) {
		if (fat_scan(old_inode, MSDOS_DOTDOT, &dotdot_bh,
			     &dotdot_de, &dotdot_i_pos) < 0) {
			res = -EIO;
			goto rename_done;
		}
	}

	if (new_dentry->d_inode) {
		res = vfat_find(new_dir, &new_dentry->d_name, &sinfo, &new_bh,
				&new_de);
		if (res < 0 || MSDOS_I(new_inode)->i_pos != sinfo.i_pos) {
			/* WTF??? Cry and fail. */
			printk(KERN_WARNING "vfat_rename: fs corrupted\n");
			goto rename_done;
		}

		if (is_dir) {
			res = fat_dir_empty(new_inode);
			if (res)
				goto rename_done;
		}
		fat_detach(new_inode);
	} else {
		res = vfat_add_entry(new_dir, &new_dentry->d_name, is_dir,
				     &sinfo, &new_bh, &new_de);
		if (res < 0)
			goto rename_done;
	}

	new_dir->i_version++;

	/* releases old_bh */
	vfat_remove_entry(old_dir, &old_sinfo, old_bh, old_de);
	old_bh = NULL;
	fat_detach(old_inode);
	fat_attach(old_inode, sinfo.i_pos);
	mark_inode_dirty(old_inode);

	old_dir->i_version++;
	old_dir->i_ctime = old_dir->i_mtime = CURRENT_TIME_SEC;
	mark_inode_dirty(old_dir);
	if (new_inode) {
		new_inode->i_nlink--;
		new_inode->i_ctime = CURRENT_TIME_SEC;
	}

	if (is_dir) {
		int start = MSDOS_I(new_dir)->i_logstart;
		dotdot_de->start = cpu_to_le16(start);
		dotdot_de->starthi = cpu_to_le16(start>>16);
		mark_buffer_dirty(dotdot_bh);
		old_dir->i_nlink--;
		if (new_inode) {
			new_inode->i_nlink--;
		} else {
			new_dir->i_nlink++;
			mark_inode_dirty(new_dir);
		}
	}

rename_done:
	brelse(dotdot_bh);
	brelse(old_bh);
	brelse(new_bh);
	unlock_kernel();
	return res;
}

static struct inode_operations vfat_dir_inode_operations = {
	.create		= vfat_create,
	.lookup		= vfat_lookup,
	.unlink		= vfat_unlink,
	.mkdir		= vfat_mkdir,
	.rmdir		= vfat_rmdir,
	.rename		= vfat_rename,
	.setattr	= fat_notify_change,
};

static int vfat_fill_super(struct super_block *sb, void *data, int silent)
{
	int res;

	res = fat_fill_super(sb, data, silent, &vfat_dir_inode_operations, 1);
	if (res)
		return res;

	if (MSDOS_SB(sb)->options.name_check != 's')
		sb->s_root->d_op = &vfat_dentry_ops[0];
	else
		sb->s_root->d_op = &vfat_dentry_ops[2];

	return 0;
}

static struct super_block *vfat_get_sb(struct file_system_type *fs_type,
				       int flags, const char *dev_name,
				       void *data)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, vfat_fill_super);
}

static struct file_system_type vfat_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "vfat",
	.get_sb		= vfat_get_sb,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init_vfat_fs(void)
{
	return register_filesystem(&vfat_fs_type);
}

static void __exit exit_vfat_fs(void)
{
	unregister_filesystem(&vfat_fs_type);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VFAT filesystem support");
MODULE_AUTHOR("Gordon Chaffee");

module_init(init_vfat_fs)
module_exit(exit_vfat_fs)
