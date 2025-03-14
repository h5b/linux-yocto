/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * dlmcommon.h
 *
 * Copyright (C) 2004 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 *
 */

#ifndef DLMCOMMON_H
#define DLMCOMMON_H

#include <linux/kref.h>

#define DLM_HB_NODE_DOWN_PRI     (0xf000000)
#define DLM_HB_NODE_UP_PRI       (0x8000000)

#define DLM_LOCKID_NAME_MAX    32

#define DLM_DOMAIN_NAME_MAX_LEN    255
#define DLM_LOCK_RES_OWNER_UNKNOWN     O2NM_MAX_NODES
#define DLM_THREAD_SHUFFLE_INTERVAL    5     // flush everything every 5 passes
#define DLM_THREAD_MS                  200   // flush at least every 200 ms

#define DLM_HASH_BITS     7
#define DLM_HASH_SIZE     (1 << DLM_HASH_BITS)
#define DLM_HASH_MASK     (DLM_HASH_SIZE - 1)

enum dlm_ast_type {
	DLM_AST = 0,
	DLM_BAST,
	DLM_ASTUNLOCK
};


#define LKM_VALID_FLAGS (LKM_VALBLK | LKM_CONVERT | LKM_UNLOCK | \
			 LKM_CANCEL | LKM_INVVALBLK | LKM_FORCE | \
			 LKM_RECOVERY | LKM_LOCAL | LKM_NOQUEUE)

#define DLM_RECOVERY_LOCK_NAME       "$RECOVERY"
#define DLM_RECOVERY_LOCK_NAME_LEN   9

static inline int dlm_is_recovery_lock(const char *lock_name, int name_len)
{
	if (name_len == DLM_RECOVERY_LOCK_NAME_LEN &&
	    memcmp(lock_name, DLM_RECOVERY_LOCK_NAME, name_len)==0)
		return 1;
	return 0;
}

#define DLM_RECO_STATE_ACTIVE  0x0001

struct dlm_recovery_ctxt
{
	struct list_head resources;
	struct list_head received;
	struct list_head node_data;
	u8  new_master;
	u8  dead_node;
	u16 state;
	unsigned long node_map[BITS_TO_LONGS(O2NM_MAX_NODES)];
	wait_queue_head_t event;
};

enum dlm_ctxt_state {
	DLM_CTXT_NEW = 0,
	DLM_CTXT_JOINED,
	DLM_CTXT_IN_SHUTDOWN,
	DLM_CTXT_LEAVING,
};

struct dlm_ctxt
{
	struct list_head list;
	struct list_head *resources;
	struct list_head dirty_list;
	struct list_head purge_list;
	struct list_head pending_asts;
	struct list_head pending_basts;
	unsigned int purge_count;
	spinlock_t spinlock;
	spinlock_t ast_lock;
	char *name;
	u8 node_num;
	u32 key;
	u8  joining_node;
	wait_queue_head_t dlm_join_events;
	unsigned long live_nodes_map[BITS_TO_LONGS(O2NM_MAX_NODES)];
	unsigned long domain_map[BITS_TO_LONGS(O2NM_MAX_NODES)];
	unsigned long recovery_map[BITS_TO_LONGS(O2NM_MAX_NODES)];
	struct dlm_recovery_ctxt reco;
	spinlock_t master_lock;
	struct list_head master_list;
	struct list_head mle_hb_events;

	/* these give a really vague idea of the system load */
	atomic_t local_resources;
	atomic_t remote_resources;
	atomic_t unknown_resources;

	/* NOTE: Next three are protected by dlm_domain_lock */
	struct kref dlm_refs;
	enum dlm_ctxt_state dlm_state;
	unsigned int num_joins;

	struct o2hb_callback_func dlm_hb_up;
	struct o2hb_callback_func dlm_hb_down;
	struct task_struct *dlm_thread_task;
	struct task_struct *dlm_reco_thread_task;
	wait_queue_head_t dlm_thread_wq;
	wait_queue_head_t dlm_reco_thread_wq;
	wait_queue_head_t ast_wq;
	wait_queue_head_t migration_wq;

	struct work_struct dispatched_work;
	struct list_head work_list;
	spinlock_t work_lock;
	struct list_head dlm_domain_handlers;
	struct list_head	dlm_eviction_callbacks;
};

/* these keventd work queue items are for less-frequently
 * called functions that cannot be directly called from the
 * net message handlers for some reason, usually because
 * they need to send net messages of their own. */
void dlm_dispatch_work(void *data);

struct dlm_lock_resource;
struct dlm_work_item;

typedef void (dlm_workfunc_t)(struct dlm_work_item *, void *);

struct dlm_request_all_locks_priv
{
	u8 reco_master;
	u8 dead_node;
};

struct dlm_mig_lockres_priv
{
	struct dlm_lock_resource *lockres;
	u8 real_master;
};

struct dlm_assert_master_priv
{
	struct dlm_lock_resource *lockres;
	u8 request_from;
	u32 flags;
	unsigned ignore_higher:1;
};


struct dlm_work_item
{
	struct list_head list;
	dlm_workfunc_t *func;
	struct dlm_ctxt *dlm;
	void *data;
	union {
		struct dlm_request_all_locks_priv ral;
		struct dlm_mig_lockres_priv ml;
		struct dlm_assert_master_priv am;
	} u;
};

static inline void dlm_init_work_item(struct dlm_ctxt *dlm,
				      struct dlm_work_item *i,
				      dlm_workfunc_t *f, void *data)
{
	memset(i, 0, sizeof(*i));
	i->func = f;
	INIT_LIST_HEAD(&i->list);
	i->data = data;
	i->dlm = dlm;  /* must have already done a dlm_grab on this! */
}



static inline void __dlm_set_joining_node(struct dlm_ctxt *dlm,
					  u8 node)
{
	assert_spin_locked(&dlm->spinlock);

	dlm->joining_node = node;
	wake_up(&dlm->dlm_join_events);
}

#define DLM_LOCK_RES_UNINITED             0x00000001
#define DLM_LOCK_RES_RECOVERING           0x00000002
#define DLM_LOCK_RES_READY                0x00000004
#define DLM_LOCK_RES_DIRTY                0x00000008
#define DLM_LOCK_RES_IN_PROGRESS          0x00000010
#define DLM_LOCK_RES_MIGRATING            0x00000020

#define DLM_PURGE_INTERVAL_MS   (8 * 1000)

struct dlm_lock_resource
{
	/* WARNING: Please see the comment in dlm_init_lockres before
	 * adding fields here. */
	struct list_head list;
	struct kref      refs;

	/* please keep these next 3 in this order
	 * some funcs want to iterate over all lists */
	struct list_head granted;
	struct list_head converting;
	struct list_head blocked;

	struct list_head dirty;
	struct list_head recovering; // dlm_recovery_ctxt.resources list

	/* unused lock resources have their last_used stamped and are
	 * put on a list for the dlm thread to run. */
	struct list_head purge;
	unsigned long    last_used;

	unsigned migration_pending:1;
	atomic_t asts_reserved;
	spinlock_t spinlock;
	wait_queue_head_t wq;
	u8  owner;              //node which owns the lock resource, or unknown
	u16 state;
	struct qstr lockname;
	char lvb[DLM_LVB_LEN];
};

struct dlm_migratable_lock
{
	__be64 cookie;

	/* these 3 are just padding for the in-memory structure, but
	 * list and flags are actually used when sent over the wire */
	__be16 pad1;
	u8 list;  // 0=granted, 1=converting, 2=blocked
	u8 flags;

	s8 type;
	s8 convert_type;
	s8 highest_blocked;
	u8 node;
};  // 16 bytes

struct dlm_lock
{
	struct dlm_migratable_lock ml;

	struct list_head list;
	struct list_head ast_list;
	struct list_head bast_list;
	struct dlm_lock_resource *lockres;
	spinlock_t spinlock;
	struct kref lock_refs;

	// ast and bast must be callable while holding a spinlock!
	dlm_astlockfunc_t *ast;
	dlm_bastlockfunc_t *bast;
	void *astdata;
	struct dlm_lockstatus *lksb;
	unsigned ast_pending:1,
		 bast_pending:1,
		 convert_pending:1,
		 lock_pending:1,
		 cancel_pending:1,
		 unlock_pending:1,
		 lksb_kernel_allocated:1;
};


#define DLM_LKSB_UNUSED1           0x01
#define DLM_LKSB_PUT_LVB           0x02
#define DLM_LKSB_GET_LVB           0x04
#define DLM_LKSB_UNUSED2           0x08
#define DLM_LKSB_UNUSED3           0x10
#define DLM_LKSB_UNUSED4           0x20
#define DLM_LKSB_UNUSED5           0x40
#define DLM_LKSB_UNUSED6           0x80


enum dlm_lockres_list {
	DLM_GRANTED_LIST = 0,
	DLM_CONVERTING_LIST,
	DLM_BLOCKED_LIST
};

static inline struct list_head *
dlm_list_idx_to_ptr(struct dlm_lock_resource *res, enum dlm_lockres_list idx)
{
	struct list_head *ret = NULL;
	if (idx == DLM_GRANTED_LIST)
		ret = &res->granted;
	else if (idx == DLM_CONVERTING_LIST)
		ret = &res->converting;
	else if (idx == DLM_BLOCKED_LIST)
		ret = &res->blocked;
	else
		BUG();
	return ret;
}




struct dlm_node_iter
{
	unsigned long node_map[BITS_TO_LONGS(O2NM_MAX_NODES)];
	int curnode;
};


enum {
	DLM_MASTER_REQUEST_MSG    = 500,
	DLM_UNUSED_MSG1,         /* 501 */
	DLM_ASSERT_MASTER_MSG,	 /* 502 */
	DLM_CREATE_LOCK_MSG,	 /* 503 */
	DLM_CONVERT_LOCK_MSG,	 /* 504 */
	DLM_PROXY_AST_MSG,	 /* 505 */
	DLM_UNLOCK_LOCK_MSG,	 /* 506 */
	DLM_UNUSED_MSG2,	 /* 507 */
	DLM_MIGRATE_REQUEST_MSG, /* 508 */
	DLM_MIG_LOCKRES_MSG, 	 /* 509 */
	DLM_QUERY_JOIN_MSG,	 /* 510 */
	DLM_ASSERT_JOINED_MSG,	 /* 511 */
	DLM_CANCEL_JOIN_MSG,	 /* 512 */
	DLM_EXIT_DOMAIN_MSG,	 /* 513 */
	DLM_MASTER_REQUERY_MSG,	 /* 514 */
	DLM_LOCK_REQUEST_MSG,	 /* 515 */
	DLM_RECO_DATA_DONE_MSG,	 /* 516 */
	DLM_BEGIN_RECO_MSG,	 /* 517 */
	DLM_FINALIZE_RECO_MSG	 /* 518 */
};

struct dlm_reco_node_data
{
	int state;
	u8 node_num;
	struct list_head list;
};

enum {
	DLM_RECO_NODE_DATA_DEAD = -1,
	DLM_RECO_NODE_DATA_INIT = 0,
	DLM_RECO_NODE_DATA_REQUESTING,
	DLM_RECO_NODE_DATA_REQUESTED,
	DLM_RECO_NODE_DATA_RECEIVING,
	DLM_RECO_NODE_DATA_DONE,
	DLM_RECO_NODE_DATA_FINALIZE_SENT,
};


enum {
	DLM_MASTER_RESP_NO = 0,
	DLM_MASTER_RESP_YES,
	DLM_MASTER_RESP_MAYBE,
	DLM_MASTER_RESP_ERROR
};


struct dlm_master_request
{
	u8 node_idx;
	u8 namelen;
	__be16 pad1;
	__be32 flags;

	u8 name[O2NM_MAX_NAME_LEN];
};

#define DLM_ASSERT_MASTER_MLE_CLEANUP      0x00000001
#define DLM_ASSERT_MASTER_REQUERY          0x00000002
#define DLM_ASSERT_MASTER_FINISH_MIGRATION 0x00000004
struct dlm_assert_master
{
	u8 node_idx;
	u8 namelen;
	__be16 pad1;
	__be32 flags;

	u8 name[O2NM_MAX_NAME_LEN];
};

struct dlm_migrate_request
{
	u8 master;
	u8 new_master;
	u8 namelen;
	u8 pad1;
	__be32 pad2;
	u8 name[O2NM_MAX_NAME_LEN];
};

struct dlm_master_requery
{
	u8 pad1;
	u8 pad2;
	u8 node_idx;
	u8 namelen;
	__be32 pad3;
	u8 name[O2NM_MAX_NAME_LEN];
};

#define DLM_MRES_RECOVERY   0x01
#define DLM_MRES_MIGRATION  0x02
#define DLM_MRES_ALL_DONE   0x04

/*
 * We would like to get one whole lockres into a single network
 * message whenever possible.  Generally speaking, there will be
 * at most one dlm_lock on a lockres for each node in the cluster,
 * plus (infrequently) any additional locks coming in from userdlm.
 *
 * struct _dlm_lockres_page
 * {
 * 	dlm_migratable_lockres mres;
 * 	dlm_migratable_lock ml[DLM_MAX_MIGRATABLE_LOCKS];
 * 	u8 pad[DLM_MIG_LOCKRES_RESERVED];
 * };
 *
 * from ../cluster/tcp.h
 *    NET_MAX_PAYLOAD_BYTES  (4096 - sizeof(net_msg))
 *    (roughly 4080 bytes)
 * and sizeof(dlm_migratable_lockres) = 112 bytes
 * and sizeof(dlm_migratable_lock) = 16 bytes
 *
 * Choosing DLM_MAX_MIGRATABLE_LOCKS=240 and
 * DLM_MIG_LOCKRES_RESERVED=128 means we have this:
 *
 *  (DLM_MAX_MIGRATABLE_LOCKS * sizeof(dlm_migratable_lock)) +
 *     sizeof(dlm_migratable_lockres) + DLM_MIG_LOCKRES_RESERVED =
 *        NET_MAX_PAYLOAD_BYTES
 *  (240 * 16) + 112 + 128 = 4080
 *
 * So a lockres would need more than 240 locks before it would
 * use more than one network packet to recover.  Not too bad.
 */
#define DLM_MAX_MIGRATABLE_LOCKS   240

struct dlm_migratable_lockres
{
	u8 master;
	u8 lockname_len;
	u8 num_locks;    // locks sent in this structure
	u8 flags;
	__be32 total_locks; // locks to be sent for this migration cookie
	__be64 mig_cookie;  // cookie for this lockres migration
			 // or zero if not needed
	// 16 bytes
	u8 lockname[DLM_LOCKID_NAME_MAX];
	// 48 bytes
	u8 lvb[DLM_LVB_LEN];
	// 112 bytes
	struct dlm_migratable_lock ml[0];  // 16 bytes each, begins at byte 112
};
#define DLM_MIG_LOCKRES_MAX_LEN  \
	(sizeof(struct dlm_migratable_lockres) + \
	 (sizeof(struct dlm_migratable_lock) * \
	  DLM_MAX_MIGRATABLE_LOCKS) )

/* from above, 128 bytes
 * for some undetermined future use */
#define DLM_MIG_LOCKRES_RESERVED   (NET_MAX_PAYLOAD_BYTES - \
				    DLM_MIG_LOCKRES_MAX_LEN)

struct dlm_create_lock
{
	__be64 cookie;

	__be32 flags;
	u8 pad1;
	u8 node_idx;
	s8 requested_type;
	u8 namelen;

	u8 name[O2NM_MAX_NAME_LEN];
};

struct dlm_convert_lock
{
	__be64 cookie;

	__be32 flags;
	u8 pad1;
	u8 node_idx;
	s8 requested_type;
	u8 namelen;

	u8 name[O2NM_MAX_NAME_LEN];

	s8 lvb[0];
};
#define DLM_CONVERT_LOCK_MAX_LEN  (sizeof(struct dlm_convert_lock)+DLM_LVB_LEN)

struct dlm_unlock_lock
{
	__be64 cookie;

	__be32 flags;
	__be16 pad1;
	u8 node_idx;
	u8 namelen;

	u8 name[O2NM_MAX_NAME_LEN];

	s8 lvb[0];
};
#define DLM_UNLOCK_LOCK_MAX_LEN  (sizeof(struct dlm_unlock_lock)+DLM_LVB_LEN)

struct dlm_proxy_ast
{
	__be64 cookie;

	__be32 flags;
	u8 node_idx;
	u8 type;
	u8 blocked_type;
	u8 namelen;

	u8 name[O2NM_MAX_NAME_LEN];

	s8 lvb[0];
};
#define DLM_PROXY_AST_MAX_LEN  (sizeof(struct dlm_proxy_ast)+DLM_LVB_LEN)

#define DLM_MOD_KEY (0x666c6172)
enum dlm_query_join_response {
	JOIN_DISALLOW = 0,
	JOIN_OK,
	JOIN_OK_NO_MAP,
};

struct dlm_lock_request
{
	u8 node_idx;
	u8 dead_node;
	__be16 pad1;
	__be32 pad2;
};

struct dlm_reco_data_done
{
	u8 node_idx;
	u8 dead_node;
	__be16 pad1;
	__be32 pad2;

	/* unused for now */
	/* eventually we can use this to attempt
	 * lvb recovery based on each node's info */
	u8 reco_lvb[DLM_LVB_LEN];
};

struct dlm_begin_reco
{
	u8 node_idx;
	u8 dead_node;
	__be16 pad1;
	__be32 pad2;
};


struct dlm_query_join_request
{
	u8 node_idx;
	u8 pad1[2];
	u8 name_len;
	u8 domain[O2NM_MAX_NAME_LEN];
};

struct dlm_assert_joined
{
	u8 node_idx;
	u8 pad1[2];
	u8 name_len;
	u8 domain[O2NM_MAX_NAME_LEN];
};

struct dlm_cancel_join
{
	u8 node_idx;
	u8 pad1[2];
	u8 name_len;
	u8 domain[O2NM_MAX_NAME_LEN];
};

struct dlm_exit_domain
{
	u8 node_idx;
	u8 pad1[3];
};

struct dlm_finalize_reco
{
	u8 node_idx;
	u8 dead_node;
	__be16 pad1;
	__be32 pad2;
};

static inline enum dlm_status
__dlm_lockres_state_to_status(struct dlm_lock_resource *res)
{
	enum dlm_status status = DLM_NORMAL;

	assert_spin_locked(&res->spinlock);

	if (res->state & DLM_LOCK_RES_RECOVERING)
		status = DLM_RECOVERING;
	else if (res->state & DLM_LOCK_RES_MIGRATING)
		status = DLM_MIGRATING;
	else if (res->state & DLM_LOCK_RES_IN_PROGRESS)
		status = DLM_FORWARD;

	return status;
}

struct dlm_lock * dlm_new_lock(int type, u8 node, u64 cookie,
			       struct dlm_lockstatus *lksb);
void dlm_lock_get(struct dlm_lock *lock);
void dlm_lock_put(struct dlm_lock *lock);

void dlm_lock_attach_lockres(struct dlm_lock *lock,
			     struct dlm_lock_resource *res);

int dlm_create_lock_handler(struct o2net_msg *msg, u32 len, void *data);
int dlm_convert_lock_handler(struct o2net_msg *msg, u32 len, void *data);
int dlm_proxy_ast_handler(struct o2net_msg *msg, u32 len, void *data);

void dlm_revert_pending_convert(struct dlm_lock_resource *res,
				struct dlm_lock *lock);
void dlm_revert_pending_lock(struct dlm_lock_resource *res,
			     struct dlm_lock *lock);

int dlm_unlock_lock_handler(struct o2net_msg *msg, u32 len, void *data);
void dlm_commit_pending_cancel(struct dlm_lock_resource *res,
			       struct dlm_lock *lock);
void dlm_commit_pending_unlock(struct dlm_lock_resource *res,
			       struct dlm_lock *lock);

int dlm_launch_thread(struct dlm_ctxt *dlm);
void dlm_complete_thread(struct dlm_ctxt *dlm);
int dlm_launch_recovery_thread(struct dlm_ctxt *dlm);
void dlm_complete_recovery_thread(struct dlm_ctxt *dlm);
void dlm_wait_for_recovery(struct dlm_ctxt *dlm);

void dlm_put(struct dlm_ctxt *dlm);
struct dlm_ctxt *dlm_grab(struct dlm_ctxt *dlm);
int dlm_domain_fully_joined(struct dlm_ctxt *dlm);

void __dlm_lockres_calc_usage(struct dlm_ctxt *dlm,
			      struct dlm_lock_resource *res);
void dlm_lockres_calc_usage(struct dlm_ctxt *dlm,
			    struct dlm_lock_resource *res);
void dlm_purge_lockres(struct dlm_ctxt *dlm,
		       struct dlm_lock_resource *lockres);
void dlm_lockres_get(struct dlm_lock_resource *res);
void dlm_lockres_put(struct dlm_lock_resource *res);
void __dlm_unhash_lockres(struct dlm_lock_resource *res);
void __dlm_insert_lockres(struct dlm_ctxt *dlm,
			  struct dlm_lock_resource *res);
struct dlm_lock_resource * __dlm_lookup_lockres(struct dlm_ctxt *dlm,
						const char *name,
						unsigned int len);
struct dlm_lock_resource * dlm_lookup_lockres(struct dlm_ctxt *dlm,
					      const char *name,
					      unsigned int len);

int dlm_is_host_down(int errno);
void dlm_change_lockres_owner(struct dlm_ctxt *dlm,
			      struct dlm_lock_resource *res,
			      u8 owner);
struct dlm_lock_resource * dlm_get_lock_resource(struct dlm_ctxt *dlm,
						 const char *lockid,
						 int flags);
struct dlm_lock_resource *dlm_new_lockres(struct dlm_ctxt *dlm,
					  const char *name,
					  unsigned int namelen);

void dlm_queue_ast(struct dlm_ctxt *dlm, struct dlm_lock *lock);
void dlm_queue_bast(struct dlm_ctxt *dlm, struct dlm_lock *lock);
void dlm_do_local_ast(struct dlm_ctxt *dlm,
		      struct dlm_lock_resource *res,
		      struct dlm_lock *lock);
int dlm_do_remote_ast(struct dlm_ctxt *dlm,
		      struct dlm_lock_resource *res,
		      struct dlm_lock *lock);
void dlm_do_local_bast(struct dlm_ctxt *dlm,
		       struct dlm_lock_resource *res,
		       struct dlm_lock *lock,
		       int blocked_type);
int dlm_send_proxy_ast_msg(struct dlm_ctxt *dlm,
			   struct dlm_lock_resource *res,
			   struct dlm_lock *lock,
			   int msg_type,
			   int blocked_type, int flags);
static inline int dlm_send_proxy_bast(struct dlm_ctxt *dlm,
				      struct dlm_lock_resource *res,
				      struct dlm_lock *lock,
				      int blocked_type)
{
	return dlm_send_proxy_ast_msg(dlm, res, lock, DLM_BAST,
				      blocked_type, 0);
}

static inline int dlm_send_proxy_ast(struct dlm_ctxt *dlm,
				     struct dlm_lock_resource *res,
				     struct dlm_lock *lock,
				     int flags)
{
	return dlm_send_proxy_ast_msg(dlm, res, lock, DLM_AST,
				      0, flags);
}

void dlm_print_one_lock_resource(struct dlm_lock_resource *res);
void __dlm_print_one_lock_resource(struct dlm_lock_resource *res);

u8 dlm_nm_this_node(struct dlm_ctxt *dlm);
void dlm_kick_thread(struct dlm_ctxt *dlm, struct dlm_lock_resource *res);
void __dlm_dirty_lockres(struct dlm_ctxt *dlm, struct dlm_lock_resource *res);


int dlm_nm_init(struct dlm_ctxt *dlm);
int dlm_heartbeat_init(struct dlm_ctxt *dlm);
void dlm_hb_node_down_cb(struct o2nm_node *node, int idx, void *data);
void dlm_hb_node_up_cb(struct o2nm_node *node, int idx, void *data);

int dlm_lockres_is_dirty(struct dlm_ctxt *dlm, struct dlm_lock_resource *res);
int dlm_migrate_lockres(struct dlm_ctxt *dlm,
			struct dlm_lock_resource *res,
			u8 target);
int dlm_finish_migration(struct dlm_ctxt *dlm,
			 struct dlm_lock_resource *res,
			 u8 old_master);
void dlm_lockres_release_ast(struct dlm_ctxt *dlm,
			     struct dlm_lock_resource *res);
void __dlm_lockres_reserve_ast(struct dlm_lock_resource *res);

int dlm_master_request_handler(struct o2net_msg *msg, u32 len, void *data);
int dlm_assert_master_handler(struct o2net_msg *msg, u32 len, void *data);
int dlm_migrate_request_handler(struct o2net_msg *msg, u32 len, void *data);
int dlm_mig_lockres_handler(struct o2net_msg *msg, u32 len, void *data);
int dlm_master_requery_handler(struct o2net_msg *msg, u32 len, void *data);
int dlm_request_all_locks_handler(struct o2net_msg *msg, u32 len, void *data);
int dlm_reco_data_done_handler(struct o2net_msg *msg, u32 len, void *data);
int dlm_begin_reco_handler(struct o2net_msg *msg, u32 len, void *data);
int dlm_finalize_reco_handler(struct o2net_msg *msg, u32 len, void *data);

int dlm_dispatch_assert_master(struct dlm_ctxt *dlm,
			       struct dlm_lock_resource *res,
			       int ignore_higher,
			       u8 request_from,
			       u32 flags);


int dlm_send_one_lockres(struct dlm_ctxt *dlm,
			 struct dlm_lock_resource *res,
			 struct dlm_migratable_lockres *mres,
			 u8 send_to,
			 u8 flags);
void dlm_move_lockres_to_recovery_list(struct dlm_ctxt *dlm,
				       struct dlm_lock_resource *res);

/* will exit holding res->spinlock, but may drop in function */
void __dlm_wait_on_lockres_flags(struct dlm_lock_resource *res, int flags);
void __dlm_wait_on_lockres_flags_set(struct dlm_lock_resource *res, int flags);

/* will exit holding res->spinlock, but may drop in function */
static inline void __dlm_wait_on_lockres(struct dlm_lock_resource *res)
{
	__dlm_wait_on_lockres_flags(res, (DLM_LOCK_RES_IN_PROGRESS|
				    	  DLM_LOCK_RES_RECOVERING|
					  DLM_LOCK_RES_MIGRATING));
}


int dlm_init_mle_cache(void);
void dlm_destroy_mle_cache(void);
void dlm_hb_event_notify_attached(struct dlm_ctxt *dlm, int idx, int node_up);
void dlm_clean_master_list(struct dlm_ctxt *dlm,
			   u8 dead_node);
int dlm_lock_basts_flushed(struct dlm_ctxt *dlm, struct dlm_lock *lock);


static inline const char * dlm_lock_mode_name(int mode)
{
	switch (mode) {
		case LKM_EXMODE:
			return "EX";
		case LKM_PRMODE:
			return "PR";
		case LKM_NLMODE:
			return "NL";
	}
	return "UNKNOWN";
}


static inline int dlm_lock_compatible(int existing, int request)
{
	/* NO_LOCK compatible with all */
	if (request == LKM_NLMODE ||
	    existing == LKM_NLMODE)
		return 1;

	/* EX incompatible with all non-NO_LOCK */
	if (request == LKM_EXMODE)
		return 0;

	/* request must be PR, which is compatible with PR */
	if (existing == LKM_PRMODE)
		return 1;

	return 0;
}

static inline int dlm_lock_on_list(struct list_head *head,
				   struct dlm_lock *lock)
{
	struct list_head *iter;
	struct dlm_lock *tmplock;

	list_for_each(iter, head) {
		tmplock = list_entry(iter, struct dlm_lock, list);
		if (tmplock == lock)
			return 1;
	}
	return 0;
}


static inline enum dlm_status dlm_err_to_dlm_status(int err)
{
	enum dlm_status ret;
	if (err == -ENOMEM)
		ret = DLM_SYSERR;
	else if (err == -ETIMEDOUT || o2net_link_down(err, NULL))
		ret = DLM_NOLOCKMGR;
	else if (err == -EINVAL)
		ret = DLM_BADPARAM;
	else if (err == -ENAMETOOLONG)
		ret = DLM_IVBUFLEN;
	else
		ret = DLM_BADARGS;
	return ret;
}


static inline void dlm_node_iter_init(unsigned long *map,
				      struct dlm_node_iter *iter)
{
	memcpy(iter->node_map, map, sizeof(iter->node_map));
	iter->curnode = -1;
}

static inline int dlm_node_iter_next(struct dlm_node_iter *iter)
{
	int bit;
	bit = find_next_bit(iter->node_map, O2NM_MAX_NODES, iter->curnode+1);
	if (bit >= O2NM_MAX_NODES) {
		iter->curnode = O2NM_MAX_NODES;
		return -ENOENT;
	}
	iter->curnode = bit;
	return bit;
}



#endif /* DLMCOMMON_H */
