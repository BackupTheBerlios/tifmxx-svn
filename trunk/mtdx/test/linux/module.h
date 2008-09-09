#ifndef _LINUX_MODULE_H
#define _LINUX_MODULE_H

#include <stddef.h>
#include <assert.h>
#include <linux/spinlock.h>
#include <linux/bitmap.h>
#include <linux/div64.h>


#define __init
#define __exit
#define GFP_KERNEL 0

#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_DEVICE_TABLE(x, y)
#define EXPORT_SYMBOL(x)

#define BUG_ON(x) assert(!(x))

typedef int gfp_t;

void msleep(unsigned int msecs);

#define msleep_interruptible msleep

void kfree(const void *);
void *kzalloc(size_t size, gfp_t flags);
void *kmalloc(size_t size, gfp_t flags);

unsigned int random32(void);

#define container_of(ptr, type, member) ({                      \
	const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
	(type *)( (char *)__mptr - offsetof(type,member) );})

typedef int (*initcall_t)(void);
typedef void (*exitcall_t)(void);

#define module_param(x, y, z)

/* Each module must use one module_init(), or one no_module_init */
#define module_init(initfn)                                     \
	static inline initcall_t __inittest(void)               \
	{ return initfn; }                                      \
	int init_module(void) __attribute__((alias(#initfn)));

/* This is only required if you want to be unloadable. */
#define module_exit(exitfn)                                     \
	static inline exitcall_t __exittest(void)               \
	{ return exitfn; }                                      \
	void cleanup_module(void) __attribute__((alias(#exitfn)));


#endif
