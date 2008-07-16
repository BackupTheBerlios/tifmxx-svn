/*
 *  MTDX simple FTL
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "mtdx_common.h"
#include <linux/module.h>

#define DRIVER_NAME "ftl_simple"

struct zone_data {
	unsigned long *useful_blocks;
	unsigned int  b_table[];
};

struct ftl_simple_data {
	spinlock_t            lock;
	atomic_long_t         usage_count;
	struct mtdx_block_map *b_map;
	struct mtdx_peb_alloc *b_alloc;
	struct mtdx_dev_geo   geo;
	struct zone_data      *zones[];
};

static int ftl_simple_dummy_new_request(struct mtdx_dev *this_dev,
					struct mtdx_dev *req_dev)
{
	return -ENODEV;
}

static void mtdx_ftl_simple_free(struct ftl_simple_data *fsd)
{
	unsigned int cnt;

	if (!fsd)
		return;

	for (cnt = (1 << fsd->geo.zone_cnt_log) - 1; cnt >= 0; --cnt) {
		if (fsd->zones[cnt]) {
			kfree(fsd->zones[cnt].useful_blocks);
			kfree(fsd->zones[cnt]);
		}
	}

	mtdx_block_map_free(fsd->b_map);
	mtdx_peb_alloc_free(fsd->b_alloc);
	kfree(fsd);
}

static int mtdx_ftl_simple_probe(struct mtdx_dev *mdev)
{
	struct mtdx_dev *parent = container_of(mdev->dev.parent,
					       struct mtdx_dev, dev);
	struct ftl_simple_data *fsd = kzalloc(sizeof(struct ftl_simple_data),
					      GFP_KERNEL);
	int rc;

	if (!fsd)
		return -ENOMEM;

	mtdx_set_drvdata(mdev, fsd);
	mdev->new_request = ftl_simple_new_request;
	mdev->get_request = ftl_simple_get_request;
	mdev->end_request = ftl_simple_end_request;
	mdev->get_data_buf_sg = ftl_simple_get_data_buf_sg;
	mdev->get_param = ftl_simple_mtdx_get_param;

	return 0;
err_out:
	mtdx_ftl_simple_free(fsd);
	return rc;
}

static void mtdx_ftl_simple_remove(struct mtdx_dev *mdev)
{
}

#ifdef CONFIG_PM

static int mtdx_ftl_simple_suspend(struct mtdx_dev *mdev, pm_message_t state)
{
}

static int mtdx_ftl_simple_resume(struct mtdx_dev *mdev)
{
}

#else

#define mtdx_ftl_simple_suspend NULL
#define mtdx_ftl_simple_resume NULL

#endif /* CONFIG_PM */

static struct mtdx_device_id mtdx_ftl_simple_id_tbl[] = {
	{ MTDX_WMODE_PAGE, MTDX_WMODE_PAGE_PEB, MTDX_RMODE_PAGE,
	  MTDX_RMODE_PAGE_PEB, MTDX_TYPE_FTL, MTDX_ID_FTL_SIMPLE },
	{ MTDX_WMODE_PAGE, MTDX_WMODE_PAGE_PEB_INC, MTDX_RMODE_PAGE,
	  MTDX_RMODE_PAGE_PEB, MTDX_TYPE_FTL, MTDX_ID_FTL_SIMPLE },
	{ MTDX_WMODE_PAGE, MTDX_WMODE_PEB, MTDX_RMODE_PAGE,
	  MTDX_RMODE_PAGE_PEB, MTDX_TYPE_FTL, MTDX_ID_FTL_SIMPLE },
	{}
};

static struct mtdx_driver mtdx_ftl_simple_driver = {
	.driver = {
		.name  = DRIVER_NAME,
		.owner = THIS_MODULE
	},
	.id_table = mtdx_ftl_simple_id_tbl,
	.probe    = mtdx_ftl_simple_probe,
	.remove   = mtdx_ftl_simple_remove,
	.suspend  = mtdx_ftl_simple_suspend,
	.resume   = mtdx_ftl_simple_resume
};

static int __init mtdx_ftl_simple_init(void)
{
	return mtdx_register_driver(&mtdx_ftl_simple_driver);
}

static void __exit mtdx_ftl_simple_exit(void)
{
	mtdx_unregister_driver(&mtdx_ftl_simple_driver);
}

module_init(mtdx_ftl_simple_init);
module_exit(mtdx_ftl_simple_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("MTDX simple FTL");
MODULE_DEVICE_TABLE(mtdx, mtdx_ftl_simple_id_tbl);
