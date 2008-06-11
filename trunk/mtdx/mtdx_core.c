/*
 *  MTDX core
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "mtdx_common.h"
#include <linux/module.h>
#include <linux/idr.h>
#include <linux/err.h>

static DEFINE_IDR(mtdx_dev_idr);
static DEFINE_SPINLOCK(mtdx_dev_lock);

static void mtdx_free_dev(struct device *dev)
{
	struct mtdx_dev *mdev = container_of(dev, struct mtdx_dev, dev);
	struct mtdx_dev *mdev_r;

	spin_lock(&mtdx_dev_lock);
	mdev_r = idr_find(&mtdx_dev_idr, mdev->ord);
	if (mdev_r == mdev)
		idr_remove(&mtdx_dev_idr, mdev->ord);
	spin_unlock(&mtdx_dev_lock);

	kfree(mdev);
}

static int mtdx_dev_match(struct mtdx_dev *mdev, struct mtdx_device_id *id)
{
	if (!id->role || !id->id)
		return 0;

	if (id->role != mdev->id.role)
		return 0;

	if (id->w_policy != mdev->id.w_policy)
		return 0;

	if (id->id != mdev->id.id)
		return 0;

	return 1;
}

static int mtdx_bus_match(struct device *dev, struct device_driver *drv)
{
	struct mtdx_dev *mdev = container_of(dev, struct mtdx_dev, dev);
	struct mtdx_driver *mdrv = container_of(drv, struct mtdx_driver,
						driver);
	struct mtdx_device_id *ids = mdrv->id_table;

	if (ids) {
		while (ids->id) {
			if (mtdx_dev_match(mdev, ids))
				return 1;
			++ids;
		}
	}
	return 0;
}


static int mtdx_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct mtdx_dev *mdev = container_of(dev, struct mtdx_dev, dev);

	if (add_uevent_var(env, "MTDX_ROLE=%02X", mdev->id.role))
		return -ENOMEM;

	if (add_uevent_var(env, "MTDX_W_POLICY=%02X", mdev->id.w_policy))
		return -ENOMEM;

	if (add_uevent_var(env, "MTDX_ID=%04X", mdev->id.id))
		return -ENOMEM;

	return 0;
}

static int mtdx_device_probe(struct device *dev)
{
	struct mtdx_dev *mdev = container_of(dev, struct mtdx_dev, dev);
	struct mtdx_driver *drv = container_of(dev->driver, struct mtdx_driver,
					       driver);
	int rc = -ENODEV;

	if (dev->driver && drv->probe) {
		rc = drv->probe(mdev);
		if (!rc)
			get_device(dev);
        }
	return rc;
}

static int mtdx_device_remove(struct device *dev)
{
	struct mtdx_dev *mdev = container_of(dev, struct mtdx_dev, dev);
	struct mtdx_driver *drv = container_of(dev->driver, struct mtdx_driver,
					       driver);

	if (dev->driver && drv->remove) {
		drv->remove(mdev);
		mdev->dev.driver = NULL;
	}

	put_device(dev);
	return 0;
}

#ifdef CONFIG_PM

static int mtdx_device_suspend(struct device *dev, pm_message_t state)
{
	struct mtdx_dev *mdev = container_of(dev, struct mtdx_dev, dev);
        struct mtdx_driver *drv = container_of(dev->driver, struct mtdx_driver,
					       driver);

	if (dev->driver && drv->suspend)
		return drv->suspend(mdev, state);

	return 0;
}

static int mtdx_device_resume(struct device *dev)
{
	struct mtdx_dev *mdev = container_of(dev, struct mtdx_dev, dev);
	struct mtdx_driver *drv = container_of(dev->driver, struct mtdx_driver,
					       driver);

	if (dev->driver && drv->resume)
		return drv->resume(mdev);
        return 0;
}

#else

#define memstick_device_suspend NULL
#define memstick_device_resume NULL

#endif /* CONFIG_PM */

#define MTDX_ATTR(name, format)                                               \
static ssize_t name##_show(struct device *dev, struct device_attribute *attr, \
                            char *buf)                                        \
{                                                                             \
	struct mtdx_dev *mdev = container_of(dev, struct mtdx_dev,            \
					     dev);                            \
	return sprintf(buf, format, mdev->id.name);                           \
}

MTDX_ATTR(role, "%02X");
MTDX_ATTR(w_policy, "%02X");
MTDX_ATTR(id, "%02X");

#define MTDX_ATTR_RO(name) __ATTR(name, S_IRUGO, name##_show, NULL)

static struct device_attribute mtdx_dev_attrs[] = {
	MTDX_ATTR_RO(role),
	MTDX_ATTR_RO(w_policy),
	MTDX_ATTR_RO(id),
	__ATTR_NULL
};

static struct bus_type mtdx_bus_type = {
	.name           = "mtdx",
	.dev_attrs      = mtdx_dev_attrs,
	.match          = mtdx_bus_match,
	.uevent         = mtdx_uevent,
	.probe          = mtdx_device_probe,
	.remove         = mtdx_device_remove,
	.suspend        = mtdx_device_suspend,
	.resume         = mtdx_device_resume

};

int mtdx_register_driver(struct mtdx_driver *drv)
{
	drv->driver.bus = &mtdx_bus_type;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL(mtdx_register_driver);

void mtdx_unregister_driver(struct mtdx_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(mtdx_unregister_driver);

/**
 * Allocate "parent" MTDX device.
 */
struct mtdx_dev *mtdx_create_dev(struct device *parent,
				 struct mtdx_device_id *id)
{
	struct mtdx_dev *mdev = kzalloc(sizeof(struct mtdx_dev), GFP_KERNEL);
	int rc;

	if (!mdev)
		return ERR_PTR(-ENOMEM);

	if (!id->role || !id->id) {
		rc = -EINVAL;
		goto err_out;
	}

	mdev->id = *id;

	if (!idr_pre_get(&mtdx_dev_idr, GFP_KERNEL)) {
		rc = -ENOMEM;
		goto err_out;
	}

	spin_lock(&mtdx_dev_lock);
	rc = idr_get_new(&mtdx_dev_idr, mdev, &mdev->ord);
	spin_unlock(&mtdx_dev_lock);

	if (rc)
		goto err_out;

	mdev->dev.parent = parent;
	mdev->dev.bus = &mtdx_bus_type;
	mdev->dev.release = mtdx_free_dev;

	snprintf(mdev->dev.bus_id, sizeof(mdev->dev.bus_id),
		 "mtdx%d", mdev->ord);

	rc = device_register(&mdev->dev);
	if (!rc)
		return mdev;

	spin_lock(&mtdx_dev_lock);
	idr_remove(&mtdx_dev_idr, mdev->ord);
	spin_unlock(&mtdx_dev_lock);

err_out:
	kfree(mdev);
	return ERR_PTR(rc);
}
EXPORT_SYMBOL(mtdx_create_dev);

/**
 * Allocate "child" MTDX device.
 */

struct mtdx_dev *mtdx_create_child(struct mtdx_dev *parent, unsigned int ord,
				   struct mtdx_device_id *id)
{
	struct mtdx_dev *mdev = kzalloc(sizeof(struct mtdx_dev), GFP_KERNEL);
	const char *c_code;
	int rc;

	if (!mdev)
		return ERR_PTR(-ENOMEM);

	if (!id->role || !id->id) {
		rc = -EINVAL;
		goto err_out;
	}

	mdev->id = *id;

	mdev->dev.parent = &parent->dev;
	mdev->dev.bus = &mtdx_bus_type;
	mdev->dev.release = mtdx_free_dev;
	mdev->ord = ord;

	switch (id->role) {
	case MTDX_ROLE_MTD:
		if (!id->w_policy)
			c_code = "rom";
		else
			c_code = "ram";
		break;
	case MTDX_ROLE_FTL:
		c_code = "t";
		break;
	case MTDX_ROLE_BLK:
		c_code = "b";
		break;
	default:
		c_code = "g";
	}

	snprintf(mdev->dev.bus_id, sizeof(mdev->dev.bus_id), "%s%s%d",
			 parent->dev.bus_id, c_code, ord);

	rc = device_register(&mdev->dev);
	if (!rc)
		return mdev;

err_out:
	kfree(mdev);
	return ERR_PTR(rc);
}
EXPORT_SYMBOL(mtdx_create_child);

static int __init mtdx_init(void)
{
	int rc;

	rc = bus_register(&mtdx_bus_type);

	return rc;
}

static void __exit mtdx_exit(void)
{
	bus_unregister(&mtdx_bus_type);
	idr_destroy(&mtdx_dev_idr);
}

module_init(mtdx_init);
module_exit(mtdx_exit);

MODULE_AUTHOR("Alex Dubov");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MTDX core driver");
