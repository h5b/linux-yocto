/*
 * raid_class.c - implementation of a simple raid visualisation class
 *
 * Copyright (c) 2005 - James Bottomley <James.Bottomley@steeleye.com>
 *
 * This file is licensed under GPLv2
 *
 * This class is designed to allow raid attributes to be visualised and
 * manipulated in a form independent of the underlying raid.  Ultimately this
 * should work for both hardware and software raids.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/raid_class.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

#define RAID_NUM_ATTRS	3

struct raid_internal {
	struct raid_template r;
	struct raid_function_template *f;
	/* The actual attributes */
	struct class_device_attribute private_attrs[RAID_NUM_ATTRS];
	/* The array of null terminated pointers to attributes 
	 * needed by scsi_sysfs.c */
	struct class_device_attribute *attrs[RAID_NUM_ATTRS + 1];
};

struct raid_component {
	struct list_head node;
	struct class_device cdev;
	int num;
};

#define to_raid_internal(tmpl)	container_of(tmpl, struct raid_internal, r)

#define tc_to_raid_internal(tcont) ({					\
	struct raid_template *r =					\
		container_of(tcont, struct raid_template, raid_attrs);	\
	to_raid_internal(r);						\
})

#define ac_to_raid_internal(acont) ({					\
	struct transport_container *tc =				\
		container_of(acont, struct transport_container, ac);	\
	tc_to_raid_internal(tc);					\
})

#define class_device_to_raid_internal(cdev) ({				\
	struct attribute_container *ac =				\
		attribute_container_classdev_to_container(cdev);	\
	ac_to_raid_internal(ac);					\
})
	

static int raid_match(struct attribute_container *cont, struct device *dev)
{
	/* We have to look for every subsystem that could house
	 * emulated RAID devices, so start with SCSI */
	struct raid_internal *i = ac_to_raid_internal(cont);

	if (scsi_is_sdev_device(dev)) {
		struct scsi_device *sdev = to_scsi_device(dev);

		if (i->f->cookie != sdev->host->hostt)
			return 0;

		return i->f->is_raid(dev);
	}
	/* FIXME: look at other subsystems too */
	return 0;
}

static int raid_setup(struct transport_container *tc, struct device *dev,
		       struct class_device *cdev)
{
	struct raid_data *rd;

	BUG_ON(class_get_devdata(cdev));

	rd = kzalloc(sizeof(*rd), GFP_KERNEL);
	if (!rd)
		return -ENOMEM;

	INIT_LIST_HEAD(&rd->component_list);
	class_set_devdata(cdev, rd);
		
	return 0;
}

static int raid_remove(struct transport_container *tc, struct device *dev,
		       struct class_device *cdev)
{
	struct raid_data *rd = class_get_devdata(cdev);
	struct raid_component *rc, *next;
	dev_printk(KERN_ERR, dev, "RAID REMOVE\n");
	class_set_devdata(cdev, NULL);
	list_for_each_entry_safe(rc, next, &rd->component_list, node) {
		list_del(&rc->node);
		dev_printk(KERN_ERR, rc->cdev.dev, "RAID COMPONENT REMOVE\n");
		class_device_unregister(&rc->cdev);
	}
	dev_printk(KERN_ERR, dev, "RAID REMOVE DONE\n");
	kfree(rd);
	return 0;
}

static DECLARE_TRANSPORT_CLASS(raid_class,
			       "raid_devices",
			       raid_setup,
			       raid_remove,
			       NULL);

static const struct {
	enum raid_state	value;
	char		*name;
} raid_states[] = {
	{ RAID_STATE_UNKNOWN, "unknown" },
	{ RAID_STATE_ACTIVE, "active" },
	{ RAID_STATE_DEGRADED, "degraded" },
	{ RAID_STATE_RESYNCING, "resyncing" },
	{ RAID_STATE_OFFLINE, "offline" },
};

static const char *raid_state_name(enum raid_state state)
{
	int i;
	char *name = NULL;

	for (i = 0; i < sizeof(raid_states)/sizeof(raid_states[0]); i++) {
		if (raid_states[i].value == state) {
			name = raid_states[i].name;
			break;
		}
	}
	return name;
}

static struct {
	enum raid_level value;
	char *name;
} raid_levels[] = {
	{ RAID_LEVEL_UNKNOWN, "unknown" },
	{ RAID_LEVEL_LINEAR, "linear" },
	{ RAID_LEVEL_0, "raid0" },
	{ RAID_LEVEL_1, "raid1" },
	{ RAID_LEVEL_3, "raid3" },
	{ RAID_LEVEL_4, "raid4" },
	{ RAID_LEVEL_5, "raid5" },
	{ RAID_LEVEL_6, "raid6" },
};

static const char *raid_level_name(enum raid_level level)
{
	int i;
	char *name = NULL;

	for (i = 0; i < sizeof(raid_levels)/sizeof(raid_levels[0]); i++) {
		if (raid_levels[i].value == level) {
			name = raid_levels[i].name;
			break;
		}
	}
	return name;
}

#define raid_attr_show_internal(attr, fmt, var, code)			\
static ssize_t raid_show_##attr(struct class_device *cdev, char *buf)	\
{									\
	struct raid_data *rd = class_get_devdata(cdev);			\
	code								\
	return snprintf(buf, 20, #fmt "\n", var);			\
}

#define raid_attr_ro_states(attr, states, code)				\
raid_attr_show_internal(attr, %s, name,					\
	const char *name;						\
	code								\
	name = raid_##states##_name(rd->attr);				\
)									\
static CLASS_DEVICE_ATTR(attr, S_IRUGO, raid_show_##attr, NULL)


#define raid_attr_ro_internal(attr, code)				\
raid_attr_show_internal(attr, %d, rd->attr, code)			\
static CLASS_DEVICE_ATTR(attr, S_IRUGO, raid_show_##attr, NULL)

#define ATTR_CODE(attr)							\
	struct raid_internal *i = class_device_to_raid_internal(cdev);	\
	if (i->f->get_##attr)						\
		i->f->get_##attr(cdev->dev);

#define raid_attr_ro(attr)	raid_attr_ro_internal(attr, )
#define raid_attr_ro_fn(attr)	raid_attr_ro_internal(attr, ATTR_CODE(attr))
#define raid_attr_ro_state(attr)	raid_attr_ro_states(attr, attr, )
#define raid_attr_ro_state_fn(attr)	raid_attr_ro_states(attr, attr, ATTR_CODE(attr))


raid_attr_ro_state(level);
raid_attr_ro_fn(resync);
raid_attr_ro_state_fn(state);

static void raid_component_release(struct class_device *cdev)
{
	struct raid_component *rc = container_of(cdev, struct raid_component,
						 cdev);
	dev_printk(KERN_ERR, rc->cdev.dev, "COMPONENT RELEASE\n");
	put_device(rc->cdev.dev);
	kfree(rc);
}

void raid_component_add(struct raid_template *r,struct device *raid_dev,
			struct device *component_dev)
{
	struct class_device *cdev =
		attribute_container_find_class_device(&r->raid_attrs.ac,
						      raid_dev);
	struct raid_component *rc;
	struct raid_data *rd = class_get_devdata(cdev);

	rc = kzalloc(sizeof(*rc), GFP_KERNEL);
	if (!rc)
		return;

	INIT_LIST_HEAD(&rc->node);
	class_device_initialize(&rc->cdev);
	rc->cdev.release = raid_component_release;
	rc->cdev.dev = get_device(component_dev);
	rc->num = rd->component_count++;

	snprintf(rc->cdev.class_id, sizeof(rc->cdev.class_id),
		 "component-%d", rc->num);
	list_add_tail(&rc->node, &rd->component_list);
	rc->cdev.parent = cdev;
	rc->cdev.class = &raid_class.class;
	class_device_add(&rc->cdev);
}
EXPORT_SYMBOL(raid_component_add);

struct raid_template *
raid_class_attach(struct raid_function_template *ft)
{
	struct raid_internal *i = kzalloc(sizeof(struct raid_internal),
					  GFP_KERNEL);
	int count = 0;

	if (unlikely(!i))
		return NULL;

	i->f = ft;

	i->r.raid_attrs.ac.class = &raid_class.class;
	i->r.raid_attrs.ac.match = raid_match;
	i->r.raid_attrs.ac.attrs = &i->attrs[0];

	attribute_container_register(&i->r.raid_attrs.ac);

	i->attrs[count++] = &class_device_attr_level;
	i->attrs[count++] = &class_device_attr_resync;
	i->attrs[count++] = &class_device_attr_state;

	i->attrs[count] = NULL;
	BUG_ON(count > RAID_NUM_ATTRS);

	return &i->r;
}
EXPORT_SYMBOL(raid_class_attach);

void
raid_class_release(struct raid_template *r)
{
	struct raid_internal *i = to_raid_internal(r);

	attribute_container_unregister(&i->r.raid_attrs.ac);

	kfree(i);
}
EXPORT_SYMBOL(raid_class_release);

static __init int raid_init(void)
{
	return transport_class_register(&raid_class);
}

static __exit void raid_exit(void)
{
	transport_class_unregister(&raid_class);
}

MODULE_AUTHOR("James Bottomley");
MODULE_DESCRIPTION("RAID device class");
MODULE_LICENSE("GPL");

module_init(raid_init);
module_exit(raid_exit);

