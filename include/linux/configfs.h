/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * configfs.h - definitions for the device driver filesystem
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
 * Based on sysfs:
 * 	sysfs is Copyright (C) 2001, 2002, 2003 Patrick Mochel
 *
 * Based on kobject.h:
 *      Copyright (c) 2002-2003	Patrick Mochel
 *      Copyright (c) 2002-2003	Open Source Development Labs
 *
 * configfs Copyright (C) 2005 Oracle.  All rights reserved.
 *
 * Please read Documentation/filesystems/configfs.txt before using the
 * configfs interface, ESPECIALLY the parts about reference counts and
 * item destructors.
 */

#ifndef _CONFIGFS_H_
#define _CONFIGFS_H_

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/list.h>
#include <linux/kref.h>

#include <asm/atomic.h>
#include <asm/semaphore.h>

#define CONFIGFS_ITEM_NAME_LEN	20

struct module;

struct configfs_item_operations;
struct configfs_group_operations;
struct configfs_attribute;
struct configfs_subsystem;

struct config_item {
	char			*ci_name;
	char			ci_namebuf[CONFIGFS_ITEM_NAME_LEN];
	struct kref		ci_kref;
	struct list_head	ci_entry;
	struct config_item	*ci_parent;
	struct config_group	*ci_group;
	struct config_item_type	*ci_type;
	struct dentry		*ci_dentry;
};

extern int config_item_set_name(struct config_item *, const char *, ...);

static inline char *config_item_name(struct config_item * item)
{
	return item->ci_name;
}

extern void config_item_init(struct config_item *);
extern void config_item_init_type_name(struct config_item *item,
				       const char *name,
				       struct config_item_type *type);
extern void config_item_cleanup(struct config_item *);

extern struct config_item * config_item_get(struct config_item *);
extern void config_item_put(struct config_item *);

struct config_item_type {
	struct module				*ct_owner;
	struct configfs_item_operations		*ct_item_ops;
	struct configfs_group_operations	*ct_group_ops;
	struct configfs_attribute		**ct_attrs;
};


/**
 *	group - a group of config_items of a specific type, belonging
 *	to a specific subsystem.
 */

struct config_group {
	struct config_item		cg_item;
	struct list_head		cg_children;
	struct configfs_subsystem 	*cg_subsys;
	struct config_group		**default_groups;
};


extern void config_group_init(struct config_group *group);
extern void config_group_init_type_name(struct config_group *group,
					const char *name,
					struct config_item_type *type);


static inline struct config_group *to_config_group(struct config_item *item)
{
	return item ? container_of(item,struct config_group,cg_item) : NULL;
}

static inline struct config_group *config_group_get(struct config_group *group)
{
	return group ? to_config_group(config_item_get(&group->cg_item)) : NULL;
}

static inline void config_group_put(struct config_group *group)
{
	config_item_put(&group->cg_item);
}

extern struct config_item *config_group_find_obj(struct config_group *, const char *);


struct configfs_attribute {
	char			*ca_name;
	struct module 		*ca_owner;
	mode_t			ca_mode;
};


/*
 * If allow_link() exists, the item can symlink(2) out to other
 * items.  If the item is a group, it may support mkdir(2).
 * Groups supply one of make_group() and make_item().  If the
 * group supports make_group(), one can create group children.  If it
 * supports make_item(), one can create config_item children.  If it has
 * default_groups on group->default_groups, it has automatically created
 * group children.  default_groups may coexist alongsize make_group() or
 * make_item(), but if the group wishes to have only default_groups
 * children (disallowing mkdir(2)), it need not provide either function.
 * If the group has commit(), it supports pending and commited (active)
 * items.
 */
struct configfs_item_operations {
	void (*release)(struct config_item *);
	ssize_t	(*show_attribute)(struct config_item *, struct configfs_attribute *,char *);
	ssize_t	(*store_attribute)(struct config_item *,struct configfs_attribute *,const char *, size_t);
	int (*allow_link)(struct config_item *src, struct config_item *target);
	int (*drop_link)(struct config_item *src, struct config_item *target);
};

struct configfs_group_operations {
	struct config_item *(*make_item)(struct config_group *group, const char *name);
	struct config_group *(*make_group)(struct config_group *group, const char *name);
	int (*commit_item)(struct config_item *item);
	void (*drop_item)(struct config_group *group, struct config_item *item);
};



/**
 * Use these macros to make defining attributes easier. See include/linux/device.h
 * for examples..
 */

#if 0
#define __ATTR(_name,_mode,_show,_store) { \
	.attr = {.ca_name = __stringify(_name), .ca_mode = _mode, .ca_owner = THIS_MODULE },	\
	.show	= _show,					\
	.store	= _store,					\
}

#define __ATTR_RO(_name) { \
	.attr	= { .ca_name = __stringify(_name), .ca_mode = 0444, .ca_owner = THIS_MODULE },	\
	.show	= _name##_show,	\
}

#define __ATTR_NULL { .attr = { .name = NULL } }

#define attr_name(_attr) (_attr).attr.name
#endif


struct configfs_subsystem {
	struct config_group	su_group;
	struct semaphore	su_sem;
};

static inline struct configfs_subsystem *to_configfs_subsystem(struct config_group *group)
{
	return group ?
		container_of(group, struct configfs_subsystem, su_group) :
		NULL;
}

int configfs_register_subsystem(struct configfs_subsystem *subsys);
void configfs_unregister_subsystem(struct configfs_subsystem *subsys);

#endif  /* __KERNEL__ */

#endif /* _CONFIGFS_H_ */
