#ifndef _LINUX_WORKQUEUE_H
#define _LINUX_WORKQUEUE_H

#include <linux/errno.h>
#include <pthread.h>

struct work_struct;

typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
	long            data;
	work_func_t     func;
	pthread_t       thread;
	pthread_mutex_t lock;
	pthread_cond_t  cond;
	int             state;
};

#define INIT_WORK(x, f) { \
	(x)->func = f;      \
	(x)->thread = 0;    \
	pthread_mutex_init(&(x)->lock, NULL); \
	pthread_cond_init(&(x)->cond, NULL);  \
	(x)->state = 0;     \
}

int schedule_work(struct work_struct *work);
int cancel_work_sync(struct work_struct *work);

#endif
