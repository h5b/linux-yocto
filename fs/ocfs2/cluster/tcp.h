/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * tcp.h
 *
 * Function prototypes
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

#ifndef O2CLUSTER_TCP_H
#define O2CLUSTER_TCP_H

#include <linux/socket.h>
#ifdef __KERNEL__
#include <net/sock.h>
#include <linux/tcp.h>
#else
#include <sys/socket.h>
#endif
#include <linux/inet.h>
#include <linux/in.h>

struct o2net_msg
{
	__be16 magic;
	__be16 data_len;
	__be16 msg_type;
	__be16 pad1;
	__be32 sys_status;
	__be32 status;
	__be32 key;
	__be32 msg_num;
	__u8  buf[0];
};

typedef int (o2net_msg_handler_func)(struct o2net_msg *msg, u32 len, void *data);

#define O2NET_MAX_PAYLOAD_BYTES  (4096 - sizeof(struct o2net_msg))

/* TODO: figure this out.... */
static inline int o2net_link_down(int err, struct socket *sock)
{
	if (sock) {
		if (sock->sk->sk_state != TCP_ESTABLISHED &&
	    	    sock->sk->sk_state != TCP_CLOSE_WAIT)
			return 1;
	}

	if (err >= 0)
		return 0;
	switch (err) {
		/* ????????????????????????? */
		case -ERESTARTSYS:
		case -EBADF:
		/* When the server has died, an ICMP port unreachable
		 * message prompts ECONNREFUSED. */
		case -ECONNREFUSED:
		case -ENOTCONN:
		case -ECONNRESET:
		case -EPIPE:
			return 1;
	}
	return 0;
}

enum {
	O2NET_DRIVER_UNINITED,
	O2NET_DRIVER_READY,
};

int o2net_init_tcp_sock(struct inode *inode);
int o2net_send_message(u32 msg_type, u32 key, void *data, u32 len,
		       u8 target_node, int *status);
int o2net_send_message_vec(u32 msg_type, u32 key, struct kvec *vec,
			   size_t veclen, u8 target_node, int *status);
int o2net_broadcast_message(u32 msg_type, u32 key, void *data, u32 len,
			    struct inode *group);

int o2net_register_handler(u32 msg_type, u32 key, u32 max_len,
			   o2net_msg_handler_func *func, void *data,
			   struct list_head *unreg_list);
void o2net_unregister_handler_list(struct list_head *list);

struct o2nm_node;
int o2net_register_hb_callbacks(void);
void o2net_unregister_hb_callbacks(void);
int o2net_start_listening(struct o2nm_node *node);
void o2net_stop_listening(struct o2nm_node *node);
void o2net_disconnect_node(struct o2nm_node *node);

int o2net_init(void);
void o2net_exit(void);
int o2net_proc_init(struct proc_dir_entry *parent);
void o2net_proc_exit(struct proc_dir_entry *parent);

#endif /* O2CLUSTER_TCP_H */
