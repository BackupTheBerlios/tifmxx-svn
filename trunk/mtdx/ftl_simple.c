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
#include "long_map.h"
#include "sg_bounce.h"

#define DRIVER_NAME "ftl_simple"

struct zone_data {
	unsigned long *useful_blocks;
	unsigned int  b_tabl e[];
};

struct ftl_simple_data {
	spinlock_t            lock;
	atomic_long_t         usage_count;
	struct long_map       *b_map;
	struct mtdx_peb_alloc *b_alloc;
	struct mtdx_dev_geo   geo;
	struct mtdx_dev       *req_dev;
	struct mtdx_request   *req_in;
	struct mtdx_request   req_out;
	struct scatterlist    req_sg;
	unsigned int          sg_off;
	unsigned int          zone_cnt;
	unsigned int          zone_block_cnt;
	unsigned int          block_size;
	unsigned int          zone_scan_pos;
	unsigned int          t_count;

	unsigned int          fill_value;
	unsigned int          dumb_copy;

	union {
		unsigned long         valid_zones;
		unsigned long         *valid_zones_ptr;
	};
	struct list_head      special_blocks;
	void                  (*end_req_fn)(struct ftl_simple_data *fsd,
					    int error, unsigned int count);
	unsigned char         *oob_buf;
	unsigned char         *block_buf;
	struct zone_data      *zones[];
};

static void *ftl_simple_alloc_pmap(unsigned long num_bits)
{
	return kmalloc(BITS_TO_LONGS(num_bits) * sizeof(unsigned long),
		       GFP_KERNEL);
}

static unsigned long *ftl_simple_zone_map(struct ftl_simple_data *fsd)
{
	if (fsd->zone_cnt > BITS_PER_LONG)
		return fsd->valid_zones_ptr;
	else
		return &fsd->valid_zones;
}

static int ftl_simple_setup_zone_scan(struct ftl_simple_data *fsd,
				      unsigned int zone)
{
}

static int ftl_simple_setup_write(struct ftl_simple_data *fsd)
{
	if (!fsd->b_alloc)
		return -EROFS;
}

static int ftl_simple_fill_data(struct ftl_simple_data *fsd)
{
	unsigned int length = fsd->req_out.length;
	int rc = 0;

	while (length) {
		if (fsd->req_sg.length <= fsd->sg_off) {
			fsd->sg_off = 0;
			rc = mtdx_get_data_buf_sg(fsd->req_in, &fsd->req_sg);
			if (rc)
				return rc;
		}
		length -= fill_sg(&fsd->req_sg, &fsd->sg_off, fsd->fill_value,
				  length);
	}

	fsd->t_count += fsd->req_out.length - length;
	return rc;
}

static int ftl_simple_setup_read(struct ftl_simple_data *fsd)
{
	unsigned int pos;
	unsigned int zone;
	unsigned int z_log_block;

try_again:
	pos = fsd->req_in->offset + fsd->t_count;
	fsd->req_out.log_block = fsd->req_in->log_block;
	fsd->req_out.log_block += pos / fsd->block_size;
	zone = fsd->req_out.log_block >> fsd->geo.zone_size_log;
	z_log_block = fsd->req_out.log_block
		      & ((1U << fsd->geo.zone_size_log) - 1U);

	if (!test_bit(zone, ftl_simple_zone_map(fsd)))
		return ftl_simple_setup_zone_scan(fsd, zone);

	fsd->req_out.cmd = MTDX_CMD_READ_DATA;
	fsd->req_out.phy_block = fsd->zones[zone]->b_table[z_log_block];
	fsd->req_out.offset = pos % fsd->block_size;
	fsd->req_out.length = min(fsd->req_in->length - fsd->t_count,
				  fsd->block_size - fsd->req_out.offset);
	if (fsd->req_out.phy_block == MTDX_INVALID_BLOCK) {
		int rc = ftl_simple_fill_data(fsd);
		if (rc)
			return rc;

		fsd->t_count += fsd->req_out.length;
		if (fsd->t_count < fsd->req_in->length)
			goto try_again;
		else
			rc = -EAGAIN;
	}

	return 0;
}

static void ftl_simple_end_request(struct mtdx_request *req, int error,
				   unsigned int count)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(req->src_dev);
	unsigned long flags;

	spin_lock_irqsave(&fsd->lock, flags);
	if (!fsd->end_req_fn) {
		fsd->t_count += count;

		if (!error && (fsd->t_count >= fsd->req_in->length))
			error = -EAGAIN;

		if (error) {
			mtdx_complete_request(fsd->req_in, error, fsd->t_count);
			fsd->req_in = NULL;
		}
	} else
		fsd->end_req_fn(fsd, error, count);

	spin_unlock_irqrestore(&fsd->lock, flags);
}

static struct mtdx_request *ftl_simple_get_request(struct mtdx_dev *this_dev)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(this_dev);
	int rc = -EAGAIN;
	unsigned long flags;

	spin_lock_irqsave(&fsd->lock, flags);
	if (fsd->req_dev) {
		while (1) {
			if (fsd->req_in) {
				if (fsd->req_in->cmd == MTDX_CMD_READ_DATA)
					rc = ftl_simple_setup_read(fsd);
				else if (fsd->req_in->cmd
					 == MTDX_CMD_WRITE_DATA)
					rc = ftl_simple_setup_write(fsd);
				else
					rc = -EINVAL;

				if (!rc)
					goto out;
				else {
					mtdx_complete_request(fsd->req_in, rc,
							      fsd->t_count);
					fsd->req_in = NULL;
				}
			} else
				rc = -EAGAIN;

			if (!rc)
				goto out;

			fsd->req_sg.length = 0;
			fsd->sg_off = 0;
			fsd->req_in = fsd->req_dev->get_request(fsd->req_dev);
		}

		fsd->req_dev = NULL;
		atomic_long_dec(&fsd->usage_count);
	}
out:
	spin_unlock_irqrestore(&fsd->lock, flags);

	return !rc ? &fsd->req_out : NULL;
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
		kfree(fsd->valid_zones_ptr);

	kfree(fsd->oob_buf);
	kfree(fsd->block_buf);

	mtdx_page_list_free(&fsd->special_blocks);
	long_map_destroy(fsd->b_map);
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
	fsd->block_size = fsd->geo.page_cnt * fsd->geo.page_size;

	spin_lock_init(&fsd->lock);

	if (fsd->zone_cnt > BITS_PER_LONG) {
		fsd->valid_zones_ptr = kzalloc(BITS_TO_LONGS(fsd->zone_cnt)
					       * sizeof(unsigned long),
					       GFP_KERNEL);
		if (!fsd->valid_zones_ptr) {
			rc = -ENOMEM;
			goto err_out;
		}
	} else
		fsd->valid_zones = 0;

	fsd->oob_buf = kmalloc(fsd->geo.oob_size, GFP_KERNEL);
	if (!fsd->oob_buf) {
		rc = -ENOMEM;
		goto err_out;
	}

	parent->get_param(parent, MTDX_PARAM_MEM_FILL_VALUE, &fsd->fill_value);
	parent->get_param(parent, MTDX_PARAM_READ_ONLY, &rc);

	if (!rc) {
		unsigned int r_cnt = min(fsd->geo.phy_block_cnt
					 - fsd->geo.log_block_cnt, 8U);

		if (((parent->id.inp_wmode == MTDX_WMODE_PAGE_PEB)
		      && (fsd->geo.page_cnt <= BITS_PER_LONG))
		     || (parent->id.inp_wmode == MTDX_WMODE_PAGE_PEB_INC)) {
			fsd->b_map = long_map_create(r_cnt, NULL, NULL, 0);
		} else if (parent->id.inp_wmode == MTDX_WMODE_PAGE_PEB) {
			fsd->b_map = long_map_create(r_cnt,
						     ftl_simple_alloc_pmap,
						     NULL, fsd->geo.page_cnt);
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

	}

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

	fsd->req_out.src_dev = mdev;
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
