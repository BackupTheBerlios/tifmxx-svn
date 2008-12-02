#include <linux/module.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include "mtdx_common.h"

void spin_lock_irqsave(spinlock_t *lock, unsigned long flags)
{
	pthread_mutex_lock(&lock->mutex);
}

void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags)
{
	pthread_mutex_unlock(&lock->mutex);
}

void spin_lock_init(spinlock_t *lock)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK_NP);
	pthread_mutex_init(&lock->mutex, &attr);
}

void kfree(const void *ptr)
{
	free((void *)ptr);
}

void *kzalloc(size_t size, gfp_t flags)
{
	return calloc(1, size);
}

void *kmalloc(size_t size, gfp_t flags)
{
	return malloc(size);
}

void msleep(unsigned int msecs)
{
	struct timespec ts = {
		.tv_sec = msecs / 1000,
		.tv_nsec = (msecs % 1000) * 1000000UL
	};
	struct timespec rem;

	while (nanosleep(&ts, &rem))
		memcpy(&ts, &rem, sizeof(ts));
}

static void *work_thread(void *data)
{
	struct work_struct *work = data;
	pthread_mutex_lock(&work->lock);
	work->state = 2;
	printf("work thread running %p, %d\n", work, work->state);
	pthread_cond_broadcast(&work->cond);
	pthread_mutex_unlock(&work->lock);
	work->func(work);
	printf("work thread finished\n");
	return NULL;
}

int cancel_work_sync(struct work_struct *work)
{
	printf("cancel work %p\n", work);
	pthread_mutex_lock(&work->lock);
	printf("locked %d\n", work->state);
	while (work->state == 1)
		pthread_cond_wait(&work->cond, &work->lock);

	printf("join\n");
	if (work->thread)
		pthread_join(work->thread, NULL);

	work->thread = 0;
	work->state = 0;
	pthread_mutex_unlock(&work->lock);
	printf("done\n");
	return 0;
}

int schedule_work(struct work_struct *work)
{
	int rc = 0;

	pthread_mutex_lock(&work->lock);
	printf("schedule work %p, %p, %d\n", work, work->thread, work->state);
	if (work->state == 1)
		goto out;
	else if (work->state == 2) {
		if (work->thread)
			pthread_join(work->thread, NULL);

		work->thread = 0;
		work->state = 0;
		printf("work thread joined\n");
	}

	work->state = 1;
	rc = pthread_create(&work->thread, NULL, work_thread, work);
	if (!rc)
		printf("work thread created\n");
	else {
		work->state = 0;
		printf("work thread failed\n");
	}

out:
	printf("schedule out %d\n", work->state);
	pthread_mutex_unlock(&work->lock);
	return rc;
}
