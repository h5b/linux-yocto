/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * dlmdomain.c
 *
 * defines domain join / leave apis
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

#include <linux/module.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/utsname.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/err.h>

#include "cluster/heartbeat.h"
#include "cluster/nodemanager.h"
#include "cluster/tcp.h"

#include "dlmapi.h"
#include "dlmcommon.h"

#include "dlmdebug.h"
#include "dlmdomain.h"

#include "dlmver.h"

#define MLOG_MASK_PREFIX (ML_DLM|ML_DLM_DOMAIN)
#include "cluster/masklog.h"

/*
 *
 * spinlock lock ordering: if multiple locks are needed, obey this ordering:
 *    dlm_domain_lock
 *    struct dlm_ctxt->spinlock
 *    struct dlm_lock_resource->spinlock
 *    struct dlm_ctxt->master_lock
 *    struct dlm_ctxt->ast_lock
 *    dlm_master_list_entry->spinlock
 *    dlm_lock->spinlock
 *
 */

spinlock_t dlm_domain_lock = SPIN_LOCK_UNLOCKED;
LIST_HEAD(dlm_domains);
static DECLARE_WAIT_QUEUE_HEAD(dlm_domain_events);

#define DLM_DOMAIN_BACKOFF_MS 200

static int dlm_query_join_handler(struct o2net_msg *msg, u32 len, void *data);
static int dlm_assert_joined_handler(struct o2net_msg *msg, u32 len, void *data);
static int dlm_cancel_join_handler(struct o2net_msg *msg, u32 len, void *data);
static int dlm_exit_domain_handler(struct o2net_msg *msg, u32 len, void *data);

static void dlm_unregister_domain_handlers(struct dlm_ctxt *dlm);

void __dlm_unhash_lockres(struct dlm_lock_resource *lockres)
{
	list_del_init(&lockres->list);
	dlm_lockres_put(lockres);
}

void __dlm_insert_lockres(struct dlm_ctxt *dlm,
		       struct dlm_lock_resource *res)
{
	struct list_head *bucket;
	struct qstr *q;

	assert_spin_locked(&dlm->spinlock);

	q = &res->lockname;
	q->hash = full_name_hash(q->name, q->len);
	bucket = &(dlm->resources[q->hash & DLM_HASH_MASK]);

	/* get a reference for our hashtable */
	dlm_lockres_get(res);

	list_add_tail(&res->list, bucket);
}

struct dlm_lock_resource * __dlm_lookup_lockres(struct dlm_ctxt *dlm,
					 const char *name,
					 unsigned int len)
{
	unsigned int hash;
	struct list_head *iter;
	struct dlm_lock_resource *tmpres=NULL;
	struct list_head *bucket;

	mlog_entry("%.*s\n", len, name);

	assert_spin_locked(&dlm->spinlock);

	hash = full_name_hash(name, len);

	bucket = &(dlm->resources[hash & DLM_HASH_MASK]);

	/* check for pre-existing lock */
	list_for_each(iter, bucket) {
		tmpres = list_entry(iter, struct dlm_lock_resource, list);
		if (tmpres->lockname.len == len &&
		    memcmp(tmpres->lockname.name, name, len) == 0) {
			dlm_lockres_get(tmpres);
			break;
		}

		tmpres = NULL;
	}
	return tmpres;
}

struct dlm_lock_resource * dlm_lookup_lockres(struct dlm_ctxt *dlm,
				    const char *name,
				    unsigned int len)
{
	struct dlm_lock_resource *res;

	spin_lock(&dlm->spinlock);
	res = __dlm_lookup_lockres(dlm, name, len);
	spin_unlock(&dlm->spinlock);
	return res;
}

static struct dlm_ctxt * __dlm_lookup_domain_full(const char *domain, int len)
{
	struct dlm_ctxt *tmp = NULL;
	struct list_head *iter;

	assert_spin_locked(&dlm_domain_lock);

	/* tmp->name here is always NULL terminated,
	 * but domain may not be! */
	list_for_each(iter, &dlm_domains) {
		tmp = list_entry (iter, struct dlm_ctxt, list);
		if (strlen(tmp->name) == len &&
		    memcmp(tmp->name, domain, len)==0)
			break;
		tmp = NULL;
	}

	return tmp;
}

/* For null terminated domain strings ONLY */
static struct dlm_ctxt * __dlm_lookup_domain(const char *domain)
{
	assert_spin_locked(&dlm_domain_lock);

	return __dlm_lookup_domain_full(domain, strlen(domain));
}


/* returns true on one of two conditions:
 * 1) the domain does not exist
 * 2) the domain exists and it's state is "joined" */
static int dlm_wait_on_domain_helper(const char *domain)
{
	int ret = 0;
	struct dlm_ctxt *tmp = NULL;

	spin_lock(&dlm_domain_lock);

	tmp = __dlm_lookup_domain(domain);
	if (!tmp)
		ret = 1;
	else if (tmp->dlm_state == DLM_CTXT_JOINED)
		ret = 1;

	spin_unlock(&dlm_domain_lock);
	return ret;
}

static void dlm_free_ctxt_mem(struct dlm_ctxt *dlm)
{
	if (dlm->resources)
		free_page((unsigned long) dlm->resources);

	if (dlm->name)
		kfree(dlm->name);

	kfree(dlm);
}

/* A little strange - this function will be called while holding
 * dlm_domain_lock and is expected to be holding it on the way out. We
 * will however drop and reacquire it multiple times */
static void dlm_ctxt_release(struct kref *kref)
{
	struct dlm_ctxt *dlm;

	dlm = container_of(kref, struct dlm_ctxt, dlm_refs);

	BUG_ON(dlm->num_joins);
	BUG_ON(dlm->dlm_state == DLM_CTXT_JOINED);

	/* we may still be in the list if we hit an error during join. */
	list_del_init(&dlm->list);

	spin_unlock(&dlm_domain_lock);

	mlog(0, "freeing memory from domain %s\n", dlm->name);

	wake_up(&dlm_domain_events);

	dlm_free_ctxt_mem(dlm);

	spin_lock(&dlm_domain_lock);
}

void dlm_put(struct dlm_ctxt *dlm)
{
	spin_lock(&dlm_domain_lock);
	kref_put(&dlm->dlm_refs, dlm_ctxt_release);
	spin_unlock(&dlm_domain_lock);
}

static void __dlm_get(struct dlm_ctxt *dlm)
{
	kref_get(&dlm->dlm_refs);
}

/* given a questionable reference to a dlm object, gets a reference if
 * it can find it in the list, otherwise returns NULL in which case
 * you shouldn't trust your pointer. */
struct dlm_ctxt *dlm_grab(struct dlm_ctxt *dlm)
{
	struct list_head *iter;
	struct dlm_ctxt *target = NULL;

	spin_lock(&dlm_domain_lock);

	list_for_each(iter, &dlm_domains) {
		target = list_entry (iter, struct dlm_ctxt, list);

		if (target == dlm) {
			__dlm_get(target);
			break;
		}

		target = NULL;
	}

	spin_unlock(&dlm_domain_lock);

	return target;
}

int dlm_domain_fully_joined(struct dlm_ctxt *dlm)
{
	int ret;

	spin_lock(&dlm_domain_lock);
	ret = (dlm->dlm_state == DLM_CTXT_JOINED) ||
		(dlm->dlm_state == DLM_CTXT_IN_SHUTDOWN);
	spin_unlock(&dlm_domain_lock);

	return ret;
}

static void dlm_complete_dlm_shutdown(struct dlm_ctxt *dlm)
{
	dlm_unregister_domain_handlers(dlm);
	dlm_complete_thread(dlm);
	dlm_complete_recovery_thread(dlm);

	/* We've left the domain. Now we can take ourselves out of the
	 * list and allow the kref stuff to help us free the
	 * memory. */
	spin_lock(&dlm_domain_lock);
	list_del_init(&dlm->list);
	spin_unlock(&dlm_domain_lock);

	/* Wake up anyone waiting for us to remove this domain */
	wake_up(&dlm_domain_events);
}

static void dlm_migrate_all_locks(struct dlm_ctxt *dlm)
{
	int i;
	struct dlm_lock_resource *res;

	mlog(0, "Migrating locks from domain %s\n", dlm->name);
restart:
	spin_lock(&dlm->spinlock);
	for (i=0; i<DLM_HASH_SIZE; i++) {
		while (!list_empty(&dlm->resources[i])) {
			res = list_entry(dlm->resources[i].next,
				     struct dlm_lock_resource, list);
			/* need reference when manually grabbing lockres */
			dlm_lockres_get(res);
			/* this should unhash the lockres
			 * and exit with dlm->spinlock */
			mlog(0, "purging res=%p\n", res);
			if (dlm_lockres_is_dirty(dlm, res)) {
				/* HACK!  this should absolutely go.
				 * need to figure out why some empty
				 * lockreses are still marked dirty */
				mlog(ML_ERROR, "lockres %.*s dirty!\n",
				     res->lockname.len, res->lockname.name);

				spin_unlock(&dlm->spinlock);
				dlm_kick_thread(dlm, res);
				wait_event(dlm->ast_wq, !dlm_lockres_is_dirty(dlm, res));
				dlm_lockres_put(res);
				goto restart;
			}
			dlm_purge_lockres(dlm, res);
			dlm_lockres_put(res);
		}
	}
	spin_unlock(&dlm->spinlock);

	mlog(0, "DONE Migrating locks from domain %s\n", dlm->name);
}

static int dlm_no_joining_node(struct dlm_ctxt *dlm)
{
	int ret;

	spin_lock(&dlm->spinlock);
	ret = dlm->joining_node == DLM_LOCK_RES_OWNER_UNKNOWN;
	spin_unlock(&dlm->spinlock);

	return ret;
}

static void dlm_mark_domain_leaving(struct dlm_ctxt *dlm)
{
	/* Yikes, a double spinlock! I need domain_lock for the dlm
	 * state and the dlm spinlock for join state... Sorry! */
again:
	spin_lock(&dlm_domain_lock);
	spin_lock(&dlm->spinlock);

	if (dlm->joining_node != DLM_LOCK_RES_OWNER_UNKNOWN) {
		mlog(0, "Node %d is joining, we wait on it.\n",
			  dlm->joining_node);
		spin_unlock(&dlm->spinlock);
		spin_unlock(&dlm_domain_lock);

		wait_event(dlm->dlm_join_events, dlm_no_joining_node(dlm));
		goto again;
	}

	dlm->dlm_state = DLM_CTXT_LEAVING;
	spin_unlock(&dlm->spinlock);
	spin_unlock(&dlm_domain_lock);
}

static void __dlm_print_nodes(struct dlm_ctxt *dlm)
{
	int node = -1;

	assert_spin_locked(&dlm->spinlock);

	mlog(ML_NOTICE, "Nodes in my domain (\"%s\"):\n", dlm->name);

	while ((node = find_next_bit(dlm->domain_map, O2NM_MAX_NODES,
				     node + 1)) < O2NM_MAX_NODES) {
		mlog(ML_NOTICE, " node %d\n", node);
	}
}

static int dlm_exit_domain_handler(struct o2net_msg *msg, u32 len, void *data)
{
	struct dlm_ctxt *dlm = data;
	unsigned int node;
	struct dlm_exit_domain *exit_msg = (struct dlm_exit_domain *) msg->buf;

	mlog_entry("%p %u %p", msg, len, data);

	if (!dlm_grab(dlm))
		return 0;

	node = exit_msg->node_idx;

	mlog(0, "Node %u leaves domain %s\n", node, dlm->name);

	spin_lock(&dlm->spinlock);
	clear_bit(node, dlm->domain_map);
	__dlm_print_nodes(dlm);

	/* notify anything attached to the heartbeat events */
	dlm_hb_event_notify_attached(dlm, node, 0);

	spin_unlock(&dlm->spinlock);

	dlm_put(dlm);

	return 0;
}

static int dlm_send_one_domain_exit(struct dlm_ctxt *dlm,
				    unsigned int node)
{
	int status;
	struct dlm_exit_domain leave_msg;

	mlog(0, "Asking node %u if we can leave the domain %s me = %u\n",
		  node, dlm->name, dlm->node_num);

	memset(&leave_msg, 0, sizeof(leave_msg));
	leave_msg.node_idx = dlm->node_num;

	status = o2net_send_message(DLM_EXIT_DOMAIN_MSG, dlm->key,
				    &leave_msg, sizeof(leave_msg), node,
				    NULL);

	mlog(0, "status return %d from o2net_send_message\n", status);

	return status;
}


static void dlm_leave_domain(struct dlm_ctxt *dlm)
{
	int node, clear_node, status;

	/* At this point we've migrated away all our locks and won't
	 * accept mastership of new ones. The dlm is responsible for
	 * almost nothing now. We make sure not to confuse any joining
	 * nodes and then commence shutdown procedure. */

	spin_lock(&dlm->spinlock);
	/* Clear ourselves from the domain map */
	clear_bit(dlm->node_num, dlm->domain_map);
	while ((node = find_next_bit(dlm->domain_map, O2NM_MAX_NODES,
				     0)) < O2NM_MAX_NODES) {
		/* Drop the dlm spinlock. This is safe wrt the domain_map.
		 * -nodes cannot be added now as the
		 *   query_join_handlers knows to respond with OK_NO_MAP
		 * -we catch the right network errors if a node is
		 *   removed from the map while we're sending him the
		 *   exit message. */
		spin_unlock(&dlm->spinlock);

		clear_node = 1;

		status = dlm_send_one_domain_exit(dlm, node);
		if (status < 0 &&
		    status != -ENOPROTOOPT &&
		    status != -ENOTCONN) {
			mlog(ML_NOTICE, "Error %d sending domain exit message "
			     "to node %d\n", status, node);

			/* Not sure what to do here but lets sleep for
			 * a bit in case this was a transient
			 * error... */
			msleep(DLM_DOMAIN_BACKOFF_MS);
			clear_node = 0;
		}

		spin_lock(&dlm->spinlock);
		/* If we're not clearing the node bit then we intend
		 * to loop back around to try again. */
		if (clear_node)
			clear_bit(node, dlm->domain_map);
	}
	spin_unlock(&dlm->spinlock);
}

int dlm_joined(struct dlm_ctxt *dlm)
{
	int ret = 0;

	spin_lock(&dlm_domain_lock);

	if (dlm->dlm_state == DLM_CTXT_JOINED)
		ret = 1;

	spin_unlock(&dlm_domain_lock);

	return ret;
}

int dlm_shutting_down(struct dlm_ctxt *dlm)
{
	int ret = 0;

	spin_lock(&dlm_domain_lock);

	if (dlm->dlm_state == DLM_CTXT_IN_SHUTDOWN)
		ret = 1;

	spin_unlock(&dlm_domain_lock);

	return ret;
}

void dlm_unregister_domain(struct dlm_ctxt *dlm)
{
	int leave = 0;

	spin_lock(&dlm_domain_lock);
	BUG_ON(dlm->dlm_state != DLM_CTXT_JOINED);
	BUG_ON(!dlm->num_joins);

	dlm->num_joins--;
	if (!dlm->num_joins) {
		/* We mark it "in shutdown" now so new register
		 * requests wait until we've completely left the
		 * domain. Don't use DLM_CTXT_LEAVING yet as we still
		 * want new domain joins to communicate with us at
		 * least until we've completed migration of our
		 * resources. */
		dlm->dlm_state = DLM_CTXT_IN_SHUTDOWN;
		leave = 1;
	}
	spin_unlock(&dlm_domain_lock);

	if (leave) {
		mlog(0, "shutting down domain %s\n", dlm->name);

		/* We changed dlm state, notify the thread */
		dlm_kick_thread(dlm, NULL);

		dlm_migrate_all_locks(dlm);
		dlm_mark_domain_leaving(dlm);
		dlm_leave_domain(dlm);
		dlm_complete_dlm_shutdown(dlm);
	}
	dlm_put(dlm);
}
EXPORT_SYMBOL_GPL(dlm_unregister_domain);

static int dlm_query_join_handler(struct o2net_msg *msg, u32 len, void *data)
{
	struct dlm_query_join_request *query;
	enum dlm_query_join_response response;
	struct dlm_ctxt *dlm = NULL;

	query = (struct dlm_query_join_request *) msg->buf;

	mlog(0, "node %u wants to join domain %s\n", query->node_idx,
		  query->domain);

	/*
	 * If heartbeat doesn't consider the node live, tell it
	 * to back off and try again.  This gives heartbeat a chance
	 * to catch up.
	 */
	if (!o2hb_check_node_heartbeating(query->node_idx)) {
		mlog(0, "node %u is not in our live map yet\n",
		     query->node_idx);

		response = JOIN_DISALLOW;
		goto respond;
	}

	response = JOIN_OK_NO_MAP;

	spin_lock(&dlm_domain_lock);
	dlm = __dlm_lookup_domain_full(query->domain, query->name_len);
	/* Once the dlm ctxt is marked as leaving then we don't want
	 * to be put in someone's domain map. */
	if (dlm && dlm->dlm_state != DLM_CTXT_LEAVING) {
		spin_lock(&dlm->spinlock);

		if (dlm->dlm_state == DLM_CTXT_NEW &&
		    dlm->joining_node == DLM_LOCK_RES_OWNER_UNKNOWN) {
			/*If this is a brand new context and we
			 * haven't started our join process yet, then
			 * the other node won the race. */
			response = JOIN_OK_NO_MAP;
		} else if (dlm->joining_node != DLM_LOCK_RES_OWNER_UNKNOWN) {
			/* Disallow parallel joins. */
			response = JOIN_DISALLOW;
		} else {
			/* Alright we're fully a part of this domain
			 * so we keep some state as to who's joining
			 * and indicate to him that needs to be fixed
			 * up. */
			response = JOIN_OK;
			__dlm_set_joining_node(dlm, query->node_idx);
		}

		spin_unlock(&dlm->spinlock);
	}
	spin_unlock(&dlm_domain_lock);

respond:
	mlog(0, "We respond with %u\n", response);

	return response;
}

static int dlm_assert_joined_handler(struct o2net_msg *msg, u32 len, void *data)
{
	struct dlm_assert_joined *assert;
	struct dlm_ctxt *dlm = NULL;

	assert = (struct dlm_assert_joined *) msg->buf;

	mlog(0, "node %u asserts join on domain %s\n", assert->node_idx,
		  assert->domain);

	spin_lock(&dlm_domain_lock);
	dlm = __dlm_lookup_domain_full(assert->domain, assert->name_len);
	/* XXX should we consider no dlm ctxt an error? */
	if (dlm) {
		spin_lock(&dlm->spinlock);

		/* Alright, this node has officially joined our
		 * domain. Set him in the map and clean up our
		 * leftover join state. */
		BUG_ON(dlm->joining_node != assert->node_idx);
		set_bit(assert->node_idx, dlm->domain_map);
		__dlm_set_joining_node(dlm, DLM_LOCK_RES_OWNER_UNKNOWN);

		__dlm_print_nodes(dlm);

		/* notify anything attached to the heartbeat events */
		dlm_hb_event_notify_attached(dlm, assert->node_idx, 1);

		spin_unlock(&dlm->spinlock);
	}
	spin_unlock(&dlm_domain_lock);

	return 0;
}

static int dlm_cancel_join_handler(struct o2net_msg *msg, u32 len, void *data)
{
	struct dlm_cancel_join *cancel;
	struct dlm_ctxt *dlm = NULL;

	cancel = (struct dlm_cancel_join *) msg->buf;

	mlog(0, "node %u cancels join on domain %s\n", cancel->node_idx,
		  cancel->domain);

	spin_lock(&dlm_domain_lock);
	dlm = __dlm_lookup_domain_full(cancel->domain, cancel->name_len);

	if (dlm) {
		spin_lock(&dlm->spinlock);

		/* Yikes, this guy wants to cancel his join. No
		 * problem, we simply cleanup our join state. */
		BUG_ON(dlm->joining_node != cancel->node_idx);
		__dlm_set_joining_node(dlm, DLM_LOCK_RES_OWNER_UNKNOWN);

		spin_unlock(&dlm->spinlock);
	}
	spin_unlock(&dlm_domain_lock);

	return 0;
}

static int dlm_send_one_join_cancel(struct dlm_ctxt *dlm,
				    unsigned int node)
{
	int status;
	struct dlm_cancel_join cancel_msg;

	memset(&cancel_msg, 0, sizeof(cancel_msg));
	cancel_msg.node_idx = dlm->node_num;
	cancel_msg.name_len = strlen(dlm->name);
	memcpy(cancel_msg.domain, dlm->name, cancel_msg.name_len);

	status = o2net_send_message(DLM_CANCEL_JOIN_MSG, DLM_MOD_KEY,
				    &cancel_msg, sizeof(cancel_msg), node,
				    NULL);
	if (status < 0) {
		mlog_errno(status);
		goto bail;
	}

bail:
	return status;
}

/* map_size should be in bytes. */
static int dlm_send_join_cancels(struct dlm_ctxt *dlm,
				 unsigned long *node_map,
				 unsigned int map_size)
{
	int status, tmpstat;
	unsigned int node;

	if (map_size != (BITS_TO_LONGS(O2NM_MAX_NODES) *
			 sizeof(unsigned long))) {
		mlog(ML_ERROR,
		     "map_size %u != BITS_TO_LONGS(O2NM_MAX_NODES) %u\n",
		     map_size, BITS_TO_LONGS(O2NM_MAX_NODES));
		return -EINVAL;
	}

	status = 0;
	node = -1;
	while ((node = find_next_bit(node_map, O2NM_MAX_NODES,
				     node + 1)) < O2NM_MAX_NODES) {
		if (node == dlm->node_num)
			continue;

		tmpstat = dlm_send_one_join_cancel(dlm, node);
		if (tmpstat) {
			mlog(ML_ERROR, "Error return %d cancelling join on "
			     "node %d\n", tmpstat, node);
			if (!status)
				status = tmpstat;
		}
	}

	if (status)
		mlog_errno(status);
	return status;
}

static int dlm_request_join(struct dlm_ctxt *dlm,
			    int node,
			    enum dlm_query_join_response *response)
{
	int status, retval;
	struct dlm_query_join_request join_msg;

	mlog(0, "querying node %d\n", node);

	memset(&join_msg, 0, sizeof(join_msg));
	join_msg.node_idx = dlm->node_num;
	join_msg.name_len = strlen(dlm->name);
	memcpy(join_msg.domain, dlm->name, join_msg.name_len);

	status = o2net_send_message(DLM_QUERY_JOIN_MSG, DLM_MOD_KEY, &join_msg,
				    sizeof(join_msg), node, &retval);
	if (status < 0 && status != -ENOPROTOOPT) {
		mlog_errno(status);
		goto bail;
	}

	/* -ENOPROTOOPT from the net code means the other side isn't
	    listening for our message type -- that's fine, it means
	    his dlm isn't up, so we can consider him a 'yes' but not
	    joined into the domain.  */
	if (status == -ENOPROTOOPT) {
		status = 0;
		*response = JOIN_OK_NO_MAP;
	} else if (retval == JOIN_DISALLOW ||
		   retval == JOIN_OK ||
		   retval == JOIN_OK_NO_MAP) {
		*response = retval;
	} else {
		status = -EINVAL;
		mlog(ML_ERROR, "invalid response %d from node %u\n", retval,
		     node);
	}

	mlog(0, "status %d, node %d response is %d\n", status, node,
		  *response);

bail:
	return status;
}

static int dlm_send_one_join_assert(struct dlm_ctxt *dlm,
				    unsigned int node)
{
	int status;
	struct dlm_assert_joined assert_msg;

	mlog(0, "Sending join assert to node %u\n", node);

	memset(&assert_msg, 0, sizeof(assert_msg));
	assert_msg.node_idx = dlm->node_num;
	assert_msg.name_len = strlen(dlm->name);
	memcpy(assert_msg.domain, dlm->name, assert_msg.name_len);

	status = o2net_send_message(DLM_ASSERT_JOINED_MSG, DLM_MOD_KEY,
				    &assert_msg, sizeof(assert_msg), node,
				    NULL);
	if (status < 0)
		mlog_errno(status);

	return status;
}

static void dlm_send_join_asserts(struct dlm_ctxt *dlm,
				  unsigned long *node_map)
{
	int status, node, live;

	status = 0;
	node = -1;
	while ((node = find_next_bit(node_map, O2NM_MAX_NODES,
				     node + 1)) < O2NM_MAX_NODES) {
		if (node == dlm->node_num)
			continue;

		do {
			/* It is very important that this message be
			 * received so we spin until either the node
			 * has died or it gets the message. */
			status = dlm_send_one_join_assert(dlm, node);

			spin_lock(&dlm->spinlock);
			live = test_bit(node, dlm->live_nodes_map);
			spin_unlock(&dlm->spinlock);

			if (status) {
				mlog(ML_ERROR, "Error return %d asserting "
				     "join on node %d\n", status, node);

				/* give us some time between errors... */
				if (live)
					msleep(DLM_DOMAIN_BACKOFF_MS);
			}
		} while (status && live);
	}
}

struct domain_join_ctxt {
	unsigned long live_map[BITS_TO_LONGS(O2NM_MAX_NODES)];
	unsigned long yes_resp_map[BITS_TO_LONGS(O2NM_MAX_NODES)];
};

static int dlm_should_restart_join(struct dlm_ctxt *dlm,
				   struct domain_join_ctxt *ctxt,
				   enum dlm_query_join_response response)
{
	int ret;

	if (response == JOIN_DISALLOW) {
		mlog(0, "Latest response of disallow -- should restart\n");
		return 1;
	}

	spin_lock(&dlm->spinlock);
	/* For now, we restart the process if the node maps have
	 * changed at all */
	ret = memcmp(ctxt->live_map, dlm->live_nodes_map,
		     sizeof(dlm->live_nodes_map));
	spin_unlock(&dlm->spinlock);

	if (ret)
		mlog(0, "Node maps changed -- should restart\n");

	return ret;
}

static int dlm_try_to_join_domain(struct dlm_ctxt *dlm)
{
	int status = 0, tmpstat, node;
	struct domain_join_ctxt *ctxt;
	enum dlm_query_join_response response;

	mlog_entry("%p", dlm);

	ctxt = kcalloc(1, sizeof(*ctxt), GFP_KERNEL);
	if (!ctxt) {
		status = -ENOMEM;
		mlog_errno(status);
		goto bail;
	}

	/* group sem locking should work for us here -- we're already
	 * registered for heartbeat events so filling this should be
	 * atomic wrt getting those handlers called. */
	o2hb_fill_node_map(dlm->live_nodes_map, sizeof(dlm->live_nodes_map));

	spin_lock(&dlm->spinlock);
	memcpy(ctxt->live_map, dlm->live_nodes_map, sizeof(ctxt->live_map));

	__dlm_set_joining_node(dlm, dlm->node_num);

	spin_unlock(&dlm->spinlock);

	node = -1;
	while ((node = find_next_bit(ctxt->live_map, O2NM_MAX_NODES,
				     node + 1)) < O2NM_MAX_NODES) {
		if (node == dlm->node_num)
			continue;

		status = dlm_request_join(dlm, node, &response);
		if (status < 0) {
			mlog_errno(status);
			goto bail;
		}

		/* Ok, either we got a response or the node doesn't have a
		 * dlm up. */
		if (response == JOIN_OK)
			set_bit(node, ctxt->yes_resp_map);

		if (dlm_should_restart_join(dlm, ctxt, response)) {
			status = -EAGAIN;
			goto bail;
		}
	}

	mlog(0, "Yay, done querying nodes!\n");

	/* Yay, everyone agree's we can join the domain. My domain is
	 * comprised of all nodes who were put in the
	 * yes_resp_map. Copy that into our domain map and send a join
	 * assert message to clean up everyone elses state. */
	spin_lock(&dlm->spinlock);
	memcpy(dlm->domain_map, ctxt->yes_resp_map,
	       sizeof(ctxt->yes_resp_map));
	set_bit(dlm->node_num, dlm->domain_map);
	spin_unlock(&dlm->spinlock);

	dlm_send_join_asserts(dlm, ctxt->yes_resp_map);

	/* Joined state *must* be set before the joining node
	 * information, otherwise the query_join handler may read no
	 * current joiner but a state of NEW and tell joining nodes
	 * we're not in the domain. */
	spin_lock(&dlm_domain_lock);
	dlm->dlm_state = DLM_CTXT_JOINED;
	dlm->num_joins++;
	spin_unlock(&dlm_domain_lock);

bail:
	spin_lock(&dlm->spinlock);
	__dlm_set_joining_node(dlm, DLM_LOCK_RES_OWNER_UNKNOWN);
	if (!status)
		__dlm_print_nodes(dlm);
	spin_unlock(&dlm->spinlock);

	if (ctxt) {
		/* Do we need to send a cancel message to any nodes? */
		if (status < 0) {
			tmpstat = dlm_send_join_cancels(dlm,
							ctxt->yes_resp_map,
							sizeof(ctxt->yes_resp_map));
			if (tmpstat < 0)
				mlog_errno(tmpstat);
		}
		kfree(ctxt);
	}

	mlog(0, "returning %d\n", status);
	return status;
}

static void dlm_unregister_domain_handlers(struct dlm_ctxt *dlm)
{
	o2hb_unregister_callback(&dlm->dlm_hb_up);
	o2hb_unregister_callback(&dlm->dlm_hb_down);
	o2net_unregister_handler_list(&dlm->dlm_domain_handlers);
}

static int dlm_register_domain_handlers(struct dlm_ctxt *dlm)
{
	int status;

	mlog(0, "registering handlers.\n");

	o2hb_setup_callback(&dlm->dlm_hb_down, O2HB_NODE_DOWN_CB,
			    dlm_hb_node_down_cb, dlm, DLM_HB_NODE_DOWN_PRI);
	status = o2hb_register_callback(&dlm->dlm_hb_down);
	if (status)
		goto bail;

	o2hb_setup_callback(&dlm->dlm_hb_up, O2HB_NODE_UP_CB,
			    dlm_hb_node_up_cb, dlm, DLM_HB_NODE_UP_PRI);
	status = o2hb_register_callback(&dlm->dlm_hb_up);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_MASTER_REQUEST_MSG, dlm->key,
					sizeof(struct dlm_master_request),
					dlm_master_request_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_ASSERT_MASTER_MSG, dlm->key,
					sizeof(struct dlm_assert_master),
					dlm_assert_master_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_CREATE_LOCK_MSG, dlm->key,
					sizeof(struct dlm_create_lock),
					dlm_create_lock_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_CONVERT_LOCK_MSG, dlm->key,
					DLM_CONVERT_LOCK_MAX_LEN,
					dlm_convert_lock_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_UNLOCK_LOCK_MSG, dlm->key,
					DLM_UNLOCK_LOCK_MAX_LEN,
					dlm_unlock_lock_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_PROXY_AST_MSG, dlm->key,
					DLM_PROXY_AST_MAX_LEN,
					dlm_proxy_ast_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_EXIT_DOMAIN_MSG, dlm->key,
					sizeof(struct dlm_exit_domain),
					dlm_exit_domain_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_MIGRATE_REQUEST_MSG, dlm->key,
					sizeof(struct dlm_migrate_request),
					dlm_migrate_request_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_MIG_LOCKRES_MSG, dlm->key,
					DLM_MIG_LOCKRES_MAX_LEN,
					dlm_mig_lockres_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_MASTER_REQUERY_MSG, dlm->key,
					sizeof(struct dlm_master_requery),
					dlm_master_requery_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_LOCK_REQUEST_MSG, dlm->key,
					sizeof(struct dlm_lock_request),
					dlm_request_all_locks_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_RECO_DATA_DONE_MSG, dlm->key,
					sizeof(struct dlm_reco_data_done),
					dlm_reco_data_done_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_BEGIN_RECO_MSG, dlm->key,
					sizeof(struct dlm_begin_reco),
					dlm_begin_reco_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_FINALIZE_RECO_MSG, dlm->key,
					sizeof(struct dlm_finalize_reco),
					dlm_finalize_reco_handler,
					dlm, &dlm->dlm_domain_handlers);
	if (status)
		goto bail;

bail:
	if (status)
		dlm_unregister_domain_handlers(dlm);

	return status;
}

static int dlm_join_domain(struct dlm_ctxt *dlm)
{
	int status;

	BUG_ON(!dlm);

	mlog(0, "Join domain %s\n", dlm->name);

	status = dlm_register_domain_handlers(dlm);
	if (status) {
		mlog_errno(status);
		goto bail;
	}

	status = dlm_launch_thread(dlm);
	if (status < 0) {
		mlog_errno(status);
		goto bail;
	}

	status = dlm_launch_recovery_thread(dlm);
	if (status < 0) {
		mlog_errno(status);
		goto bail;
	}

	do {
		unsigned int backoff;
		status = dlm_try_to_join_domain(dlm);

		/* If we're racing another node to the join, then we
		 * need to back off temporarily and let them
		 * complete. */
		if (status == -EAGAIN) {
			if (signal_pending(current)) {
				status = -ERESTARTSYS;
				goto bail;
			}

			/*
			 * <chip> After you!
			 * <dale> No, after you!
			 * <chip> I insist!
			 * <dale> But you first!
			 * ...
			 */
			backoff = (unsigned int)(jiffies & 0x3);
			backoff *= DLM_DOMAIN_BACKOFF_MS;
			mlog(0, "backoff %d\n", backoff);
			msleep(backoff);
		}
	} while (status == -EAGAIN);

	if (status < 0) {
		mlog_errno(status);
		goto bail;
	}

	status = 0;
bail:
	wake_up(&dlm_domain_events);

	if (status) {
		dlm_unregister_domain_handlers(dlm);
		dlm_complete_thread(dlm);
		dlm_complete_recovery_thread(dlm);
	}

	return status;
}

static struct dlm_ctxt *dlm_alloc_ctxt(const char *domain,
				u32 key)
{
	int i;
	struct dlm_ctxt *dlm = NULL;

	dlm = kcalloc(1, sizeof(*dlm), GFP_KERNEL);
	if (!dlm) {
		mlog_errno(-ENOMEM);
		goto leave;
	}

	dlm->name = kmalloc(strlen(domain) + 1, GFP_KERNEL);
	if (dlm->name == NULL) {
		mlog_errno(-ENOMEM);
		kfree(dlm);
		dlm = NULL;
		goto leave;
	}

	dlm->resources = (struct list_head *) __get_free_page(GFP_KERNEL);
	if (!dlm->resources) {
		mlog_errno(-ENOMEM);
		kfree(dlm->name);
		kfree(dlm);
		dlm = NULL;
		goto leave;
	}
	memset(dlm->resources, 0, PAGE_SIZE);

	for (i=0; i<DLM_HASH_SIZE; i++)
		INIT_LIST_HEAD(&dlm->resources[i]);

	strcpy(dlm->name, domain);
	dlm->key = key;
	dlm->node_num = o2nm_this_node();

	spin_lock_init(&dlm->spinlock);
	spin_lock_init(&dlm->master_lock);
	spin_lock_init(&dlm->ast_lock);
	INIT_LIST_HEAD(&dlm->list);
	INIT_LIST_HEAD(&dlm->dirty_list);
	INIT_LIST_HEAD(&dlm->reco.resources);
	INIT_LIST_HEAD(&dlm->reco.received);
	INIT_LIST_HEAD(&dlm->reco.node_data);
	INIT_LIST_HEAD(&dlm->purge_list);
	INIT_LIST_HEAD(&dlm->dlm_domain_handlers);
	dlm->reco.state = 0;

	INIT_LIST_HEAD(&dlm->pending_asts);
	INIT_LIST_HEAD(&dlm->pending_basts);

	mlog(0, "dlm->recovery_map=%p, &(dlm->recovery_map[0])=%p\n",
		  dlm->recovery_map, &(dlm->recovery_map[0]));

	memset(dlm->recovery_map, 0, sizeof(dlm->recovery_map));
	memset(dlm->live_nodes_map, 0, sizeof(dlm->live_nodes_map));
	memset(dlm->domain_map, 0, sizeof(dlm->domain_map));

	dlm->dlm_thread_task = NULL;
	dlm->dlm_reco_thread_task = NULL;
	init_waitqueue_head(&dlm->dlm_thread_wq);
	init_waitqueue_head(&dlm->dlm_reco_thread_wq);
	init_waitqueue_head(&dlm->reco.event);
	init_waitqueue_head(&dlm->ast_wq);
	init_waitqueue_head(&dlm->migration_wq);
	INIT_LIST_HEAD(&dlm->master_list);
	INIT_LIST_HEAD(&dlm->mle_hb_events);

	dlm->joining_node = DLM_LOCK_RES_OWNER_UNKNOWN;
	init_waitqueue_head(&dlm->dlm_join_events);

	dlm->reco.new_master = O2NM_INVALID_NODE_NUM;
	dlm->reco.dead_node = O2NM_INVALID_NODE_NUM;
	atomic_set(&dlm->local_resources, 0);
	atomic_set(&dlm->remote_resources, 0);
	atomic_set(&dlm->unknown_resources, 0);

	spin_lock_init(&dlm->work_lock);
	INIT_LIST_HEAD(&dlm->work_list);
	INIT_WORK(&dlm->dispatched_work, dlm_dispatch_work, dlm);

	kref_init(&dlm->dlm_refs);
	dlm->dlm_state = DLM_CTXT_NEW;

	INIT_LIST_HEAD(&dlm->dlm_eviction_callbacks);

	mlog(0, "context init: refcount %u\n",
		  atomic_read(&dlm->dlm_refs.refcount));

leave:
	return dlm;
}

/*
 * dlm_register_domain: one-time setup per "domain"
 */
struct dlm_ctxt * dlm_register_domain(const char *domain,
			       u32 key)
{
	int ret;
	struct dlm_ctxt *dlm = NULL;
	struct dlm_ctxt *new_ctxt = NULL;

	if (strlen(domain) > O2NM_MAX_NAME_LEN) {
		ret = -ENAMETOOLONG;
		mlog(ML_ERROR, "domain name length too long\n");
		goto leave;
	}

	if (!o2hb_check_local_node_heartbeating()) {
		mlog(ML_ERROR, "the local node has not been configured, or is "
		     "not heartbeating\n");
		ret = -EPROTO;
		goto leave;
	}

	mlog(0, "register called for domain \"%s\"\n", domain);

retry:
	dlm = NULL;
	if (signal_pending(current)) {
		ret = -ERESTARTSYS;
		mlog_errno(ret);
		goto leave;
	}

	spin_lock(&dlm_domain_lock);

	dlm = __dlm_lookup_domain(domain);
	if (dlm) {
		if (dlm->dlm_state != DLM_CTXT_JOINED) {
			spin_unlock(&dlm_domain_lock);

			mlog(0, "This ctxt is not joined yet!\n");
			wait_event_interruptible(dlm_domain_events,
						 dlm_wait_on_domain_helper(
							 domain));
			goto retry;
		}

		__dlm_get(dlm);
		dlm->num_joins++;

		spin_unlock(&dlm_domain_lock);

		ret = 0;
		goto leave;
	}

	/* doesn't exist */
	if (!new_ctxt) {
		spin_unlock(&dlm_domain_lock);

		new_ctxt = dlm_alloc_ctxt(domain, key);
		if (new_ctxt)
			goto retry;

		ret = -ENOMEM;
		mlog_errno(ret);
		goto leave;
	}

	/* a little variable switch-a-roo here... */
	dlm = new_ctxt;
	new_ctxt = NULL;

	/* add the new domain */
	list_add_tail(&dlm->list, &dlm_domains);
	spin_unlock(&dlm_domain_lock);

	ret = dlm_join_domain(dlm);
	if (ret) {
		mlog_errno(ret);
		dlm_put(dlm);
		goto leave;
	}

	ret = 0;
leave:
	if (new_ctxt)
		dlm_free_ctxt_mem(new_ctxt);

	if (ret < 0)
		dlm = ERR_PTR(ret);

	return dlm;
}
EXPORT_SYMBOL_GPL(dlm_register_domain);

static LIST_HEAD(dlm_join_handlers);

static void dlm_unregister_net_handlers(void)
{
	o2net_unregister_handler_list(&dlm_join_handlers);
}

static int dlm_register_net_handlers(void)
{
	int status = 0;

	status = o2net_register_handler(DLM_QUERY_JOIN_MSG, DLM_MOD_KEY,
					sizeof(struct dlm_query_join_request),
					dlm_query_join_handler,
					NULL, &dlm_join_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_ASSERT_JOINED_MSG, DLM_MOD_KEY,
					sizeof(struct dlm_assert_joined),
					dlm_assert_joined_handler,
					NULL, &dlm_join_handlers);
	if (status)
		goto bail;

	status = o2net_register_handler(DLM_CANCEL_JOIN_MSG, DLM_MOD_KEY,
					sizeof(struct dlm_cancel_join),
					dlm_cancel_join_handler,
					NULL, &dlm_join_handlers);

bail:
	if (status < 0)
		dlm_unregister_net_handlers();

	return status;
}

/* Domain eviction callback handling.
 *
 * The file system requires notification of node death *before* the
 * dlm completes it's recovery work, otherwise it may be able to
 * acquire locks on resources requiring recovery. Since the dlm can
 * evict a node from it's domain *before* heartbeat fires, a similar
 * mechanism is required. */

/* Eviction is not expected to happen often, so a per-domain lock is
 * not necessary. Eviction callbacks are allowed to sleep for short
 * periods of time. */
static DECLARE_RWSEM(dlm_callback_sem);

void dlm_fire_domain_eviction_callbacks(struct dlm_ctxt *dlm,
					int node_num)
{
	struct list_head *iter;
	struct dlm_eviction_cb *cb;

	down_read(&dlm_callback_sem);
	list_for_each(iter, &dlm->dlm_eviction_callbacks) {
		cb = list_entry(iter, struct dlm_eviction_cb, ec_item);

		cb->ec_func(node_num, cb->ec_data);
	}
	up_read(&dlm_callback_sem);
}

void dlm_setup_eviction_cb(struct dlm_eviction_cb *cb,
			   dlm_eviction_func *f,
			   void *data)
{
	INIT_LIST_HEAD(&cb->ec_item);
	cb->ec_func = f;
	cb->ec_data = data;
}
EXPORT_SYMBOL_GPL(dlm_setup_eviction_cb);

void dlm_register_eviction_cb(struct dlm_ctxt *dlm,
			      struct dlm_eviction_cb *cb)
{
	down_write(&dlm_callback_sem);
	list_add_tail(&cb->ec_item, &dlm->dlm_eviction_callbacks);
	up_write(&dlm_callback_sem);
}
EXPORT_SYMBOL_GPL(dlm_register_eviction_cb);

void dlm_unregister_eviction_cb(struct dlm_eviction_cb *cb)
{
	down_write(&dlm_callback_sem);
	list_del_init(&cb->ec_item);
	up_write(&dlm_callback_sem);
}
EXPORT_SYMBOL_GPL(dlm_unregister_eviction_cb);

static int __init dlm_init(void)
{
	int status;

	dlm_print_version();

	status = dlm_init_mle_cache();
	if (status)
		return -1;

	status = dlm_register_net_handlers();
	if (status) {
		dlm_destroy_mle_cache();
		return -1;
	}

	return 0;
}

static void __exit dlm_exit (void)
{
	dlm_unregister_net_handlers();
	dlm_destroy_mle_cache();
}

MODULE_AUTHOR("Oracle");
MODULE_LICENSE("GPL");

module_init(dlm_init);
module_exit(dlm_exit);
