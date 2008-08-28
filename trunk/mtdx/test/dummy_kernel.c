#include <linux/module.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/workqueue.h>
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
	pthread_mutex_init(&lock->mutex, NULL);
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

int bitmap_region_empty(unsigned long *bitmap, unsigned int offset,
			unsigned int length)
{
	unsigned long w_b, w_e, m_b, m_e, cnt;

	w_b = offset / BITS_PER_LONG;
	w_e = (offset + length) / BITS_PER_LONG;

	m_b = ~((1UL << (offset % BITS_PER_LONG)) - 1UL);
	m_e = (1UL << ((offset + length) % BITS_PER_LONG)) - 1UL;

	if (w_b == w_e) {
		return bitmap[w_b] & (m_b ^ m_e) ? 0 : 1;
	} else {
		if (bitmap[w_b] & m_b)
			return 0;

		if (m_e && (bitmap[w_e] & m_e))
			return 0;

		for (cnt = w_b + 1; cnt < w_e; ++cnt)
			if (bitmap[cnt])
				return 0;

		return 1;
	}
}

void bitmap_clear_region(unsigned long *bitmap, unsigned int offset,
			 unsigned int length)
{
	unsigned long w_b, w_e, m_b, m_e, cnt;

	w_b = offset / BITS_PER_LONG;
	w_e = (offset + length) / BITS_PER_LONG;

	m_b = ~((1UL << (offset % BITS_PER_LONG)) - 1UL);
	m_e = (1UL << ((offset + length) % BITS_PER_LONG)) - 1UL;

	if (w_b == w_e) {
		bitmap[w_b] &= ~(m_b & m_e);
	} else {
		bitmap[w_b] &= ~m_b;

		if (m_e)
			bitmap[w_e] &= ~m_e;

		for (cnt = w_b + 1; cnt < w_e; ++cnt)
			bitmap[cnt] = 0UL;
	}
}

void bitmap_set_region(unsigned long *bitmap, unsigned int offset,
		       unsigned int length)
{
	unsigned long w_b, w_e, m_b, m_e, cnt;

	w_b = offset / BITS_PER_LONG;
	w_e = (offset + length) / BITS_PER_LONG;

	m_b = ~((1UL << (offset % BITS_PER_LONG)) - 1UL);
	m_e = (1UL << ((offset + length) % BITS_PER_LONG)) - 1UL;

	if (w_b == w_e) {
		bitmap[w_b] |= m_b & m_e;
	} else {
		bitmap[w_b] |= m_b;

		if (m_e)
			bitmap[w_e] |= m_e;

		for (cnt = w_b + 1; cnt < w_e; ++cnt)
			bitmap[cnt] = ~0UL;
	}
}

int mtdx_page_list_append(struct list_head *head, struct mtdx_page_info *info)
{
	struct list_head *p = head;
	struct mtdx_page_info *c_info;

	if (!list_empty(head)) {
		list_for_each (p, head) {
			c_info = list_entry(p, struct mtdx_page_info, node);
			if (c_info->phy_block < info->phy_block)
				continue;
			else if (c_info->phy_block > info->phy_block)
				break;
			else {
				if (c_info->page_offset < info->page_offset)
					continue;
				else if (c_info->page_offset
					 > info->page_offset)
					break;
				else {
					c_info->status = info->status;
					c_info->log_block = info->log_block;
					return 0;
				}
			}
		}
	}

	c_info = kmalloc(sizeof(struct mtdx_page_info), GFP_KERNEL);
	if (!c_info)
		return -ENOMEM;

	INIT_LIST_HEAD(&c_info->node);
	c_info->status = info->status;
	c_info->phy_block = info->phy_block;
	c_info->log_block = info->log_block;
	c_info->page_offset = info->page_offset;
	list_add_tail(&c_info->node, p);
	return 0;
}

void mtdx_page_list_free(struct list_head *head)
{
	struct mtdx_page_info *c_info;

	while (!list_empty(head)) {
		c_info = list_first_entry(head, struct mtdx_page_info, node);
		list_del(&c_info->node);
		kfree(c_info);
	}
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

void __mtdx_free_dev(struct mtdx_dev *mdev)
{
	free(mdev);
}

struct mtdx_dev *mtdx_alloc_dev(struct device *parent,
				const struct mtdx_device_id *id)
{
	struct mtdx_dev *mdev = kzalloc(sizeof(struct mtdx_dev), GFP_KERNEL);
	int rc;

	if (!mdev)
		return NULL;

	memcpy(&mdev->id, id, sizeof(struct mtdx_device_id));
	mdev->dev.parent = parent;
//	mdev->dev.bus = &mtdx_bus_type;
//	mdev->dev.release = mtdx_free_dev;
//	mdev->dev.type = &mtdx_type;

	snprintf(mdev->dev.bus_id, sizeof(mdev->dev.bus_id),
		 "mtdx%d", mdev->ord);

	return mdev;

err_out_free:
	kfree(mdev);
	return NULL;
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

void mtdx_drop_children(struct mtdx_dev *mdev)
{
}
