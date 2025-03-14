/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * Copyright (C) 2004, 2005 Oracle.  All rights reserved.
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
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sysctl.h>
#include <linux/configfs.h>

#include "endian.h"
#include "tcp.h"
#include "nodemanager.h"
#include "heartbeat.h"
#include "masklog.h"
#include "sys.h"
#include "ver.h"

/* for now we operate under the assertion that there can be only one
 * cluster active at a time.  Changing this will require trickling
 * cluster references throughout where nodes are looked up */
static struct o2nm_cluster *o2nm_single_cluster = NULL;

#define OCFS2_MAX_HB_CTL_PATH 256
static char ocfs2_hb_ctl_path[OCFS2_MAX_HB_CTL_PATH] = "/sbin/ocfs2_hb_ctl";

static ctl_table ocfs2_nm_table[] = {
	{
		.ctl_name	= 1,
		.procname	= "hb_ctl_path",
		.data		= ocfs2_hb_ctl_path,
		.maxlen		= OCFS2_MAX_HB_CTL_PATH,
		.mode		= 0644,
		.proc_handler	= &proc_dostring,
		.strategy	= &sysctl_string,
	},
	{ .ctl_name = 0 }
};

static ctl_table ocfs2_mod_table[] = {
	{
		.ctl_name	= KERN_OCFS2_NM,
		.procname	= "nm",
		.data		= NULL,
		.maxlen		= 0,
		.mode		= 0555,
		.child		= ocfs2_nm_table
	},
	{ .ctl_name = 0}
};

static ctl_table ocfs2_kern_table[] = {
	{
		.ctl_name	= KERN_OCFS2,
		.procname	= "ocfs2",
		.data		= NULL,
		.maxlen		= 0,
		.mode		= 0555,
		.child		= ocfs2_mod_table
	},
	{ .ctl_name = 0}
};

static ctl_table ocfs2_root_table[] = {
	{
		.ctl_name	= CTL_FS,
		.procname	= "fs",
		.data		= NULL,
		.maxlen		= 0,
		.mode		= 0555,
		.child		= ocfs2_kern_table
	},
	{ .ctl_name = 0 }
};

static struct ctl_table_header *ocfs2_table_header = NULL;

const char *o2nm_get_hb_ctl_path(void)
{
	return ocfs2_hb_ctl_path;
}
EXPORT_SYMBOL_GPL(o2nm_get_hb_ctl_path);

struct o2nm_cluster {
	struct config_group	cl_group;
	unsigned		cl_has_local:1;
	u8			cl_local_node;
	rwlock_t		cl_nodes_lock;
	struct o2nm_node  	*cl_nodes[O2NM_MAX_NODES];
	struct rb_root		cl_node_ip_tree;
	/* this bitmap is part of a hack for disk bitmap.. will go eventually. - zab */
	unsigned long	cl_nodes_bitmap[BITS_TO_LONGS(O2NM_MAX_NODES)];
};

struct o2nm_node *o2nm_get_node_by_num(u8 node_num)
{
	struct o2nm_node *node = NULL;

	if (node_num >= O2NM_MAX_NODES || o2nm_single_cluster == NULL)
		goto out;

	read_lock(&o2nm_single_cluster->cl_nodes_lock);
	node = o2nm_single_cluster->cl_nodes[node_num];
	if (node)
		config_item_get(&node->nd_item);
	read_unlock(&o2nm_single_cluster->cl_nodes_lock);
out:
	return node;
}
EXPORT_SYMBOL_GPL(o2nm_get_node_by_num);

int o2nm_configured_node_map(unsigned long *map, unsigned bytes)
{
	struct o2nm_cluster *cluster = o2nm_single_cluster;

	BUG_ON(bytes < (sizeof(cluster->cl_nodes_bitmap)));

	if (cluster == NULL)
		return -EINVAL;

	read_lock(&cluster->cl_nodes_lock);
	memcpy(map, cluster->cl_nodes_bitmap, sizeof(cluster->cl_nodes_bitmap));
	read_unlock(&cluster->cl_nodes_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(o2nm_configured_node_map);

static struct o2nm_node *o2nm_node_ip_tree_lookup(struct o2nm_cluster *cluster,
						  __be32 ip_needle,
						  struct rb_node ***ret_p,
						  struct rb_node **ret_parent)
{
	struct rb_node **p = &cluster->cl_node_ip_tree.rb_node;
	struct rb_node *parent = NULL;
	struct o2nm_node *node, *ret = NULL;

	while (*p) {
		parent = *p;
		node = rb_entry(parent, struct o2nm_node, nd_ip_node);

		if (memcmp(&ip_needle, &node->nd_ipv4_address,
		           sizeof(ip_needle)) < 0)
			p = &(*p)->rb_left;
		else if (memcmp(&ip_needle, &node->nd_ipv4_address,
			        sizeof(ip_needle)) > 0)
			p = &(*p)->rb_right;
		else {
			ret = node;
			break;
		}
	}

	if (ret_p != NULL)
		*ret_p = p;
	if (ret_parent != NULL)
		*ret_parent = parent;

	return ret;
}

struct o2nm_node *o2nm_get_node_by_ip(__be32 addr)
{
	struct o2nm_node *node = NULL;
	struct o2nm_cluster *cluster = o2nm_single_cluster;

	if (cluster == NULL)
		goto out;

	read_lock(&cluster->cl_nodes_lock);
	node = o2nm_node_ip_tree_lookup(cluster, addr, NULL, NULL);
	if (node)
		config_item_get(&node->nd_item);
	read_unlock(&cluster->cl_nodes_lock);

out:
	return node;
}
EXPORT_SYMBOL_GPL(o2nm_get_node_by_ip);

void o2nm_node_put(struct o2nm_node *node)
{
	config_item_put(&node->nd_item);
}
EXPORT_SYMBOL_GPL(o2nm_node_put);

void o2nm_node_get(struct o2nm_node *node)
{
	config_item_get(&node->nd_item);
}
EXPORT_SYMBOL_GPL(o2nm_node_get);

u8 o2nm_this_node(void)
{
	u8 node_num = O2NM_MAX_NODES;

	if (o2nm_single_cluster && o2nm_single_cluster->cl_has_local)
		node_num = o2nm_single_cluster->cl_local_node;

	return node_num;
}
EXPORT_SYMBOL_GPL(o2nm_this_node);

/* node configfs bits */

static struct o2nm_cluster *to_o2nm_cluster(struct config_item *item)
{
	return item ?
		container_of(to_config_group(item), struct o2nm_cluster,
			     cl_group)
		: NULL;
}

static struct o2nm_node *to_o2nm_node(struct config_item *item)
{
	return item ? container_of(item, struct o2nm_node, nd_item) : NULL;
}

static void o2nm_node_release(struct config_item *item)
{
	struct o2nm_node *node = to_o2nm_node(item);
	kfree(node);
}

static ssize_t o2nm_node_num_read(struct o2nm_node *node, char *page)
{
	return sprintf(page, "%d\n", node->nd_num);
}

static struct o2nm_cluster *to_o2nm_cluster_from_node(struct o2nm_node *node)
{
	/* through the first node_set .parent
	 * mycluster/nodes/mynode == o2nm_cluster->o2nm_node_group->o2nm_node */
	return to_o2nm_cluster(node->nd_item.ci_parent->ci_parent);
}

enum {
	O2NM_NODE_ATTR_NUM = 0,
	O2NM_NODE_ATTR_PORT,
	O2NM_NODE_ATTR_ADDRESS,
	O2NM_NODE_ATTR_LOCAL,
};

static ssize_t o2nm_node_num_write(struct o2nm_node *node, const char *page,
				   size_t count)
{
	struct o2nm_cluster *cluster = to_o2nm_cluster_from_node(node);
	unsigned long tmp;
	char *p = (char *)page;

	tmp = simple_strtoul(p, &p, 0);
	if (!p || (*p && (*p != '\n')))
		return -EINVAL;

	if (tmp >= O2NM_MAX_NODES)
		return -ERANGE;

	/* once we're in the cl_nodes tree networking can look us up by
	 * node number and try to use our address and port attributes
	 * to connect to this node.. make sure that they've been set
	 * before writing the node attribute? */
	if (!test_bit(O2NM_NODE_ATTR_ADDRESS, &node->nd_set_attributes) ||
	    !test_bit(O2NM_NODE_ATTR_PORT, &node->nd_set_attributes))
		return -EINVAL; /* XXX */

	write_lock(&cluster->cl_nodes_lock);
	if (cluster->cl_nodes[tmp])
		p = NULL;
	else  {
		cluster->cl_nodes[tmp] = node;
		node->nd_num = tmp;
		set_bit(tmp, cluster->cl_nodes_bitmap);
	}
	write_unlock(&cluster->cl_nodes_lock);
	if (p == NULL)
		return -EEXIST;

	return count;
}
static ssize_t o2nm_node_ipv4_port_read(struct o2nm_node *node, char *page)
{
	return sprintf(page, "%u\n", ntohs(node->nd_ipv4_port));
}

static ssize_t o2nm_node_ipv4_port_write(struct o2nm_node *node,
					 const char *page, size_t count)
{
	unsigned long tmp;
	char *p = (char *)page;

	tmp = simple_strtoul(p, &p, 0);
	if (!p || (*p && (*p != '\n')))
		return -EINVAL;

	if (tmp == 0)
		return -EINVAL;
	if (tmp >= (u16)-1)
		return -ERANGE;

	node->nd_ipv4_port = htons(tmp);

	return count;
}

static ssize_t o2nm_node_ipv4_address_read(struct o2nm_node *node, char *page)
{
	return sprintf(page, "%u.%u.%u.%u\n", NIPQUAD(node->nd_ipv4_address));
}

static ssize_t o2nm_node_ipv4_address_write(struct o2nm_node *node,
					    const char *page,
					    size_t count)
{
	struct o2nm_cluster *cluster = to_o2nm_cluster_from_node(node);
	int ret, i;
	struct rb_node **p, *parent;
	unsigned int octets[4];
	__be32 ipv4_addr = 0;

	ret = sscanf(page, "%3u.%3u.%3u.%3u", &octets[3], &octets[2],
		     &octets[1], &octets[0]);
	if (ret != 4)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(octets); i++) {
		if (octets[i] > 255)
			return -ERANGE;
		be32_add_cpu(&ipv4_addr, octets[i] << (i * 8));
	}

	ret = 0;
	write_lock(&cluster->cl_nodes_lock);
	if (o2nm_node_ip_tree_lookup(cluster, ipv4_addr, &p, &parent))
		ret = -EEXIST;
	else {
		rb_link_node(&node->nd_ip_node, parent, p);
		rb_insert_color(&node->nd_ip_node, &cluster->cl_node_ip_tree);
	}
	write_unlock(&cluster->cl_nodes_lock);
	if (ret)
		return ret;

	memcpy(&node->nd_ipv4_address, &ipv4_addr, sizeof(ipv4_addr));

	return count;
}

static ssize_t o2nm_node_local_read(struct o2nm_node *node, char *page)
{
	return sprintf(page, "%d\n", node->nd_local);
}

static ssize_t o2nm_node_local_write(struct o2nm_node *node, const char *page,
				     size_t count)
{
	struct o2nm_cluster *cluster = to_o2nm_cluster_from_node(node);
	unsigned long tmp;
	char *p = (char *)page;
	ssize_t ret;

	tmp = simple_strtoul(p, &p, 0);
	if (!p || (*p && (*p != '\n')))
		return -EINVAL;

	tmp = !!tmp; /* boolean of whether this node wants to be local */

	/* setting local turns on networking rx for now so we require having
	 * set everything else first */
	if (!test_bit(O2NM_NODE_ATTR_ADDRESS, &node->nd_set_attributes) ||
	    !test_bit(O2NM_NODE_ATTR_NUM, &node->nd_set_attributes) ||
	    !test_bit(O2NM_NODE_ATTR_PORT, &node->nd_set_attributes))
		return -EINVAL; /* XXX */

	/* the only failure case is trying to set a new local node
	 * when a different one is already set */
	if (tmp && tmp == cluster->cl_has_local &&
	    cluster->cl_local_node != node->nd_num)
		return -EBUSY;

	/* bring up the rx thread if we're setting the new local node. */
	if (tmp && !cluster->cl_has_local) {
		ret = o2net_start_listening(node);
		if (ret)
			return ret;
	}

	if (!tmp && cluster->cl_has_local &&
	    cluster->cl_local_node == node->nd_num) {
		o2net_stop_listening(node);
		cluster->cl_local_node = O2NM_INVALID_NODE_NUM;
	}

	node->nd_local = tmp;
	if (node->nd_local) {
		cluster->cl_has_local = tmp;
		cluster->cl_local_node = node->nd_num;
	}

	return count;
}

struct o2nm_node_attribute {
	struct configfs_attribute attr;
	ssize_t (*show)(struct o2nm_node *, char *);
	ssize_t (*store)(struct o2nm_node *, const char *, size_t);
};

static struct o2nm_node_attribute o2nm_node_attr_num = {
	.attr	= { .ca_owner = THIS_MODULE,
		    .ca_name = "num",
		    .ca_mode = S_IRUGO | S_IWUSR },
	.show	= o2nm_node_num_read,
	.store	= o2nm_node_num_write,
};

static struct o2nm_node_attribute o2nm_node_attr_ipv4_port = {
	.attr	= { .ca_owner = THIS_MODULE,
		    .ca_name = "ipv4_port",
		    .ca_mode = S_IRUGO | S_IWUSR },
	.show	= o2nm_node_ipv4_port_read,
	.store	= o2nm_node_ipv4_port_write,
};

static struct o2nm_node_attribute o2nm_node_attr_ipv4_address = {
	.attr	= { .ca_owner = THIS_MODULE,
		    .ca_name = "ipv4_address",
		    .ca_mode = S_IRUGO | S_IWUSR },
	.show	= o2nm_node_ipv4_address_read,
	.store	= o2nm_node_ipv4_address_write,
};

static struct o2nm_node_attribute o2nm_node_attr_local = {
	.attr	= { .ca_owner = THIS_MODULE,
		    .ca_name = "local",
		    .ca_mode = S_IRUGO | S_IWUSR },
	.show	= o2nm_node_local_read,
	.store	= o2nm_node_local_write,
};

static struct configfs_attribute *o2nm_node_attrs[] = {
	[O2NM_NODE_ATTR_NUM] = &o2nm_node_attr_num.attr,
	[O2NM_NODE_ATTR_PORT] = &o2nm_node_attr_ipv4_port.attr,
	[O2NM_NODE_ATTR_ADDRESS] = &o2nm_node_attr_ipv4_address.attr,
	[O2NM_NODE_ATTR_LOCAL] = &o2nm_node_attr_local.attr,
	NULL,
};

static int o2nm_attr_index(struct configfs_attribute *attr)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(o2nm_node_attrs); i++) {
		if (attr == o2nm_node_attrs[i])
			return i;
	}
	BUG();
	return 0;
}

static ssize_t o2nm_node_show(struct config_item *item,
			      struct configfs_attribute *attr,
			      char *page)
{
	struct o2nm_node *node = to_o2nm_node(item);
	struct o2nm_node_attribute *o2nm_node_attr =
		container_of(attr, struct o2nm_node_attribute, attr);
	ssize_t ret = 0;

	if (o2nm_node_attr->show)
		ret = o2nm_node_attr->show(node, page);
	return ret;
}

static ssize_t o2nm_node_store(struct config_item *item,
			       struct configfs_attribute *attr,
			       const char *page, size_t count)
{
	struct o2nm_node *node = to_o2nm_node(item);
	struct o2nm_node_attribute *o2nm_node_attr =
		container_of(attr, struct o2nm_node_attribute, attr);
	ssize_t ret;
	int attr_index = o2nm_attr_index(attr);

	if (o2nm_node_attr->store == NULL) {
		ret = -EINVAL;
		goto out;
	}

	if (test_bit(attr_index, &node->nd_set_attributes))
		return -EBUSY;

	ret = o2nm_node_attr->store(node, page, count);
	if (ret < count)
		goto out;

	set_bit(attr_index, &node->nd_set_attributes);
out:
	return ret;
}

static struct configfs_item_operations o2nm_node_item_ops = {
	.release		= o2nm_node_release,
	.show_attribute		= o2nm_node_show,
	.store_attribute	= o2nm_node_store,
};

static struct config_item_type o2nm_node_type = {
	.ct_item_ops	= &o2nm_node_item_ops,
	.ct_attrs	= o2nm_node_attrs,
	.ct_owner	= THIS_MODULE,
};

/* node set */

struct o2nm_node_group {
	struct config_group ns_group;
	/* some stuff? */
};

#if 0
static struct o2nm_node_group *to_o2nm_node_group(struct config_group *group)
{
	return group ?
		container_of(group, struct o2nm_node_group, ns_group)
		: NULL;
}
#endif

static struct config_item *o2nm_node_group_make_item(struct config_group *group,
						     const char *name)
{
	struct o2nm_node *node = NULL;
	struct config_item *ret = NULL;

	if (strlen(name) > O2NM_MAX_NAME_LEN)
		goto out; /* ENAMETOOLONG */

	node = kcalloc(1, sizeof(struct o2nm_node), GFP_KERNEL);
	if (node == NULL)
		goto out; /* ENOMEM */

	strcpy(node->nd_name, name); /* use item.ci_namebuf instead? */
	config_item_init_type_name(&node->nd_item, name, &o2nm_node_type);
	spin_lock_init(&node->nd_lock);

	ret = &node->nd_item;

out:
	if (ret == NULL)
		kfree(node);

	return ret;
}

static void o2nm_node_group_drop_item(struct config_group *group,
				      struct config_item *item)
{
	struct o2nm_node *node = to_o2nm_node(item);
	struct o2nm_cluster *cluster = to_o2nm_cluster(group->cg_item.ci_parent);

	o2net_disconnect_node(node);

	if (cluster->cl_has_local &&
	    (cluster->cl_local_node == node->nd_num)) {
		cluster->cl_has_local = 0;
		cluster->cl_local_node = O2NM_INVALID_NODE_NUM;
		o2net_stop_listening(node);
	}

	/* XXX call into net to stop this node from trading messages */

	write_lock(&cluster->cl_nodes_lock);

	/* XXX sloppy */
	if (node->nd_ipv4_address)
		rb_erase(&node->nd_ip_node, &cluster->cl_node_ip_tree);

	/* nd_num might be 0 if the node number hasn't been set.. */
	if (cluster->cl_nodes[node->nd_num] == node) {
		cluster->cl_nodes[node->nd_num] = NULL;
		clear_bit(node->nd_num, cluster->cl_nodes_bitmap);
	}
	write_unlock(&cluster->cl_nodes_lock);

	config_item_put(item);
}

static struct configfs_group_operations o2nm_node_group_group_ops = {
	.make_item	= o2nm_node_group_make_item,
	.drop_item	= o2nm_node_group_drop_item,
};

static struct config_item_type o2nm_node_group_type = {
	.ct_group_ops	= &o2nm_node_group_group_ops,
	.ct_owner	= THIS_MODULE,
};

/* cluster */

static void o2nm_cluster_release(struct config_item *item)
{
	struct o2nm_cluster *cluster = to_o2nm_cluster(item);

	kfree(cluster->cl_group.default_groups);
	kfree(cluster);
}

static struct configfs_item_operations o2nm_cluster_item_ops = {
	.release	= o2nm_cluster_release,
};

static struct config_item_type o2nm_cluster_type = {
	.ct_item_ops	= &o2nm_cluster_item_ops,
	.ct_owner	= THIS_MODULE,
};

/* cluster set */

struct o2nm_cluster_group {
	struct configfs_subsystem cs_subsys;
	/* some stuff? */
};

#if 0
static struct o2nm_cluster_group *to_o2nm_cluster_group(struct config_group *group)
{
	return group ?
		container_of(to_configfs_subsystem(group), struct o2nm_cluster_group, cs_subsys)
	       : NULL;
}
#endif

static struct config_group *o2nm_cluster_group_make_group(struct config_group *group,
							  const char *name)
{
	struct o2nm_cluster *cluster = NULL;
	struct o2nm_node_group *ns = NULL;
	struct config_group *o2hb_group = NULL, *ret = NULL;
	void *defs = NULL;

	/* this runs under the parent dir's i_mutex; there can be only
	 * one caller in here at a time */
	if (o2nm_single_cluster)
		goto out; /* ENOSPC */

	cluster = kcalloc(1, sizeof(struct o2nm_cluster), GFP_KERNEL);
	ns = kcalloc(1, sizeof(struct o2nm_node_group), GFP_KERNEL);
	defs = kcalloc(3, sizeof(struct config_group *), GFP_KERNEL);
	o2hb_group = o2hb_alloc_hb_set();
	if (cluster == NULL || ns == NULL || o2hb_group == NULL || defs == NULL)
		goto out;

	config_group_init_type_name(&cluster->cl_group, name,
				    &o2nm_cluster_type);
	config_group_init_type_name(&ns->ns_group, "node",
				    &o2nm_node_group_type);

	cluster->cl_group.default_groups = defs;
	cluster->cl_group.default_groups[0] = &ns->ns_group;
	cluster->cl_group.default_groups[1] = o2hb_group;
	cluster->cl_group.default_groups[2] = NULL;
	rwlock_init(&cluster->cl_nodes_lock);
	cluster->cl_node_ip_tree = RB_ROOT;

	ret = &cluster->cl_group;
	o2nm_single_cluster = cluster;

out:
	if (ret == NULL) {
		kfree(cluster);
		kfree(ns);
		o2hb_free_hb_set(o2hb_group);
		kfree(defs);
	}

	return ret;
}

static void o2nm_cluster_group_drop_item(struct config_group *group, struct config_item *item)
{
	struct o2nm_cluster *cluster = to_o2nm_cluster(item);
	int i;
	struct config_item *killme;

	BUG_ON(o2nm_single_cluster != cluster);
	o2nm_single_cluster = NULL;

	for (i = 0; cluster->cl_group.default_groups[i]; i++) {
		killme = &cluster->cl_group.default_groups[i]->cg_item;
		cluster->cl_group.default_groups[i] = NULL;
		config_item_put(killme);
	}

	config_item_put(item);
}

static struct configfs_group_operations o2nm_cluster_group_group_ops = {
	.make_group	= o2nm_cluster_group_make_group,
	.drop_item	= o2nm_cluster_group_drop_item,
};

static struct config_item_type o2nm_cluster_group_type = {
	.ct_group_ops	= &o2nm_cluster_group_group_ops,
	.ct_owner	= THIS_MODULE,
};

static struct o2nm_cluster_group o2nm_cluster_group = {
	.cs_subsys = {
		.su_group = {
			.cg_item = {
				.ci_namebuf = "cluster",
				.ci_type = &o2nm_cluster_group_type,
			},
		},
	},
};

static void __exit exit_o2nm(void)
{
	if (ocfs2_table_header)
		unregister_sysctl_table(ocfs2_table_header);

	/* XXX sync with hb callbacks and shut down hb? */
	o2net_unregister_hb_callbacks();
	configfs_unregister_subsystem(&o2nm_cluster_group.cs_subsys);
	o2cb_sys_shutdown();

	o2net_exit();
}

static int __init init_o2nm(void)
{
	int ret = -1;

	cluster_print_version();

	o2hb_init();
	o2net_init();

	ocfs2_table_header = register_sysctl_table(ocfs2_root_table, 0);
	if (!ocfs2_table_header) {
		printk(KERN_ERR "nodemanager: unable to register sysctl\n");
		ret = -ENOMEM; /* or something. */
		goto out;
	}

	ret = o2net_register_hb_callbacks();
	if (ret)
		goto out_sysctl;

	config_group_init(&o2nm_cluster_group.cs_subsys.su_group);
	init_MUTEX(&o2nm_cluster_group.cs_subsys.su_sem);
	ret = configfs_register_subsystem(&o2nm_cluster_group.cs_subsys);
	if (ret) {
		printk(KERN_ERR "nodemanager: Registration returned %d\n", ret);
		goto out_callbacks;
	}

	ret = o2cb_sys_init();
	if (!ret)
		goto out;

	configfs_unregister_subsystem(&o2nm_cluster_group.cs_subsys);
out_callbacks:
	o2net_unregister_hb_callbacks();
out_sysctl:
	unregister_sysctl_table(ocfs2_table_header);
out:
	return ret;
}

MODULE_AUTHOR("Oracle");
MODULE_LICENSE("GPL");

module_init(init_o2nm)
module_exit(exit_o2nm)
