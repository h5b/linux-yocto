#define MSNFS	/* HACK HACK */
/*
 * linux/fs/nfsd/vfs.c
 *
 * File operations used by nfsd. Some of these have been ripped from
 * other parts of the kernel because they weren't exported, others
 * are partial duplicates with added or changed functionality.
 *
 * Note that several functions dget() the dentry upon which they want
 * to act, most notably those that create directory entries. Response
 * dentry's are dput()'d if necessary in the release callback.
 * So if you notice code paths that apparently fail to dput() the
 * dentry, don't worry--they have been taken care of.
 *
 * Copyright (C) 1995-1999 Olaf Kirch <okir@monad.swb.de>
 * Zerocpy NFS support (C) 2002 Hirokazu Takahashi <taka@valinux.co.jp>
 */

#include <linux/config.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mount.h>
#include <linux/major.h>
#include <linux/ext2_fs.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/net.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/in.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/vfs.h>
#include <linux/delay.h>
#include <linux/sunrpc/svc.h>
#include <linux/nfsd/nfsd.h>
#ifdef CONFIG_NFSD_V3
#include <linux/nfs3.h>
#include <linux/nfsd/xdr3.h>
#endif /* CONFIG_NFSD_V3 */
#include <linux/nfsd/nfsfh.h>
#include <linux/quotaops.h>
#include <linux/fsnotify.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/xattr.h>
#ifdef CONFIG_NFSD_V4
#include <linux/nfs4.h>
#include <linux/nfs4_acl.h>
#include <linux/nfsd_idmap.h>
#include <linux/security.h>
#endif /* CONFIG_NFSD_V4 */

#include <asm/uaccess.h>

#define NFSDDBG_FACILITY		NFSDDBG_FILEOP
#define NFSD_PARANOIA


/* We must ignore files (but only files) which might have mandatory
 * locks on them because there is no way to know if the accesser has
 * the lock.
 */
#define IS_ISMNDLK(i)	(S_ISREG((i)->i_mode) && MANDATORY_LOCK(i))

/*
 * This is a cache of readahead params that help us choose the proper
 * readahead strategy. Initially, we set all readahead parameters to 0
 * and let the VFS handle things.
 * If you increase the number of cached files very much, you'll need to
 * add a hash table here.
 */
struct raparms {
	struct raparms		*p_next;
	unsigned int		p_count;
	ino_t			p_ino;
	dev_t			p_dev;
	int			p_set;
	struct file_ra_state	p_ra;
};

static struct raparms *		raparml;
static struct raparms *		raparm_cache;

/* 
 * Called from nfsd_lookup and encode_dirent. Check if we have crossed 
 * a mount point.
 * Returns -EAGAIN leaving *dpp and *expp unchanged, 
 *  or nfs_ok having possibly changed *dpp and *expp
 */
int
nfsd_cross_mnt(struct svc_rqst *rqstp, struct dentry **dpp, 
		        struct svc_export **expp)
{
	struct svc_export *exp = *expp, *exp2 = NULL;
	struct dentry *dentry = *dpp;
	struct vfsmount *mnt = mntget(exp->ex_mnt);
	struct dentry *mounts = dget(dentry);
	int err = nfs_ok;

	while (follow_down(&mnt,&mounts)&&d_mountpoint(mounts));

	exp2 = exp_get_by_name(exp->ex_client, mnt, mounts, &rqstp->rq_chandle);
	if (IS_ERR(exp2)) {
		err = PTR_ERR(exp2);
		dput(mounts);
		mntput(mnt);
		goto out;
	}
	if (exp2 && ((exp->ex_flags & NFSEXP_CROSSMOUNT) || EX_NOHIDE(exp2))) {
		/* successfully crossed mount point */
		exp_put(exp);
		*expp = exp2;
		dput(dentry);
		*dpp = mounts;
	} else {
		if (exp2) exp_put(exp2);
		dput(mounts);
	}
	mntput(mnt);
out:
	return err;
}

/*
 * Look up one component of a pathname.
 * N.B. After this call _both_ fhp and resfh need an fh_put
 *
 * If the lookup would cross a mountpoint, and the mounted filesystem
 * is exported to the client with NFSEXP_NOHIDE, then the lookup is
 * accepted as it stands and the mounted directory is
 * returned. Otherwise the covered directory is returned.
 * NOTE: this mountpoint crossing is not supported properly by all
 *   clients and is explicitly disallowed for NFSv3
 *      NeilBrown <neilb@cse.unsw.edu.au>
 */
int
nfsd_lookup(struct svc_rqst *rqstp, struct svc_fh *fhp, const char *name,
					int len, struct svc_fh *resfh)
{
	struct svc_export	*exp;
	struct dentry		*dparent;
	struct dentry		*dentry;
	int			err;

	dprintk("nfsd: nfsd_lookup(fh %s, %.*s)\n", SVCFH_fmt(fhp), len,name);

	/* Obtain dentry and export. */
	err = fh_verify(rqstp, fhp, S_IFDIR, MAY_EXEC);
	if (err)
		return err;

	dparent = fhp->fh_dentry;
	exp  = fhp->fh_export;
	exp_get(exp);

	err = nfserr_acces;

	/* Lookup the name, but don't follow links */
	if (isdotent(name, len)) {
		if (len==1)
			dentry = dget(dparent);
		else if (dparent != exp->ex_dentry) {
			dentry = dget_parent(dparent);
		} else  if (!EX_NOHIDE(exp))
			dentry = dget(dparent); /* .. == . just like at / */
		else {
			/* checking mountpoint crossing is very different when stepping up */
			struct svc_export *exp2 = NULL;
			struct dentry *dp;
			struct vfsmount *mnt = mntget(exp->ex_mnt);
			dentry = dget(dparent);
			while(dentry == mnt->mnt_root && follow_up(&mnt, &dentry))
				;
			dp = dget_parent(dentry);
			dput(dentry);
			dentry = dp;

			exp2 = exp_parent(exp->ex_client, mnt, dentry,
					  &rqstp->rq_chandle);
			if (IS_ERR(exp2)) {
				err = PTR_ERR(exp2);
				dput(dentry);
				mntput(mnt);
				goto out_nfserr;
			}
			if (!exp2) {
				dput(dentry);
				dentry = dget(dparent);
			} else {
				exp_put(exp);
				exp = exp2;
			}
			mntput(mnt);
		}
	} else {
		fh_lock(fhp);
		dentry = lookup_one_len(name, dparent, len);
		err = PTR_ERR(dentry);
		if (IS_ERR(dentry))
			goto out_nfserr;
		/*
		 * check if we have crossed a mount point ...
		 */
		if (d_mountpoint(dentry)) {
			if ((err = nfsd_cross_mnt(rqstp, &dentry, &exp))) {
				dput(dentry);
				goto out_nfserr;
			}
		}
	}
	/*
	 * Note: we compose the file handle now, but as the
	 * dentry may be negative, it may need to be updated.
	 */
	err = fh_compose(resfh, exp, dentry, fhp);
	if (!err && !dentry->d_inode)
		err = nfserr_noent;
	dput(dentry);
out:
	exp_put(exp);
	return err;

out_nfserr:
	err = nfserrno(err);
	goto out;
}

/*
 * Set various file attributes.
 * N.B. After this call fhp needs an fh_put
 */
int
nfsd_setattr(struct svc_rqst *rqstp, struct svc_fh *fhp, struct iattr *iap,
	     int check_guard, time_t guardtime)
{
	struct dentry	*dentry;
	struct inode	*inode;
	int		accmode = MAY_SATTR;
	int		ftype = 0;
	int		imode;
	int		err;
	int		size_change = 0;

	if (iap->ia_valid & (ATTR_ATIME | ATTR_MTIME | ATTR_SIZE))
		accmode |= MAY_WRITE|MAY_OWNER_OVERRIDE;
	if (iap->ia_valid & ATTR_SIZE)
		ftype = S_IFREG;

	/* Get inode */
	err = fh_verify(rqstp, fhp, ftype, accmode);
	if (err)
		goto out;

	dentry = fhp->fh_dentry;
	inode = dentry->d_inode;

	/* Ignore any mode updates on symlinks */
	if (S_ISLNK(inode->i_mode))
		iap->ia_valid &= ~ATTR_MODE;

	if (!iap->ia_valid)
		goto out;

	/* NFSv2 does not differentiate between "set-[ac]time-to-now"
	 * which only requires access, and "set-[ac]time-to-X" which
	 * requires ownership.
	 * So if it looks like it might be "set both to the same time which
	 * is close to now", and if inode_change_ok fails, then we
	 * convert to "set to now" instead of "set to explicit time"
	 *
	 * We only call inode_change_ok as the last test as technically
	 * it is not an interface that we should be using.  It is only
	 * valid if the filesystem does not define it's own i_op->setattr.
	 */
#define BOTH_TIME_SET (ATTR_ATIME_SET | ATTR_MTIME_SET)
#define	MAX_TOUCH_TIME_ERROR (30*60)
	if ((iap->ia_valid & BOTH_TIME_SET) == BOTH_TIME_SET
	    && iap->ia_mtime.tv_sec == iap->ia_atime.tv_sec
	    ) {
	    /* Looks probable.  Now just make sure time is in the right ballpark.
	     * Solaris, at least, doesn't seem to care what the time request is.
	     * We require it be within 30 minutes of now.
	     */
	    time_t delta = iap->ia_atime.tv_sec - get_seconds();
	    if (delta<0) delta = -delta;
	    if (delta < MAX_TOUCH_TIME_ERROR &&
		inode_change_ok(inode, iap) != 0) {
		/* turn off ATTR_[AM]TIME_SET but leave ATTR_[AM]TIME
		 * this will cause notify_change to set these times to "now"
		 */
		iap->ia_valid &= ~BOTH_TIME_SET;
	    }
	}
	    
	/* The size case is special. It changes the file as well as the attributes.  */
	if (iap->ia_valid & ATTR_SIZE) {
		if (iap->ia_size < inode->i_size) {
			err = nfsd_permission(fhp->fh_export, dentry, MAY_TRUNC|MAY_OWNER_OVERRIDE);
			if (err)
				goto out;
		}

		/*
		 * If we are changing the size of the file, then
		 * we need to break all leases.
		 */
		err = break_lease(inode, FMODE_WRITE | O_NONBLOCK);
		if (err == -EWOULDBLOCK)
			err = -ETIMEDOUT;
		if (err) /* ENOMEM or EWOULDBLOCK */
			goto out_nfserr;

		err = get_write_access(inode);
		if (err)
			goto out_nfserr;

		size_change = 1;
		err = locks_verify_truncate(inode, NULL, iap->ia_size);
		if (err) {
			put_write_access(inode);
			goto out_nfserr;
		}
		DQUOT_INIT(inode);
	}

	imode = inode->i_mode;
	if (iap->ia_valid & ATTR_MODE) {
		iap->ia_mode &= S_IALLUGO;
		imode = iap->ia_mode |= (imode & ~S_IALLUGO);
	}

	/* Revoke setuid/setgid bit on chown/chgrp */
	if ((iap->ia_valid & ATTR_UID) && iap->ia_uid != inode->i_uid)
		iap->ia_valid |= ATTR_KILL_SUID;
	if ((iap->ia_valid & ATTR_GID) && iap->ia_gid != inode->i_gid)
		iap->ia_valid |= ATTR_KILL_SGID;

	/* Change the attributes. */

	iap->ia_valid |= ATTR_CTIME;

	err = nfserr_notsync;
	if (!check_guard || guardtime == inode->i_ctime.tv_sec) {
		fh_lock(fhp);
		err = notify_change(dentry, iap);
		err = nfserrno(err);
		fh_unlock(fhp);
	}
	if (size_change)
		put_write_access(inode);
	if (!err)
		if (EX_ISSYNC(fhp->fh_export))
			write_inode_now(inode, 1);
out:
	return err;

out_nfserr:
	err = nfserrno(err);
	goto out;
}

#if defined(CONFIG_NFSD_V2_ACL) || \
    defined(CONFIG_NFSD_V3_ACL) || \
    defined(CONFIG_NFSD_V4)
static ssize_t nfsd_getxattr(struct dentry *dentry, char *key, void **buf)
{
	ssize_t buflen;
	int error;

	buflen = vfs_getxattr(dentry, key, NULL, 0);
	if (buflen <= 0)
		return buflen;

	*buf = kmalloc(buflen, GFP_KERNEL);
	if (!*buf)
		return -ENOMEM;

	error = vfs_getxattr(dentry, key, *buf, buflen);
	if (error < 0)
		return error;
	return buflen;
}
#endif

#if defined(CONFIG_NFSD_V4)
static int
set_nfsv4_acl_one(struct dentry *dentry, struct posix_acl *pacl, char *key)
{
	int len;
	size_t buflen;
	char *buf = NULL;
	int error = 0;

	buflen = posix_acl_xattr_size(pacl->a_count);
	buf = kmalloc(buflen, GFP_KERNEL);
	error = -ENOMEM;
	if (buf == NULL)
		goto out;

	len = posix_acl_to_xattr(pacl, buf, buflen);
	if (len < 0) {
		error = len;
		goto out;
	}

	error = vfs_setxattr(dentry, key, buf, len, 0);
out:
	kfree(buf);
	return error;
}

int
nfsd4_set_nfs4_acl(struct svc_rqst *rqstp, struct svc_fh *fhp,
    struct nfs4_acl *acl)
{
	int error;
	struct dentry *dentry;
	struct inode *inode;
	struct posix_acl *pacl = NULL, *dpacl = NULL;
	unsigned int flags = 0;

	/* Get inode */
	error = fh_verify(rqstp, fhp, 0 /* S_IFREG */, MAY_SATTR);
	if (error)
		goto out;

	dentry = fhp->fh_dentry;
	inode = dentry->d_inode;
	if (S_ISDIR(inode->i_mode))
		flags = NFS4_ACL_DIR;

	error = nfs4_acl_nfsv4_to_posix(acl, &pacl, &dpacl, flags);
	if (error == -EINVAL) {
		error = nfserr_attrnotsupp;
		goto out;
	} else if (error < 0)
		goto out_nfserr;

	if (pacl) {
		error = set_nfsv4_acl_one(dentry, pacl, POSIX_ACL_XATTR_ACCESS);
		if (error < 0)
			goto out_nfserr;
	}

	if (dpacl) {
		error = set_nfsv4_acl_one(dentry, dpacl, POSIX_ACL_XATTR_DEFAULT);
		if (error < 0)
			goto out_nfserr;
	}

	error = nfs_ok;

out:
	posix_acl_release(pacl);
	posix_acl_release(dpacl);
	return (error);
out_nfserr:
	error = nfserrno(error);
	goto out;
}

static struct posix_acl *
_get_posix_acl(struct dentry *dentry, char *key)
{
	void *buf = NULL;
	struct posix_acl *pacl = NULL;
	int buflen;

	buflen = nfsd_getxattr(dentry, key, &buf);
	if (!buflen)
		buflen = -ENODATA;
	if (buflen <= 0)
		return ERR_PTR(buflen);

	pacl = posix_acl_from_xattr(buf, buflen);
	kfree(buf);
	return pacl;
}

int
nfsd4_get_nfs4_acl(struct svc_rqst *rqstp, struct dentry *dentry, struct nfs4_acl **acl)
{
	struct inode *inode = dentry->d_inode;
	int error = 0;
	struct posix_acl *pacl = NULL, *dpacl = NULL;
	unsigned int flags = 0;

	pacl = _get_posix_acl(dentry, POSIX_ACL_XATTR_ACCESS);
	if (IS_ERR(pacl) && PTR_ERR(pacl) == -ENODATA)
		pacl = posix_acl_from_mode(inode->i_mode, GFP_KERNEL);
	if (IS_ERR(pacl)) {
		error = PTR_ERR(pacl);
		pacl = NULL;
		goto out;
	}

	if (S_ISDIR(inode->i_mode)) {
		dpacl = _get_posix_acl(dentry, POSIX_ACL_XATTR_DEFAULT);
		if (IS_ERR(dpacl) && PTR_ERR(dpacl) == -ENODATA)
			dpacl = NULL;
		else if (IS_ERR(dpacl)) {
			error = PTR_ERR(dpacl);
			dpacl = NULL;
			goto out;
		}
		flags = NFS4_ACL_DIR;
	}

	*acl = nfs4_acl_posix_to_nfsv4(pacl, dpacl, flags);
	if (IS_ERR(*acl)) {
		error = PTR_ERR(*acl);
		*acl = NULL;
	}
 out:
	posix_acl_release(pacl);
	posix_acl_release(dpacl);
	return error;
}

#endif /* defined(CONFIG_NFS_V4) */

#ifdef CONFIG_NFSD_V3
/*
 * Check server access rights to a file system object
 */
struct accessmap {
	u32		access;
	int		how;
};
static struct accessmap	nfs3_regaccess[] = {
    {	NFS3_ACCESS_READ,	MAY_READ			},
    {	NFS3_ACCESS_EXECUTE,	MAY_EXEC			},
    {	NFS3_ACCESS_MODIFY,	MAY_WRITE|MAY_TRUNC		},
    {	NFS3_ACCESS_EXTEND,	MAY_WRITE			},

    {	0,			0				}
};

static struct accessmap	nfs3_diraccess[] = {
    {	NFS3_ACCESS_READ,	MAY_READ			},
    {	NFS3_ACCESS_LOOKUP,	MAY_EXEC			},
    {	NFS3_ACCESS_MODIFY,	MAY_EXEC|MAY_WRITE|MAY_TRUNC	},
    {	NFS3_ACCESS_EXTEND,	MAY_EXEC|MAY_WRITE		},
    {	NFS3_ACCESS_DELETE,	MAY_REMOVE			},

    {	0,			0				}
};

static struct accessmap	nfs3_anyaccess[] = {
	/* Some clients - Solaris 2.6 at least, make an access call
	 * to the server to check for access for things like /dev/null
	 * (which really, the server doesn't care about).  So
	 * We provide simple access checking for them, looking
	 * mainly at mode bits, and we make sure to ignore read-only
	 * filesystem checks
	 */
    {	NFS3_ACCESS_READ,	MAY_READ			},
    {	NFS3_ACCESS_EXECUTE,	MAY_EXEC			},
    {	NFS3_ACCESS_MODIFY,	MAY_WRITE|MAY_LOCAL_ACCESS	},
    {	NFS3_ACCESS_EXTEND,	MAY_WRITE|MAY_LOCAL_ACCESS	},

    {	0,			0				}
};

int
nfsd_access(struct svc_rqst *rqstp, struct svc_fh *fhp, u32 *access, u32 *supported)
{
	struct accessmap	*map;
	struct svc_export	*export;
	struct dentry		*dentry;
	u32			query, result = 0, sresult = 0;
	unsigned int		error;

	error = fh_verify(rqstp, fhp, 0, MAY_NOP);
	if (error)
		goto out;

	export = fhp->fh_export;
	dentry = fhp->fh_dentry;

	if (S_ISREG(dentry->d_inode->i_mode))
		map = nfs3_regaccess;
	else if (S_ISDIR(dentry->d_inode->i_mode))
		map = nfs3_diraccess;
	else
		map = nfs3_anyaccess;


	query = *access;
	for  (; map->access; map++) {
		if (map->access & query) {
			unsigned int err2;

			sresult |= map->access;

			err2 = nfsd_permission(export, dentry, map->how);
			switch (err2) {
			case nfs_ok:
				result |= map->access;
				break;
				
			/* the following error codes just mean the access was not allowed,
			 * rather than an error occurred */
			case nfserr_rofs:
			case nfserr_acces:
			case nfserr_perm:
				/* simply don't "or" in the access bit. */
				break;
			default:
				error = err2;
				goto out;
			}
		}
	}
	*access = result;
	if (supported)
		*supported = sresult;

 out:
	return error;
}
#endif /* CONFIG_NFSD_V3 */



/*
 * Open an existing file or directory.
 * The access argument indicates the type of open (read/write/lock)
 * N.B. After this call fhp needs an fh_put
 */
int
nfsd_open(struct svc_rqst *rqstp, struct svc_fh *fhp, int type,
			int access, struct file **filp)
{
	struct dentry	*dentry;
	struct inode	*inode;
	int		flags = O_RDONLY|O_LARGEFILE, err;

	/*
	 * If we get here, then the client has already done an "open",
	 * and (hopefully) checked permission - so allow OWNER_OVERRIDE
	 * in case a chmod has now revoked permission.
	 */
	err = fh_verify(rqstp, fhp, type, access | MAY_OWNER_OVERRIDE);
	if (err)
		goto out;

	dentry = fhp->fh_dentry;
	inode = dentry->d_inode;

	/* Disallow write access to files with the append-only bit set
	 * or any access when mandatory locking enabled
	 */
	err = nfserr_perm;
	if (IS_APPEND(inode) && (access & MAY_WRITE))
		goto out;
	if (IS_ISMNDLK(inode))
		goto out;

	if (!inode->i_fop)
		goto out;

	/*
	 * Check to see if there are any leases on this file.
	 * This may block while leases are broken.
	 */
	err = break_lease(inode, O_NONBLOCK | ((access & MAY_WRITE) ? FMODE_WRITE : 0));
	if (err == -EWOULDBLOCK)
		err = -ETIMEDOUT;
	if (err) /* NOMEM or WOULDBLOCK */
		goto out_nfserr;

	if (access & MAY_WRITE) {
		flags = O_WRONLY|O_LARGEFILE;

		DQUOT_INIT(inode);
	}
	*filp = dentry_open(dget(dentry), mntget(fhp->fh_export->ex_mnt), flags);
	if (IS_ERR(*filp))
		err = PTR_ERR(*filp);
out_nfserr:
	if (err)
		err = nfserrno(err);
out:
	return err;
}

/*
 * Close a file.
 */
void
nfsd_close(struct file *filp)
{
	fput(filp);
}

/*
 * Sync a file
 * As this calls fsync (not fdatasync) there is no need for a write_inode
 * after it.
 */
static inline int nfsd_dosync(struct file *filp, struct dentry *dp,
			      struct file_operations *fop)
{
	struct inode *inode = dp->d_inode;
	int (*fsync) (struct file *, struct dentry *, int);
	int err = nfs_ok;

	filemap_fdatawrite(inode->i_mapping);
	if (fop && (fsync = fop->fsync))
		err=fsync(filp, dp, 0);
	filemap_fdatawait(inode->i_mapping);

	return nfserrno(err);
}
	

static int
nfsd_sync(struct file *filp)
{
        int err;
	struct inode *inode = filp->f_dentry->d_inode;
	dprintk("nfsd: sync file %s\n", filp->f_dentry->d_name.name);
	mutex_lock(&inode->i_mutex);
	err=nfsd_dosync(filp, filp->f_dentry, filp->f_op);
	mutex_unlock(&inode->i_mutex);

	return err;
}

void
nfsd_sync_dir(struct dentry *dp)
{
	nfsd_dosync(NULL, dp, dp->d_inode->i_fop);
}

/*
 * Obtain the readahead parameters for the file
 * specified by (dev, ino).
 */
static DEFINE_SPINLOCK(ra_lock);

static inline struct raparms *
nfsd_get_raparms(dev_t dev, ino_t ino)
{
	struct raparms	*ra, **rap, **frap = NULL;
	int depth = 0;

	spin_lock(&ra_lock);
	for (rap = &raparm_cache; (ra = *rap); rap = &ra->p_next) {
		if (ra->p_ino == ino && ra->p_dev == dev)
			goto found;
		depth++;
		if (ra->p_count == 0)
			frap = rap;
	}
	depth = nfsdstats.ra_size*11/10;
	if (!frap) {	
		spin_unlock(&ra_lock);
		return NULL;
	}
	rap = frap;
	ra = *frap;
	ra->p_dev = dev;
	ra->p_ino = ino;
	ra->p_set = 0;
found:
	if (rap != &raparm_cache) {
		*rap = ra->p_next;
		ra->p_next   = raparm_cache;
		raparm_cache = ra;
	}
	ra->p_count++;
	nfsdstats.ra_depth[depth*10/nfsdstats.ra_size]++;
	spin_unlock(&ra_lock);
	return ra;
}

/*
 * Grab and keep cached pages assosiated with a file in the svc_rqst
 * so that they can be passed to the netowork sendmsg/sendpage routines
 * directrly. They will be released after the sending has completed.
 */
static int
nfsd_read_actor(read_descriptor_t *desc, struct page *page, unsigned long offset , unsigned long size)
{
	unsigned long count = desc->count;
	struct svc_rqst *rqstp = desc->arg.data;

	if (size > count)
		size = count;

	if (rqstp->rq_res.page_len == 0) {
		get_page(page);
		rqstp->rq_respages[rqstp->rq_resused++] = page;
		rqstp->rq_res.page_base = offset;
		rqstp->rq_res.page_len = size;
	} else if (page != rqstp->rq_respages[rqstp->rq_resused-1]) {
		get_page(page);
		rqstp->rq_respages[rqstp->rq_resused++] = page;
		rqstp->rq_res.page_len += size;
	} else {
		rqstp->rq_res.page_len += size;
	}

	desc->count = count - size;
	desc->written += size;
	return size;
}

static inline int
nfsd_vfs_read(struct svc_rqst *rqstp, struct svc_fh *fhp, struct file *file,
              loff_t offset, struct kvec *vec, int vlen, unsigned long *count)
{
	struct inode *inode;
	struct raparms	*ra;
	mm_segment_t	oldfs;
	int		err;

	err = nfserr_perm;
	inode = file->f_dentry->d_inode;
#ifdef MSNFS
	if ((fhp->fh_export->ex_flags & NFSEXP_MSNFS) &&
		(!lock_may_read(inode, offset, *count)))
		goto out;
#endif

	/* Get readahead parameters */
	ra = nfsd_get_raparms(inode->i_sb->s_dev, inode->i_ino);

	if (ra && ra->p_set)
		file->f_ra = ra->p_ra;

	if (file->f_op->sendfile) {
		svc_pushback_unused_pages(rqstp);
		err = file->f_op->sendfile(file, &offset, *count,
						 nfsd_read_actor, rqstp);
	} else {
		oldfs = get_fs();
		set_fs(KERNEL_DS);
		err = vfs_readv(file, (struct iovec __user *)vec, vlen, &offset);
		set_fs(oldfs);
	}

	/* Write back readahead params */
	if (ra) {
		spin_lock(&ra_lock);
		ra->p_ra = file->f_ra;
		ra->p_set = 1;
		ra->p_count--;
		spin_unlock(&ra_lock);
	}

	if (err >= 0) {
		nfsdstats.io_read += err;
		*count = err;
		err = 0;
		fsnotify_access(file->f_dentry);
	} else 
		err = nfserrno(err);
out:
	return err;
}

static void kill_suid(struct dentry *dentry)
{
	struct iattr	ia;
	ia.ia_valid = ATTR_KILL_SUID | ATTR_KILL_SGID;

	mutex_lock(&dentry->d_inode->i_mutex);
	notify_change(dentry, &ia);
	mutex_unlock(&dentry->d_inode->i_mutex);
}

static inline int
nfsd_vfs_write(struct svc_rqst *rqstp, struct svc_fh *fhp, struct file *file,
				loff_t offset, struct kvec *vec, int vlen,
	   			unsigned long cnt, int *stablep)
{
	struct svc_export	*exp;
	struct dentry		*dentry;
	struct inode		*inode;
	mm_segment_t		oldfs;
	int			err = 0;
	int			stable = *stablep;

	err = nfserr_perm;

#ifdef MSNFS
	if ((fhp->fh_export->ex_flags & NFSEXP_MSNFS) &&
		(!lock_may_write(file->f_dentry->d_inode, offset, cnt)))
		goto out;
#endif

	dentry = file->f_dentry;
	inode = dentry->d_inode;
	exp   = fhp->fh_export;

	/*
	 * Request sync writes if
	 *  -	the sync export option has been set, or
	 *  -	the client requested O_SYNC behavior (NFSv3 feature).
	 *  -   The file system doesn't support fsync().
	 * When gathered writes have been configured for this volume,
	 * flushing the data to disk is handled separately below.
	 */

	if (file->f_op->fsync == 0) {/* COMMIT3 cannot work */
	       stable = 2;
	       *stablep = 2; /* FILE_SYNC */
	}

	if (!EX_ISSYNC(exp))
		stable = 0;
	if (stable && !EX_WGATHER(exp))
		file->f_flags |= O_SYNC;

	/* Write the data. */
	oldfs = get_fs(); set_fs(KERNEL_DS);
	err = vfs_writev(file, (struct iovec __user *)vec, vlen, &offset);
	set_fs(oldfs);
	if (err >= 0) {
		nfsdstats.io_write += cnt;
		fsnotify_modify(file->f_dentry);
	}

	/* clear setuid/setgid flag after write */
	if (err >= 0 && (inode->i_mode & (S_ISUID | S_ISGID)))
		kill_suid(dentry);

	if (err >= 0 && stable) {
		static ino_t	last_ino;
		static dev_t	last_dev;

		/*
		 * Gathered writes: If another process is currently
		 * writing to the file, there's a high chance
		 * this is another nfsd (triggered by a bulk write
		 * from a client's biod). Rather than syncing the
		 * file with each write request, we sleep for 10 msec.
		 *
		 * I don't know if this roughly approximates
		 * C. Juszak's idea of gathered writes, but it's a
		 * nice and simple solution (IMHO), and it seems to
		 * work:-)
		 */
		if (EX_WGATHER(exp)) {
			if (atomic_read(&inode->i_writecount) > 1
			    || (last_ino == inode->i_ino && last_dev == inode->i_sb->s_dev)) {
				dprintk("nfsd: write defer %d\n", current->pid);
				msleep(10);
				dprintk("nfsd: write resume %d\n", current->pid);
			}

			if (inode->i_state & I_DIRTY) {
				dprintk("nfsd: write sync %d\n", current->pid);
				err=nfsd_sync(file);
			}
#if 0
			wake_up(&inode->i_wait);
#endif
		}
		last_ino = inode->i_ino;
		last_dev = inode->i_sb->s_dev;
	}

	dprintk("nfsd: write complete err=%d\n", err);
	if (err >= 0)
		err = 0;
	else 
		err = nfserrno(err);
out:
	return err;
}

/*
 * Read data from a file. count must contain the requested read count
 * on entry. On return, *count contains the number of bytes actually read.
 * N.B. After this call fhp needs an fh_put
 */
int
nfsd_read(struct svc_rqst *rqstp, struct svc_fh *fhp, struct file *file,
		loff_t offset, struct kvec *vec, int vlen,
		unsigned long *count)
{
	int		err;

	if (file) {
		err = nfsd_permission(fhp->fh_export, fhp->fh_dentry,
				MAY_READ|MAY_OWNER_OVERRIDE);
		if (err)
			goto out;
		err = nfsd_vfs_read(rqstp, fhp, file, offset, vec, vlen, count);
	} else {
		err = nfsd_open(rqstp, fhp, S_IFREG, MAY_READ, &file);
		if (err)
			goto out;
		err = nfsd_vfs_read(rqstp, fhp, file, offset, vec, vlen, count);
		nfsd_close(file);
	}
out:
	return err;
}

/*
 * Write data to a file.
 * The stable flag requests synchronous writes.
 * N.B. After this call fhp needs an fh_put
 */
int
nfsd_write(struct svc_rqst *rqstp, struct svc_fh *fhp, struct file *file,
		loff_t offset, struct kvec *vec, int vlen, unsigned long cnt,
		int *stablep)
{
	int			err = 0;

	if (file) {
		err = nfsd_permission(fhp->fh_export, fhp->fh_dentry,
				MAY_WRITE|MAY_OWNER_OVERRIDE);
		if (err)
			goto out;
		err = nfsd_vfs_write(rqstp, fhp, file, offset, vec, vlen, cnt,
				stablep);
	} else {
		err = nfsd_open(rqstp, fhp, S_IFREG, MAY_WRITE, &file);
		if (err)
			goto out;

		if (cnt)
			err = nfsd_vfs_write(rqstp, fhp, file, offset, vec, vlen,
					     cnt, stablep);
		nfsd_close(file);
	}
out:
	return err;
}

#ifdef CONFIG_NFSD_V3
/*
 * Commit all pending writes to stable storage.
 * Strictly speaking, we could sync just the indicated file region here,
 * but there's currently no way we can ask the VFS to do so.
 *
 * Unfortunately we cannot lock the file to make sure we return full WCC
 * data to the client, as locking happens lower down in the filesystem.
 */
int
nfsd_commit(struct svc_rqst *rqstp, struct svc_fh *fhp,
               loff_t offset, unsigned long count)
{
	struct file	*file;
	int		err;

	if ((u64)count > ~(u64)offset)
		return nfserr_inval;

	if ((err = nfsd_open(rqstp, fhp, S_IFREG, MAY_WRITE, &file)) != 0)
		return err;
	if (EX_ISSYNC(fhp->fh_export)) {
		if (file->f_op && file->f_op->fsync) {
			err = nfsd_sync(file);
		} else {
			err = nfserr_notsupp;
		}
	}

	nfsd_close(file);
	return err;
}
#endif /* CONFIG_NFSD_V3 */

/*
 * Create a file (regular, directory, device, fifo); UNIX sockets 
 * not yet implemented.
 * If the response fh has been verified, the parent directory should
 * already be locked. Note that the parent directory is left locked.
 *
 * N.B. Every call to nfsd_create needs an fh_put for _both_ fhp and resfhp
 */
int
nfsd_create(struct svc_rqst *rqstp, struct svc_fh *fhp,
		char *fname, int flen, struct iattr *iap,
		int type, dev_t rdev, struct svc_fh *resfhp)
{
	struct dentry	*dentry, *dchild = NULL;
	struct inode	*dirp;
	int		err;

	err = nfserr_perm;
	if (!flen)
		goto out;
	err = nfserr_exist;
	if (isdotent(fname, flen))
		goto out;

	err = fh_verify(rqstp, fhp, S_IFDIR, MAY_CREATE);
	if (err)
		goto out;

	dentry = fhp->fh_dentry;
	dirp = dentry->d_inode;

	err = nfserr_notdir;
	if(!dirp->i_op || !dirp->i_op->lookup)
		goto out;
	/*
	 * Check whether the response file handle has been verified yet.
	 * If it has, the parent directory should already be locked.
	 */
	if (!resfhp->fh_dentry) {
		/* called from nfsd_proc_mkdir, or possibly nfsd3_proc_create */
		fh_lock(fhp);
		dchild = lookup_one_len(fname, dentry, flen);
		err = PTR_ERR(dchild);
		if (IS_ERR(dchild))
			goto out_nfserr;
		err = fh_compose(resfhp, fhp->fh_export, dchild, fhp);
		if (err)
			goto out;
	} else {
		/* called from nfsd_proc_create */
		dchild = dget(resfhp->fh_dentry);
		if (!fhp->fh_locked) {
			/* not actually possible */
			printk(KERN_ERR
				"nfsd_create: parent %s/%s not locked!\n",
				dentry->d_parent->d_name.name,
				dentry->d_name.name);
			err = -EIO;
			goto out;
		}
	}
	/*
	 * Make sure the child dentry is still negative ...
	 */
	err = nfserr_exist;
	if (dchild->d_inode) {
		dprintk("nfsd_create: dentry %s/%s not negative!\n",
			dentry->d_name.name, dchild->d_name.name);
		goto out; 
	}

	if (!(iap->ia_valid & ATTR_MODE))
		iap->ia_mode = 0;
	iap->ia_mode = (iap->ia_mode & S_IALLUGO) | type;

	/*
	 * Get the dir op function pointer.
	 */
	err = nfserr_perm;
	switch (type) {
	case S_IFREG:
		err = vfs_create(dirp, dchild, iap->ia_mode, NULL);
		break;
	case S_IFDIR:
		err = vfs_mkdir(dirp, dchild, iap->ia_mode);
		break;
	case S_IFCHR:
	case S_IFBLK:
	case S_IFIFO:
	case S_IFSOCK:
		err = vfs_mknod(dirp, dchild, iap->ia_mode, rdev);
		break;
	default:
	        printk("nfsd: bad file type %o in nfsd_create\n", type);
		err = -EINVAL;
	}
	if (err < 0)
		goto out_nfserr;

	if (EX_ISSYNC(fhp->fh_export)) {
		nfsd_sync_dir(dentry);
		write_inode_now(dchild->d_inode, 1);
	}


	/* Set file attributes. Mode has already been set and
	 * setting uid/gid works only for root. Irix appears to
	 * send along the gid when it tries to implement setgid
	 * directories via NFS.
	 */
	err = 0;
	if ((iap->ia_valid &= ~(ATTR_UID|ATTR_GID|ATTR_MODE)) != 0)
		err = nfsd_setattr(rqstp, resfhp, iap, 0, (time_t)0);
	/*
	 * Update the file handle to get the new inode info.
	 */
	if (!err)
		err = fh_update(resfhp);
out:
	if (dchild && !IS_ERR(dchild))
		dput(dchild);
	return err;

out_nfserr:
	err = nfserrno(err);
	goto out;
}

#ifdef CONFIG_NFSD_V3
/*
 * NFSv3 version of nfsd_create
 */
int
nfsd_create_v3(struct svc_rqst *rqstp, struct svc_fh *fhp,
		char *fname, int flen, struct iattr *iap,
		struct svc_fh *resfhp, int createmode, u32 *verifier,
	        int *truncp)
{
	struct dentry	*dentry, *dchild = NULL;
	struct inode	*dirp;
	int		err;
	__u32		v_mtime=0, v_atime=0;
	int		v_mode=0;

	err = nfserr_perm;
	if (!flen)
		goto out;
	err = nfserr_exist;
	if (isdotent(fname, flen))
		goto out;
	if (!(iap->ia_valid & ATTR_MODE))
		iap->ia_mode = 0;
	err = fh_verify(rqstp, fhp, S_IFDIR, MAY_CREATE);
	if (err)
		goto out;

	dentry = fhp->fh_dentry;
	dirp = dentry->d_inode;

	/* Get all the sanity checks out of the way before
	 * we lock the parent. */
	err = nfserr_notdir;
	if(!dirp->i_op || !dirp->i_op->lookup)
		goto out;
	fh_lock(fhp);

	/*
	 * Compose the response file handle.
	 */
	dchild = lookup_one_len(fname, dentry, flen);
	err = PTR_ERR(dchild);
	if (IS_ERR(dchild))
		goto out_nfserr;

	err = fh_compose(resfhp, fhp->fh_export, dchild, fhp);
	if (err)
		goto out;

	if (createmode == NFS3_CREATE_EXCLUSIVE) {
		/* while the verifier would fit in mtime+atime,
		 * solaris7 gets confused (bugid 4218508) if these have
		 * the high bit set, so we use the mode as well
		 */
		v_mtime = verifier[0]&0x7fffffff;
		v_atime = verifier[1]&0x7fffffff;
		v_mode  = S_IFREG
			| ((verifier[0]&0x80000000) >> (32-7)) /* u+x */
			| ((verifier[1]&0x80000000) >> (32-9)) /* u+r */
			;
	}
	
	if (dchild->d_inode) {
		err = 0;

		switch (createmode) {
		case NFS3_CREATE_UNCHECKED:
			if (! S_ISREG(dchild->d_inode->i_mode))
				err = nfserr_exist;
			else if (truncp) {
				/* in nfsv4, we need to treat this case a little
				 * differently.  we don't want to truncate the
				 * file now; this would be wrong if the OPEN
				 * fails for some other reason.  furthermore,
				 * if the size is nonzero, we should ignore it
				 * according to spec!
				 */
				*truncp = (iap->ia_valid & ATTR_SIZE) && !iap->ia_size;
			}
			else {
				iap->ia_valid &= ATTR_SIZE;
				goto set_attr;
			}
			break;
		case NFS3_CREATE_EXCLUSIVE:
			if (   dchild->d_inode->i_mtime.tv_sec == v_mtime
			    && dchild->d_inode->i_atime.tv_sec == v_atime
			    && dchild->d_inode->i_mode  == v_mode
			    && dchild->d_inode->i_size  == 0 )
				break;
			 /* fallthru */
		case NFS3_CREATE_GUARDED:
			err = nfserr_exist;
		}
		goto out;
	}

	err = vfs_create(dirp, dchild, iap->ia_mode, NULL);
	if (err < 0)
		goto out_nfserr;

	if (EX_ISSYNC(fhp->fh_export)) {
		nfsd_sync_dir(dentry);
		/* setattr will sync the child (or not) */
	}

	/*
	 * Update the filehandle to get the new inode info.
	 */
	err = fh_update(resfhp);
	if (err)
		goto out;

	if (createmode == NFS3_CREATE_EXCLUSIVE) {
		/* Cram the verifier into atime/mtime/mode */
		iap->ia_valid = ATTR_MTIME|ATTR_ATIME
			| ATTR_MTIME_SET|ATTR_ATIME_SET
			| ATTR_MODE;
		/* XXX someone who knows this better please fix it for nsec */ 
		iap->ia_mtime.tv_sec = v_mtime;
		iap->ia_atime.tv_sec = v_atime;
		iap->ia_mtime.tv_nsec = 0;
		iap->ia_atime.tv_nsec = 0;
		iap->ia_mode  = v_mode;
	}

	/* Set file attributes.
	 * Mode has already been set but we might need to reset it
	 * for CREATE_EXCLUSIVE
	 * Irix appears to send along the gid when it tries to
	 * implement setgid directories via NFS. Clear out all that cruft.
	 */
 set_attr:
	if ((iap->ia_valid &= ~(ATTR_UID|ATTR_GID)) != 0)
 		err = nfsd_setattr(rqstp, resfhp, iap, 0, (time_t)0);

 out:
	fh_unlock(fhp);
	if (dchild && !IS_ERR(dchild))
		dput(dchild);
 	return err;
 
 out_nfserr:
	err = nfserrno(err);
	goto out;
}
#endif /* CONFIG_NFSD_V3 */

/*
 * Read a symlink. On entry, *lenp must contain the maximum path length that
 * fits into the buffer. On return, it contains the true length.
 * N.B. After this call fhp needs an fh_put
 */
int
nfsd_readlink(struct svc_rqst *rqstp, struct svc_fh *fhp, char *buf, int *lenp)
{
	struct dentry	*dentry;
	struct inode	*inode;
	mm_segment_t	oldfs;
	int		err;

	err = fh_verify(rqstp, fhp, S_IFLNK, MAY_NOP);
	if (err)
		goto out;

	dentry = fhp->fh_dentry;
	inode = dentry->d_inode;

	err = nfserr_inval;
	if (!inode->i_op || !inode->i_op->readlink)
		goto out;

	touch_atime(fhp->fh_export->ex_mnt, dentry);
	/* N.B. Why does this call need a get_fs()??
	 * Remove the set_fs and watch the fireworks:-) --okir
	 */

	oldfs = get_fs(); set_fs(KERNEL_DS);
	err = inode->i_op->readlink(dentry, buf, *lenp);
	set_fs(oldfs);

	if (err < 0)
		goto out_nfserr;
	*lenp = err;
	err = 0;
out:
	return err;

out_nfserr:
	err = nfserrno(err);
	goto out;
}

/*
 * Create a symlink and look up its inode
 * N.B. After this call _both_ fhp and resfhp need an fh_put
 */
int
nfsd_symlink(struct svc_rqst *rqstp, struct svc_fh *fhp,
				char *fname, int flen,
				char *path,  int plen,
				struct svc_fh *resfhp,
				struct iattr *iap)
{
	struct dentry	*dentry, *dnew;
	int		err, cerr;
	umode_t		mode;

	err = nfserr_noent;
	if (!flen || !plen)
		goto out;
	err = nfserr_exist;
	if (isdotent(fname, flen))
		goto out;

	err = fh_verify(rqstp, fhp, S_IFDIR, MAY_CREATE);
	if (err)
		goto out;
	fh_lock(fhp);
	dentry = fhp->fh_dentry;
	dnew = lookup_one_len(fname, dentry, flen);
	err = PTR_ERR(dnew);
	if (IS_ERR(dnew))
		goto out_nfserr;

	mode = S_IALLUGO;
	/* Only the MODE ATTRibute is even vaguely meaningful */
	if (iap && (iap->ia_valid & ATTR_MODE))
		mode = iap->ia_mode & S_IALLUGO;

	if (unlikely(path[plen] != 0)) {
		char *path_alloced = kmalloc(plen+1, GFP_KERNEL);
		if (path_alloced == NULL)
			err = -ENOMEM;
		else {
			strncpy(path_alloced, path, plen);
			path_alloced[plen] = 0;
			err = vfs_symlink(dentry->d_inode, dnew, path_alloced, mode);
			kfree(path_alloced);
		}
	} else
		err = vfs_symlink(dentry->d_inode, dnew, path, mode);

	if (!err) {
		if (EX_ISSYNC(fhp->fh_export))
			nfsd_sync_dir(dentry);
	} else
		err = nfserrno(err);
	fh_unlock(fhp);

	cerr = fh_compose(resfhp, fhp->fh_export, dnew, fhp);
	dput(dnew);
	if (err==0) err = cerr;
out:
	return err;

out_nfserr:
	err = nfserrno(err);
	goto out;
}

/*
 * Create a hardlink
 * N.B. After this call _both_ ffhp and tfhp need an fh_put
 */
int
nfsd_link(struct svc_rqst *rqstp, struct svc_fh *ffhp,
				char *name, int len, struct svc_fh *tfhp)
{
	struct dentry	*ddir, *dnew, *dold;
	struct inode	*dirp, *dest;
	int		err;

	err = fh_verify(rqstp, ffhp, S_IFDIR, MAY_CREATE);
	if (err)
		goto out;
	err = fh_verify(rqstp, tfhp, -S_IFDIR, MAY_NOP);
	if (err)
		goto out;

	err = nfserr_perm;
	if (!len)
		goto out;
	err = nfserr_exist;
	if (isdotent(name, len))
		goto out;

	fh_lock(ffhp);
	ddir = ffhp->fh_dentry;
	dirp = ddir->d_inode;

	dnew = lookup_one_len(name, ddir, len);
	err = PTR_ERR(dnew);
	if (IS_ERR(dnew))
		goto out_nfserr;

	dold = tfhp->fh_dentry;
	dest = dold->d_inode;

	err = vfs_link(dold, dirp, dnew);
	if (!err) {
		if (EX_ISSYNC(ffhp->fh_export)) {
			nfsd_sync_dir(ddir);
			write_inode_now(dest, 1);
		}
	} else {
		if (err == -EXDEV && rqstp->rq_vers == 2)
			err = nfserr_acces;
		else
			err = nfserrno(err);
	}

	fh_unlock(ffhp);
	dput(dnew);
out:
	return err;

out_nfserr:
	err = nfserrno(err);
	goto out;
}

/*
 * Rename a file
 * N.B. After this call _both_ ffhp and tfhp need an fh_put
 */
int
nfsd_rename(struct svc_rqst *rqstp, struct svc_fh *ffhp, char *fname, int flen,
			    struct svc_fh *tfhp, char *tname, int tlen)
{
	struct dentry	*fdentry, *tdentry, *odentry, *ndentry, *trap;
	struct inode	*fdir, *tdir;
	int		err;

	err = fh_verify(rqstp, ffhp, S_IFDIR, MAY_REMOVE);
	if (err)
		goto out;
	err = fh_verify(rqstp, tfhp, S_IFDIR, MAY_CREATE);
	if (err)
		goto out;

	fdentry = ffhp->fh_dentry;
	fdir = fdentry->d_inode;

	tdentry = tfhp->fh_dentry;
	tdir = tdentry->d_inode;

	err = (rqstp->rq_vers == 2) ? nfserr_acces : nfserr_xdev;
	if (fdir->i_sb != tdir->i_sb)
		goto out;

	err = nfserr_perm;
	if (!flen || isdotent(fname, flen) || !tlen || isdotent(tname, tlen))
		goto out;

	/* cannot use fh_lock as we need deadlock protective ordering
	 * so do it by hand */
	trap = lock_rename(tdentry, fdentry);
	ffhp->fh_locked = tfhp->fh_locked = 1;
	fill_pre_wcc(ffhp);
	fill_pre_wcc(tfhp);

	odentry = lookup_one_len(fname, fdentry, flen);
	err = PTR_ERR(odentry);
	if (IS_ERR(odentry))
		goto out_nfserr;

	err = -ENOENT;
	if (!odentry->d_inode)
		goto out_dput_old;
	err = -EINVAL;
	if (odentry == trap)
		goto out_dput_old;

	ndentry = lookup_one_len(tname, tdentry, tlen);
	err = PTR_ERR(ndentry);
	if (IS_ERR(ndentry))
		goto out_dput_old;
	err = -ENOTEMPTY;
	if (ndentry == trap)
		goto out_dput_new;

#ifdef MSNFS
	if ((ffhp->fh_export->ex_flags & NFSEXP_MSNFS) &&
		((atomic_read(&odentry->d_count) > 1)
		 || (atomic_read(&ndentry->d_count) > 1))) {
			err = nfserr_perm;
	} else
#endif
	err = vfs_rename(fdir, odentry, tdir, ndentry);
	if (!err && EX_ISSYNC(tfhp->fh_export)) {
		nfsd_sync_dir(tdentry);
		nfsd_sync_dir(fdentry);
	}

 out_dput_new:
	dput(ndentry);
 out_dput_old:
	dput(odentry);
 out_nfserr:
	if (err)
		err = nfserrno(err);

	/* we cannot reply on fh_unlock on the two filehandles,
	 * as that would do the wrong thing if the two directories
	 * were the same, so again we do it by hand
	 */
	fill_post_wcc(ffhp);
	fill_post_wcc(tfhp);
	unlock_rename(tdentry, fdentry);
	ffhp->fh_locked = tfhp->fh_locked = 0;

out:
	return err;
}

/*
 * Unlink a file or directory
 * N.B. After this call fhp needs an fh_put
 */
int
nfsd_unlink(struct svc_rqst *rqstp, struct svc_fh *fhp, int type,
				char *fname, int flen)
{
	struct dentry	*dentry, *rdentry;
	struct inode	*dirp;
	int		err;

	err = nfserr_acces;
	if (!flen || isdotent(fname, flen))
		goto out;
	err = fh_verify(rqstp, fhp, S_IFDIR, MAY_REMOVE);
	if (err)
		goto out;

	fh_lock(fhp);
	dentry = fhp->fh_dentry;
	dirp = dentry->d_inode;

	rdentry = lookup_one_len(fname, dentry, flen);
	err = PTR_ERR(rdentry);
	if (IS_ERR(rdentry))
		goto out_nfserr;

	if (!rdentry->d_inode) {
		dput(rdentry);
		err = nfserr_noent;
		goto out;
	}

	if (!type)
		type = rdentry->d_inode->i_mode & S_IFMT;

	if (type != S_IFDIR) { /* It's UNLINK */
#ifdef MSNFS
		if ((fhp->fh_export->ex_flags & NFSEXP_MSNFS) &&
			(atomic_read(&rdentry->d_count) > 1)) {
			err = nfserr_perm;
		} else
#endif
		err = vfs_unlink(dirp, rdentry);
	} else { /* It's RMDIR */
		err = vfs_rmdir(dirp, rdentry);
	}

	dput(rdentry);

	if (err)
		goto out_nfserr;
	if (EX_ISSYNC(fhp->fh_export)) 
		nfsd_sync_dir(dentry);

out:
	return err;

out_nfserr:
	err = nfserrno(err);
	goto out;
}

/*
 * Read entries from a directory.
 * The  NFSv3/4 verifier we ignore for now.
 */
int
nfsd_readdir(struct svc_rqst *rqstp, struct svc_fh *fhp, loff_t *offsetp, 
	     struct readdir_cd *cdp, encode_dent_fn func)
{
	int		err;
	struct file	*file;
	loff_t		offset = *offsetp;

	err = nfsd_open(rqstp, fhp, S_IFDIR, MAY_READ, &file);
	if (err)
		goto out;

	offset = vfs_llseek(file, offset, 0);
	if (offset < 0) {
		err = nfserrno((int)offset);
		goto out_close;
	}

	/*
	 * Read the directory entries. This silly loop is necessary because
	 * readdir() is not guaranteed to fill up the entire buffer, but
	 * may choose to do less.
	 */

	do {
		cdp->err = nfserr_eof; /* will be cleared on successful read */
		err = vfs_readdir(file, (filldir_t) func, cdp);
	} while (err >=0 && cdp->err == nfs_ok);
	if (err)
		err = nfserrno(err);
	else
		err = cdp->err;
	*offsetp = vfs_llseek(file, 0, 1);

	if (err == nfserr_eof || err == nfserr_toosmall)
		err = nfs_ok; /* can still be found in ->err */
out_close:
	nfsd_close(file);
out:
	return err;
}

/*
 * Get file system stats
 * N.B. After this call fhp needs an fh_put
 */
int
nfsd_statfs(struct svc_rqst *rqstp, struct svc_fh *fhp, struct kstatfs *stat)
{
	int err = fh_verify(rqstp, fhp, 0, MAY_NOP);
	if (!err && vfs_statfs(fhp->fh_dentry->d_inode->i_sb,stat))
		err = nfserr_io;
	return err;
}

/*
 * Check for a user's access permissions to this inode.
 */
int
nfsd_permission(struct svc_export *exp, struct dentry *dentry, int acc)
{
	struct inode	*inode = dentry->d_inode;
	int		err;

	if (acc == MAY_NOP)
		return 0;
#if 0
	dprintk("nfsd: permission 0x%x%s%s%s%s%s%s%s mode 0%o%s%s%s\n",
		acc,
		(acc & MAY_READ)?	" read"  : "",
		(acc & MAY_WRITE)?	" write" : "",
		(acc & MAY_EXEC)?	" exec"  : "",
		(acc & MAY_SATTR)?	" sattr" : "",
		(acc & MAY_TRUNC)?	" trunc" : "",
		(acc & MAY_LOCK)?	" lock"  : "",
		(acc & MAY_OWNER_OVERRIDE)? " owneroverride" : "",
		inode->i_mode,
		IS_IMMUTABLE(inode)?	" immut" : "",
		IS_APPEND(inode)?	" append" : "",
		IS_RDONLY(inode)?	" ro" : "");
	dprintk("      owner %d/%d user %d/%d\n",
		inode->i_uid, inode->i_gid, current->fsuid, current->fsgid);
#endif

	/* Normally we reject any write/sattr etc access on a read-only file
	 * system.  But if it is IRIX doing check on write-access for a 
	 * device special file, we ignore rofs.
	 */
	if (!(acc & MAY_LOCAL_ACCESS))
		if (acc & (MAY_WRITE | MAY_SATTR | MAY_TRUNC)) {
			if (EX_RDONLY(exp) || IS_RDONLY(inode))
				return nfserr_rofs;
			if (/* (acc & MAY_WRITE) && */ IS_IMMUTABLE(inode))
				return nfserr_perm;
		}
	if ((acc & MAY_TRUNC) && IS_APPEND(inode))
		return nfserr_perm;

	if (acc & MAY_LOCK) {
		/* If we cannot rely on authentication in NLM requests,
		 * just allow locks, otherwise require read permission, or
		 * ownership
		 */
		if (exp->ex_flags & NFSEXP_NOAUTHNLM)
			return 0;
		else
			acc = MAY_READ | MAY_OWNER_OVERRIDE;
	}
	/*
	 * The file owner always gets access permission for accesses that
	 * would normally be checked at open time. This is to make
	 * file access work even when the client has done a fchmod(fd, 0).
	 *
	 * However, `cp foo bar' should fail nevertheless when bar is
	 * readonly. A sensible way to do this might be to reject all
	 * attempts to truncate a read-only file, because a creat() call
	 * always implies file truncation.
	 * ... but this isn't really fair.  A process may reasonably call
	 * ftruncate on an open file descriptor on a file with perm 000.
	 * We must trust the client to do permission checking - using "ACCESS"
	 * with NFSv3.
	 */
	if ((acc & MAY_OWNER_OVERRIDE) &&
	    inode->i_uid == current->fsuid)
		return 0;

	err = permission(inode, acc & (MAY_READ|MAY_WRITE|MAY_EXEC), NULL);

	/* Allow read access to binaries even when mode 111 */
	if (err == -EACCES && S_ISREG(inode->i_mode) &&
	    acc == (MAY_READ | MAY_OWNER_OVERRIDE))
		err = permission(inode, MAY_EXEC, NULL);

	return err? nfserrno(err) : 0;
}

void
nfsd_racache_shutdown(void)
{
	if (!raparm_cache)
		return;
	dprintk("nfsd: freeing readahead buffers.\n");
	kfree(raparml);
	raparm_cache = raparml = NULL;
}
/*
 * Initialize readahead param cache
 */
int
nfsd_racache_init(int cache_size)
{
	int	i;

	if (raparm_cache)
		return 0;
	raparml = kmalloc(sizeof(struct raparms) * cache_size, GFP_KERNEL);

	if (raparml != NULL) {
		dprintk("nfsd: allocating %d readahead buffers.\n",
			cache_size);
		memset(raparml, 0, sizeof(struct raparms) * cache_size);
		for (i = 0; i < cache_size - 1; i++) {
			raparml[i].p_next = raparml + i + 1;
		}
		raparm_cache = raparml;
	} else {
		printk(KERN_WARNING
		       "nfsd: Could not allocate memory read-ahead cache.\n");
		return -ENOMEM;
	}
	nfsdstats.ra_size = cache_size;
	return 0;
}

#if defined(CONFIG_NFSD_V2_ACL) || defined(CONFIG_NFSD_V3_ACL)
struct posix_acl *
nfsd_get_posix_acl(struct svc_fh *fhp, int type)
{
	struct inode *inode = fhp->fh_dentry->d_inode;
	char *name;
	void *value = NULL;
	ssize_t size;
	struct posix_acl *acl;

	if (!IS_POSIXACL(inode))
		return ERR_PTR(-EOPNOTSUPP);

	switch (type) {
	case ACL_TYPE_ACCESS:
		name = POSIX_ACL_XATTR_ACCESS;
		break;
	case ACL_TYPE_DEFAULT:
		name = POSIX_ACL_XATTR_DEFAULT;
		break;
	default:
		return ERR_PTR(-EOPNOTSUPP);
	}

	size = nfsd_getxattr(fhp->fh_dentry, name, &value);
	if (size < 0)
		return ERR_PTR(size);

	acl = posix_acl_from_xattr(value, size);
	kfree(value);
	return acl;
}

int
nfsd_set_posix_acl(struct svc_fh *fhp, int type, struct posix_acl *acl)
{
	struct inode *inode = fhp->fh_dentry->d_inode;
	char *name;
	void *value = NULL;
	size_t size;
	int error;

	if (!IS_POSIXACL(inode) || !inode->i_op ||
	    !inode->i_op->setxattr || !inode->i_op->removexattr)
		return -EOPNOTSUPP;
	switch(type) {
		case ACL_TYPE_ACCESS:
			name = POSIX_ACL_XATTR_ACCESS;
			break;
		case ACL_TYPE_DEFAULT:
			name = POSIX_ACL_XATTR_DEFAULT;
			break;
		default:
			return -EOPNOTSUPP;
	}

	if (acl && acl->a_count) {
		size = posix_acl_xattr_size(acl->a_count);
		value = kmalloc(size, GFP_KERNEL);
		if (!value)
			return -ENOMEM;
		size = posix_acl_to_xattr(acl, value, size);
		if (size < 0) {
			error = size;
			goto getout;
		}
	} else
		size = 0;

	if (size)
		error = vfs_setxattr(fhp->fh_dentry, name, value, size, 0);
	else {
		if (!S_ISDIR(inode->i_mode) && type == ACL_TYPE_DEFAULT)
			error = 0;
		else {
			error = vfs_removexattr(fhp->fh_dentry, name);
			if (error == -ENODATA)
				error = 0;
		}
	}

getout:
	kfree(value);
	return error;
}
#endif  /* defined(CONFIG_NFSD_V2_ACL) || defined(CONFIG_NFSD_V3_ACL) */
