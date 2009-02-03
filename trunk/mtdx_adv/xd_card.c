/*
 *  xD Picture Card/SmartMedia support
 *
 *  Copyright (C) 2009 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "xd_card.h"
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/kthread.h>
#include <linux/wait.h>

#define DRIVER_NAME "xd_card"

//#undef dev_dbg
//#define dev_dbg dev_emerg

//#define FUNC_START_DBG(card) dev_dbg(&(card)->dev, "%s start\n", __func__)

static void xd_card_mtdx_new_request(struct mtdx_dev *this_dev,
				     struct mtdx_dev *req_dev)
{
	struct xd_card_host *host = xd_card_host_from_dev(this_dev->dev.parent);

	dev_dbg(&this_dev->dev, "get device %p\n", req_dev);
	get_device(&req_dev->dev);
	mtdx_dev_queue_push_back(&host->c_queue, req_dev);
	host->request(host);
}

static struct mtdx_dev *xd_card_insert_media(struct xd_card_host *host)
{
	const struct mtdx_device_id c_id = {
		MTDX_WMODE_PEB,
		MTDX_WMODE_NONE,
		MTDX_RMODE_PAGE_PEB,
		MTDX_RMODE_NONE,
		MTDX_TYPE_MEDIA,
		MTDX_ID_MEDIA_SMARTMEDIA
	};
	struct mtdx_dev *card = mtdx_alloc_dev(host->dev, &c_id);
	int rc;

	if (!card)
		return NULL;

	card->new_request = xd_card_mtdx_new_request;
	card->get_request = xd_card_mtdx_get_request;
	card->end_request = xd_card_mtdx_end_request;

	card->oob_to_info = xd_card_mtdx_oob_to_info;
	card->info_to_oob = xd_card_mtdx_info_to_oob;
	card->get_param = xd_card_mtdx_get_param;

	rc = device_add(&card->dev);
	dev_dbg(&card->dev, "card %p, dev add %d\n", card, rc);
	if (rc) {
		__mtdx_free_dev(card);
		return NULL;
	}

	rc = xd_card_sysfs_register(card);
	dev_dbg(&card->dev, "sysfs register %d\n", rc);

	if (!rc) {
		struct mtdx_dev *cdev;
		struct mtdx_device_id c_id = {
			MTDX_WMODE_PAGE, MTDX_WMODE_PAGE_PEB_INC,
			MTDX_RMODE_PAGE, MTDX_RMODE_PAGE_PEB,
			MTDX_TYPE_FTL, MTDX_ID_FTL_SIMPLE
		};

		/* Temporary hack to insert ftl */
		cdev = mtdx_alloc_dev(&card->dev, &c_id);
		if (cdev) {
			rc = device_add(&cdev->dev);
			if (rc)
				__mtdx_free_dev(cdev);
		}
		return 0;
	}

	device_unregister(&card->dev);
	return NULL;
}

static void xd_card_check(struct work_struct *work)
{
	struct xd_card_host *host = container_of(work, struct xd_card_host,
                                                 media_checker);
	struct mtdx_dev *card;

	mutex_lock(&host->lock);
	if (!host->card)
		host->set_param(host, XD_CARD_POWER, XD_CARD_POWER_ON);
	else {
		if (xd_card_validate(host->card)) {
			device_unregister(&host->card->dev);
			host->card = NULL;
		}
	}

	if (!host->card)
		host->set_param(host, XD_CARD_POWER, XD_CARD_POWER_OFF);
	mutex_unlock(&host->lock);
}

void xd_card_detect_media(struct xd_card_host *host)
{
	schedule_work(&host->media_checker);
}
EXPORT_SYMBOL(xd_card_detect_media);

struct xd_card_host *xd_card_alloc_host(struct device *dev, unsigned long extra)
{
	struct xd_card_host *host = kzalloc(sizeof(struct xd_card_host) + extra,
					    GFP_KERNEL);
	if (!host)
		return NULL;

	host->dev = dev;
	mutex_init(&host->lock);
	INIT_WORK(&host->media_checker, xd_card_check);
	mtdx_dev_queue_init(&host->c_queue);
	dev_set_drvdata(dev, host);
	return host;
}
EXPORT_SYMBOL(xd_card_alloc_host);

void xd_card_free_host(struct xd_card_host *host)
{
	flush_scheduled_work();
	mutex_lock(&host->lock);
	if (host->card) {
		device_unregister(&host->card->dev);
		host->card = NULL;
	}
	mutex_unlock(&host->lock);
	kfree(host);
}
EXPORT_SYMBOL(xd_card_free_host);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("xD picture card block device driver");
