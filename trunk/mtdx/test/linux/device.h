#ifndef _LINUX_DEVICE_H
#define _LINUX_DEVICE_H

#include <linux/list.h>
#include <linux/errno.h>
#include <stdio.h>

#define THIS_MODULE NULL

typedef int pm_message_t;

struct device_driver {
	const char *name;
	void *owner;
};

struct device {
	struct device *parent;
	char          bus_id[32];
	void          *driver_data;
};


static inline void *dev_get_drvdata(struct device *dev)
{
	return dev->driver_data;
}

static inline void dev_set_drvdata(struct device *dev, void *data)
{
	dev->driver_data = data;
}

#define dev_dbg(dev, format, arg...)            \
        printf("%s: "format, (dev)->bus_id, ## arg)

#endif
