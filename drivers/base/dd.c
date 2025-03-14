/*
 *	drivers/base/dd.c - The core device/driver interactions.
 *
 * 	This file contains the (sometimes tricky) code that controls the
 *	interactions between devices and drivers, which primarily includes
 *	driver binding and unbinding.
 *
 *	All of this code used to exist in drivers/base/bus.c, but was
 *	relocated to here in the name of compartmentalization (since it wasn't
 *	strictly code just for the 'struct bus_type'.
 *
 *	Copyright (c) 2002-5 Patrick Mochel
 *	Copyright (c) 2002-3 Open Source Development Labs
 *
 *	This file is released under the GPLv2
 */

#include <linux/device.h>
#include <linux/module.h>

#include "base.h"
#include "power/power.h"

#define to_drv(node) container_of(node, struct device_driver, kobj.entry)


/**
 *	device_bind_driver - bind a driver to one device.
 *	@dev:	device.
 *
 *	Allow manual attachment of a driver to a device.
 *	Caller must have already set @dev->driver.
 *
 *	Note that this does not modify the bus reference count
 *	nor take the bus's rwsem. Please verify those are accounted
 *	for before calling this. (It is ok to call with no other effort
 *	from a driver's probe() method.)
 *
 *	This function must be called with @dev->sem held.
 */
void device_bind_driver(struct device * dev)
{
	if (klist_node_attached(&dev->knode_driver))
		return;

	pr_debug("bound device '%s' to driver '%s'\n",
		 dev->bus_id, dev->driver->name);
	klist_add_tail(&dev->knode_driver, &dev->driver->klist_devices);
	sysfs_create_link(&dev->driver->kobj, &dev->kobj,
			  kobject_name(&dev->kobj));
	sysfs_create_link(&dev->kobj, &dev->driver->kobj, "driver");
}

/**
 *	driver_probe_device - attempt to bind device & driver.
 *	@drv:	driver.
 *	@dev:	device.
 *
 *	First, we call the bus's match function, if one present, which
 *	should compare the device IDs the driver supports with the
 *	device IDs of the device. Note we don't do this ourselves
 *	because we don't know the format of the ID structures, nor what
 *	is to be considered a match and what is not.
 *
 *	This function returns 1 if a match is found, an error if one
 *	occurs (that is not -ENODEV or -ENXIO), and 0 otherwise.
 *
 *	This function must be called with @dev->sem held.  When called
 *	for a USB interface, @dev->parent->sem must be held as well.
 */
int driver_probe_device(struct device_driver * drv, struct device * dev)
{
	int ret = 0;

	if (drv->bus->match && !drv->bus->match(dev, drv))
		goto Done;

	pr_debug("%s: Matched Device %s with Driver %s\n",
		 drv->bus->name, dev->bus_id, drv->name);
	dev->driver = drv;
	if (drv->probe) {
		ret = drv->probe(dev);
		if (ret) {
			dev->driver = NULL;
			goto ProbeFailed;
		}
	}
	device_bind_driver(dev);
	ret = 1;
	pr_debug("%s: Bound Device %s to Driver %s\n",
		 drv->bus->name, dev->bus_id, drv->name);
	goto Done;

 ProbeFailed:
	if (ret == -ENODEV || ret == -ENXIO) {
		/* Driver matched, but didn't support device
		 * or device not found.
		 * Not an error; keep going.
		 */
		ret = 0;
	} else {
		/* driver matched but the probe failed */
		printk(KERN_WARNING
		       "%s: probe of %s failed with error %d\n",
		       drv->name, dev->bus_id, ret);
	}
 Done:
	return ret;
}

static int __device_attach(struct device_driver * drv, void * data)
{
	struct device * dev = data;
	return driver_probe_device(drv, dev);
}

/**
 *	device_attach - try to attach device to a driver.
 *	@dev:	device.
 *
 *	Walk the list of drivers that the bus has and call
 *	driver_probe_device() for each pair. If a compatible
 *	pair is found, break out and return.
 *
 *	Returns 1 if the device was bound to a driver;
 *	0 if no matching device was found; error code otherwise.
 *
 *	When called for a USB interface, @dev->parent->sem must be held.
 */
int device_attach(struct device * dev)
{
	int ret = 0;

	down(&dev->sem);
	if (dev->driver) {
		device_bind_driver(dev);
		ret = 1;
	} else
		ret = bus_for_each_drv(dev->bus, NULL, dev, __device_attach);
	up(&dev->sem);
	return ret;
}

static int __driver_attach(struct device * dev, void * data)
{
	struct device_driver * drv = data;

	/*
	 * Lock device and try to bind to it. We drop the error
	 * here and always return 0, because we need to keep trying
	 * to bind to devices and some drivers will return an error
	 * simply if it didn't support the device.
	 *
	 * driver_probe_device() will spit a warning if there
	 * is an error.
	 */

	if (dev->parent)	/* Needed for USB */
		down(&dev->parent->sem);
	down(&dev->sem);
	if (!dev->driver)
		driver_probe_device(drv, dev);
	up(&dev->sem);
	if (dev->parent)
		up(&dev->parent->sem);

	return 0;
}

/**
 *	driver_attach - try to bind driver to devices.
 *	@drv:	driver.
 *
 *	Walk the list of devices that the bus has on it and try to
 *	match the driver with each one.  If driver_probe_device()
 *	returns 0 and the @dev->driver is set, we've found a
 *	compatible pair.
 */
void driver_attach(struct device_driver * drv)
{
	bus_for_each_dev(drv->bus, NULL, drv, __driver_attach);
}

/**
 *	device_release_driver - manually detach device from driver.
 *	@dev:	device.
 *
 *	Manually detach device from driver.
 *
 *	__device_release_driver() must be called with @dev->sem held.
 *	When called for a USB interface, @dev->parent->sem must be held
 *	as well.
 */

static void __device_release_driver(struct device * dev)
{
	struct device_driver * drv;

	drv = dev->driver;
	if (drv) {
		get_driver(drv);
		sysfs_remove_link(&drv->kobj, kobject_name(&dev->kobj));
		sysfs_remove_link(&dev->kobj, "driver");
		klist_remove(&dev->knode_driver);

		if (drv->remove)
			drv->remove(dev);
		dev->driver = NULL;
		put_driver(drv);
	}
}

void device_release_driver(struct device * dev)
{
	/*
	 * If anyone calls device_release_driver() recursively from
	 * within their ->remove callback for the same device, they
	 * will deadlock right here.
	 */
	down(&dev->sem);
	__device_release_driver(dev);
	up(&dev->sem);
}


/**
 * driver_detach - detach driver from all devices it controls.
 * @drv: driver.
 */
void driver_detach(struct device_driver * drv)
{
	struct device * dev;

	for (;;) {
		spin_lock(&drv->klist_devices.k_lock);
		if (list_empty(&drv->klist_devices.k_list)) {
			spin_unlock(&drv->klist_devices.k_lock);
			break;
		}
		dev = list_entry(drv->klist_devices.k_list.prev,
				struct device, knode_driver.n_node);
		get_device(dev);
		spin_unlock(&drv->klist_devices.k_lock);

		if (dev->parent)	/* Needed for USB */
			down(&dev->parent->sem);
		down(&dev->sem);
		if (dev->driver == drv)
			__device_release_driver(dev);
		up(&dev->sem);
		if (dev->parent)
			up(&dev->parent->sem);
		put_device(dev);
	}
}


EXPORT_SYMBOL_GPL(device_bind_driver);
EXPORT_SYMBOL_GPL(device_release_driver);
EXPORT_SYMBOL_GPL(device_attach);
EXPORT_SYMBOL_GPL(driver_attach);

