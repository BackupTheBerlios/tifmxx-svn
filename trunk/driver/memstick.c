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

static unsigned int cmd_retries = 3;
module_param(cmd_retries, uint, 0644);

static struct workqueue_struct *workqueue;
static DEFINE_IDR(memstick_host_idr);
static DEFINE_SPINLOCK(memstick_host_lock);

static int memstick_dev_match(struct memstick_dev *card,
			      struct memstick_device_id *id)
{
	const unsigned char any_id = MEMSTICK_ANY_ID;

	if ((id->type == any_id || id->type == card->type)
	    && (id->category == any_id || id->category == card->category)
	    && (id->class == any_id || id->class == card->class))
		return 1;
	else
		return 0;
}

static int memstick_null_id(struct memstick_device_id *id)
{
	return !id->type && !id->category && !id->class;
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
		while (!memstick_null_id(ids)) {
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
			   "MEMSTICK_TYPE=%02X", card->type))
		return -ENOMEM;
	if (add_uevent_var(envp, num_envp, &i, buffer, buffer_size, &length,
			   "MEMSTICK_CATEGORY=%02X", card->category))
		return -ENOMEM;
	if (add_uevent_var(envp, num_envp, &i, buffer, buffer_size, &length,
			   "MEMSTICK_CLASS=%02X", card->class))
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
	return sprintf(buf, format, card->name);                              \
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

static memstick_error_t memstick_dummy_check(struct memstick_dev *card)
{
	return MEMSTICK_ERR_BADMEDIA;
}

static struct memstick_dev* memstick_alloc_card(struct memstick_host *host,
						struct memstick_device_id *id)
{
	struct memstick_dev *card = kzalloc(sizeof(struct memstick_dev),
					    GFP_KERNEL);

	if (card) {
		card->type = id->type;
		card->category = id->category;
		card->class = id->class;
		card->host = host;
		card->dev.parent = host->cdev.dev;
		card->dev.bus = &memstick_bus_type;
		card->dev.release = memstick_free_card;
		card->check = memstick_dummy_check;
	}
	return card;
}

void memstick_detect_change(struct memstick_host *host, unsigned long delay)
{
	queue_delayed_work(workqueue, &host->media_checker, delay);
}
EXPORT_SYMBOL(memstick_detect_change);

static void memstick_wait_done(struct memstick_request *mrq)
{
	complete(mrq->done_data);
}

void memstick_wait_for_req(struct memstick_host *host,
			   struct memstick_request *mrq)
{
	DECLARE_COMPLETION_ONSTACK(complete);

	mrq->done_data = &complete;
	mrq->done = memstick_wait_done;

	host->request(host, mrq);

	wait_for_completion(&complete);
}
EXPORT_SYMBOL(memstick_wait_for_req);

memstick_error_t memstick_read_reg(struct memstick_host *host,
				   struct ms_register *ms_reg)
{
	struct memstick_request mrq = {0};

	mrq.tpc = MS_TPC_SET_RW_REG_ADRS;
	mrq.short_data_len = 4;
	mrq.short_data[1] = sizeof(struct ms_register) - 1;
	mrq.short_data[3] = sizeof(struct ms_register) - 1;
	mrq.short_data_dir = WRITE;

	mrq.retries = cmd_retries;
	memstick_wait_for_req(host, &mrq);
	if (mrq.error)
		return mrq.error;

	mrq.tpc = MS_TPC_READ_REG;
	mrq.short_data_len = sizeof(struct ms_register) - 1;
	mrq.short_data_dir = READ;

	mrq.retries = cmd_retries;
	memstick_wait_for_req(host, &mrq);

	if (!mrq.error)
		memcpy(ms_reg, mrq.short_data, sizeof(struct ms_register));

	return mrq.error;
}
EXPORT_SYMBOL(memstick_read_reg);

memstick_error_t memstick_get_int(struct memstick_host *host,
				  unsigned char *int_reg)
{
	struct memstick_request mrq = {0};

	mrq.tpc = MS_TPC_GET_INT;
	mrq.short_data_len = 1;
	mrq.short_data_dir = READ;
	mrq.retries = cmd_retries;
	memstick_wait_for_req(host, &mrq);

	if (!mrq.error)
		*int_reg = mrq.short_data[0];

	return mrq.error;
}
EXPORT_SYMBOL(memstick_get_int);

memstick_error_t memstick_set_rw_reg_adrs(struct memstick_host *host,
					  unsigned char read_off,
					  unsigned char read_len,
					  unsigned char write_off,
					  unsigned char write_len)
{
	struct memstick_request mrq = {0};

	mrq.tpc = MS_TPC_SET_RW_REG_ADRS;
	mrq.short_data_len = 4;
	mrq.short_data[0] = read_off;
	mrq.short_data[1] = read_len;
	mrq.short_data[2] = write_off;
	mrq.short_data[3] = write_len;
	mrq.short_data_dir = WRITE;

	mrq.retries = cmd_retries;
	memstick_wait_for_req(host, &mrq);

	return mrq.error;
}
EXPORT_SYMBOL(memstick_set_rw_reg_adrs);

memstick_error_t memstick_set_cmd(struct memstick_host *host,
				  memstick_cmd_t cmd, int req_ms_int)
{
	struct memstick_request mrq = {0};

	mrq.tpc = MS_TPC_SET_CMD;
	mrq.short_data_len = 1;
	mrq.short_data[0] = cmd;
	mrq.short_data_dir = WRITE;
	mrq.need_card_int = req_ms_int ? 1 : 0;

	mrq.retries = cmd_retries;
	memstick_wait_for_req(host, &mrq);

	return mrq.error;
}
EXPORT_SYMBOL(memstick_set_cmd);

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
						  media_checker.work);
	memstick_error_t err;
	struct ms_register ms_reg;
	struct memstick_device_id media_id;
	struct memstick_dev *card;

	mutex_lock(&host->lock);
	if (MEMSTICK_POWER_ON != host->ios.power_mode)
		memstick_power_on(host);

	err = memstick_read_reg(host, &ms_reg);

	if (!err) {
		media_id.type = ms_reg.status.type;
		media_id.category = ms_reg.status.category;
		media_id.class = ms_reg.status.class;

		if (host->card && (!memstick_dev_match(host->card, &media_id)
				   || host->card->check(host->card))) {
			card = host->card;
			host->card = NULL;
			mutex_unlock(&host->lock);
			device_unregister(&card->dev);
			mutex_lock(&host->lock);
		}

		if (!host->card) {
			card = memstick_alloc_card(host, &media_id);
			if (card) {
				snprintf(card->dev.bus_id,
					 sizeof(card->dev.bus_id),
					 "%s:0", host->cdev.class_id);
				mutex_unlock(&host->lock);
				if (device_register(&card->dev)) {
					memstick_free_card(&card->dev);
					card = NULL;
				}
				mutex_lock(&host->lock);
				if (card)
					host->card = card;
			}
		}
	} else if (host->card) {
		card = host->card;
		host->card = NULL;
		mutex_unlock(&host->lock);
		device_unregister(&card->dev);
		mutex_lock(&host->lock);
	}

	if (!host->card)
		memstick_power_off(host);
	mutex_unlock(&host->lock);
}

void memstick_request_done(struct memstick_host *host,
			   struct memstick_request *mrq)
{
	if (mrq->error && mrq->retries) {
		mrq->retries--;
		mrq->error = MEMSTICK_ERR_NONE;
		host->request(host, mrq);
	} else if (mrq->done) {
		mrq->done(mrq);
	}
}
EXPORT_SYMBOL(memstick_request_done);

struct memstick_host *memstick_alloc_host(unsigned int extra,
					  struct device *dev)
{
	struct memstick_host *host;

	host = kzalloc(sizeof(struct memstick_host) + extra, GFP_KERNEL);
	if (host) {
		host->cdev.class = &memstick_host_class;
		host->cdev.dev = dev;
		class_device_initialize(&host->cdev);
		mutex_init(&host->lock);
		INIT_DELAYED_WORK(&host->media_checker, memstick_check);
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
	memstick_detect_change(host, 0);
	return 0;
}
EXPORT_SYMBOL(memstick_add_host);

void memstick_remove_host(struct memstick_host *host)
{
	flush_workqueue(workqueue);
	if (host->card)
		device_unregister(&host->card->dev);
	memstick_power_off(host);

	spin_lock(&memstick_host_lock);
	idr_remove(&memstick_host_idr, host->id);
	spin_unlock(&memstick_host_lock);
	class_device_del(&host->cdev);
}
EXPORT_SYMBOL(memstick_remove_host);

void memstick_free_host(struct memstick_host *host)
{
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
	if (rc)
		goto err_out_wq;

	rc = class_register(&memstick_host_class);

	if (!rc)
		return 0;

	bus_unregister(&memstick_bus_type);

err_out_wq:
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
