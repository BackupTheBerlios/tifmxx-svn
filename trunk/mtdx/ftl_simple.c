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

#include <linux/module.h>
#include "mtdx_common.h"
#include "peb_alloc.h"
#include "block_map.h"

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
	struct mtdx_dev       *req_dev;
	struct scatterlist    req_sg;
	unsigned int          zone_cnt;
	unsigned int          zone_block_cnt;
	unsigned long         *valid_zones;
	struct list_head      special_blocks;
	unsigned char         *block_buf;
	struct zone_data      *zones[];
};

static int ftl_simple_zone_valid(struct ftl_simple_data *fsd, unsigned int zone)
{
	if (fsd->zone_cnt > BITS_PER_LONG)
		return test_bit(zone, fsd->valid_zones);
	else
		return test_bit(zone, &fsd->valid_zones);
}

static int ftl_simple_dummy_new_request(struct mtdx_dev *this_dev,
					struct mtdx_dev *req_dev)
{
	return -ENODEV;
}

static int ftl_simple_new_request(struct mtdx_dev *this_dev,
				  struct mtdx_dev *req_dev)
{
	struct mtdx_dev *parent = container_of(this_dev->dev.parent,
					       struct mtdx_dev, dev);
	struct ftl_simple_data *fsd = mtdx_get_drvdata(this_dev);
	unsigned long flags;
	int rc;

	atomic_long_inc(&fsd->usage_count);
	rc = parent->new_request(parent, this_dev);

	spin_lock_irqsave(&fsd->lock, flags);
	if (!rc && fsd->req_dev)
		rc = -EIO;

	if (rc) {
		spin_unlock_irqrestore(&fsd->lock, flags);
		atomic_long_dec(&fsd->usage_count);
		return rc;
	}

	fsd->req_dev = req_dev;
	spin_unlock_irqrestore(&fsd->lock, flags);
	return 0;
}

static int ftl_simple_get_param(struct mtdx_dev *this_dev,
				enum mtdx_param param, void *val)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(this_dev);

	switch (param) {
	case MTDX_PARAM_GEO: {
		struct mtdx_dev_geo *geo = (struct mtdx_dev_geo *)val;
		geo->zone_size_log = sizeof(unsigned int);
		geo->log_block_cnt = fsd->geo.log_block_cnt;
		geo->phy_block_cnt = 0;
		geo->page_cnt = fsd->geo.page_cnt;
		geo->page_size = fsd->geo.page_size;
		geo->oob_size = 0;
		return 0;
	}
	case MTDX_PARAM_SPECIAL_BLOCKS:
	/* What about them in FTLs? */
		return -EINVAL;
	case MTDX_PARAM_HD_GEO:
	/* Really, we should make something up instead off blindly relying on
	 * parent to provide this info.
	 */
	default: {
		struct mtdx_dev *parent = container_of(this_dev->dev.parent,
						       struct mtdx_dev, dev);
		return parent->get_param(parent, param, val);
	}
	}
}

static void ftl_simple_free(struct ftl_simple_data *fsd)
{
	unsigned int cnt;

	if (!fsd)
		return;

	for (cnt = 0; cnt < fsd->zone_cnt; ++cnt) {
		if (fsd->zones[cnt]) {
			kfree(fsd->zones[cnt]->useful_blocks);
			kfree(fsd->zones[cnt]);
		}
	}

	if (fsd->zone_cnt > BITS_PER_LONG)
		kfree(fsd->valid_zones);

	kfree(fsd->block_buf);

	mtdx_page_list_free(&fsd->special_blocks);
	mtdx_block_map_free(fsd->b_map);
	mtdx_peb_alloc_free(fsd->b_alloc);
	kfree(fsd);
}

static int ftl_simple_probe(struct mtdx_dev *mdev)
{
	struct mtdx_dev *parent = container_of(mdev->dev.parent,
					       struct mtdx_dev, dev);
	struct ftl_simple_data *fsd;
	int rc;

	{
		struct mtdx_dev_geo geo;
		unsigned int zone_cnt;

		rc = parent->get_param(parent, MTDX_PARAM_GEO, &geo);
		if (rc)
			return rc;

		zone_cnt = geo.phy_block_cnt >> geo.zone_size_log;
		if (!zone_cnt)
			zone_cnt = 1;

		fsd = kzalloc(sizeof(struct ftl_simple_data)
			      + sizeof(struct zone_data) * zone_cnt,
			      GFP_KERNEL);
		if (!fsd)
			return -ENOMEM;

		fsd->zone_cnt = zone_cnt;
		memcpy(&fsd->geo, &geo, sizeof(struct mtdx_dev_geo));
	}

	fsd->zone_block_cnt = min(fsd->geo.phy_block_cnt,
				  1U << fsd->geo.zone_size_log);

	spin_lock_init(&fsd->lock);

	switch (parent->id.inp_wmode) {
	case MTDX_WMODE_PAGE_PEB:
		fsd->b_map = mtdx_block_map_alloc(fsd->geo.page_cnt,
						  min(fsd->geo.phy_block_cnt
						      - fsd->geo.log_block_cnt,
						      8U),
						  MTDX_BLOCK_MAP_RANDOM);
		break;
	case MTDX_WMODE_PAGE_PEB_INC:
		fsd->b_map = mtdx_block_map_alloc(fsd->geo.page_cnt,
						  min(fsd->geo.phy_block_cnt
						      - fsd->geo.log_block_cnt,
						      8U),
						  MTDX_BLOCK_MAP_INCREMENTAL);
		break;
	case MTDX_WMODE_PEB:
		break;
	default:
		rc = -EINVAL;
		goto err_out;
	}

	fsd->b_alloc = mtdx_rand_peb_alloc(fsd->geo.zone_size_log,
					   fsd->geo.phy_block_cnt);
	if (!fsd->b_alloc) {
		rc = -ENOMEM;
		goto err_out;
	}

	fsd->block_buf = kmalloc(fsd->geo.page_cnt * fsd->geo.page_size,
				 GFP_KERNEL);
	if (!fsd->block_buf) {
		rc = -ENOMEM;
		goto err_out;
	}

	if (fsd->zone_cnt > BITS_PER_LONG) {
		fsd->valid_zones = kzalloc(BITS_TO_LONGS(fsd->zone_cnt)
					   * sizeof(unsigned long), GFP_KERNEL);
		if (!fsd->valid_zones) {
			rc = -ENOMEM;
			goto err_out;
		}
	} else
		fsd->valid_zones = NULL;

	for (rc = 0; rc < fsd->zone_cnt; ++rc) {
		fsd->zones[rc] = kzalloc(sizeof(struct zone_data)
					 + (1 << fsd->geo.zone_size_log)
					   * sizeof(unsigned int),
					 GFP_KERNEL);
		if (!fsd->zones[rc]) {
			rc = -ENOMEM;
			goto err_out;
		}

		if (fsd->b_map) {
			fsd->zones[rc]->useful_blocks
				= kzalloc(BITS_TO_LONGS(fsd->zone_block_cnt)
					  * sizeof(unsigned long), GFP_KERNEL);
			if (!fsd->zones[rc]->useful_blocks) {
				rc = -ENOMEM;
				goto err_out;
			}
		}
	}

	parent->get_param(parent, MTDX_PARAM_SPECIAL_BLOCKS,
			  &fsd->special_blocks);

	mtdx_set_drvdata(mdev, fsd);
	mdev->new_request = ftl_simple_new_request;
	mdev->get_request = ftl_simple_get_request;
	mdev->end_request = ftl_simple_end_request;
	mdev->get_data_buf_sg = ftl_simple_get_data_buf_sg;
	mdev->get_param = ftl_simple_get_param;

	return 0;
err_out:
	ftl_simple_free(fsd);
	return rc;
}

static void ftl_simple_remove(struct mtdx_dev *mdev)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(mdev);

	mdev->new_request = ftl_simple_dummy_new_request;

	/* Just wait for everybody to go away! */
	while (atomic_long_read(&fsd->usage_count))
		msleep(1);

	mtdx_set_drvdata(mdev, NULL);
	ftl_simple_free(fsd);
}

#ifdef CONFIG_PM

static int ftl_simple_suspend(struct mtdx_dev *mdev, pm_message_t state)
{
}

static int ftl_simple_resume(struct mtdx_dev *mdev)
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

static struct mtdx_driver ftl_simple_driver = {
	.driver = {
		.name  = DRIVER_NAME,
		.owner = THIS_MODULE
	},
	.id_table = mtdx_ftl_simple_id_tbl,
	.probe    = ftl_simple_probe,
	.remove   = ftl_simple_remove,
	.suspend  = ftl_simple_suspend,
	.resume   = ftl_simple_resume
};

static int __init mtdx_ftl_simple_init(void)
{
	return mtdx_register_driver(&ftl_simple_driver);
}

static void __exit mtdx_ftl_simple_exit(void)
{
	mtdx_unregister_driver(&ftl_simple_driver);
}

module_init(mtdx_ftl_simple_init);
module_exit(mtdx_ftl_simple_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("MTDX simple FTL");
MODULE_DEVICE_TABLE(mtdx, mtdx_ftl_simple_id_tbl);
