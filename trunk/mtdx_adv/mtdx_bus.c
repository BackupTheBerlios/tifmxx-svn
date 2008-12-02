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

static DEFINE_IDA(mtdx_dev_ida);
static DEFINE_MUTEX(mtdx_dev_lock);

static struct device_type mtdx_type = {
	.name = "mtdx"
};


static void mtdx_free_dev(struct device *dev)
{
	struct mtdx_dev *mdev = container_of(dev, struct mtdx_dev, dev);

	kfree(mdev);
}

static int mtdx_dev_match(struct mtdx_dev *mdev, struct mtdx_device_id *id)
{
	return !memcmp(id, &mdev->id, sizeof(struct mtdx_device_id));
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

	if (add_uevent_var(env, "MTDX_INP_WMODE=%02X", mdev->id.inp_wmode))
		return -ENOMEM;
	if (add_uevent_var(env, "MTDX_OUT_WMODE=%02X", mdev->id.out_wmode))
		return -ENOMEM;
	if (add_uevent_var(env, "MTDX_INP_RMODE=%02X", mdev->id.inp_rmode))
		return -ENOMEM;
	if (add_uevent_var(env, "MTDX_OUT_RMODE=%02X", mdev->id.out_rmode))
		return -ENOMEM;

	if (add_uevent_var(env, "MTDX_TYPE=%04X", mdev->id.type))
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

	mutex_lock(&mtdx_dev_lock);
	ida_remove(&mtdx_dev_ida, mdev->ord);
	mutex_unlock(&mtdx_dev_lock);

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

#define mtdx_device_suspend NULL
#define mtdx_device_resume NULL

#endif /* CONFIG_PM */

struct mtdx_print_buf {
	char         *buf;
	unsigned int size;
	unsigned int offset;
};

static int mtdx_print_child_id(struct device *dev, void *data)
{
	struct mtdx_dev *cdev = container_of(dev, struct mtdx_dev, dev);
	struct mtdx_print_buf *pb = data;
	int rc;

	if (pb->offset >= pb->size)
		return -EAGAIN;

	/* Skip non-mtdx children */
	if (dev->type != &mtdx_type)
		return 0;

	rc = scnprintf(pb->buf + pb->offset, pb->size - pb->offset,
		       "mtdx%d: iw%02xow%02xir%02xor%02xt%04xi%04x\n",
		       cdev->ord, cdev->id.inp_wmode, cdev->id.out_wmode,
		       cdev->id.inp_rmode, cdev->id.out_rmode, cdev->id.type,
		       cdev->id.id);

	if (rc < 0)
		return rc;

	pb->offset += rc;
	return 0;
}

static ssize_t mtdx_children_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct mtdx_print_buf pb = { buf, PAGE_SIZE, 0 };
	ssize_t rc;

	rc = device_for_each_child(dev, &pb, mtdx_print_child_id);

	if (rc >= 0)
		rc = pb.offset;

	return rc;
}

static ssize_t mtdx_children_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct mtdx_device_id id;
	struct mtdx_dev *pdev = container_of(dev, struct mtdx_dev, dev);
	struct mtdx_dev *mdev;
	int rc;

	if (count == strnlen(buf, count))
		return -EINVAL;

	rc = sscanf(buf, "iw%hhxow%hhxir%hhxor%hhxt%hxi%hx", &id.inp_wmode,
		    &id.out_wmode, &id.inp_rmode, &id.out_rmode,
		    &id.type, &id.id);

	if (rc != 6)
		return -EINVAL;

	if ((pdev->id.inp_wmode != id.out_wmode)
	    || (pdev->id.inp_rmode != id.out_rmode)) {
		dev_err(dev, "Couldn't attach child device with "
			"out_wmode %02x, out_rmode %02x\n", id.out_wmode,
			id.out_rmode);
		return -ENODEV;
	}

	mdev = mtdx_alloc_dev(dev, &id);
	if (!mdev)
		return -ENOMEM;

	rc = device_add(&mdev->dev);
	if (rc) {
		__mtdx_free_dev(mdev);
		return rc;
	}
	return count;
}

#define MTDX_ATTR(name, format)                                      \
static ssize_t mtdx_ ## name ## _show(struct device *dev,            \
				      struct device_attribute *attr, \
				      char *buf)                     \
{                                                                    \
	struct mtdx_dev *mdev = container_of(dev, struct mtdx_dev,   \
					     dev);                   \
	return sprintf(buf, format, mdev->id.name);                  \
}

MTDX_ATTR(inp_wmode, "%02X");
MTDX_ATTR(out_wmode, "%02X");
MTDX_ATTR(inp_rmode, "%02X");
MTDX_ATTR(out_rmode, "%02X");
MTDX_ATTR(type, "%04X");
MTDX_ATTR(id, "%04X");

#define MTDX_ATTR_RO(name) __ATTR(name, S_IRUGO, mtdx_ ## name ## _show, NULL)

static struct device_attribute mtdx_dev_attrs[] = {
	MTDX_ATTR_RO(inp_wmode),
	MTDX_ATTR_RO(out_wmode),
	MTDX_ATTR_RO(inp_rmode),
	MTDX_ATTR_RO(out_rmode),
	MTDX_ATTR_RO(type),
	MTDX_ATTR_RO(id),
	__ATTR(children, (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH),
	       mtdx_children_show, mtdx_children_store),
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

struct mtdx_dev *mtdx_alloc_dev(struct device *parent,
				const struct mtdx_device_id *id)
{
	struct mtdx_dev *mdev = kzalloc(sizeof(struct mtdx_dev), GFP_KERNEL);
	int rc;

	if (!mdev)
		return NULL;

	if (!ida_pre_get(&mtdx_dev_ida, GFP_KERNEL))
		goto err_out_free;

	mutex_lock(&mtdx_dev_lock);
	rc = ida_get_new(&mtdx_dev_ida, &mdev->ord);
	mutex_unlock(&mtdx_dev_lock);

	if (rc)
		goto err_out_free;

	memcpy(&mdev->id, id, sizeof(struct mtdx_device_id));
	mdev->dev.parent = parent;
	mdev->dev.bus = &mtdx_bus_type;
	mdev->dev.release = mtdx_free_dev;
	mdev->dev.type = &mtdx_type;
	INIT_LIST_HEAD(&mdev->q_node);

	snprintf(mdev->dev.bus_id, sizeof(mdev->dev.bus_id),
		 "mtdx%d", mdev->ord);
	device_initialize(&mdev->dev);

	return mdev;

err_out_free:
	kfree(mdev);
	return NULL;
}
EXPORT_SYMBOL(mtdx_alloc_dev);

void __mtdx_free_dev(struct mtdx_dev *mdev)
{
	if (mdev) {
		mutex_lock(&mtdx_dev_lock);
		ida_remove(&mtdx_dev_ida, mdev->ord);
		mutex_unlock(&mtdx_dev_lock);
		kfree(mdev);
	}
}
EXPORT_SYMBOL(__mtdx_free_dev);

static int mtdx_child_unregister(struct device *dev, void *data)
{
	if (dev->type == &mtdx_type)
		device_unregister(dev);

	return 0;
}

void mtdx_drop_children(struct mtdx_dev *mdev)
{
	device_for_each_child(&mdev->dev, NULL, mtdx_child_unregister);
}
EXPORT_SYMBOL(mtdx_drop_children);

int mtdx_page_list_append(struct list_head *head, struct mtdx_page_info *info)
{
	struct list_head *p = head;
	struct mtdx_page_info *c_info;

	if (!list_empty(head)) {
		list_for_each (p, head) {
			c_info = list_entry(p, struct mtdx_page_info, node);
			if (c_info->phy_block < info->phy_block)
				continue;
			else if (c_info->phy_block > info->phy_block)
				break;
			else {
				if (c_info->page_offset < info->page_offset)
					continue;
				else if (c_info->page_offset
					 > info->page_offset)
					break;
				else {
					c_info->status = info->status;
					c_info->log_block = info->log_block;
					return 0;
				}
			}
		}
	}

	c_info = kmalloc(sizeof(struct mtdx_page_info), GFP_KERNEL);
	if (!c_info)
		return -ENOMEM;

	INIT_LIST_HEAD(&c_info->node);
	c_info->status = info->status;
	c_info->phy_block = info->phy_block;
	c_info->log_block = info->log_block;
	c_info->page_offset = info->page_offset;
	list_add_tail(&c_info->node, p);
	return 0;
}
EXPORT_SYMBOL(mtdx_page_list_append);

void mtdx_page_list_free(struct list_head *head)
{
	struct mtdx_page_info *c_info;

	while (!list_empty(head)) {
		c_info = list_first_entry(head, struct mtdx_page_info, node);
		list_del(&c_info->node);
		kfree(c_info);
	}
}
EXPORT_SYMBOL(mtdx_page_list_free);

void mtdx_dev_queue_push_back(struct mtdx_dev_queue *devq,
                              struct mtdx_dev *mdev)
{
	struct list_head *p;
	unsigned long flags;

	spin_lock_irqsave(&devq->lock, flags);
	__list_for_each(p, &devq->head) {
		if (p == &mdev->q_node)
			goto out;
	}
	list_add_tail(&mdev->q_node, &devq->head);
out:
	spin_unlock_irqrestore(&devq->lock, flags);
}
EXPORT_SYMBOL(mtdx_dev_queue_push_back);

struct mtdx_dev *mtdx_dev_queue_pop_front(struct mtdx_dev_queue *devq)
{
	struct mtdx_dev *rv = NULL;
	unsigned long flags;

	spin_lock_irqsave(&devq->lock, flags);
	if (!list_empty(&devq->head)) {
		rv = list_first_entry(&devq->head, struct mtdx_dev, q_node);
		list_del(&rv->q_node);
		INIT_LIST_HEAD(&rv->q_node);
	}
	spin_unlock_irqrestore(&devq->lock, flags);
	return rv;
}
EXPORT_SYMBOL(mtdx_dev_queue_pop_front);

int mtdx_dev_queue_empty(struct mtdx_dev_queue *devq)
{
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&devq->lock, flags);
	rc = list_empty(&devq->head);
	spin_unlock_irqrestore(&devq->lock, flags);
	return rc;
}
EXPORT_SYMBOL(mtdx_dev_queue_empty);

int bitmap_region_empty(unsigned long *bitmap, unsigned int offset,
			unsigned int length)
{
	unsigned long w_b, w_e, m_b, m_e, cnt;

	w_b = offset / BITS_PER_LONG;
	w_e = (offset + length) / BITS_PER_LONG;

	m_b = ~((1UL << (offset % BITS_PER_LONG)) - 1UL);
	m_e = (1UL << ((offset + length) % BITS_PER_LONG)) - 1UL;

	if (w_b == w_e) {
		return bitmap[w_b] & (m_b ^ m_e) ? 0 : 1;
	} else {
		if (bitmap[w_b] & m_b)
			return 0;

		if (m_e && (bitmap[w_e] & m_e))
			return 0;

		for (cnt = w_b + 1; cnt < w_e; ++cnt)
			if (bitmap[cnt])
				return 0;

		return 1;
	}
}
EXPORT_SYMBOL(bitmap_region_empty);

void bitmap_clear_region(unsigned long *bitmap, unsigned int offset,
			 unsigned int length)
{
	unsigned long w_b, w_e, m_b, m_e, cnt;

	w_b = offset / BITS_PER_LONG;
	w_e = (offset + length) / BITS_PER_LONG;

	m_b = ~((1UL << (offset % BITS_PER_LONG)) - 1UL);
	m_e = (1UL << ((offset + length) % BITS_PER_LONG)) - 1UL;

	if (w_b == w_e) {
		bitmap[w_b] &= ~(m_b & m_e);
	} else {
		bitmap[w_b] &= ~m_b;

		if (m_e)
			bitmap[w_e] &= ~m_e;

		for (cnt = w_b + 1; cnt < w_e; ++cnt)
			bitmap[cnt] = 0UL;
	}
}
EXPORT_SYMBOL(bitmap_clear_region);

void bitmap_set_region(unsigned long *bitmap, unsigned int offset,
		       unsigned int length)
{
	unsigned long w_b, w_e, m_b, m_e, cnt;

	w_b = offset / BITS_PER_LONG;
	w_e = (offset + length) / BITS_PER_LONG;

	m_b = ~((1UL << (offset % BITS_PER_LONG)) - 1UL);
	m_e = (1UL << ((offset + length) % BITS_PER_LONG)) - 1UL;

	if (w_b == w_e) {
		bitmap[w_b] |= m_b & m_e;
	} else {
		bitmap[w_b] |= m_b;

		if (m_e)
			bitmap[w_e] |= m_e;

		for (cnt = w_b + 1; cnt < w_e; ++cnt)
			bitmap[cnt] = ~0UL;
	}
}
EXPORT_SYMBOL(bitmap_set_region);

static int __init mtdx_init(void)
{
	int rc;

	rc = bus_register(&mtdx_bus_type);

	return rc;
}

static void __exit mtdx_exit(void)
{
	bus_unregister(&mtdx_bus_type);
	ida_destroy(&mtdx_dev_ida);
}

module_init(mtdx_init);
module_exit(mtdx_exit);

MODULE_AUTHOR("Alex Dubov");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MTDX core driver");
