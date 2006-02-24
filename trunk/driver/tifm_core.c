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
#include <linux/rwsem.h>

#include "tifm.h"

static DECLARE_RWSEM(tifm_all_devices_rwsem);

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

struct bus_type tifm_bus_type = {
	.name    = "tifm",
	.match   = tifm_device_match,
	.hotplug = tifm_hotplug,
	.suspend = tifm_suspend,
	.resume  = tifm_resume
};

static int tifm_probe_interface(struct device *dev)
{
	printk("Error from here!\n");
	return -ENODEV;
}

static int tifm_unbind_interface(struct device *dev)
{
	return 0;
}

int tifm_register(struct tifm_driver *new_driver)
{
	int rc = 0;

	new_driver->driver.name = (char *)new_driver->name;
	new_driver->driver.bus = &tifm_bus_type;
	new_driver->driver.probe = tifm_probe_interface;
	new_driver->driver.remove = tifm_unbind_interface;
	new_driver->driver.owner = new_driver->owner;

	down_write(&tifm_all_devices_rwsem);
	rc = driver_register(&new_driver->driver);
	up_write(&tifm_all_devices_rwsem);
	
	return rc;
}

void tifm_unregister(struct tifm_driver *driver)
{
	down_write(&tifm_all_devices_rwsem);
	driver_unregister(&driver->driver);
	up_write(&tifm_all_devices_rwsem);
}

static struct class *tifm_host_class;

static int tifm_host_init(void)
{
	
	tifm_host_class = class_create(THIS_MODULE, "tifm_host");
	if(IS_ERR(tifm_host_class)) return PTR_ERR(tifm_host_class);
	return 0;
}

static void tifm_host_cleanup(void)
{
	class_destroy(tifm_host_class);
}

static int __init tifm_init(void)
{
	int rc = 0;

	if((rc = bus_register(&tifm_bus_type))) goto err_out;
	
	if((rc = tifm_host_init())) goto err_bus_unregister;

//	if((rc = tifm_register())) goto err_host_cleanup;
	return 0;

//err_host_cleanup:
//	tifm_host_cleanup();
err_bus_unregister:
	bus_unregister(&tifm_bus_type);
err_out:
	return rc;
}

static void __exit tifm_exit(void)
{
	tifm_host_cleanup();
	bus_unregister(&tifm_bus_type);
}

subsys_initcall(tifm_init);
module_exit(tifm_exit);

MODULE_LICENSE("GPL");
