/*
 * Copyright 2000 by Hans Reiser, licensing governed by reiserfs/README
 *
 * Trivial changes by Alan Cox to add the LFS fixes
 *
 * Trivial Changes:
 * Rights granted to Hans Reiser to redistribute under other terms providing
 * he accepts all liability including but not limited to patent, fitness
 * for purpose, and direct or indirect claims arising from failure to perform.
 *
 * NO WARRANTY
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/time.h>
#include <asm/uaccess.h>
#include <linux/reiserfs_fs.h>
#include <linux/reiserfs_acl.h>
#include <linux/reiserfs_xattr.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include <linux/namespace.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/quotaops.h>

struct file_system_type reiserfs_fs_type;

static const char reiserfs_3_5_magic_string[] = REISERFS_SUPER_MAGIC_STRING;
static const char reiserfs_3_6_magic_string[] = REISER2FS_SUPER_MAGIC_STRING;
static const char reiserfs_jr_magic_string[] = REISER2FS_JR_SUPER_MAGIC_STRING;

int is_reiserfs_3_5(struct reiserfs_super_block *rs)
{
	return !strncmp(rs->s_v1.s_magic, reiserfs_3_5_magic_string,
			strlen(reiserfs_3_5_magic_string));
}

int is_reiserfs_3_6(struct reiserfs_super_block *rs)
{
	return !strncmp(rs->s_v1.s_magic, reiserfs_3_6_magic_string,
			strlen(reiserfs_3_6_magic_string));
}

int is_reiserfs_jr(struct reiserfs_super_block *rs)
{
	return !strncmp(rs->s_v1.s_magic, reiserfs_jr_magic_string,
			strlen(reiserfs_jr_magic_string));
}

static int is_any_reiserfs_magic_string(struct reiserfs_super_block *rs)
{
	return (is_reiserfs_3_5(rs) || is_reiserfs_3_6(rs) ||
		is_reiserfs_jr(rs));
}

static int reiserfs_remount(struct super_block *s, int *flags, char *data);
static int reiserfs_statfs(struct super_block *s, struct kstatfs *buf);

static int reiserfs_sync_fs(struct super_block *s, int wait)
{
	if (!(s->s_flags & MS_RDONLY)) {
		struct reiserfs_transaction_handle th;
		reiserfs_write_lock(s);
		if (!journal_begin(&th, s, 1))
			if (!journal_end_sync(&th, s, 1))
				reiserfs_flush_old_commits(s);
		s->s_dirt = 0;	/* Even if it's not true.
				 * We'll loop forever in sync_supers otherwise */
		reiserfs_write_unlock(s);
	} else {
		s->s_dirt = 0;
	}
	return 0;
}

static void reiserfs_write_super(struct super_block *s)
{
	reiserfs_sync_fs(s, 1);
}

static void reiserfs_write_super_lockfs(struct super_block *s)
{
	struct reiserfs_transaction_handle th;
	reiserfs_write_lock(s);
	if (!(s->s_flags & MS_RDONLY)) {
		int err = journal_begin(&th, s, 1);
		if (err) {
			reiserfs_block_writes(&th);
		} else {
			reiserfs_prepare_for_journal(s, SB_BUFFER_WITH_SB(s),
						     1);
			journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB(s));
			reiserfs_block_writes(&th);
			journal_end_sync(&th, s, 1);
		}
	}
	s->s_dirt = 0;
	reiserfs_write_unlock(s);
}

static void reiserfs_unlockfs(struct super_block *s)
{
	reiserfs_allow_writes(s);
}

extern const struct in_core_key MAX_IN_CORE_KEY;

/* this is used to delete "save link" when there are no items of a
   file it points to. It can either happen if unlink is completed but
   "save unlink" removal, or if file has both unlink and truncate
   pending and as unlink completes first (because key of "save link"
   protecting unlink is bigger that a key lf "save link" which
   protects truncate), so there left no items to make truncate
   completion on */
static int remove_save_link_only(struct super_block *s,
				 struct reiserfs_key *key, int oid_free)
{
	struct reiserfs_transaction_handle th;
	int err;

	/* we are going to do one balancing */
	err = journal_begin(&th, s, JOURNAL_PER_BALANCE_CNT);
	if (err)
		return err;

	reiserfs_delete_solid_item(&th, NULL, key);
	if (oid_free)
		/* removals are protected by direct items */
		reiserfs_release_objectid(&th, le32_to_cpu(key->k_objectid));

	return journal_end(&th, s, JOURNAL_PER_BALANCE_CNT);
}

#ifdef CONFIG_QUOTA
static int reiserfs_quota_on_mount(struct super_block *, int);
#endif

/* look for uncompleted unlinks and truncates and complete them */
static int finish_unfinished(struct super_block *s)
{
	INITIALIZE_PATH(path);
	struct cpu_key max_cpu_key, obj_key;
	struct reiserfs_key save_link_key;
	int retval = 0;
	struct item_head *ih;
	struct buffer_head *bh;
	int item_pos;
	char *item;
	int done;
	struct inode *inode;
	int truncate;
#ifdef CONFIG_QUOTA
	int i;
	int ms_active_set;
#endif

	/* compose key to look for "save" links */
	max_cpu_key.version = KEY_FORMAT_3_5;
	max_cpu_key.on_disk_key.k_dir_id = ~0U;
	max_cpu_key.on_disk_key.k_objectid = ~0U;
	set_cpu_key_k_offset(&max_cpu_key, ~0U);
	max_cpu_key.key_length = 3;

#ifdef CONFIG_QUOTA
	/* Needed for iput() to work correctly and not trash data */
	if (s->s_flags & MS_ACTIVE) {
		ms_active_set = 0;
	} else {
		ms_active_set = 1;
		s->s_flags |= MS_ACTIVE;
	}
	/* Turn on quotas so that they are updated correctly */
	for (i = 0; i < MAXQUOTAS; i++) {
		if (REISERFS_SB(s)->s_qf_names[i]) {
			int ret = reiserfs_quota_on_mount(s, i);
			if (ret < 0)
				reiserfs_warning(s,
						 "reiserfs: cannot turn on journalled quota: error %d",
						 ret);
		}
	}
#endif

	done = 0;
	REISERFS_SB(s)->s_is_unlinked_ok = 1;
	while (!retval) {
		retval = search_item(s, &max_cpu_key, &path);
		if (retval != ITEM_NOT_FOUND) {
			reiserfs_warning(s,
					 "vs-2140: finish_unfinished: search_by_key returned %d",
					 retval);
			break;
		}

		bh = get_last_bh(&path);
		item_pos = get_item_pos(&path);
		if (item_pos != B_NR_ITEMS(bh)) {
			reiserfs_warning(s,
					 "vs-2060: finish_unfinished: wrong position found");
			break;
		}
		item_pos--;
		ih = B_N_PITEM_HEAD(bh, item_pos);

		if (le32_to_cpu(ih->ih_key.k_dir_id) != MAX_KEY_OBJECTID)
			/* there are no "save" links anymore */
			break;

		save_link_key = ih->ih_key;
		if (is_indirect_le_ih(ih))
			truncate = 1;
		else
			truncate = 0;

		/* reiserfs_iget needs k_dirid and k_objectid only */
		item = B_I_PITEM(bh, ih);
		obj_key.on_disk_key.k_dir_id = le32_to_cpu(*(__le32 *) item);
		obj_key.on_disk_key.k_objectid =
		    le32_to_cpu(ih->ih_key.k_objectid);
		obj_key.on_disk_key.k_offset = 0;
		obj_key.on_disk_key.k_type = 0;

		pathrelse(&path);

		inode = reiserfs_iget(s, &obj_key);
		if (!inode) {
			/* the unlink almost completed, it just did not manage to remove
			   "save" link and release objectid */
			reiserfs_warning(s,
					 "vs-2180: finish_unfinished: iget failed for %K",
					 &obj_key);
			retval = remove_save_link_only(s, &save_link_key, 1);
			continue;
		}

		if (!truncate && inode->i_nlink) {
			/* file is not unlinked */
			reiserfs_warning(s,
					 "vs-2185: finish_unfinished: file %K is not unlinked",
					 &obj_key);
			retval = remove_save_link_only(s, &save_link_key, 0);
			continue;
		}
		DQUOT_INIT(inode);

		if (truncate && S_ISDIR(inode->i_mode)) {
			/* We got a truncate request for a dir which is impossible.
			   The only imaginable way is to execute unfinished truncate request
			   then boot into old kernel, remove the file and create dir with
			   the same key. */
			reiserfs_warning(s,
					 "green-2101: impossible truncate on a directory %k. Please report",
					 INODE_PKEY(inode));
			retval = remove_save_link_only(s, &save_link_key, 0);
			truncate = 0;
			iput(inode);
			continue;
		}

		if (truncate) {
			REISERFS_I(inode)->i_flags |=
			    i_link_saved_truncate_mask;
			/* not completed truncate found. New size was committed together
			   with "save" link */
			reiserfs_info(s, "Truncating %k to %Ld ..",
				      INODE_PKEY(inode), inode->i_size);
			reiserfs_truncate_file(inode,
					       0
					       /*don't update modification time */
					       );
			retval = remove_save_link(inode, truncate);
		} else {
			REISERFS_I(inode)->i_flags |= i_link_saved_unlink_mask;
			/* not completed unlink (rmdir) found */
			reiserfs_info(s, "Removing %k..", INODE_PKEY(inode));
			/* removal gets completed in iput */
			retval = 0;
		}

		iput(inode);
		printk("done\n");
		done++;
	}
	REISERFS_SB(s)->s_is_unlinked_ok = 0;

#ifdef CONFIG_QUOTA
	/* Turn quotas off */
	for (i = 0; i < MAXQUOTAS; i++) {
		if (sb_dqopt(s)->files[i])
			vfs_quota_off_mount(s, i);
	}
	if (ms_active_set)
		/* Restore the flag back */
		s->s_flags &= ~MS_ACTIVE;
#endif
	pathrelse(&path);
	if (done)
		reiserfs_info(s, "There were %d uncompleted unlinks/truncates. "
			      "Completed\n", done);
	return retval;
}

/* to protect file being unlinked from getting lost we "safe" link files
   being unlinked. This link will be deleted in the same transaction with last
   item of file. mounting the filesytem we scan all these links and remove
   files which almost got lost */
void add_save_link(struct reiserfs_transaction_handle *th,
		   struct inode *inode, int truncate)
{
	INITIALIZE_PATH(path);
	int retval;
	struct cpu_key key;
	struct item_head ih;
	__le32 link;

	BUG_ON(!th->t_trans_id);

	/* file can only get one "save link" of each kind */
	RFALSE(truncate &&
	       (REISERFS_I(inode)->i_flags & i_link_saved_truncate_mask),
	       "saved link already exists for truncated inode %lx",
	       (long)inode->i_ino);
	RFALSE(!truncate &&
	       (REISERFS_I(inode)->i_flags & i_link_saved_unlink_mask),
	       "saved link already exists for unlinked inode %lx",
	       (long)inode->i_ino);

	/* setup key of "save" link */
	key.version = KEY_FORMAT_3_5;
	key.on_disk_key.k_dir_id = MAX_KEY_OBJECTID;
	key.on_disk_key.k_objectid = inode->i_ino;
	if (!truncate) {
		/* unlink, rmdir, rename */
		set_cpu_key_k_offset(&key, 1 + inode->i_sb->s_blocksize);
		set_cpu_key_k_type(&key, TYPE_DIRECT);

		/* item head of "safe" link */
		make_le_item_head(&ih, &key, key.version,
				  1 + inode->i_sb->s_blocksize, TYPE_DIRECT,
				  4 /*length */ , 0xffff /*free space */ );
	} else {
		/* truncate */
		if (S_ISDIR(inode->i_mode))
			reiserfs_warning(inode->i_sb,
					 "green-2102: Adding a truncate savelink for a directory %k! Please report",
					 INODE_PKEY(inode));
		set_cpu_key_k_offset(&key, 1);
		set_cpu_key_k_type(&key, TYPE_INDIRECT);

		/* item head of "safe" link */
		make_le_item_head(&ih, &key, key.version, 1, TYPE_INDIRECT,
				  4 /*length */ , 0 /*free space */ );
	}
	key.key_length = 3;

	/* look for its place in the tree */
	retval = search_item(inode->i_sb, &key, &path);
	if (retval != ITEM_NOT_FOUND) {
		if (retval != -ENOSPC)
			reiserfs_warning(inode->i_sb, "vs-2100: add_save_link:"
					 "search_by_key (%K) returned %d", &key,
					 retval);
		pathrelse(&path);
		return;
	}

	/* body of "save" link */
	link = INODE_PKEY(inode)->k_dir_id;

	/* put "save" link inot tree, don't charge quota to anyone */
	retval =
	    reiserfs_insert_item(th, &path, &key, &ih, NULL, (char *)&link);
	if (retval) {
		if (retval != -ENOSPC)
			reiserfs_warning(inode->i_sb,
					 "vs-2120: add_save_link: insert_item returned %d",
					 retval);
	} else {
		if (truncate)
			REISERFS_I(inode)->i_flags |=
			    i_link_saved_truncate_mask;
		else
			REISERFS_I(inode)->i_flags |= i_link_saved_unlink_mask;
	}
}

/* this opens transaction unlike add_save_link */
int remove_save_link(struct inode *inode, int truncate)
{
	struct reiserfs_transaction_handle th;
	struct reiserfs_key key;
	int err;

	/* we are going to do one balancing only */
	err = journal_begin(&th, inode->i_sb, JOURNAL_PER_BALANCE_CNT);
	if (err)
		return err;

	/* setup key of "save" link */
	key.k_dir_id = cpu_to_le32(MAX_KEY_OBJECTID);
	key.k_objectid = INODE_PKEY(inode)->k_objectid;
	if (!truncate) {
		/* unlink, rmdir, rename */
		set_le_key_k_offset(KEY_FORMAT_3_5, &key,
				    1 + inode->i_sb->s_blocksize);
		set_le_key_k_type(KEY_FORMAT_3_5, &key, TYPE_DIRECT);
	} else {
		/* truncate */
		set_le_key_k_offset(KEY_FORMAT_3_5, &key, 1);
		set_le_key_k_type(KEY_FORMAT_3_5, &key, TYPE_INDIRECT);
	}

	if ((truncate &&
	     (REISERFS_I(inode)->i_flags & i_link_saved_truncate_mask)) ||
	    (!truncate &&
	     (REISERFS_I(inode)->i_flags & i_link_saved_unlink_mask)))
		/* don't take quota bytes from anywhere */
		reiserfs_delete_solid_item(&th, NULL, &key);
	if (!truncate) {
		reiserfs_release_objectid(&th, inode->i_ino);
		REISERFS_I(inode)->i_flags &= ~i_link_saved_unlink_mask;
	} else
		REISERFS_I(inode)->i_flags &= ~i_link_saved_truncate_mask;

	return journal_end(&th, inode->i_sb, JOURNAL_PER_BALANCE_CNT);
}

static void reiserfs_put_super(struct super_block *s)
{
	int i;
	struct reiserfs_transaction_handle th;
	th.t_trans_id = 0;

	if (REISERFS_SB(s)->xattr_root) {
		d_invalidate(REISERFS_SB(s)->xattr_root);
		dput(REISERFS_SB(s)->xattr_root);
	}

	if (REISERFS_SB(s)->priv_root) {
		d_invalidate(REISERFS_SB(s)->priv_root);
		dput(REISERFS_SB(s)->priv_root);
	}

	/* change file system state to current state if it was mounted with read-write permissions */
	if (!(s->s_flags & MS_RDONLY)) {
		if (!journal_begin(&th, s, 10)) {
			reiserfs_prepare_for_journal(s, SB_BUFFER_WITH_SB(s),
						     1);
			set_sb_umount_state(SB_DISK_SUPER_BLOCK(s),
					    REISERFS_SB(s)->s_mount_state);
			journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB(s));
		}
	}

	/* note, journal_release checks for readonly mount, and can decide not
	 ** to do a journal_end
	 */
	journal_release(&th, s);

	for (i = 0; i < SB_BMAP_NR(s); i++)
		brelse(SB_AP_BITMAP(s)[i].bh);

	vfree(SB_AP_BITMAP(s));

	brelse(SB_BUFFER_WITH_SB(s));

	print_statistics(s);

	if (REISERFS_SB(s)->s_kmallocs != 0) {
		reiserfs_warning(s,
				 "vs-2004: reiserfs_put_super: allocated memory left %d",
				 REISERFS_SB(s)->s_kmallocs);
	}

	if (REISERFS_SB(s)->reserved_blocks != 0) {
		reiserfs_warning(s,
				 "green-2005: reiserfs_put_super: reserved blocks left %d",
				 REISERFS_SB(s)->reserved_blocks);
	}

	reiserfs_proc_info_done(s);

	kfree(s->s_fs_info);
	s->s_fs_info = NULL;

	return;
}

static kmem_cache_t *reiserfs_inode_cachep;

static struct inode *reiserfs_alloc_inode(struct super_block *sb)
{
	struct reiserfs_inode_info *ei;
	ei = (struct reiserfs_inode_info *)
	    kmem_cache_alloc(reiserfs_inode_cachep, SLAB_KERNEL);
	if (!ei)
		return NULL;
	return &ei->vfs_inode;
}

static void reiserfs_destroy_inode(struct inode *inode)
{
	kmem_cache_free(reiserfs_inode_cachep, REISERFS_I(inode));
}

static void init_once(void *foo, kmem_cache_t * cachep, unsigned long flags)
{
	struct reiserfs_inode_info *ei = (struct reiserfs_inode_info *)foo;

	if ((flags & (SLAB_CTOR_VERIFY | SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR) {
		INIT_LIST_HEAD(&ei->i_prealloc_list);
		inode_init_once(&ei->vfs_inode);
		ei->i_acl_access = NULL;
		ei->i_acl_default = NULL;
	}
}

static int init_inodecache(void)
{
	reiserfs_inode_cachep = kmem_cache_create("reiser_inode_cache",
						  sizeof(struct
							 reiserfs_inode_info),
						  0, SLAB_RECLAIM_ACCOUNT,
						  init_once, NULL);
	if (reiserfs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	if (kmem_cache_destroy(reiserfs_inode_cachep))
		reiserfs_warning(NULL,
				 "reiserfs_inode_cache: not all structures were freed");
}

/* we don't mark inodes dirty, we just log them */
static void reiserfs_dirty_inode(struct inode *inode)
{
	struct reiserfs_transaction_handle th;

	int err = 0;
	if (inode->i_sb->s_flags & MS_RDONLY) {
		reiserfs_warning(inode->i_sb,
				 "clm-6006: writing inode %lu on readonly FS",
				 inode->i_ino);
		return;
	}
	reiserfs_write_lock(inode->i_sb);

	/* this is really only used for atime updates, so they don't have
	 ** to be included in O_SYNC or fsync
	 */
	err = journal_begin(&th, inode->i_sb, 1);
	if (err) {
		reiserfs_write_unlock(inode->i_sb);
		return;
	}
	reiserfs_update_sd(&th, inode);
	journal_end(&th, inode->i_sb, 1);
	reiserfs_write_unlock(inode->i_sb);
}

static void reiserfs_clear_inode(struct inode *inode)
{
	struct posix_acl *acl;

	acl = REISERFS_I(inode)->i_acl_access;
	if (acl && !IS_ERR(acl))
		posix_acl_release(acl);
	REISERFS_I(inode)->i_acl_access = NULL;

	acl = REISERFS_I(inode)->i_acl_default;
	if (acl && !IS_ERR(acl))
		posix_acl_release(acl);
	REISERFS_I(inode)->i_acl_default = NULL;
}

#ifdef CONFIG_QUOTA
static ssize_t reiserfs_quota_write(struct super_block *, int, const char *,
				    size_t, loff_t);
static ssize_t reiserfs_quota_read(struct super_block *, int, char *, size_t,
				   loff_t);
#endif

static struct super_operations reiserfs_sops = {
	.alloc_inode = reiserfs_alloc_inode,
	.destroy_inode = reiserfs_destroy_inode,
	.write_inode = reiserfs_write_inode,
	.dirty_inode = reiserfs_dirty_inode,
	.delete_inode = reiserfs_delete_inode,
	.clear_inode = reiserfs_clear_inode,
	.put_super = reiserfs_put_super,
	.write_super = reiserfs_write_super,
	.sync_fs = reiserfs_sync_fs,
	.write_super_lockfs = reiserfs_write_super_lockfs,
	.unlockfs = reiserfs_unlockfs,
	.statfs = reiserfs_statfs,
	.remount_fs = reiserfs_remount,
#ifdef CONFIG_QUOTA
	.quota_read = reiserfs_quota_read,
	.quota_write = reiserfs_quota_write,
#endif
};

#ifdef CONFIG_QUOTA
#define QTYPE2NAME(t) ((t)==USRQUOTA?"user":"group")

static int reiserfs_dquot_initialize(struct inode *, int);
static int reiserfs_dquot_drop(struct inode *);
static int reiserfs_write_dquot(struct dquot *);
static int reiserfs_acquire_dquot(struct dquot *);
static int reiserfs_release_dquot(struct dquot *);
static int reiserfs_mark_dquot_dirty(struct dquot *);
static int reiserfs_write_info(struct super_block *, int);
static int reiserfs_quota_on(struct super_block *, int, int, char *);

static struct dquot_operations reiserfs_quota_operations = {
	.initialize = reiserfs_dquot_initialize,
	.drop = reiserfs_dquot_drop,
	.alloc_space = dquot_alloc_space,
	.alloc_inode = dquot_alloc_inode,
	.free_space = dquot_free_space,
	.free_inode = dquot_free_inode,
	.transfer = dquot_transfer,
	.write_dquot = reiserfs_write_dquot,
	.acquire_dquot = reiserfs_acquire_dquot,
	.release_dquot = reiserfs_release_dquot,
	.mark_dirty = reiserfs_mark_dquot_dirty,
	.write_info = reiserfs_write_info,
};

static struct quotactl_ops reiserfs_qctl_operations = {
	.quota_on = reiserfs_quota_on,
	.quota_off = vfs_quota_off,
	.quota_sync = vfs_quota_sync,
	.get_info = vfs_get_dqinfo,
	.set_info = vfs_set_dqinfo,
	.get_dqblk = vfs_get_dqblk,
	.set_dqblk = vfs_set_dqblk,
};
#endif

static struct export_operations reiserfs_export_ops = {
	.encode_fh = reiserfs_encode_fh,
	.decode_fh = reiserfs_decode_fh,
	.get_parent = reiserfs_get_parent,
	.get_dentry = reiserfs_get_dentry,
};

/* this struct is used in reiserfs_getopt () for containing the value for those
   mount options that have values rather than being toggles. */
typedef struct {
	char *value;
	int setmask;		/* bitmask which is to set on mount_options bitmask when this
				   value is found, 0 is no bits are to be changed. */
	int clrmask;		/* bitmask which is to clear on mount_options bitmask when  this
				   value is found, 0 is no bits are to be changed. This is
				   applied BEFORE setmask */
} arg_desc_t;

/* Set this bit in arg_required to allow empty arguments */
#define REISERFS_OPT_ALLOWEMPTY 31

/* this struct is used in reiserfs_getopt() for describing the set of reiserfs
   mount options */
typedef struct {
	char *option_name;
	int arg_required;	/* 0 if argument is not required, not 0 otherwise */
	const arg_desc_t *values;	/* list of values accepted by an option */
	int setmask;		/* bitmask which is to set on mount_options bitmask when this
				   value is found, 0 is no bits are to be changed. */
	int clrmask;		/* bitmask which is to clear on mount_options bitmask when  this
				   value is found, 0 is no bits are to be changed. This is
				   applied BEFORE setmask */
} opt_desc_t;

/* possible values for -o data= */
static const arg_desc_t logging_mode[] = {
	{"ordered", 1 << REISERFS_DATA_ORDERED,
	 (1 << REISERFS_DATA_LOG | 1 << REISERFS_DATA_WRITEBACK)},
	{"journal", 1 << REISERFS_DATA_LOG,
	 (1 << REISERFS_DATA_ORDERED | 1 << REISERFS_DATA_WRITEBACK)},
	{"writeback", 1 << REISERFS_DATA_WRITEBACK,
	 (1 << REISERFS_DATA_ORDERED | 1 << REISERFS_DATA_LOG)},
	{NULL, 0}
};

/* possible values for -o barrier= */
static const arg_desc_t barrier_mode[] = {
	{"none", 1 << REISERFS_BARRIER_NONE, 1 << REISERFS_BARRIER_FLUSH},
	{"flush", 1 << REISERFS_BARRIER_FLUSH, 1 << REISERFS_BARRIER_NONE},
	{NULL, 0}
};

/* possible values for "-o block-allocator=" and bits which are to be set in
   s_mount_opt of reiserfs specific part of in-core super block */
static const arg_desc_t balloc[] = {
	{"noborder", 1 << REISERFS_NO_BORDER, 0},
	{"border", 0, 1 << REISERFS_NO_BORDER},
	{"no_unhashed_relocation", 1 << REISERFS_NO_UNHASHED_RELOCATION, 0},
	{"hashed_relocation", 1 << REISERFS_HASHED_RELOCATION, 0},
	{"test4", 1 << REISERFS_TEST4, 0},
	{"notest4", 0, 1 << REISERFS_TEST4},
	{NULL, 0, 0}
};

static const arg_desc_t tails[] = {
	{"on", 1 << REISERFS_LARGETAIL, 1 << REISERFS_SMALLTAIL},
	{"off", 0, (1 << REISERFS_LARGETAIL) | (1 << REISERFS_SMALLTAIL)},
	{"small", 1 << REISERFS_SMALLTAIL, 1 << REISERFS_LARGETAIL},
	{NULL, 0, 0}
};

static const arg_desc_t error_actions[] = {
	{"panic", 1 << REISERFS_ERROR_PANIC,
	 (1 << REISERFS_ERROR_RO | 1 << REISERFS_ERROR_CONTINUE)},
	{"ro-remount", 1 << REISERFS_ERROR_RO,
	 (1 << REISERFS_ERROR_PANIC | 1 << REISERFS_ERROR_CONTINUE)},
#ifdef REISERFS_JOURNAL_ERROR_ALLOWS_NO_LOG
	{"continue", 1 << REISERFS_ERROR_CONTINUE,
	 (1 << REISERFS_ERROR_PANIC | 1 << REISERFS_ERROR_RO)},
#endif
	{NULL, 0, 0},
};

int reiserfs_default_io_size = 128 * 1024;	/* Default recommended I/O size is 128k.
						   There might be broken applications that are
						   confused by this. Use nolargeio mount option
						   to get usual i/o size = PAGE_SIZE.
						 */

/* proceed only one option from a list *cur - string containing of mount options
   opts - array of options which are accepted
   opt_arg - if option is found and requires an argument and if it is specifed
   in the input - pointer to the argument is stored here
   bit_flags - if option requires to set a certain bit - it is set here
   return -1 if unknown option is found, opt->arg_required otherwise */
static int reiserfs_getopt(struct super_block *s, char **cur, opt_desc_t * opts,
			   char **opt_arg, unsigned long *bit_flags)
{
	char *p;
	/* foo=bar, 
	   ^   ^  ^
	   |   |  +-- option_end
	   |   +-- arg_start
	   +-- option_start
	 */
	const opt_desc_t *opt;
	const arg_desc_t *arg;

	p = *cur;

	/* assume argument cannot contain commas */
	*cur = strchr(p, ',');
	if (*cur) {
		*(*cur) = '\0';
		(*cur)++;
	}

	if (!strncmp(p, "alloc=", 6)) {
		/* Ugly special case, probably we should redo options parser so that
		   it can understand several arguments for some options, also so that
		   it can fill several bitfields with option values. */
		if (reiserfs_parse_alloc_options(s, p + 6)) {
			return -1;
		} else {
			return 0;
		}
	}

	/* for every option in the list */
	for (opt = opts; opt->option_name; opt++) {
		if (!strncmp(p, opt->option_name, strlen(opt->option_name))) {
			if (bit_flags) {
				if (opt->clrmask ==
				    (1 << REISERFS_UNSUPPORTED_OPT))
					reiserfs_warning(s, "%s not supported.",
							 p);
				else
					*bit_flags &= ~opt->clrmask;
				if (opt->setmask ==
				    (1 << REISERFS_UNSUPPORTED_OPT))
					reiserfs_warning(s, "%s not supported.",
							 p);
				else
					*bit_flags |= opt->setmask;
			}
			break;
		}
	}
	if (!opt->option_name) {
		reiserfs_warning(s, "unknown mount option \"%s\"", p);
		return -1;
	}

	p += strlen(opt->option_name);
	switch (*p) {
	case '=':
		if (!opt->arg_required) {
			reiserfs_warning(s,
					 "the option \"%s\" does not require an argument",
					 opt->option_name);
			return -1;
		}
		break;

	case 0:
		if (opt->arg_required) {
			reiserfs_warning(s,
					 "the option \"%s\" requires an argument",
					 opt->option_name);
			return -1;
		}
		break;
	default:
		reiserfs_warning(s, "head of option \"%s\" is only correct",
				 opt->option_name);
		return -1;
	}

	/* move to the argument, or to next option if argument is not required */
	p++;

	if (opt->arg_required
	    && !(opt->arg_required & (1 << REISERFS_OPT_ALLOWEMPTY))
	    && !strlen(p)) {
		/* this catches "option=," if not allowed */
		reiserfs_warning(s, "empty argument for \"%s\"",
				 opt->option_name);
		return -1;
	}

	if (!opt->values) {
		/* *=NULLopt_arg contains pointer to argument */
		*opt_arg = p;
		return opt->arg_required & ~(1 << REISERFS_OPT_ALLOWEMPTY);
	}

	/* values possible for this option are listed in opt->values */
	for (arg = opt->values; arg->value; arg++) {
		if (!strcmp(p, arg->value)) {
			if (bit_flags) {
				*bit_flags &= ~arg->clrmask;
				*bit_flags |= arg->setmask;
			}
			return opt->arg_required;
		}
	}

	reiserfs_warning(s, "bad value \"%s\" for option \"%s\"", p,
			 opt->option_name);
	return -1;
}

/* returns 0 if something is wrong in option string, 1 - otherwise */
static int reiserfs_parse_options(struct super_block *s, char *options,	/* string given via mount's -o */
				  unsigned long *mount_options,
				  /* after the parsing phase, contains the
				     collection of bitflags defining what
				     mount options were selected. */
				  unsigned long *blocks,	/* strtol-ed from NNN of resize=NNN */
				  char **jdev_name,
				  unsigned int *commit_max_age)
{
	int c;
	char *arg = NULL;
	char *pos;
	opt_desc_t opts[] = {
		/* Compatibility stuff, so that -o notail for old setups still work */
		{"tails",.arg_required = 't',.values = tails},
		{"notail",.clrmask =
		 (1 << REISERFS_LARGETAIL) | (1 << REISERFS_SMALLTAIL)},
		{"conv",.setmask = 1 << REISERFS_CONVERT},
		{"attrs",.setmask = 1 << REISERFS_ATTRS},
		{"noattrs",.clrmask = 1 << REISERFS_ATTRS},
#ifdef CONFIG_REISERFS_FS_XATTR
		{"user_xattr",.setmask = 1 << REISERFS_XATTRS_USER},
		{"nouser_xattr",.clrmask = 1 << REISERFS_XATTRS_USER},
#else
		{"user_xattr",.setmask = 1 << REISERFS_UNSUPPORTED_OPT},
		{"nouser_xattr",.clrmask = 1 << REISERFS_UNSUPPORTED_OPT},
#endif
#ifdef CONFIG_REISERFS_FS_POSIX_ACL
		{"acl",.setmask = 1 << REISERFS_POSIXACL},
		{"noacl",.clrmask = 1 << REISERFS_POSIXACL},
#else
		{"acl",.setmask = 1 << REISERFS_UNSUPPORTED_OPT},
		{"noacl",.clrmask = 1 << REISERFS_UNSUPPORTED_OPT},
#endif
		{"nolog",},	/* This is unsupported */
		{"replayonly",.setmask = 1 << REPLAYONLY},
		{"block-allocator",.arg_required = 'a',.values = balloc},
		{"data",.arg_required = 'd',.values = logging_mode},
		{"barrier",.arg_required = 'b',.values = barrier_mode},
		{"resize",.arg_required = 'r',.values = NULL},
		{"jdev",.arg_required = 'j',.values = NULL},
		{"nolargeio",.arg_required = 'w',.values = NULL},
		{"commit",.arg_required = 'c',.values = NULL},
		{"usrquota",.setmask = 1 << REISERFS_QUOTA},
		{"grpquota",.setmask = 1 << REISERFS_QUOTA},
		{"noquota",.clrmask = 1 << REISERFS_QUOTA},
		{"errors",.arg_required = 'e',.values = error_actions},
		{"usrjquota",.arg_required =
		 'u' | (1 << REISERFS_OPT_ALLOWEMPTY),.values = NULL},
		{"grpjquota",.arg_required =
		 'g' | (1 << REISERFS_OPT_ALLOWEMPTY),.values = NULL},
		{"jqfmt",.arg_required = 'f',.values = NULL},
		{NULL,}
	};

	*blocks = 0;
	if (!options || !*options)
		/* use default configuration: create tails, journaling on, no
		   conversion to newest format */
		return 1;

	for (pos = options; pos;) {
		c = reiserfs_getopt(s, &pos, opts, &arg, mount_options);
		if (c == -1)
			/* wrong option is given */
			return 0;

		if (c == 'r') {
			char *p;

			p = NULL;
			/* "resize=NNN" or "resize=auto" */

			if (!strcmp(arg, "auto")) {
				/* From JFS code, to auto-get the size. */
				*blocks =
				    s->s_bdev->bd_inode->i_size >> s->
				    s_blocksize_bits;
			} else {
				*blocks = simple_strtoul(arg, &p, 0);
				if (*p != '\0') {
					/* NNN does not look like a number */
					reiserfs_warning(s,
							 "reiserfs_parse_options: bad value %s",
							 arg);
					return 0;
				}
			}
		}

		if (c == 'c') {
			char *p = NULL;
			unsigned long val = simple_strtoul(arg, &p, 0);
			/* commit=NNN (time in seconds) */
			if (*p != '\0' || val >= (unsigned int)-1) {
				reiserfs_warning(s,
						 "reiserfs_parse_options: bad value %s",
						 arg);
				return 0;
			}
			*commit_max_age = (unsigned int)val;
		}

		if (c == 'w') {
			char *p = NULL;
			int val = simple_strtoul(arg, &p, 0);

			if (*p != '\0') {
				reiserfs_warning(s,
						 "reiserfs_parse_options: non-numeric value %s for nolargeio option",
						 arg);
				return 0;
			}
			if (val)
				reiserfs_default_io_size = PAGE_SIZE;
			else
				reiserfs_default_io_size = 128 * 1024;
		}

		if (c == 'j') {
			if (arg && *arg && jdev_name) {
				if (*jdev_name) {	//Hm, already assigned?
					reiserfs_warning(s,
							 "reiserfs_parse_options: journal device was already  specified to be %s",
							 *jdev_name);
					return 0;
				}
				*jdev_name = arg;
			}
		}
#ifdef CONFIG_QUOTA
		if (c == 'u' || c == 'g') {
			int qtype = c == 'u' ? USRQUOTA : GRPQUOTA;

			if (sb_any_quota_enabled(s)) {
				reiserfs_warning(s,
						 "reiserfs_parse_options: cannot change journalled quota options when quota turned on.");
				return 0;
			}
			if (*arg) {	/* Some filename specified? */
				if (REISERFS_SB(s)->s_qf_names[qtype]
				    && strcmp(REISERFS_SB(s)->s_qf_names[qtype],
					      arg)) {
					reiserfs_warning(s,
							 "reiserfs_parse_options: %s quota file already specified.",
							 QTYPE2NAME(qtype));
					return 0;
				}
				if (strchr(arg, '/')) {
					reiserfs_warning(s,
							 "reiserfs_parse_options: quotafile must be on filesystem root.");
					return 0;
				}
				REISERFS_SB(s)->s_qf_names[qtype] =
				    kmalloc(strlen(arg) + 1, GFP_KERNEL);
				if (!REISERFS_SB(s)->s_qf_names[qtype]) {
					reiserfs_warning(s,
							 "reiserfs_parse_options: not enough memory for storing quotafile name.");
					return 0;
				}
				strcpy(REISERFS_SB(s)->s_qf_names[qtype], arg);
				*mount_options |= 1 << REISERFS_QUOTA;
			} else {
				kfree(REISERFS_SB(s)->s_qf_names[qtype]);
				REISERFS_SB(s)->s_qf_names[qtype] = NULL;
			}
		}
		if (c == 'f') {
			if (!strcmp(arg, "vfsold"))
				REISERFS_SB(s)->s_jquota_fmt = QFMT_VFS_OLD;
			else if (!strcmp(arg, "vfsv0"))
				REISERFS_SB(s)->s_jquota_fmt = QFMT_VFS_V0;
			else {
				reiserfs_warning(s,
						 "reiserfs_parse_options: unknown quota format specified.");
				return 0;
			}
		}
#else
		if (c == 'u' || c == 'g' || c == 'f') {
			reiserfs_warning(s,
					 "reiserfs_parse_options: journalled quota options not supported.");
			return 0;
		}
#endif
	}

#ifdef CONFIG_QUOTA
	if (!REISERFS_SB(s)->s_jquota_fmt
	    && (REISERFS_SB(s)->s_qf_names[USRQUOTA]
		|| REISERFS_SB(s)->s_qf_names[GRPQUOTA])) {
		reiserfs_warning(s,
				 "reiserfs_parse_options: journalled quota format not specified.");
		return 0;
	}
	/* This checking is not precise wrt the quota type but for our purposes it is sufficient */
	if (!(*mount_options & (1 << REISERFS_QUOTA))
	    && sb_any_quota_enabled(s)) {
		reiserfs_warning(s,
				 "reiserfs_parse_options: quota options must be present when quota is turned on.");
		return 0;
	}
#endif

	return 1;
}

static void switch_data_mode(struct super_block *s, unsigned long mode)
{
	REISERFS_SB(s)->s_mount_opt &= ~((1 << REISERFS_DATA_LOG) |
					 (1 << REISERFS_DATA_ORDERED) |
					 (1 << REISERFS_DATA_WRITEBACK));
	REISERFS_SB(s)->s_mount_opt |= (1 << mode);
}

static void handle_data_mode(struct super_block *s, unsigned long mount_options)
{
	if (mount_options & (1 << REISERFS_DATA_LOG)) {
		if (!reiserfs_data_log(s)) {
			switch_data_mode(s, REISERFS_DATA_LOG);
			reiserfs_info(s, "switching to journaled data mode\n");
		}
	} else if (mount_options & (1 << REISERFS_DATA_ORDERED)) {
		if (!reiserfs_data_ordered(s)) {
			switch_data_mode(s, REISERFS_DATA_ORDERED);
			reiserfs_info(s, "switching to ordered data mode\n");
		}
	} else if (mount_options & (1 << REISERFS_DATA_WRITEBACK)) {
		if (!reiserfs_data_writeback(s)) {
			switch_data_mode(s, REISERFS_DATA_WRITEBACK);
			reiserfs_info(s, "switching to writeback data mode\n");
		}
	}
}

static void handle_barrier_mode(struct super_block *s, unsigned long bits)
{
	int flush = (1 << REISERFS_BARRIER_FLUSH);
	int none = (1 << REISERFS_BARRIER_NONE);
	int all_barrier = flush | none;

	if (bits & all_barrier) {
		REISERFS_SB(s)->s_mount_opt &= ~all_barrier;
		if (bits & flush) {
			REISERFS_SB(s)->s_mount_opt |= flush;
			printk("reiserfs: enabling write barrier flush mode\n");
		} else if (bits & none) {
			REISERFS_SB(s)->s_mount_opt |= none;
			printk("reiserfs: write barriers turned off\n");
		}
	}
}

static void handle_attrs(struct super_block *s)
{
	struct reiserfs_super_block *rs = SB_DISK_SUPER_BLOCK(s);

	if (reiserfs_attrs(s)) {
		if (old_format_only(s)) {
			reiserfs_warning(s,
					 "reiserfs: cannot support attributes on 3.5.x disk format");
			REISERFS_SB(s)->s_mount_opt &= ~(1 << REISERFS_ATTRS);
			return;
		}
		if (!(le32_to_cpu(rs->s_flags) & reiserfs_attrs_cleared)) {
			reiserfs_warning(s,
					 "reiserfs: cannot support attributes until flag is set in super-block");
			REISERFS_SB(s)->s_mount_opt &= ~(1 << REISERFS_ATTRS);
		}
	} else if (le32_to_cpu(rs->s_flags) & reiserfs_attrs_cleared) {
		REISERFS_SB(s)->s_mount_opt |= REISERFS_ATTRS;
	}
}

static int reiserfs_remount(struct super_block *s, int *mount_flags, char *arg)
{
	struct reiserfs_super_block *rs;
	struct reiserfs_transaction_handle th;
	unsigned long blocks;
	unsigned long mount_options = REISERFS_SB(s)->s_mount_opt;
	unsigned long safe_mask = 0;
	unsigned int commit_max_age = (unsigned int)-1;
	struct reiserfs_journal *journal = SB_JOURNAL(s);
	int err;
#ifdef CONFIG_QUOTA
	int i;
#endif

	rs = SB_DISK_SUPER_BLOCK(s);

	if (!reiserfs_parse_options
	    (s, arg, &mount_options, &blocks, NULL, &commit_max_age)) {
#ifdef CONFIG_QUOTA
		for (i = 0; i < MAXQUOTAS; i++) {
			kfree(REISERFS_SB(s)->s_qf_names[i]);
			REISERFS_SB(s)->s_qf_names[i] = NULL;
		}
#endif
		return -EINVAL;
	}

	handle_attrs(s);

	/* Add options that are safe here */
	safe_mask |= 1 << REISERFS_SMALLTAIL;
	safe_mask |= 1 << REISERFS_LARGETAIL;
	safe_mask |= 1 << REISERFS_NO_BORDER;
	safe_mask |= 1 << REISERFS_NO_UNHASHED_RELOCATION;
	safe_mask |= 1 << REISERFS_HASHED_RELOCATION;
	safe_mask |= 1 << REISERFS_TEST4;
	safe_mask |= 1 << REISERFS_ATTRS;
	safe_mask |= 1 << REISERFS_XATTRS_USER;
	safe_mask |= 1 << REISERFS_POSIXACL;
	safe_mask |= 1 << REISERFS_BARRIER_FLUSH;
	safe_mask |= 1 << REISERFS_BARRIER_NONE;
	safe_mask |= 1 << REISERFS_ERROR_RO;
	safe_mask |= 1 << REISERFS_ERROR_CONTINUE;
	safe_mask |= 1 << REISERFS_ERROR_PANIC;
	safe_mask |= 1 << REISERFS_QUOTA;

	/* Update the bitmask, taking care to keep
	 * the bits we're not allowed to change here */
	REISERFS_SB(s)->s_mount_opt =
	    (REISERFS_SB(s)->
	     s_mount_opt & ~safe_mask) | (mount_options & safe_mask);

	if (commit_max_age != 0 && commit_max_age != (unsigned int)-1) {
		journal->j_max_commit_age = commit_max_age;
		journal->j_max_trans_age = commit_max_age;
	} else if (commit_max_age == 0) {
		/* 0 means restore defaults. */
		journal->j_max_commit_age = journal->j_default_max_commit_age;
		journal->j_max_trans_age = JOURNAL_MAX_TRANS_AGE;
	}

	if (blocks) {
		int rc = reiserfs_resize(s, blocks);
		if (rc != 0)
			return rc;
	}

	if (*mount_flags & MS_RDONLY) {
		reiserfs_xattr_init(s, *mount_flags);
		/* remount read-only */
		if (s->s_flags & MS_RDONLY)
			/* it is read-only already */
			return 0;
		/* try to remount file system with read-only permissions */
		if (sb_umount_state(rs) == REISERFS_VALID_FS
		    || REISERFS_SB(s)->s_mount_state != REISERFS_VALID_FS) {
			return 0;
		}

		err = journal_begin(&th, s, 10);
		if (err)
			return err;

		/* Mounting a rw partition read-only. */
		reiserfs_prepare_for_journal(s, SB_BUFFER_WITH_SB(s), 1);
		set_sb_umount_state(rs, REISERFS_SB(s)->s_mount_state);
		journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB(s));
	} else {
		/* remount read-write */
		if (!(s->s_flags & MS_RDONLY)) {
			reiserfs_xattr_init(s, *mount_flags);
			return 0;	/* We are read-write already */
		}

		if (reiserfs_is_journal_aborted(journal))
			return journal->j_errno;

		handle_data_mode(s, mount_options);
		handle_barrier_mode(s, mount_options);
		REISERFS_SB(s)->s_mount_state = sb_umount_state(rs);
		s->s_flags &= ~MS_RDONLY;	/* now it is safe to call journal_begin */
		err = journal_begin(&th, s, 10);
		if (err)
			return err;

		/* Mount a partition which is read-only, read-write */
		reiserfs_prepare_for_journal(s, SB_BUFFER_WITH_SB(s), 1);
		REISERFS_SB(s)->s_mount_state = sb_umount_state(rs);
		s->s_flags &= ~MS_RDONLY;
		set_sb_umount_state(rs, REISERFS_ERROR_FS);
		/* mark_buffer_dirty (SB_BUFFER_WITH_SB (s), 1); */
		journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB(s));
		REISERFS_SB(s)->s_mount_state = REISERFS_VALID_FS;
	}
	/* this will force a full flush of all journal lists */
	SB_JOURNAL(s)->j_must_wait = 1;
	err = journal_end(&th, s, 10);
	if (err)
		return err;
	s->s_dirt = 0;

	if (!(*mount_flags & MS_RDONLY)) {
		finish_unfinished(s);
		reiserfs_xattr_init(s, *mount_flags);
	}

	return 0;
}

/* load_bitmap_info_data - Sets up the reiserfs_bitmap_info structure from disk.
 * @sb - superblock for this filesystem
 * @bi - the bitmap info to be loaded. Requires that bi->bh is valid.
 *
 * This routine counts how many free bits there are, finding the first zero
 * as a side effect. Could also be implemented as a loop of test_bit() calls, or
 * a loop of find_first_zero_bit() calls. This implementation is similar to
 * find_first_zero_bit(), but doesn't return after it finds the first bit.
 * Should only be called on fs mount, but should be fairly efficient anyways.
 *
 * bi->first_zero_hint is considered unset if it == 0, since the bitmap itself
 * will * invariably occupt block 0 represented in the bitmap. The only
 * exception to this is when free_count also == 0, since there will be no
 * free blocks at all.
 */

static void load_bitmap_info_data(struct super_block *sb,
				  struct reiserfs_bitmap_info *bi)
{
	unsigned long *cur = (unsigned long *)bi->bh->b_data;

	while ((char *)cur < (bi->bh->b_data + sb->s_blocksize)) {

		/* No need to scan if all 0's or all 1's.
		 * Since we're only counting 0's, we can simply ignore all 1's */
		if (*cur == 0) {
			if (bi->first_zero_hint == 0) {
				bi->first_zero_hint =
				    ((char *)cur - bi->bh->b_data) << 3;
			}
			bi->free_count += sizeof(unsigned long) * 8;
		} else if (*cur != ~0L) {
			int b;
			for (b = 0; b < sizeof(unsigned long) * 8; b++) {
				if (!reiserfs_test_le_bit(b, cur)) {
					bi->free_count++;
					if (bi->first_zero_hint == 0)
						bi->first_zero_hint =
						    (((char *)cur -
						      bi->bh->b_data) << 3) + b;
				}
			}
		}
		cur++;
	}

#ifdef CONFIG_REISERFS_CHECK
// This outputs a lot of unneded info on big FSes
//    reiserfs_warning ("bitmap loaded from block %d: %d free blocks",
//                    bi->bh->b_blocknr, bi->free_count);
#endif
}

static int read_bitmaps(struct super_block *s)
{
	int i, bmap_nr;

	SB_AP_BITMAP(s) =
	    vmalloc(sizeof(struct reiserfs_bitmap_info) * SB_BMAP_NR(s));
	if (SB_AP_BITMAP(s) == 0)
		return 1;
	memset(SB_AP_BITMAP(s), 0,
	       sizeof(struct reiserfs_bitmap_info) * SB_BMAP_NR(s));
	for (i = 0, bmap_nr =
	     REISERFS_DISK_OFFSET_IN_BYTES / s->s_blocksize + 1;
	     i < SB_BMAP_NR(s); i++, bmap_nr = s->s_blocksize * 8 * i) {
		SB_AP_BITMAP(s)[i].bh = sb_getblk(s, bmap_nr);
		if (!buffer_uptodate(SB_AP_BITMAP(s)[i].bh))
			ll_rw_block(READ, 1, &SB_AP_BITMAP(s)[i].bh);
	}
	for (i = 0; i < SB_BMAP_NR(s); i++) {
		wait_on_buffer(SB_AP_BITMAP(s)[i].bh);
		if (!buffer_uptodate(SB_AP_BITMAP(s)[i].bh)) {
			reiserfs_warning(s, "sh-2029: reiserfs read_bitmaps: "
					 "bitmap block (#%lu) reading failed",
					 SB_AP_BITMAP(s)[i].bh->b_blocknr);
			for (i = 0; i < SB_BMAP_NR(s); i++)
				brelse(SB_AP_BITMAP(s)[i].bh);
			vfree(SB_AP_BITMAP(s));
			SB_AP_BITMAP(s) = NULL;
			return 1;
		}
		load_bitmap_info_data(s, SB_AP_BITMAP(s) + i);
	}
	return 0;
}

static int read_old_bitmaps(struct super_block *s)
{
	int i;
	struct reiserfs_super_block *rs = SB_DISK_SUPER_BLOCK(s);
	int bmp1 = (REISERFS_OLD_DISK_OFFSET_IN_BYTES / s->s_blocksize) + 1;	/* first of bitmap blocks */

	/* read true bitmap */
	SB_AP_BITMAP(s) =
	    vmalloc(sizeof(struct reiserfs_buffer_info *) * sb_bmap_nr(rs));
	if (SB_AP_BITMAP(s) == 0)
		return 1;

	memset(SB_AP_BITMAP(s), 0,
	       sizeof(struct reiserfs_buffer_info *) * sb_bmap_nr(rs));

	for (i = 0; i < sb_bmap_nr(rs); i++) {
		SB_AP_BITMAP(s)[i].bh = sb_bread(s, bmp1 + i);
		if (!SB_AP_BITMAP(s)[i].bh)
			return 1;
		load_bitmap_info_data(s, SB_AP_BITMAP(s) + i);
	}

	return 0;
}

static int read_super_block(struct super_block *s, int offset)
{
	struct buffer_head *bh;
	struct reiserfs_super_block *rs;
	int fs_blocksize;

	bh = sb_bread(s, offset / s->s_blocksize);
	if (!bh) {
		reiserfs_warning(s, "sh-2006: read_super_block: "
				 "bread failed (dev %s, block %lu, size %lu)",
				 reiserfs_bdevname(s), offset / s->s_blocksize,
				 s->s_blocksize);
		return 1;
	}

	rs = (struct reiserfs_super_block *)bh->b_data;
	if (!is_any_reiserfs_magic_string(rs)) {
		brelse(bh);
		return 1;
	}
	//
	// ok, reiserfs signature (old or new) found in at the given offset
	//    
	fs_blocksize = sb_blocksize(rs);
	brelse(bh);
	sb_set_blocksize(s, fs_blocksize);

	bh = sb_bread(s, offset / s->s_blocksize);
	if (!bh) {
		reiserfs_warning(s, "sh-2007: read_super_block: "
				 "bread failed (dev %s, block %lu, size %lu)\n",
				 reiserfs_bdevname(s), offset / s->s_blocksize,
				 s->s_blocksize);
		return 1;
	}

	rs = (struct reiserfs_super_block *)bh->b_data;
	if (sb_blocksize(rs) != s->s_blocksize) {
		reiserfs_warning(s, "sh-2011: read_super_block: "
				 "can't find a reiserfs filesystem on (dev %s, block %Lu, size %lu)\n",
				 reiserfs_bdevname(s),
				 (unsigned long long)bh->b_blocknr,
				 s->s_blocksize);
		brelse(bh);
		return 1;
	}

	if (rs->s_v1.s_root_block == cpu_to_le32(-1)) {
		brelse(bh);
		reiserfs_warning(s,
				 "Unfinished reiserfsck --rebuild-tree run detected. Please run\n"
				 "reiserfsck --rebuild-tree and wait for a completion. If that fails\n"
				 "get newer reiserfsprogs package");
		return 1;
	}

	SB_BUFFER_WITH_SB(s) = bh;
	SB_DISK_SUPER_BLOCK(s) = rs;

	if (is_reiserfs_jr(rs)) {
		/* magic is of non-standard journal filesystem, look at s_version to
		   find which format is in use */
		if (sb_version(rs) == REISERFS_VERSION_2)
			reiserfs_warning(s,
					 "read_super_block: found reiserfs format \"3.6\""
					 " with non-standard journal");
		else if (sb_version(rs) == REISERFS_VERSION_1)
			reiserfs_warning(s,
					 "read_super_block: found reiserfs format \"3.5\""
					 " with non-standard journal");
		else {
			reiserfs_warning(s,
					 "sh-2012: read_super_block: found unknown "
					 "format \"%u\" of reiserfs with non-standard magic",
					 sb_version(rs));
			return 1;
		}
	} else
		/* s_version of standard format may contain incorrect information,
		   so we just look at the magic string */
		reiserfs_info(s,
			      "found reiserfs format \"%s\" with standard journal\n",
			      is_reiserfs_3_5(rs) ? "3.5" : "3.6");

	s->s_op = &reiserfs_sops;
	s->s_export_op = &reiserfs_export_ops;
#ifdef CONFIG_QUOTA
	s->s_qcop = &reiserfs_qctl_operations;
	s->dq_op = &reiserfs_quota_operations;
#endif

	/* new format is limited by the 32 bit wide i_blocks field, want to
	 ** be one full block below that.
	 */
	s->s_maxbytes = (512LL << 32) - s->s_blocksize;
	return 0;
}

/* after journal replay, reread all bitmap and super blocks */
static int reread_meta_blocks(struct super_block *s)
{
	int i;
	ll_rw_block(READ, 1, &(SB_BUFFER_WITH_SB(s)));
	wait_on_buffer(SB_BUFFER_WITH_SB(s));
	if (!buffer_uptodate(SB_BUFFER_WITH_SB(s))) {
		reiserfs_warning(s,
				 "reread_meta_blocks, error reading the super");
		return 1;
	}

	for (i = 0; i < SB_BMAP_NR(s); i++) {
		ll_rw_block(READ, 1, &(SB_AP_BITMAP(s)[i].bh));
		wait_on_buffer(SB_AP_BITMAP(s)[i].bh);
		if (!buffer_uptodate(SB_AP_BITMAP(s)[i].bh)) {
			reiserfs_warning(s,
					 "reread_meta_blocks, error reading bitmap block number %d at %llu",
					 i,
					 (unsigned long long)SB_AP_BITMAP(s)[i].
					 bh->b_blocknr);
			return 1;
		}
	}
	return 0;

}

/////////////////////////////////////////////////////
// hash detection stuff

// if root directory is empty - we set default - Yura's - hash and
// warn about it
// FIXME: we look for only one name in a directory. If tea and yura
// bith have the same value - we ask user to send report to the
// mailing list
static __u32 find_hash_out(struct super_block *s)
{
	int retval;
	struct inode *inode;
	struct cpu_key key;
	INITIALIZE_PATH(path);
	struct reiserfs_dir_entry de;
	__u32 hash = DEFAULT_HASH;

	inode = s->s_root->d_inode;

	do {			// Some serious "goto"-hater was there ;)
		u32 teahash, r5hash, yurahash;

		make_cpu_key(&key, inode, ~0, TYPE_DIRENTRY, 3);
		retval = search_by_entry_key(s, &key, &path, &de);
		if (retval == IO_ERROR) {
			pathrelse(&path);
			return UNSET_HASH;
		}
		if (retval == NAME_NOT_FOUND)
			de.de_entry_num--;
		set_de_name_and_namelen(&de);
		if (deh_offset(&(de.de_deh[de.de_entry_num])) == DOT_DOT_OFFSET) {
			/* allow override in this case */
			if (reiserfs_rupasov_hash(s)) {
				hash = YURA_HASH;
			}
			reiserfs_warning(s, "FS seems to be empty, autodetect "
					 "is using the default hash");
			break;
		}
		r5hash = GET_HASH_VALUE(r5_hash(de.de_name, de.de_namelen));
		teahash = GET_HASH_VALUE(keyed_hash(de.de_name, de.de_namelen));
		yurahash = GET_HASH_VALUE(yura_hash(de.de_name, de.de_namelen));
		if (((teahash == r5hash)
		     &&
		     (GET_HASH_VALUE(deh_offset(&(de.de_deh[de.de_entry_num])))
		      == r5hash)) || ((teahash == yurahash)
				      && (yurahash ==
					  GET_HASH_VALUE(deh_offset
							 (&
							  (de.
							   de_deh[de.
								  de_entry_num])))))
		    || ((r5hash == yurahash)
			&& (yurahash ==
			    GET_HASH_VALUE(deh_offset
					   (&(de.de_deh[de.de_entry_num])))))) {
			reiserfs_warning(s,
					 "Unable to automatically detect hash function. "
					 "Please mount with -o hash={tea,rupasov,r5}",
					 reiserfs_bdevname(s));
			hash = UNSET_HASH;
			break;
		}
		if (GET_HASH_VALUE(deh_offset(&(de.de_deh[de.de_entry_num]))) ==
		    yurahash)
			hash = YURA_HASH;
		else if (GET_HASH_VALUE
			 (deh_offset(&(de.de_deh[de.de_entry_num]))) == teahash)
			hash = TEA_HASH;
		else if (GET_HASH_VALUE
			 (deh_offset(&(de.de_deh[de.de_entry_num]))) == r5hash)
			hash = R5_HASH;
		else {
			reiserfs_warning(s, "Unrecognised hash function");
			hash = UNSET_HASH;
		}
	} while (0);

	pathrelse(&path);
	return hash;
}

// finds out which hash names are sorted with
static int what_hash(struct super_block *s)
{
	__u32 code;

	code = sb_hash_function_code(SB_DISK_SUPER_BLOCK(s));

	/* reiserfs_hash_detect() == true if any of the hash mount options
	 ** were used.  We must check them to make sure the user isn't
	 ** using a bad hash value
	 */
	if (code == UNSET_HASH || reiserfs_hash_detect(s))
		code = find_hash_out(s);

	if (code != UNSET_HASH && reiserfs_hash_detect(s)) {
		/* detection has found the hash, and we must check against the 
		 ** mount options 
		 */
		if (reiserfs_rupasov_hash(s) && code != YURA_HASH) {
			reiserfs_warning(s, "Error, %s hash detected, "
					 "unable to force rupasov hash",
					 reiserfs_hashname(code));
			code = UNSET_HASH;
		} else if (reiserfs_tea_hash(s) && code != TEA_HASH) {
			reiserfs_warning(s, "Error, %s hash detected, "
					 "unable to force tea hash",
					 reiserfs_hashname(code));
			code = UNSET_HASH;
		} else if (reiserfs_r5_hash(s) && code != R5_HASH) {
			reiserfs_warning(s, "Error, %s hash detected, "
					 "unable to force r5 hash",
					 reiserfs_hashname(code));
			code = UNSET_HASH;
		}
	} else {
		/* find_hash_out was not called or could not determine the hash */
		if (reiserfs_rupasov_hash(s)) {
			code = YURA_HASH;
		} else if (reiserfs_tea_hash(s)) {
			code = TEA_HASH;
		} else if (reiserfs_r5_hash(s)) {
			code = R5_HASH;
		}
	}

	/* if we are mounted RW, and we have a new valid hash code, update 
	 ** the super
	 */
	if (code != UNSET_HASH &&
	    !(s->s_flags & MS_RDONLY) &&
	    code != sb_hash_function_code(SB_DISK_SUPER_BLOCK(s))) {
		set_sb_hash_function_code(SB_DISK_SUPER_BLOCK(s), code);
	}
	return code;
}

// return pointer to appropriate function
static hashf_t hash_function(struct super_block *s)
{
	switch (what_hash(s)) {
	case TEA_HASH:
		reiserfs_info(s, "Using tea hash to sort names\n");
		return keyed_hash;
	case YURA_HASH:
		reiserfs_info(s, "Using rupasov hash to sort names\n");
		return yura_hash;
	case R5_HASH:
		reiserfs_info(s, "Using r5 hash to sort names\n");
		return r5_hash;
	}
	return NULL;
}

// this is used to set up correct value for old partitions
static int function2code(hashf_t func)
{
	if (func == keyed_hash)
		return TEA_HASH;
	if (func == yura_hash)
		return YURA_HASH;
	if (func == r5_hash)
		return R5_HASH;

	BUG();			// should never happen

	return 0;
}

#define SWARN(silent, s, ...)			\
	if (!(silent))				\
		reiserfs_warning (s, __VA_ARGS__)

static int reiserfs_fill_super(struct super_block *s, void *data, int silent)
{
	struct inode *root_inode;
	int j;
	struct reiserfs_transaction_handle th;
	int old_format = 0;
	unsigned long blocks;
	unsigned int commit_max_age = 0;
	int jinit_done = 0;
	struct reiserfs_iget_args args;
	struct reiserfs_super_block *rs;
	char *jdev_name;
	struct reiserfs_sb_info *sbi;
	int errval = -EINVAL;

	sbi = kmalloc(sizeof(struct reiserfs_sb_info), GFP_KERNEL);
	if (!sbi) {
		errval = -ENOMEM;
		goto error;
	}
	s->s_fs_info = sbi;
	memset(sbi, 0, sizeof(struct reiserfs_sb_info));
	/* Set default values for options: non-aggressive tails, RO on errors */
	REISERFS_SB(s)->s_mount_opt |= (1 << REISERFS_SMALLTAIL);
	REISERFS_SB(s)->s_mount_opt |= (1 << REISERFS_ERROR_RO);
	/* no preallocation minimum, be smart in
	   reiserfs_file_write instead */
	REISERFS_SB(s)->s_alloc_options.preallocmin = 0;
	/* Preallocate by 16 blocks (17-1) at once */
	REISERFS_SB(s)->s_alloc_options.preallocsize = 17;
	/* Initialize the rwsem for xattr dir */
	init_rwsem(&REISERFS_SB(s)->xattr_dir_sem);

	/* setup default block allocator options */
	reiserfs_init_alloc_options(s);

	jdev_name = NULL;
	if (reiserfs_parse_options
	    (s, (char *)data, &(sbi->s_mount_opt), &blocks, &jdev_name,
	     &commit_max_age) == 0) {
		goto error;
	}

	if (blocks) {
		SWARN(silent, s, "jmacd-7: reiserfs_fill_super: resize option "
		      "for remount only");
		goto error;
	}

	/* try old format (undistributed bitmap, super block in 8-th 1k block of a device) */
	if (!read_super_block(s, REISERFS_OLD_DISK_OFFSET_IN_BYTES))
		old_format = 1;
	/* try new format (64-th 1k block), which can contain reiserfs super block */
	else if (read_super_block(s, REISERFS_DISK_OFFSET_IN_BYTES)) {
		SWARN(silent, s,
		      "sh-2021: reiserfs_fill_super: can not find reiserfs on %s",
		      reiserfs_bdevname(s));
		goto error;
	}

	rs = SB_DISK_SUPER_BLOCK(s);
	/* Let's do basic sanity check to verify that underlying device is not
	   smaller than the filesystem. If the check fails then abort and scream,
	   because bad stuff will happen otherwise. */
	if (s->s_bdev && s->s_bdev->bd_inode
	    && i_size_read(s->s_bdev->bd_inode) <
	    sb_block_count(rs) * sb_blocksize(rs)) {
		SWARN(silent, s,
		      "Filesystem on %s cannot be mounted because it is bigger than the device",
		      reiserfs_bdevname(s));
		SWARN(silent, s,
		      "You may need to run fsck or increase size of your LVM partition");
		SWARN(silent, s,
		      "Or may be you forgot to reboot after fdisk when it told you to");
		goto error;
	}

	sbi->s_mount_state = SB_REISERFS_STATE(s);
	sbi->s_mount_state = REISERFS_VALID_FS;

	if (old_format ? read_old_bitmaps(s) : read_bitmaps(s)) {
		SWARN(silent, s,
		      "jmacd-8: reiserfs_fill_super: unable to read bitmap");
		goto error;
	}
#ifdef CONFIG_REISERFS_CHECK
	SWARN(silent, s, "CONFIG_REISERFS_CHECK is set ON");
	SWARN(silent, s, "- it is slow mode for debugging.");
#endif

	/* make data=ordered the default */
	if (!reiserfs_data_log(s) && !reiserfs_data_ordered(s) &&
	    !reiserfs_data_writeback(s)) {
		REISERFS_SB(s)->s_mount_opt |= (1 << REISERFS_DATA_ORDERED);
	}

	if (reiserfs_data_log(s)) {
		reiserfs_info(s, "using journaled data mode\n");
	} else if (reiserfs_data_ordered(s)) {
		reiserfs_info(s, "using ordered data mode\n");
	} else {
		reiserfs_info(s, "using writeback data mode\n");
	}
	if (reiserfs_barrier_flush(s)) {
		printk("reiserfs: using flush barriers\n");
	}
	// set_device_ro(s->s_dev, 1) ;
	if (journal_init(s, jdev_name, old_format, commit_max_age)) {
		SWARN(silent, s,
		      "sh-2022: reiserfs_fill_super: unable to initialize journal space");
		goto error;
	} else {
		jinit_done = 1;	/* once this is set, journal_release must be called
				 ** if we error out of the mount
				 */
	}
	if (reread_meta_blocks(s)) {
		SWARN(silent, s,
		      "jmacd-9: reiserfs_fill_super: unable to reread meta blocks after journal init");
		goto error;
	}

	if (replay_only(s))
		goto error;

	if (bdev_read_only(s->s_bdev) && !(s->s_flags & MS_RDONLY)) {
		SWARN(silent, s,
		      "clm-7000: Detected readonly device, marking FS readonly");
		s->s_flags |= MS_RDONLY;
	}
	args.objectid = REISERFS_ROOT_OBJECTID;
	args.dirid = REISERFS_ROOT_PARENT_OBJECTID;
	root_inode =
	    iget5_locked(s, REISERFS_ROOT_OBJECTID, reiserfs_find_actor,
			 reiserfs_init_locked_inode, (void *)(&args));
	if (!root_inode) {
		SWARN(silent, s,
		      "jmacd-10: reiserfs_fill_super: get root inode failed");
		goto error;
	}

	if (root_inode->i_state & I_NEW) {
		reiserfs_read_locked_inode(root_inode, &args);
		unlock_new_inode(root_inode);
	}

	s->s_root = d_alloc_root(root_inode);
	if (!s->s_root) {
		iput(root_inode);
		goto error;
	}
	// define and initialize hash function
	sbi->s_hash_function = hash_function(s);
	if (sbi->s_hash_function == NULL) {
		dput(s->s_root);
		s->s_root = NULL;
		goto error;
	}

	if (is_reiserfs_3_5(rs)
	    || (is_reiserfs_jr(rs) && SB_VERSION(s) == REISERFS_VERSION_1))
		set_bit(REISERFS_3_5, &(sbi->s_properties));
	else
		set_bit(REISERFS_3_6, &(sbi->s_properties));

	if (!(s->s_flags & MS_RDONLY)) {

		errval = journal_begin(&th, s, 1);
		if (errval) {
			dput(s->s_root);
			s->s_root = NULL;
			goto error;
		}
		reiserfs_prepare_for_journal(s, SB_BUFFER_WITH_SB(s), 1);

		set_sb_umount_state(rs, REISERFS_ERROR_FS);
		set_sb_fs_state(rs, 0);

		if (old_format_only(s)) {
			/* filesystem of format 3.5 either with standard or non-standard
			   journal */
			if (convert_reiserfs(s)) {
				/* and -o conv is given */
				if (!silent)
					reiserfs_info(s,
						      "converting 3.5 filesystem to the 3.6 format");

				if (is_reiserfs_3_5(rs))
					/* put magic string of 3.6 format. 2.2 will not be able to
					   mount this filesystem anymore */
					memcpy(rs->s_v1.s_magic,
					       reiserfs_3_6_magic_string,
					       sizeof
					       (reiserfs_3_6_magic_string));

				set_sb_version(rs, REISERFS_VERSION_2);
				reiserfs_convert_objectid_map_v1(s);
				set_bit(REISERFS_3_6, &(sbi->s_properties));
				clear_bit(REISERFS_3_5, &(sbi->s_properties));
			} else if (!silent) {
				reiserfs_info(s, "using 3.5.x disk format\n");
			}
		}

		journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB(s));
		errval = journal_end(&th, s, 1);
		if (errval) {
			dput(s->s_root);
			s->s_root = NULL;
			goto error;
		}

		if ((errval = reiserfs_xattr_init(s, s->s_flags))) {
			dput(s->s_root);
			s->s_root = NULL;
			goto error;
		}

		/* look for files which were to be removed in previous session */
		finish_unfinished(s);
	} else {
		if (old_format_only(s) && !silent) {
			reiserfs_info(s, "using 3.5.x disk format\n");
		}

		if ((errval = reiserfs_xattr_init(s, s->s_flags))) {
			dput(s->s_root);
			s->s_root = NULL;
			goto error;
		}
	}
	// mark hash in super block: it could be unset. overwrite should be ok
	set_sb_hash_function_code(rs, function2code(sbi->s_hash_function));

	handle_attrs(s);

	reiserfs_proc_info_init(s);

	init_waitqueue_head(&(sbi->s_wait));
	spin_lock_init(&sbi->bitmap_lock);

	return (0);

      error:
	if (jinit_done) {	/* kill the commit thread, free journal ram */
		journal_release_error(NULL, s);
	}
	if (SB_DISK_SUPER_BLOCK(s)) {
		for (j = 0; j < SB_BMAP_NR(s); j++) {
			if (SB_AP_BITMAP(s))
				brelse(SB_AP_BITMAP(s)[j].bh);
		}
		vfree(SB_AP_BITMAP(s));
	}
	if (SB_BUFFER_WITH_SB(s))
		brelse(SB_BUFFER_WITH_SB(s));
#ifdef CONFIG_QUOTA
	for (j = 0; j < MAXQUOTAS; j++) {
		kfree(sbi->s_qf_names[j]);
		sbi->s_qf_names[j] = NULL;
	}
#endif
	kfree(sbi);

	s->s_fs_info = NULL;
	return errval;
}

static int reiserfs_statfs(struct super_block *s, struct kstatfs *buf)
{
	struct reiserfs_super_block *rs = SB_DISK_SUPER_BLOCK(s);

	buf->f_namelen = (REISERFS_MAX_NAME(s->s_blocksize));
	buf->f_bfree = sb_free_blocks(rs);
	buf->f_bavail = buf->f_bfree;
	buf->f_blocks = sb_block_count(rs) - sb_bmap_nr(rs) - 1;
	buf->f_bsize = s->s_blocksize;
	/* changed to accommodate gcc folks. */
	buf->f_type = REISERFS_SUPER_MAGIC;
	return 0;
}

#ifdef CONFIG_QUOTA
static int reiserfs_dquot_initialize(struct inode *inode, int type)
{
	struct reiserfs_transaction_handle th;
	int ret, err;

	/* We may create quota structure so we need to reserve enough blocks */
	reiserfs_write_lock(inode->i_sb);
	ret =
	    journal_begin(&th, inode->i_sb,
			  2 * REISERFS_QUOTA_INIT_BLOCKS(inode->i_sb));
	if (ret)
		goto out;
	ret = dquot_initialize(inode, type);
	err =
	    journal_end(&th, inode->i_sb,
			2 * REISERFS_QUOTA_INIT_BLOCKS(inode->i_sb));
	if (!ret && err)
		ret = err;
      out:
	reiserfs_write_unlock(inode->i_sb);
	return ret;
}

static int reiserfs_dquot_drop(struct inode *inode)
{
	struct reiserfs_transaction_handle th;
	int ret, err;

	/* We may delete quota structure so we need to reserve enough blocks */
	reiserfs_write_lock(inode->i_sb);
	ret =
	    journal_begin(&th, inode->i_sb,
			  2 * REISERFS_QUOTA_DEL_BLOCKS(inode->i_sb));
	if (ret)
		goto out;
	ret = dquot_drop(inode);
	err =
	    journal_end(&th, inode->i_sb,
			2 * REISERFS_QUOTA_DEL_BLOCKS(inode->i_sb));
	if (!ret && err)
		ret = err;
      out:
	reiserfs_write_unlock(inode->i_sb);
	return ret;
}

static int reiserfs_write_dquot(struct dquot *dquot)
{
	struct reiserfs_transaction_handle th;
	int ret, err;

	reiserfs_write_lock(dquot->dq_sb);
	ret =
	    journal_begin(&th, dquot->dq_sb,
			  REISERFS_QUOTA_TRANS_BLOCKS(dquot->dq_sb));
	if (ret)
		goto out;
	ret = dquot_commit(dquot);
	err =
	    journal_end(&th, dquot->dq_sb,
			REISERFS_QUOTA_TRANS_BLOCKS(dquot->dq_sb));
	if (!ret && err)
		ret = err;
      out:
	reiserfs_write_unlock(dquot->dq_sb);
	return ret;
}

static int reiserfs_acquire_dquot(struct dquot *dquot)
{
	struct reiserfs_transaction_handle th;
	int ret, err;

	reiserfs_write_lock(dquot->dq_sb);
	ret =
	    journal_begin(&th, dquot->dq_sb,
			  REISERFS_QUOTA_INIT_BLOCKS(dquot->dq_sb));
	if (ret)
		goto out;
	ret = dquot_acquire(dquot);
	err =
	    journal_end(&th, dquot->dq_sb,
			REISERFS_QUOTA_INIT_BLOCKS(dquot->dq_sb));
	if (!ret && err)
		ret = err;
      out:
	reiserfs_write_unlock(dquot->dq_sb);
	return ret;
}

static int reiserfs_release_dquot(struct dquot *dquot)
{
	struct reiserfs_transaction_handle th;
	int ret, err;

	reiserfs_write_lock(dquot->dq_sb);
	ret =
	    journal_begin(&th, dquot->dq_sb,
			  REISERFS_QUOTA_DEL_BLOCKS(dquot->dq_sb));
	if (ret)
		goto out;
	ret = dquot_release(dquot);
	err =
	    journal_end(&th, dquot->dq_sb,
			REISERFS_QUOTA_DEL_BLOCKS(dquot->dq_sb));
	if (!ret && err)
		ret = err;
      out:
	reiserfs_write_unlock(dquot->dq_sb);
	return ret;
}

static int reiserfs_mark_dquot_dirty(struct dquot *dquot)
{
	/* Are we journalling quotas? */
	if (REISERFS_SB(dquot->dq_sb)->s_qf_names[USRQUOTA] ||
	    REISERFS_SB(dquot->dq_sb)->s_qf_names[GRPQUOTA]) {
		dquot_mark_dquot_dirty(dquot);
		return reiserfs_write_dquot(dquot);
	} else
		return dquot_mark_dquot_dirty(dquot);
}

static int reiserfs_write_info(struct super_block *sb, int type)
{
	struct reiserfs_transaction_handle th;
	int ret, err;

	/* Data block + inode block */
	reiserfs_write_lock(sb);
	ret = journal_begin(&th, sb, 2);
	if (ret)
		goto out;
	ret = dquot_commit_info(sb, type);
	err = journal_end(&th, sb, 2);
	if (!ret && err)
		ret = err;
      out:
	reiserfs_write_unlock(sb);
	return ret;
}

/*
 * Turn on quotas during mount time - we need to find the quota file and such...
 */
static int reiserfs_quota_on_mount(struct super_block *sb, int type)
{
	return vfs_quota_on_mount(sb, REISERFS_SB(sb)->s_qf_names[type],
				  REISERFS_SB(sb)->s_jquota_fmt, type);
}

/*
 * Standard function to be called on quota_on
 */
static int reiserfs_quota_on(struct super_block *sb, int type, int format_id,
			     char *path)
{
	int err;
	struct nameidata nd;

	if (!(REISERFS_SB(sb)->s_mount_opt & (1 << REISERFS_QUOTA)))
		return -EINVAL;
	err = path_lookup(path, LOOKUP_FOLLOW, &nd);
	if (err)
		return err;
	/* Quotafile not on the same filesystem? */
	if (nd.mnt->mnt_sb != sb) {
		path_release(&nd);
		return -EXDEV;
	}
	/* We must not pack tails for quota files on reiserfs for quota IO to work */
	if (!REISERFS_I(nd.dentry->d_inode)->i_flags & i_nopack_mask) {
		reiserfs_warning(sb,
				 "reiserfs: Quota file must have tail packing disabled.");
		path_release(&nd);
		return -EINVAL;
	}
	/* Not journalling quota? No more tests needed... */
	if (!REISERFS_SB(sb)->s_qf_names[USRQUOTA] &&
	    !REISERFS_SB(sb)->s_qf_names[GRPQUOTA]) {
		path_release(&nd);
		return vfs_quota_on(sb, type, format_id, path);
	}
	/* Quotafile not of fs root? */
	if (nd.dentry->d_parent->d_inode != sb->s_root->d_inode)
		reiserfs_warning(sb,
				 "reiserfs: Quota file not on filesystem root. "
				 "Journalled quota will not work.");
	path_release(&nd);
	return vfs_quota_on(sb, type, format_id, path);
}

/* Read data from quotafile - avoid pagecache and such because we cannot afford
 * acquiring the locks... As quota files are never truncated and quota code
 * itself serializes the operations (and noone else should touch the files)
 * we don't have to be afraid of races */
static ssize_t reiserfs_quota_read(struct super_block *sb, int type, char *data,
				   size_t len, loff_t off)
{
	struct inode *inode = sb_dqopt(sb)->files[type];
	unsigned long blk = off >> sb->s_blocksize_bits;
	int err = 0, offset = off & (sb->s_blocksize - 1), tocopy;
	size_t toread;
	struct buffer_head tmp_bh, *bh;
	loff_t i_size = i_size_read(inode);

	if (off > i_size)
		return 0;
	if (off + len > i_size)
		len = i_size - off;
	toread = len;
	while (toread > 0) {
		tocopy =
		    sb->s_blocksize - offset <
		    toread ? sb->s_blocksize - offset : toread;
		tmp_bh.b_state = 0;
		/* Quota files are without tails so we can safely use this function */
		reiserfs_write_lock(sb);
		err = reiserfs_get_block(inode, blk, &tmp_bh, 0);
		reiserfs_write_unlock(sb);
		if (err)
			return err;
		if (!buffer_mapped(&tmp_bh))	/* A hole? */
			memset(data, 0, tocopy);
		else {
			bh = sb_bread(sb, tmp_bh.b_blocknr);
			if (!bh)
				return -EIO;
			memcpy(data, bh->b_data + offset, tocopy);
			brelse(bh);
		}
		offset = 0;
		toread -= tocopy;
		data += tocopy;
		blk++;
	}
	return len;
}

/* Write to quotafile (we know the transaction is already started and has
 * enough credits) */
static ssize_t reiserfs_quota_write(struct super_block *sb, int type,
				    const char *data, size_t len, loff_t off)
{
	struct inode *inode = sb_dqopt(sb)->files[type];
	unsigned long blk = off >> sb->s_blocksize_bits;
	int err = 0, offset = off & (sb->s_blocksize - 1), tocopy;
	int journal_quota = REISERFS_SB(sb)->s_qf_names[type] != NULL;
	size_t towrite = len;
	struct buffer_head tmp_bh, *bh;

	mutex_lock(&inode->i_mutex);
	while (towrite > 0) {
		tocopy = sb->s_blocksize - offset < towrite ?
		    sb->s_blocksize - offset : towrite;
		tmp_bh.b_state = 0;
		err = reiserfs_get_block(inode, blk, &tmp_bh, GET_BLOCK_CREATE);
		if (err)
			goto out;
		if (offset || tocopy != sb->s_blocksize)
			bh = sb_bread(sb, tmp_bh.b_blocknr);
		else
			bh = sb_getblk(sb, tmp_bh.b_blocknr);
		if (!bh) {
			err = -EIO;
			goto out;
		}
		lock_buffer(bh);
		memcpy(bh->b_data + offset, data, tocopy);
		flush_dcache_page(bh->b_page);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		reiserfs_prepare_for_journal(sb, bh, 1);
		journal_mark_dirty(current->journal_info, sb, bh);
		if (!journal_quota)
			reiserfs_add_ordered_list(inode, bh);
		brelse(bh);
		offset = 0;
		towrite -= tocopy;
		data += tocopy;
		blk++;
	}
      out:
	if (len == towrite)
		return err;
	if (inode->i_size < off + len - towrite)
		i_size_write(inode, off + len - towrite);
	inode->i_version++;
	inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	mark_inode_dirty(inode);
	mutex_unlock(&inode->i_mutex);
	return len - towrite;
}

#endif

static struct super_block *get_super_block(struct file_system_type *fs_type,
					   int flags, const char *dev_name,
					   void *data)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, reiserfs_fill_super);
}

static int __init init_reiserfs_fs(void)
{
	int ret;

	if ((ret = init_inodecache())) {
		return ret;
	}

	if ((ret = reiserfs_xattr_register_handlers()))
		goto failed_reiserfs_xattr_register_handlers;

	reiserfs_proc_info_global_init();
	reiserfs_proc_register_global("version",
				      reiserfs_global_version_in_proc);

	ret = register_filesystem(&reiserfs_fs_type);

	if (ret == 0) {
		return 0;
	}

	reiserfs_xattr_unregister_handlers();

      failed_reiserfs_xattr_register_handlers:
	reiserfs_proc_unregister_global("version");
	reiserfs_proc_info_global_done();
	destroy_inodecache();

	return ret;
}

static void __exit exit_reiserfs_fs(void)
{
	reiserfs_xattr_unregister_handlers();
	reiserfs_proc_unregister_global("version");
	reiserfs_proc_info_global_done();
	unregister_filesystem(&reiserfs_fs_type);
	destroy_inodecache();
}

struct file_system_type reiserfs_fs_type = {
	.owner = THIS_MODULE,
	.name = "reiserfs",
	.get_sb = get_super_block,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};

MODULE_DESCRIPTION("ReiserFS journaled filesystem");
MODULE_AUTHOR("Hans Reiser <reiser@namesys.com>");
MODULE_LICENSE("GPL");

module_init(init_reiserfs_fs);
module_exit(exit_reiserfs_fs);
