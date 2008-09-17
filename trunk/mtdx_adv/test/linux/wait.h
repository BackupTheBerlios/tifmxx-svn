#ifndef _WAIT_H
#define _WAIT_H

typedef struct wait_queue_head {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int count;
} wait_queue_head_t;

#define wait_event_interruptible(wq, c)                            \
	({                                                         \
		int __rc = 0;                                      \
		pthread_mutex_lock(&(wq).lock);                    \
		(wq).count++;                                      \
		pthread_mutex_unlock(&(wq).lock);                  \
		while (!(c)) {                                     \
			pthread_mutex_lock(&(wq).lock);            \
			pthread_cond_wait(&(wq).cond, &(wq).lock); \
			pthread_mutex_unlock(&(wq).lock);          \
		}                                                  \
		pthread_mutex_lock(&(wq).lock);                    \
		(wq).count--;                                      \
		pthread_mutex_unlock(&(wq).lock);                  \
		__rc;                                              \
	})                                                         \

static inline void wake_up(wait_queue_head_t *wq)
{
	pthread_cond_signal(&wq->cond);
}

static inline void wake_up_all(wait_queue_head_t *wq)
{
	pthread_cond_broadcast(&wq->cond);
}

static inline int waitqueue_active(wait_queue_head_t *wq)
{
	int rc;
	pthread_mutex_lock(&wq->lock);
	rc = wq->count;
	pthread_mutex_unlock(&wq->lock);
	return rc;
}

static inline void init_waitqueue_head(wait_queue_head_t *wq)
{
	pthread_mutexattr_t m_attr;
	
	pthread_mutexattr_init(&m_attr);
	pthread_mutexattr_settype(&m_attr, PTHREAD_MUTEX_ERRORCHECK_NP);
	pthread_mutex_init(&wq->lock, &m_attr);
	
	pthread_cond_init(&wq->cond, NULL);
	wq->count = 0;
	
}

#endif
