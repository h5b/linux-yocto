/*
 *  linux/fs/hfs/inode.c
 *
 * Copyright (C) 1995-1997  Paul H. Hargrove
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 * This file may be distributed under the terms of the GNU General Public License.
 *
 * This file contains inode-related functions which do not depend on
 * which scheme is being used to represent forks.
 *
 * Based on the minix file system code, (C) 1991, 1992 by Linus Torvalds
 */

#include <linux/pagemap.h>
#include <linux/mpage.h>

#include "hfs_fs.h"
#include "btree.h"

static struct file_operations hfs_file_operations;
static struct inode_operations hfs_file_inode_operations;

/*================ Variable-like macros ================*/

#define HFS_VALID_MODE_BITS  (S_IFREG | S_IFDIR | S_IRWXUGO)

static int hfs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, hfs_get_block, wbc);
}

static int hfs_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page, hfs_get_block);
}

static int hfs_prepare_write(struct file *file, struct page *page, unsigned from, unsigned to)
{
	return cont_prepare_write(page, from, to, hfs_get_block,
				  &HFS_I(page->mapping->host)->phys_size);
}

static sector_t hfs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, hfs_get_block);
}

static int hfs_releasepage(struct page *page, gfp_t mask)
{
	struct inode *inode = page->mapping->host;
	struct super_block *sb = inode->i_sb;
	struct hfs_btree *tree;
	struct hfs_bnode *node;
	u32 nidx;
	int i, res = 1;

	switch (inode->i_ino) {
	case HFS_EXT_CNID:
		tree = HFS_SB(sb)->ext_tree;
		break;
	case HFS_CAT_CNID:
		tree = HFS_SB(sb)->cat_tree;
		break;
	default:
		BUG();
		return 0;
	}
	if (tree->node_size >= PAGE_CACHE_SIZE) {
		nidx = page->index >> (tree->node_size_shift - PAGE_CACHE_SHIFT);
		spin_lock(&tree->hash_lock);
		node = hfs_bnode_findhash(tree, nidx);
		if (!node)
			;
		else if (atomic_read(&node->refcnt))
			res = 0;
		if (res && node) {
			hfs_bnode_unhash(node);
			hfs_bnode_free(node);
		}
		spin_unlock(&tree->hash_lock);
	} else {
		nidx = page->index << (PAGE_CACHE_SHIFT - tree->node_size_shift);
		i = 1 << (PAGE_CACHE_SHIFT - tree->node_size_shift);
		spin_lock(&tree->hash_lock);
		do {
			node = hfs_bnode_findhash(tree, nidx++);
			if (!node)
				continue;
			if (atomic_read(&node->refcnt)) {
				res = 0;
				break;
			}
			hfs_bnode_unhash(node);
			hfs_bnode_free(node);
		} while (--i && nidx < tree->node_count);
		spin_unlock(&tree->hash_lock);
	}
	//printk("releasepage: %lu,%x = %d\n", page->index, mask, res);
	return res ? try_to_free_buffers(page) : 0;
}

static int hfs_get_blocks(struct inode *inode, sector_t iblock, unsigned long max_blocks,
			  struct buffer_head *bh_result, int create)
{
	int ret;

	ret = hfs_get_block(inode, iblock, bh_result, create);
	if (!ret)
		bh_result->b_size = (1 << inode->i_blkbits);
	return ret;
}

static ssize_t hfs_direct_IO(int rw, struct kiocb *iocb,
		const struct iovec *iov, loff_t offset, unsigned long nr_segs)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_dentry->d_inode->i_mapping->host;

	return blockdev_direct_IO(rw, iocb, inode, inode->i_sb->s_bdev, iov,
				  offset, nr_segs, hfs_get_blocks, NULL);
}

static int hfs_writepages(struct address_space *mapping,
			  struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, hfs_get_block);
}

struct address_space_operations hfs_btree_aops = {
	.readpage	= hfs_readpage,
	.writepage	= hfs_writepage,
	.sync_page	= block_sync_page,
	.prepare_write	= hfs_prepare_write,
	.commit_write	= generic_commit_write,
	.bmap		= hfs_bmap,
	.releasepage	= hfs_releasepage,
};

struct address_space_operations hfs_aops = {
	.readpage	= hfs_readpage,
	.writepage	= hfs_writepage,
	.sync_page	= block_sync_page,
	.prepare_write	= hfs_prepare_write,
	.commit_write	= generic_commit_write,
	.bmap		= hfs_bmap,
	.direct_IO	= hfs_direct_IO,
	.writepages	= hfs_writepages,
};

/*
 * hfs_new_inode
 */
struct inode *hfs_new_inode(struct inode *dir, struct qstr *name, int mode)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = new_inode(sb);
	if (!inode)
		return NULL;

	init_MUTEX(&HFS_I(inode)->extents_lock);
	INIT_LIST_HEAD(&HFS_I(inode)->open_dir_list);
	hfs_cat_build_key(sb, (btree_key *)&HFS_I(inode)->cat_key, dir->i_ino, name);
	inode->i_ino = HFS_SB(sb)->next_id++;
	inode->i_mode = mode;
	inode->i_uid = current->fsuid;
	inode->i_gid = current->fsgid;
	inode->i_nlink = 1;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME_SEC;
	inode->i_blksize = HFS_SB(sb)->alloc_blksz;
	HFS_I(inode)->flags = 0;
	HFS_I(inode)->rsrc_inode = NULL;
	HFS_I(inode)->fs_blocks = 0;
	if (S_ISDIR(mode)) {
		inode->i_size = 2;
		HFS_SB(sb)->folder_count++;
		if (dir->i_ino == HFS_ROOT_CNID)
			HFS_SB(sb)->root_dirs++;
		inode->i_op = &hfs_dir_inode_operations;
		inode->i_fop = &hfs_dir_operations;
		inode->i_mode |= S_IRWXUGO;
		inode->i_mode &= ~HFS_SB(inode->i_sb)->s_dir_umask;
	} else if (S_ISREG(mode)) {
		HFS_I(inode)->clump_blocks = HFS_SB(sb)->clumpablks;
		HFS_SB(sb)->file_count++;
		if (dir->i_ino == HFS_ROOT_CNID)
			HFS_SB(sb)->root_files++;
		inode->i_op = &hfs_file_inode_operations;
		inode->i_fop = &hfs_file_operations;
		inode->i_mapping->a_ops = &hfs_aops;
		inode->i_mode |= S_IRUGO|S_IXUGO;
		if (mode & S_IWUSR)
			inode->i_mode |= S_IWUGO;
		inode->i_mode &= ~HFS_SB(inode->i_sb)->s_file_umask;
		HFS_I(inode)->phys_size = 0;
		HFS_I(inode)->alloc_blocks = 0;
		HFS_I(inode)->first_blocks = 0;
		HFS_I(inode)->cached_start = 0;
		HFS_I(inode)->cached_blocks = 0;
		memset(HFS_I(inode)->first_extents, 0, sizeof(hfs_extent_rec));
		memset(HFS_I(inode)->cached_extents, 0, sizeof(hfs_extent_rec));
	}
	insert_inode_hash(inode);
	mark_inode_dirty(inode);
	set_bit(HFS_FLG_MDB_DIRTY, &HFS_SB(sb)->flags);
	sb->s_dirt = 1;

	return inode;
}

void hfs_delete_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	dprint(DBG_INODE, "delete_inode: %lu\n", inode->i_ino);
	if (S_ISDIR(inode->i_mode)) {
		HFS_SB(sb)->folder_count--;
		if (HFS_I(inode)->cat_key.ParID == cpu_to_be32(HFS_ROOT_CNID))
			HFS_SB(sb)->root_dirs--;
		set_bit(HFS_FLG_MDB_DIRTY, &HFS_SB(sb)->flags);
		sb->s_dirt = 1;
		return;
	}
	HFS_SB(sb)->file_count--;
	if (HFS_I(inode)->cat_key.ParID == cpu_to_be32(HFS_ROOT_CNID))
		HFS_SB(sb)->root_files--;
	if (S_ISREG(inode->i_mode)) {
		if (!inode->i_nlink) {
			inode->i_size = 0;
			hfs_file_truncate(inode);
		}
	}
	set_bit(HFS_FLG_MDB_DIRTY, &HFS_SB(sb)->flags);
	sb->s_dirt = 1;
}

void hfs_inode_read_fork(struct inode *inode, struct hfs_extent *ext,
			 __be32 __log_size, __be32 phys_size, u32 clump_size)
{
	struct super_block *sb = inode->i_sb;
	u32 log_size = be32_to_cpu(__log_size);
	u16 count;
	int i;

	memcpy(HFS_I(inode)->first_extents, ext, sizeof(hfs_extent_rec));
	for (count = 0, i = 0; i < 3; i++)
		count += be16_to_cpu(ext[i].count);
	HFS_I(inode)->first_blocks = count;

	inode->i_size = HFS_I(inode)->phys_size = log_size;
	HFS_I(inode)->fs_blocks = (log_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
	inode_set_bytes(inode, HFS_I(inode)->fs_blocks << sb->s_blocksize_bits);
	HFS_I(inode)->alloc_blocks = be32_to_cpu(phys_size) /
				     HFS_SB(sb)->alloc_blksz;
	HFS_I(inode)->clump_blocks = clump_size / HFS_SB(sb)->alloc_blksz;
	if (!HFS_I(inode)->clump_blocks)
		HFS_I(inode)->clump_blocks = HFS_SB(sb)->clumpablks;
}

struct hfs_iget_data {
	struct hfs_cat_key *key;
	hfs_cat_rec *rec;
};

static int hfs_test_inode(struct inode *inode, void *data)
{
	struct hfs_iget_data *idata = data;
	hfs_cat_rec *rec;

	rec = idata->rec;
	switch (rec->type) {
	case HFS_CDR_DIR:
		return inode->i_ino == be32_to_cpu(rec->dir.DirID);
	case HFS_CDR_FIL:
		return inode->i_ino == be32_to_cpu(rec->file.FlNum);
	default:
		BUG();
		return 1;
	}
}

/*
 * hfs_read_inode
 */
static int hfs_read_inode(struct inode *inode, void *data)
{
	struct hfs_iget_data *idata = data;
	struct hfs_sb_info *hsb = HFS_SB(inode->i_sb);
	hfs_cat_rec *rec;

	HFS_I(inode)->flags = 0;
	HFS_I(inode)->rsrc_inode = NULL;
	init_MUTEX(&HFS_I(inode)->extents_lock);
	INIT_LIST_HEAD(&HFS_I(inode)->open_dir_list);

	/* Initialize the inode */
	inode->i_uid = hsb->s_uid;
	inode->i_gid = hsb->s_gid;
	inode->i_nlink = 1;
	inode->i_blksize = HFS_SB(inode->i_sb)->alloc_blksz;

	if (idata->key)
		HFS_I(inode)->cat_key = *idata->key;
	else
		HFS_I(inode)->flags |= HFS_FLG_RSRC;
	HFS_I(inode)->tz_secondswest = sys_tz.tz_minuteswest * 60;

	rec = idata->rec;
	switch (rec->type) {
	case HFS_CDR_FIL:
		if (!HFS_IS_RSRC(inode)) {
			hfs_inode_read_fork(inode, rec->file.ExtRec, rec->file.LgLen,
					    rec->file.PyLen, be16_to_cpu(rec->file.ClpSize));
		} else {
			hfs_inode_read_fork(inode, rec->file.RExtRec, rec->file.RLgLen,
					    rec->file.RPyLen, be16_to_cpu(rec->file.ClpSize));
		}

		inode->i_ino = be32_to_cpu(rec->file.FlNum);
		inode->i_mode = S_IRUGO | S_IXUGO;
		if (!(rec->file.Flags & HFS_FIL_LOCK))
			inode->i_mode |= S_IWUGO;
		inode->i_mode &= ~hsb->s_file_umask;
		inode->i_mode |= S_IFREG;
		inode->i_ctime = inode->i_atime = inode->i_mtime =
				hfs_m_to_utime(rec->file.MdDat);
		inode->i_op = &hfs_file_inode_operations;
		inode->i_fop = &hfs_file_operations;
		inode->i_mapping->a_ops = &hfs_aops;
		break;
	case HFS_CDR_DIR:
		inode->i_ino = be32_to_cpu(rec->dir.DirID);
		inode->i_size = be16_to_cpu(rec->dir.Val) + 2;
		HFS_I(inode)->fs_blocks = 0;
		inode->i_mode = S_IFDIR | (S_IRWXUGO & ~hsb->s_dir_umask);
		inode->i_ctime = inode->i_atime = inode->i_mtime =
				hfs_m_to_utime(rec->dir.MdDat);
		inode->i_op = &hfs_dir_inode_operations;
		inode->i_fop = &hfs_dir_operations;
		break;
	default:
		make_bad_inode(inode);
	}
	return 0;
}

/*
 * __hfs_iget()
 *
 * Given the MDB for a HFS filesystem, a 'key' and an 'entry' in
 * the catalog B-tree and the 'type' of the desired file return the
 * inode for that file/directory or NULL.  Note that 'type' indicates
 * whether we want the actual file or directory, or the corresponding
 * metadata (AppleDouble header file or CAP metadata file).
 */
struct inode *hfs_iget(struct super_block *sb, struct hfs_cat_key *key, hfs_cat_rec *rec)
{
	struct hfs_iget_data data = { key, rec };
	struct inode *inode;
	u32 cnid;

	switch (rec->type) {
	case HFS_CDR_DIR:
		cnid = be32_to_cpu(rec->dir.DirID);
		break;
	case HFS_CDR_FIL:
		cnid = be32_to_cpu(rec->file.FlNum);
		break;
	default:
		return NULL;
	}
	inode = iget5_locked(sb, cnid, hfs_test_inode, hfs_read_inode, &data);
	if (inode && (inode->i_state & I_NEW))
		unlock_new_inode(inode);
	return inode;
}

void hfs_inode_write_fork(struct inode *inode, struct hfs_extent *ext,
			  __be32 *log_size, __be32 *phys_size)
{
	memcpy(ext, HFS_I(inode)->first_extents, sizeof(hfs_extent_rec));

	if (log_size)
		*log_size = cpu_to_be32(inode->i_size);
	if (phys_size)
		*phys_size = cpu_to_be32(HFS_I(inode)->alloc_blocks *
					 HFS_SB(inode->i_sb)->alloc_blksz);
}

int hfs_write_inode(struct inode *inode, int unused)
{
	struct inode *main_inode = inode;
	struct hfs_find_data fd;
	hfs_cat_rec rec;

	dprint(DBG_INODE, "hfs_write_inode: %lu\n", inode->i_ino);
	hfs_ext_write_extent(inode);

	if (inode->i_ino < HFS_FIRSTUSER_CNID) {
		switch (inode->i_ino) {
		case HFS_ROOT_CNID:
			break;
		case HFS_EXT_CNID:
			hfs_btree_write(HFS_SB(inode->i_sb)->ext_tree);
			return 0;
		case HFS_CAT_CNID:
			hfs_btree_write(HFS_SB(inode->i_sb)->cat_tree);
			return 0;
		default:
			BUG();
			return -EIO;
		}
	}

	if (HFS_IS_RSRC(inode))
		main_inode = HFS_I(inode)->rsrc_inode;

	if (!main_inode->i_nlink)
		return 0;

	if (hfs_find_init(HFS_SB(main_inode->i_sb)->cat_tree, &fd))
		/* panic? */
		return -EIO;

	fd.search_key->cat = HFS_I(main_inode)->cat_key;
	if (hfs_brec_find(&fd))
		/* panic? */
		goto out;

	if (S_ISDIR(main_inode->i_mode)) {
		if (fd.entrylength < sizeof(struct hfs_cat_dir))
			/* panic? */;
		hfs_bnode_read(fd.bnode, &rec, fd.entryoffset,
			   sizeof(struct hfs_cat_dir));
		if (rec.type != HFS_CDR_DIR ||
		    be32_to_cpu(rec.dir.DirID) != inode->i_ino) {
		}

		rec.dir.MdDat = hfs_u_to_mtime(inode->i_mtime);
		rec.dir.Val = cpu_to_be16(inode->i_size - 2);

		hfs_bnode_write(fd.bnode, &rec, fd.entryoffset,
			    sizeof(struct hfs_cat_dir));
	} else if (HFS_IS_RSRC(inode)) {
		hfs_bnode_read(fd.bnode, &rec, fd.entryoffset,
			       sizeof(struct hfs_cat_file));
		hfs_inode_write_fork(inode, rec.file.RExtRec,
				     &rec.file.RLgLen, &rec.file.RPyLen);
		hfs_bnode_write(fd.bnode, &rec, fd.entryoffset,
				sizeof(struct hfs_cat_file));
	} else {
		if (fd.entrylength < sizeof(struct hfs_cat_file))
			/* panic? */;
		hfs_bnode_read(fd.bnode, &rec, fd.entryoffset,
			   sizeof(struct hfs_cat_file));
		if (rec.type != HFS_CDR_FIL ||
		    be32_to_cpu(rec.file.FlNum) != inode->i_ino) {
		}

		if (inode->i_mode & S_IWUSR)
			rec.file.Flags &= ~HFS_FIL_LOCK;
		else
			rec.file.Flags |= HFS_FIL_LOCK;
		hfs_inode_write_fork(inode, rec.file.ExtRec, &rec.file.LgLen, &rec.file.PyLen);
		rec.file.MdDat = hfs_u_to_mtime(inode->i_mtime);

		hfs_bnode_write(fd.bnode, &rec, fd.entryoffset,
			    sizeof(struct hfs_cat_file));
	}
out:
	hfs_find_exit(&fd);
	return 0;
}

static struct dentry *hfs_file_lookup(struct inode *dir, struct dentry *dentry,
				      struct nameidata *nd)
{
	struct inode *inode = NULL;
	hfs_cat_rec rec;
	struct hfs_find_data fd;
	int res;

	if (HFS_IS_RSRC(dir) || strcmp(dentry->d_name.name, "rsrc"))
		goto out;

	inode = HFS_I(dir)->rsrc_inode;
	if (inode)
		goto out;

	inode = new_inode(dir->i_sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	hfs_find_init(HFS_SB(dir->i_sb)->cat_tree, &fd);
	fd.search_key->cat = HFS_I(dir)->cat_key;
	res = hfs_brec_read(&fd, &rec, sizeof(rec));
	if (!res) {
		struct hfs_iget_data idata = { NULL, &rec };
		hfs_read_inode(inode, &idata);
	}
	hfs_find_exit(&fd);
	if (res) {
		iput(inode);
		return ERR_PTR(res);
	}
	HFS_I(inode)->rsrc_inode = dir;
	HFS_I(dir)->rsrc_inode = inode;
	igrab(dir);
	hlist_add_head(&inode->i_hash, &HFS_SB(dir->i_sb)->rsrc_inodes);
	mark_inode_dirty(inode);
out:
	d_add(dentry, inode);
	return NULL;
}

void hfs_clear_inode(struct inode *inode)
{
	if (HFS_IS_RSRC(inode) && HFS_I(inode)->rsrc_inode) {
		HFS_I(HFS_I(inode)->rsrc_inode)->rsrc_inode = NULL;
		iput(HFS_I(inode)->rsrc_inode);
	}
}

static int hfs_permission(struct inode *inode, int mask,
			  struct nameidata *nd)
{
	if (S_ISREG(inode->i_mode) && mask & MAY_EXEC)
		return 0;
	return generic_permission(inode, mask, NULL);
}

static int hfs_file_open(struct inode *inode, struct file *file)
{
	if (HFS_IS_RSRC(inode))
		inode = HFS_I(inode)->rsrc_inode;
	if (atomic_read(&file->f_count) != 1)
		return 0;
	atomic_inc(&HFS_I(inode)->opencnt);
	return 0;
}

static int hfs_file_release(struct inode *inode, struct file *file)
{
	//struct super_block *sb = inode->i_sb;

	if (HFS_IS_RSRC(inode))
		inode = HFS_I(inode)->rsrc_inode;
	if (atomic_read(&file->f_count) != 0)
		return 0;
	if (atomic_dec_and_test(&HFS_I(inode)->opencnt)) {
		mutex_lock(&inode->i_mutex);
		hfs_file_truncate(inode);
		//if (inode->i_flags & S_DEAD) {
		//	hfs_delete_cat(inode->i_ino, HFSPLUS_SB(sb).hidden_dir, NULL);
		//	hfs_delete_inode(inode);
		//}
		mutex_unlock(&inode->i_mutex);
	}
	return 0;
}

/*
 * hfs_notify_change()
 *
 * Based very closely on fs/msdos/inode.c by Werner Almesberger
 *
 * This is the notify_change() field in the super_operations structure
 * for HFS file systems.  The purpose is to take that changes made to
 * an inode and apply then in a filesystem-dependent manner.  In this
 * case the process has a few of tasks to do:
 *  1) prevent changes to the i_uid and i_gid fields.
 *  2) map file permissions to the closest allowable permissions
 *  3) Since multiple Linux files can share the same on-disk inode under
 *     HFS (for instance the data and resource forks of a file) a change
 *     to permissions must be applied to all other in-core inodes which
 *     correspond to the same HFS file.
 */

int hfs_inode_setattr(struct dentry *dentry, struct iattr * attr)
{
	struct inode *inode = dentry->d_inode;
	struct hfs_sb_info *hsb = HFS_SB(inode->i_sb);
	int error;

	error = inode_change_ok(inode, attr); /* basic permission checks */
	if (error)
		return error;

	/* no uig/gid changes and limit which mode bits can be set */
	if (((attr->ia_valid & ATTR_UID) &&
	     (attr->ia_uid != hsb->s_uid)) ||
	    ((attr->ia_valid & ATTR_GID) &&
	     (attr->ia_gid != hsb->s_gid)) ||
	    ((attr->ia_valid & ATTR_MODE) &&
	     ((S_ISDIR(inode->i_mode) &&
	       (attr->ia_mode != inode->i_mode)) ||
	      (attr->ia_mode & ~HFS_VALID_MODE_BITS)))) {
		return hsb->s_quiet ? 0 : error;
	}

	if (attr->ia_valid & ATTR_MODE) {
		/* Only the 'w' bits can ever change and only all together. */
		if (attr->ia_mode & S_IWUSR)
			attr->ia_mode = inode->i_mode | S_IWUGO;
		else
			attr->ia_mode = inode->i_mode & ~S_IWUGO;
		attr->ia_mode &= S_ISDIR(inode->i_mode) ? ~hsb->s_dir_umask: ~hsb->s_file_umask;
	}
	error = inode_setattr(inode, attr);
	if (error)
		return error;

	return 0;
}


static struct file_operations hfs_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_file_read,
	.write		= generic_file_write,
	.mmap		= generic_file_mmap,
	.sendfile	= generic_file_sendfile,
	.fsync		= file_fsync,
	.open		= hfs_file_open,
	.release	= hfs_file_release,
};

static struct inode_operations hfs_file_inode_operations = {
	.lookup		= hfs_file_lookup,
	.truncate	= hfs_file_truncate,
	.setattr	= hfs_inode_setattr,
	.permission	= hfs_permission,
	.setxattr	= hfs_setxattr,
	.getxattr	= hfs_getxattr,
	.listxattr	= hfs_listxattr,
};
