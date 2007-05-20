/*
 *  memstick.c - Sony MemoryStick support
 *
 *  Copyright (C) 2006 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Special thanks to Carlos Corbacho for providing various MemoryStick cards
 * that made this driver possible.
 *
 */

#include "linux/tifm.h"
#include "linux/memstick.h"
#include <linux/idr.h>
#include <linux/scatterlist.h>

static unsigned int cmd_retries = 3;
module_param(cmd_retries, uint, 0644);

static struct workqueue_struct *workqueue;
static DEFINE_IDR(memstick_host_idr);
static DEFINE_SPINLOCK(memstick_host_lock);

static int memstick_dev_match(struct memstick_dev *card,
			      struct memstick_device_id *id)
{
	if (id->match_flags & MEMSTICK_MATCH_ALL) {
		if ((id->type == card->id.type)
		    && (id->category == card->id.category)
		    && (id->class == card->id.class))
			return 1;
	}

	return 0;
}

static int memstick_bus_match(struct device *dev, struct device_driver *drv)
{
	struct memstick_dev *card = container_of(dev, struct memstick_dev,
						 dev);
	struct memstick_driver *ms_drv = container_of(drv,
						      struct memstick_driver,
						      driver);
	struct memstick_device_id *ids = ms_drv->id_table;

	if (ids) {
		while (ids->match_flags) {
			if (memstick_dev_match(card, ids))
				return 1;
			++ids;
		}
	}
	return 0;
}

static int memstick_uevent(struct device *dev, char **envp, int num_envp,
			   char *buffer, int buffer_size)
{
	struct memstick_dev *card = container_of(dev, struct memstick_dev,
						  dev);
	int i = 0;
	int length = 0;

	if (add_uevent_var(envp, num_envp, &i, buffer, buffer_size, &length,
			   "MEMSTICK_TYPE=%02X", card->id.type))
		return -ENOMEM;
	if (add_uevent_var(envp, num_envp, &i, buffer, buffer_size, &length,
			   "MEMSTICK_CATEGORY=%02X", card->id.category))
		return -ENOMEM;
	if (add_uevent_var(envp, num_envp, &i, buffer, buffer_size, &length,
			   "MEMSTICK_CLASS=%02X", card->id.class))
		return -ENOMEM;

	return 0;
}

static int memstick_device_probe(struct device *dev)
{
	struct memstick_dev *card = container_of(dev, struct memstick_dev,
						 dev);
	struct memstick_driver *drv = container_of(dev->driver,
						   struct memstick_driver,
						   driver);
	int rc = -ENODEV;

	get_device(dev);
	if (dev->driver && drv->probe) {
		rc = drv->probe(card);
		if (!rc)
			return 0;
	}
	put_device(dev);
	return rc;
}

static int memstick_device_remove(struct device *dev)
{
	struct memstick_dev *card = container_of(dev, struct memstick_dev,
						  dev);
	struct memstick_driver *drv = container_of(dev->driver,
						   struct memstick_driver,
						   driver);

	if (dev->driver && drv->remove) {
		drv->remove(card);
		card->dev.driver = NULL;
	}

	put_device(dev);
	return 0;
}

#ifdef CONFIG_PM

static int memstick_device_suspend(struct device *dev, pm_message_t state)
{
	struct memstick_dev *card = container_of(dev, struct memstick_dev,
						  dev);
	struct memstick_driver *drv = container_of(dev->driver,
						   struct memstick_driver,
						   driver);

	if (dev->driver && drv->suspend)
		return drv->suspend(card, state);
	return 0;
}

static int memstick_device_resume(struct device *dev)
{
	struct memstick_dev *card = container_of(dev, struct memstick_dev,
						  dev);
	struct memstick_driver *drv = container_of(dev->driver,
						   struct memstick_driver,
						   driver);

	if (dev->driver && drv->resume)
		return drv->resume(card);
	return 0;
}

#else

#define tifm_device_suspend NULL
#define tifm_device_resume NULL

#endif /* CONFIG_PM */

#define MEMSTICK_ATTR(name, format)                                           \
static ssize_t name##_show(struct device *dev, struct device_attribute *attr, \
			    char *buf)                                        \
{                                                                             \
	struct memstick_dev *card = container_of(dev, struct memstick_dev,    \
						 dev);                        \
	return sprintf(buf, format, card->id.name);                           \
}

MEMSTICK_ATTR(type, "%02X");
MEMSTICK_ATTR(category, "%02X");
MEMSTICK_ATTR(class, "%02X");

#define MEMSTICK_ATTR_RO(name) __ATTR(name, S_IRUGO, name##_show, NULL)

static struct device_attribute memstick_dev_attrs[] = {
	MEMSTICK_ATTR_RO(type),
	MEMSTICK_ATTR_RO(category),
	MEMSTICK_ATTR_RO(class),
	__ATTR_NULL
};

static struct bus_type memstick_bus_type = {
	.name           = "memstick",
	.dev_attrs      = memstick_dev_attrs,
	.match          = memstick_bus_match,
	.uevent         = memstick_uevent,
	.probe          = memstick_device_probe,
	.remove         = memstick_device_remove,
	.suspend        = memstick_device_suspend,
	.resume         = memstick_device_resume
};

static void memstick_free(struct class_device *cdev)
{
	struct memstick_host *host = container_of(cdev, struct memstick_host,
						  cdev);
	kfree(host);
}

static struct class memstick_host_class = {
	.name       = "memstick_host",
	.release    = memstick_free
};

static void memstick_free_card(struct device *dev)
{
	struct memstick_dev *card = container_of(dev, struct memstick_dev,
						 dev);
	kfree(card);
}

static int memstick_dummy_check(struct memstick_dev *card)
{
	return MEMSTICK_ERR_INTERNAL;
}

void memstick_detect_change(struct memstick_host *host)
{
	queue_work(workqueue, &host->media_checker);
}
EXPORT_SYMBOL(memstick_detect_change);

struct memstick_request* memstick_next_req(struct memstick_host *host,
					   struct memstick_request *mrq)
{
	unsigned long flags;

	spin_lock_irqsave(&host->req_lock, flags);
	if (mrq->error) {
		host->retries--;
		if (host->retries) {
			mrq->error = MEMSTICK_ERR_INTERNAL;
			goto out;
		} else
			list_splice_init(&host->preq_list,
					 host->creq_list.prev);
	} else {
		list_move_tail(&mrq->node, &host->creq_list);
		host->retries = cmd_retries;
	}

	if (!list_empty(&host->preq_list))
		mrq = list_entry(host->preq_list.next, struct memstick_request,
				 node);
	else {
		mrq = NULL;
		wake_up_all(&host->req_wait);
	}

out:
	spin_unlock_irqrestore(&host->req_lock, flags);
	return mrq;
}
EXPORT_SYMBOL(memstick_next_req);

void memstick_queue_req(struct memstick_host *host,
			struct memstick_request *mrq)
{
	unsigned long flags;
	int restart = 0;

	spin_lock_irqsave(&host->req_lock, flags);

	if (list_empty(&host->preq_list)) {
		restart = 1;
		host->retries = cmd_retries;
	}

	list_add_tail(&mrq->node, &host->preq_list);
	spin_unlock_irqrestore(&host->req_lock, flags);

	if (restart)
		host->request(host, mrq);
}
EXPORT_SYMBOL(memstick_queue_req);

static int memstick_any_completed(struct memstick_host *host)
{
	int rc = 1;
	unsigned long flags;
	
	spin_lock_irqsave(&host->req_lock, flags);
	if (list_empty(&host->creq_list))
		rc = 0;
	spin_unlock_irqrestore(&host->req_lock, flags);
	return rc;
}

struct memstick_request* memstick_get_req(struct memstick_host *host)
{
	struct memstick_request *mrq = NULL;
	unsigned long flags;

	spin_lock_irqsave(&host->req_lock, flags);

	if (!list_empty(&host->preq_list)) {
		spin_unlock_irqrestore(&host->req_lock, flags);
		wait_event(host->req_wait,
			   memstick_any_completed(host));
		spin_lock_irqsave(&host->req_lock, flags);
	}
	
	if (!list_empty(&host->creq_list)) {
		mrq = list_entry(host->creq_list.next,
				 struct memstick_request, node);
		list_del(&mrq->node);
	}
	
	spin_unlock_irqrestore(&host->req_lock, flags);
	return mrq;
}
EXPORT_SYMBOL(memstick_get_req);

void memstick_init_req_sg(struct memstick_request *mrq, unsigned char tpc,
			  struct scatterlist *sg)
{
	mrq->tpc = tpc;
	mrq->error = MEMSTICK_ERR_INTERNAL;
	mrq->length = 0;
	mrq->sg = sg;
	INIT_LIST_HEAD(&mrq->node);
	if (tpc & 8)
		mrq->data_dir = WRITE;
	else
		mrq->data_dir = READ;

	if (tpc == MS_TPC_SET_CMD || tpc == MS_TPC_EX_SET_CMD)
		mrq->need_card_int = 1;
	else
		mrq->need_card_int = 0;

	mrq->block_io = 1;
}
EXPORT_SYMBOL(memstick_init_req_sg);

void memstick_init_req(struct memstick_request *mrq, unsigned char tpc,
		       unsigned char *buf, size_t length)
{
	mrq->tpc = tpc;
	mrq->error = MEMSTICK_ERR_INTERNAL;
	mrq->length = length;
	mrq->data = buf;
	INIT_LIST_HEAD(&mrq->node);
	if (tpc & 8)
		mrq->data_dir = WRITE;
	else
		mrq->data_dir = READ;

	if (tpc == MS_TPC_SET_CMD || tpc == MS_TPC_EX_SET_CMD)
		mrq->need_card_int = 1;
	else
		mrq->need_card_int = 0;

	mrq->block_io = 0;
}
EXPORT_SYMBOL(memstick_init_req);

int memstick_set_rw_addr(struct memstick_dev *card)
{
	struct memstick_request req;

	memstick_init_req(&req, MS_TPC_SET_RW_REG_ADRS,
			  (char*)&card->reg_addr, sizeof(card->reg_addr));
	memstick_queue_req(card->host, &req);
	memstick_get_req(card->host);

	if (req.error)
		return -EIO;

	return 0;
}
EXPORT_SYMBOL(memstick_set_rw_addr);

static struct memstick_dev* memstick_alloc_card(struct memstick_host *host)
{
	struct memstick_dev *card = kzalloc(sizeof(struct memstick_dev),
					    GFP_KERNEL);
	struct memstick_request mrq;
	struct ms_register reg;

	if (card) {
		card->host = host;
		snprintf(card->dev.bus_id, sizeof(card->dev.bus_id),
			 "%s", host->cdev.class_id);
		card->dev.parent = host->cdev.dev;
		card->dev.bus = &memstick_bus_type;
		card->dev.release = memstick_free_card;
		card->check = memstick_dummy_check;

		card->reg_addr = (struct ms_register_addr){
			0, sizeof(reg) - 1, 0, sizeof(reg) - 1
		};

		if (memstick_set_rw_addr(card))
			goto err_out;

		memstick_init_req(&mrq, MS_TPC_READ_REG, (char*)&reg,
				  sizeof(reg) - 1);
		memstick_queue_req(host, &mrq);
		memstick_get_req(host);
		if (mrq.error)
			goto err_out;

		card->id.match_flags = MEMSTICK_MATCH_ALL;
		card->id.type = reg.status.type;
		card->id.category = reg.status.category;
		card->id.class = reg.status.class;
	}
	return card;
err_out:
	kfree(card);
	return NULL;
}

static void memstick_power_off(struct memstick_host *host)
{
	host->ios.power_mode = MEMSTICK_POWER_OFF;
	host->ios.interface = MEMSTICK_SERIAL;
	host->set_ios(host, &host->ios);
}

static void memstick_power_on(struct memstick_host *host)
{
	host->ios.power_mode = MEMSTICK_POWER_ON;
	host->ios.interface = MEMSTICK_SERIAL;
	host->set_ios(host, &host->ios);
	msleep(1);
}

static void memstick_check(struct work_struct *work)
{
	struct memstick_host *host = container_of(work, struct memstick_host,
						  media_checker);
	struct memstick_dev *card;

	dev_dbg(host->cdev.dev, "memstick_check started\n");
	mutex_lock(&host->lock);
	if (MEMSTICK_POWER_ON != host->ios.power_mode)
		memstick_power_on(host);

	card = memstick_alloc_card(host);

	if (!card) {
		if (host->card) {
			device_unregister(&host->card->dev);
			host->card = NULL;
		}
	} else {
		dev_dbg(host->cdev.dev, "new card %02x, %02x, %02x\n",
			card->id.type, card->id.category, card->id.class);
		if (host->card) {
			if (!memstick_dev_match(host->card, &card->id)
			    || host->card->check(host->card)) {
				device_unregister(&host->card->dev);
				host->card = NULL;
			}
		}

		if (!host->card) {
			mutex_unlock(&host->lock);
			if (device_register(&card->dev)) {
				kfree(card);
				card = NULL;
			}
			mutex_lock(&host->lock);
			if (card)
				host->card = card;
		} else
			kfree(card);
	}

	if (!host->card)
		memstick_power_off(host);
	mutex_unlock(&host->lock);
	dev_dbg(host->cdev.dev, "memstick_check finished\n");
}

struct memstick_host *memstick_alloc_host(unsigned int extra,
					  struct device *dev)
{
	struct memstick_host *host;

	host = kzalloc(sizeof(struct memstick_host) + extra, GFP_KERNEL);
	if (host) {
		mutex_init(&host->lock);
		INIT_WORK(&host->media_checker, memstick_check);
		host->cdev.class = &memstick_host_class;
		host->cdev.dev = dev;
		class_device_initialize(&host->cdev);
		INIT_LIST_HEAD(&host->preq_list);
		INIT_LIST_HEAD(&host->creq_list);
		spin_lock_init(&host->req_lock);
		init_waitqueue_head(&host->req_wait);

	}
	return host;
}
EXPORT_SYMBOL(memstick_alloc_host);

int memstick_add_host(struct memstick_host *host)
{
	int rc;

	if (!idr_pre_get(&memstick_host_idr, GFP_KERNEL))
		return -ENOMEM;

	spin_lock(&memstick_host_lock);
	rc = idr_get_new(&memstick_host_idr, host, &host->id);
	spin_unlock(&memstick_host_lock);
	if (rc)
		return rc;

	snprintf(host->cdev.class_id, BUS_ID_SIZE,
		 "memstick%u", host->id);

	rc = class_device_add(&host->cdev);
	if (rc) {
		spin_lock(&memstick_host_lock);
		idr_remove(&memstick_host_idr, host->id);
		spin_unlock(&memstick_host_lock);
		return rc;
	}

	memstick_power_off(host);
	memstick_detect_change(host);
	return 0;
}
EXPORT_SYMBOL(memstick_add_host);

void memstick_remove_host(struct memstick_host *host)
{
	flush_workqueue(workqueue);
	mutex_lock(&host->lock);
	if (host->card)
		device_unregister(&host->card->dev);
	host->card = NULL;
	memstick_power_off(host);
	mutex_unlock(&host->lock);

	spin_lock(&memstick_host_lock);
	idr_remove(&memstick_host_idr, host->id);
	spin_unlock(&memstick_host_lock);
	class_device_del(&host->cdev);
}
EXPORT_SYMBOL(memstick_remove_host);

void memstick_free_host(struct memstick_host *host)
{
	mutex_destroy(&host->lock);
	class_device_put(&host->cdev);
}
EXPORT_SYMBOL(memstick_free_host);

int memstick_register_driver(struct memstick_driver *drv)
{
	drv->driver.bus = &memstick_bus_type;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL(memstick_register_driver);

void memstick_unregister_driver(struct memstick_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(memstick_unregister_driver);


static int __init memstick_init(void)
{
	int rc;

	workqueue = create_singlethread_workqueue("kmemstick");
	if (!workqueue)
		return -ENOMEM;

	rc = bus_register(&memstick_bus_type);
	if (!rc)
		rc = class_register(&memstick_host_class);

	if (!rc)
		return 0;

	bus_unregister(&memstick_bus_type);
	destroy_workqueue(workqueue);

	return rc;
}

static void __exit memstick_exit(void)
{
	class_unregister(&memstick_host_class);
	bus_unregister(&memstick_bus_type);
	destroy_workqueue(workqueue);
}

module_init(memstick_init);
module_exit(memstick_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Sony MemoryStick core driver");
