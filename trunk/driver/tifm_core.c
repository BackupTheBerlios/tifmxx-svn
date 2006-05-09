/*
 *
 *  TI FlashMedia driver
 *
 *  Copyright (C) 2006 Alex Dubov <oakad@yahoo.com>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/idr.h>

#include "tifm.h"

#define DRIVER_NAME "tifm_core"
#define DRIVER_VERSION "0.2"

static DEFINE_IDR(tifm_adapter_idr);
static DEFINE_SPINLOCK(tifm_adapter_lock);

static tifm_device_id* tifm_device_match(tifm_device_id *ids, struct tifm_dev *dev)
{
	while(*ids) {
		if(dev->media_id == *ids) return ids;
		ids++;
	}
	return 0;
}

static int tifm_match(struct device *dev, struct device_driver *drv)
{
	struct tifm_dev *fm_dev = container_of(dev, struct tifm_dev, dev);
	struct tifm_driver *fm_drv = container_of(drv, struct tifm_driver, driver);

	DBG("match called for drv %s\n", drv->name);
	if(!fm_drv->id_table) return -EINVAL;
	if(tifm_device_match(fm_drv->id_table, fm_dev)) return 1;
	return -ENODEV;
}

static int tifm_uevent(struct device *dev, char **envp, int num_envp,
		       char *buffer, int buffer_size)
{
	return -ENODEV;
}

static int tifm_suspend(struct device *dev, pm_message_t message)
{
	return 0;
}

static int tifm_resume(struct device *dev)
{
	return 0;
}

static struct bus_type tifm_bus_type = {
	.name    = "tifm",
	.match   = tifm_match,
	.uevent  = tifm_uevent,
	.suspend = tifm_suspend,
	.resume  = tifm_resume
};

static void tifm_free(struct class_device *cdev)
{
	struct tifm_adapter *fm = container_of(cdev, struct tifm_adapter, cdev);

	if(fm->sockets) kfree(fm->sockets);
	if(fm->wq) destroy_workqueue(fm->wq);
	kfree(fm);
}

static struct class tifm_adapter_class = {
	.name    = "tifm_adapter",
	.release = tifm_free
};

struct tifm_adapter* tifm_alloc_adapter(void)
{
	struct tifm_adapter *fm = kzalloc(sizeof(struct tifm_adapter), GFP_KERNEL);

	if(fm) {
		fm->cdev.class = &tifm_adapter_class; 
		spin_lock_init(&fm->lock);
		class_device_initialize(&fm->cdev);
	}
	return fm;
}
EXPORT_SYMBOL(tifm_alloc_adapter);

void tifm_free_adapter(struct tifm_adapter *fm)
{
	class_device_put(&fm->cdev);
}
EXPORT_SYMBOL(tifm_free_adapter);

int tifm_add_adapter(struct tifm_adapter *fm)
{
	int rc;

	if(!idr_pre_get(&tifm_adapter_idr, GFP_KERNEL)) return -ENOMEM;

	spin_lock(&tifm_adapter_lock);
	rc = idr_get_new(&tifm_adapter_idr, fm, &fm->id);
	spin_unlock(&tifm_adapter_lock);
	if(!rc) {
		snprintf(fm->cdev.class_id, BUS_ID_SIZE, "tifm_%x", fm->id);
		snprintf(fm->wq_name, KOBJ_NAME_LEN, "wq/tifm_%x", fm->id);

		fm->wq = create_workqueue(fm->wq_name);
		if(fm->wq) return class_device_add(&fm->cdev);
				
		spin_lock(&tifm_adapter_lock);
		idr_remove(&tifm_adapter_idr, fm->id);
		spin_unlock(&tifm_adapter_lock);
		rc = -ENOMEM;
	}
	return rc;
}
EXPORT_SYMBOL(tifm_add_adapter);

void tifm_remove_adapter(struct tifm_adapter *fm)
{
	class_device_del(&fm->cdev);

	spin_lock(&tifm_adapter_lock);
	idr_remove(&tifm_adapter_idr, fm->id);
	spin_unlock(&tifm_adapter_lock);
}
EXPORT_SYMBOL(tifm_remove_adapter);


static void tifm_free_device(struct device *dev)
{
	struct tifm_dev *fm_dev = container_of(dev, struct tifm_dev, dev);
	if(fm_dev->wq) destroy_workqueue(fm_dev->wq);
	kfree(fm_dev);
}


struct tifm_dev* tifm_alloc_device(struct tifm_adapter *fm, unsigned int id)
{
	struct tifm_dev *dev = kzalloc(sizeof(struct tifm_dev), GFP_KERNEL);
	
	if(dev) {
		spin_lock_init(&dev->lock);
		snprintf(dev->wq_name, KOBJ_NAME_LEN, "wq/tifm_%x:%x", fm->id, id);
		dev->wq = create_workqueue(dev->wq_name);
		if(!dev->wq) {
			kfree(dev);
			return 0;
		}
		dev->dev.parent = fm->dev;
		dev->dev.bus = &tifm_bus_type;
		dev->dev.dma_mask = fm->dev->dma_mask;
		dev->dev.release = tifm_free_device;
	}
	return dev;
}
EXPORT_SYMBOL(tifm_alloc_device);

void tifm_sock_power(struct tifm_dev *sock, int power_on)
{
	struct tifm_adapter *fm = (struct tifm_adapter*)dev_get_drvdata(sock->dev.parent);
	fm->power(fm, sock, power_on);
}
EXPORT_SYMBOL(tifm_sock_power);

void tifm_eject(struct tifm_dev *sock)
{
	struct tifm_adapter *fm = (struct tifm_adapter*)dev_get_drvdata(sock->dev.parent);
	fm->eject(fm, sock);
}
EXPORT_SYMBOL(tifm_eject);

int tifm_map_sg(struct tifm_dev *sock, struct scatterlist *sg, int nents,
		int direction)
{
	struct tifm_adapter *fm = (struct tifm_adapter*)dev_get_drvdata(sock->dev.parent);
	return pci_map_sg(to_pci_dev(fm->dev), sg, nents, direction);
}
EXPORT_SYMBOL(tifm_map_sg);

void tifm_unmap_sg(struct tifm_dev *sock, struct scatterlist *sg, int nents,
		   int direction)
{
	struct tifm_adapter *fm = (struct tifm_adapter*)dev_get_drvdata(sock->dev.parent);
	return pci_unmap_sg(to_pci_dev(fm->dev), sg, nents, direction);
}
EXPORT_SYMBOL(tifm_unmap_sg);

static int tifm_device_probe(struct device *dev)
{
	struct tifm_driver *drv = container_of(dev->driver, struct tifm_driver, driver);
	struct tifm_dev *fm_dev = container_of(dev, struct tifm_dev, dev);
	int rc = 0;
	const tifm_device_id *id;

	get_device(dev);
	if(!fm_dev->drv && drv->probe && drv->id_table) {
		rc = -ENODEV;
		id = tifm_device_match(drv->id_table, fm_dev);
		if(id) rc = drv->probe(fm_dev);
		if(rc >= 0) {
			rc = 0;
			fm_dev->drv = drv;
		}
	}
	if(rc) put_device(dev);
	return rc;
}

static int tifm_device_remove(struct device *dev)
{
	struct tifm_dev *fm_dev = container_of(dev, struct tifm_dev, dev);
	struct tifm_driver *drv = fm_dev->drv;

	if(drv) {
		if(drv->remove) drv->remove(fm_dev);
		fm_dev->drv = 0;
	}

	put_device(dev);
	return 0;
}

int tifm_register_driver(struct tifm_driver *drv)
{
	drv->driver.bus = &tifm_bus_type;
	drv->driver.probe = tifm_device_probe;
	drv->driver.remove = tifm_device_remove;
	
	return driver_register(&drv->driver);
}
EXPORT_SYMBOL(tifm_register_driver);

void tifm_unregister_driver(struct tifm_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(tifm_unregister_driver);

static int __init tifm_init(void)
{
	int rc = bus_register(&tifm_bus_type);

	if(!rc) {
		rc = class_register(&tifm_adapter_class);
		if(rc) bus_unregister(&tifm_bus_type);
	}

	return rc;
}

static void __exit tifm_exit(void)
{
	class_unregister(&tifm_adapter_class);
	bus_unregister(&tifm_bus_type);
}

subsys_initcall(tifm_init);
module_exit(tifm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("TI FlashMedia core driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
