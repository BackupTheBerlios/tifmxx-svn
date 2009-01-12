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

struct xd_card_host *xd_card_alloc_host()
{
}
EXPORT_SYMBOL(xd_card_alloc_host);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("xD picture card block device driver");
