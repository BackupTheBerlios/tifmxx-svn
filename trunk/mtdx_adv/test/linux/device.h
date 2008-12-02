#ifndef _LINUX_DEVICE_H
#define _LINUX_DEVICE_H

#include <linux/list.h>
#include <linux/errno.h>
#include <sys/stat.h>
#include <stdio.h>

#define __stringify_1(x)        #x
#define __stringify(x)          __stringify_1(x)

#define THIS_MODULE NULL
/*
#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001
*/
#define S_IRWXUGO       (S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO       (S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO         (S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO         (S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO         (S_IXUSR|S_IXGRP|S_IXOTH)


typedef int pm_message_t;

struct device_type {
	const char *name;
};

struct kobj_uevent_env {
};

struct bus_type;

struct device_driver {
	const char *name;
	struct bus_type *bus;
	void *owner;
};

struct attribute {
	const char              *name;
	struct module           *owner;
	mode_t                  mode;
};


#define __ATTR(_name,_mode,_show,_store) { \
	.attr = {.name = __stringify(_name), .mode = _mode },   \
	.show   = _show,                                        \
	.store  = _store,                                       \
}

#define __ATTR_RO(_name) { \
	.attr   = { .name = __stringify(_name), .mode = 0444 }, \
	.show   = _name##_show,                                 \
}

#define __ATTR_NULL { .attr = { .name = NULL } }


struct device {
	struct device *parent;
	char          bus_id[32];
	void          *driver_data;
	struct device_driver *driver;
	struct device_type *type;
	struct bus_type *bus;
	void    (*release)(struct device *dev);
};

struct device_attribute {
	struct attribute        attr;
	ssize_t (*show)(struct device *dev, struct device_attribute *attr,
		        char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
		        const char *buf, size_t count);
};

struct bus_type {
	const char              *name;
	struct bus_attribute    *bus_attrs;
	struct device_attribute *dev_attrs;
	struct driver_attribute *drv_attrs;

	int (*match)(struct device *dev, struct device_driver *drv);
	int (*uevent)(struct device *dev, struct kobj_uevent_env *env);
	int (*probe)(struct device *dev);
	int (*remove)(struct device *dev);
	void (*shutdown)(struct device *dev);

	int (*suspend)(struct device *dev, pm_message_t state);
	int (*suspend_late)(struct device *dev, pm_message_t state);
	int (*resume_early)(struct device *dev);
	int (*resume)(struct device *dev);
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
#define dev_emerg(dev, format, arg...)            \
        printf("%s: "format, (dev)->bus_id, ## arg)
	
#endif
