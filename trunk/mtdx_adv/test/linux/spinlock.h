#ifndef _LINUX_SPINLOCK_H
#define _LINUX_SPINLOCK_H

#include <pthread.h>

#define DEFINE_MUTEX(x) pthread_mutex_t x = PTHREAD_MUTEX_INITIALIZER

typedef struct {
	pthread_mutex_t mutex;
} spinlock_t;

void spin_lock_irqsave(spinlock_t *lock, unsigned long flags);
void spin_lock_irqrestore(spinlock_t *lock, unsigned long flags);
void spin_lock_init(spinlock_t *lock);


#endif
