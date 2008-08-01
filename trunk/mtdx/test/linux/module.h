#ifndef _LINUX_MODULE_H
#define _LINUX_MODULE_H

#include <stddef.h>
#include <assert.h>
#include <atomic_ops.h>
#include <linux/spinlock.h>
#include <linux/bitmap.h>
#include <linux/div64.h>


#define __init
#define __exit
#define GFP_KERNEL 0

#define module_init(x)
#define module_exit(x)

#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_DEVICE_TABLE(x, y)
#define EXPORT_SYMBOL(x)

#define BUG_ON assert

typedef int gfp_t;
typedef AO_t atomic_long_t;

#define atomic_long_read AO_load
static inline void atomic_long_inc(atomic_long_t *l)
{
	AO_fetch_and_add1(l);
}

static inline void atomic_long_dec(atomic_long_t *l)
{
	AO_fetch_and_sub1(l);
}

void msleep(unsigned int msecs);

void kfree(const void *);
void *kzalloc(size_t size, gfp_t flags);
void *kmalloc(size_t size, gfp_t flags);

unsigned int random32(void);

#define container_of(ptr, type, member) ({                      \
	const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
	(type *)( (char *)__mptr - offsetof(type,member) );})


#endif
