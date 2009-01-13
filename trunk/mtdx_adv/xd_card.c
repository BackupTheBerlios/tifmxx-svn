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

#include "mtdx_common.h"
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/kthread.h>
#include <linux/wait.h>

#define DRIVER_NAME "xd_card"

//#undef dev_dbg
//#define dev_dbg dev_emerg

//#define FUNC_START_DBG(card) dev_dbg(&(card)->dev, "%s start\n", __func__)

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
			host->card = 0;


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
	dev_set_drvdata(dev, host);
	return host;
}
EXPORT_SYMBOL(xd_card_alloc_host);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("xD picture card block device driver");
