#ifndef _LINUX_WORKQUEUE_H
#define _LINUX_WORKQUEUE_H

#include <linux/errno.h>

typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
	long data;
	work_func_t func;
};

#endif
