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

static DEFINE_IDR(tifm_adapter_idr);
static DEFINE_SPINLOCK(tifm_adapter_lock);

static int tifm_device_match(struct device *dev, struct device_driver *drv)
{
	return 0;
}

static int tifm_hotplug(struct device *dev, char **envp, int num_envp, 
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
	.match   = tifm_device_match,
	.hotplug = tifm_hotplug,
	.suspend = tifm_suspend,
	.resume  = tifm_resume
};

static void tifm_free(struct class_device *cdev)
{
	struct tifm_adapter *fm = container_of(cdev, struct tifm_adapter, cdev);
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
		snprintf(fm->cdev.class_id, BUS_ID_SIZE, "tifm%d", fm->id);
		rc = class_device_add(&fm->cdev);
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
