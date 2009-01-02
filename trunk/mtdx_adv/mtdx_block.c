/*
 *  MTDX block device interface
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/idr.h>
#include <linux/hdreg.h>

#include "mtdx_common.h"

#define DRIVER_NAME "mtdx_block"
#define MTDX_BLOCK_PART_SHIFT 3
#define MTDX_BLOCK_MAX_SEGS  32
#define MTDX_BLOCK_MAX_PAGES 0x7ffffff

//#undef dev_dbg
//#define dev_dbg dev_emerg

static int major;
module_param(major, int, 0644);

struct mtdx_block_data {
	struct mtdx_dev       *mdev;
	unsigned int          usage_count;
	struct gendisk        *disk;
	struct request_queue  *queue;
	struct request        *block_req;
	struct mtdx_request   req_out;
	struct mtdx_data_iter req_data;
	spinlock_t            q_lock;
	struct mtdx_geo       geo;
	unsigned int          peb_size;
	struct hd_geometry    hd_geo;
	unsigned int          read_only:1,
			      eject:1;
};

static DEFINE_MUTEX(mtdx_block_disk_lock);
static DEFINE_IDA(mtdx_block_disk_ida);

static int mtdx_block_bd_open(struct inode *inode, struct file *filp)
{
	struct gendisk *disk = inode->i_bdev->bd_disk;
	struct mtdx_block_data *mbd = disk->private_data;
	int rc = -ENXIO;

	mutex_lock(&mtdx_block_disk_lock);

	if (mbd && mbd->mdev) {
		mbd->usage_count++;
		if ((filp->f_mode & FMODE_WRITE) && mbd->read_only)
			rc = -EROFS;
		else
			rc = 0;
	}

	mutex_unlock(&mtdx_block_disk_lock);

	return rc;
}

static int mtdx_block_disk_release(struct gendisk *disk)
{
	struct mtdx_block_data *mbd = disk->private_data;
	int disk_id = disk->first_minor >> MTDX_BLOCK_PART_SHIFT;

	mutex_lock(&mtdx_block_disk_lock);

	if (mbd) {
		printk(KERN_INFO "mtdx_block release, %d\n", mbd->usage_count);
		if (mbd->usage_count)
			mbd->usage_count--;

		if (!mbd->usage_count) {
			kfree(mbd);
			disk->private_data = NULL;
			ida_remove(&mtdx_block_disk_ida, disk_id);
			put_disk(disk);
		}
	}

	mutex_unlock(&mtdx_block_disk_lock);

	return 0;
}

static int mtdx_block_bd_release(struct inode *inode, struct file *filp)
{
	struct gendisk *disk = inode->i_bdev->bd_disk;
	return mtdx_block_disk_release(disk);
}

static int mtdx_block_bd_getgeo(struct block_device *bdev,
				struct hd_geometry *geo)
{
	struct mtdx_block_data *mbd = bdev->bd_disk->private_data;

	memcpy(geo, &mbd->hd_geo, sizeof(struct hd_geometry));

	return 0;
}

static struct block_device_operations mtdx_block_bdops = {
	.open    = mtdx_block_bd_open,
	.release = mtdx_block_bd_release,
	.getgeo  = mtdx_block_bd_getgeo,
	.owner   = THIS_MODULE
};

static void mtdx_block_end_request(struct mtdx_dev *this_dev,
				   struct mtdx_request *req,
				   unsigned int count,
				   int dst_error, int src_error)
{
	struct mtdx_block_data *mbd = mtdx_get_drvdata(this_dev);
	unsigned int flags;

	dev_dbg(&this_dev->dev, "end_request 1 %d, %x\n", dst_error, count);
	spin_lock_irqsave(&mbd->q_lock, flags);
	if (count)
		dst_error = 0;
	else {
		if (!dst_error)
			dst_error = -EIO;

		count = blk_rq_bytes(mbd->block_req);
	}

	dev_dbg(&this_dev->dev, "end_request 2 %d, %x\n", dst_error, count);

	__blk_end_request(mbd->block_req, dst_error, count);
	mbd->block_req = NULL;
	spin_unlock_irqrestore(&mbd->q_lock, flags);
}

//int limit = 500;

static struct mtdx_request *mtdx_block_get_request(struct mtdx_dev *mdev)
{
	struct mtdx_block_data *mbd = mtdx_get_drvdata(mdev);
	sector_t t_sec;
	unsigned int flags;

//	if (!limit)
//		return NULL;
//	limit--;
	spin_lock_irqsave(&mbd->q_lock, flags);
try_again:
	if (mbd->block_req) {
		t_sec = mbd->block_req->sector << 9;
		mbd->req_out.phy.b_addr = MTDX_INVALID_BLOCK;
		mbd->req_out.phy.offset = sector_div(t_sec, mbd->peb_size);
		mbd->req_out.logical = t_sec;
		mbd->req_out.length = blk_rq_bytes(mbd->block_req);

		dev_dbg(&mdev->dev, "req: logical %x, offset %x, length %x, "
			"peb_size %x\n", mbd->req_out.logical,
			mbd->req_out.phy.offset,
			mbd->req_out.length, mbd->peb_size);

		mbd->req_out.cmd = rq_data_dir(mbd->block_req) == READ
				   ? MTDX_CMD_READ
				   : MTDX_CMD_WRITE;

		mtdx_data_iter_init_bio(&mbd->req_data, mbd->block_req->bio);
		mbd->req_out.req_data = &mbd->req_data;
		spin_unlock_irqrestore(&mbd->q_lock, flags);
		return &mbd->req_out;
	} else {
		dev_dbg(&mdev->dev, "elv_next\n");
		mbd->block_req = elv_next_request(mbd->queue);
		if (!mbd->block_req) {
			dev_dbg(&mdev->dev, "issue end\n");
			spin_unlock_irqrestore(&mbd->q_lock, flags);
			return NULL;
		}
	}
	goto try_again;
}

static void mtdx_block_submit_req(struct request_queue *q)
{
	struct mtdx_dev *mdev = q->queuedata;
	struct mtdx_dev *parent = container_of(mdev->dev.parent,
					       struct mtdx_dev, dev);
	struct mtdx_block_data *mbd = mtdx_get_drvdata(mdev);
	struct request *req = NULL;

	if (mbd->block_req)
		return;

	if (mbd->eject) {
		while ((req = elv_next_request(q)) != NULL)
			end_queued_request(req, -ENODEV);

		return;
	}

	parent->new_request(parent, mdev);
}

static int mtdx_block_prepare_req(struct request_queue *q, struct request *req)
{
	if (!blk_fs_request(req) && !blk_pc_request(req)) {
		blk_dump_rq_flags(req, "MTDX unsupported request");
		return BLKPREP_KILL;
	}

	req->cmd_flags |= REQ_DONTPREP;

	return BLKPREP_OK;
}

static int mtdx_block_init_disk(struct mtdx_dev *mdev)
{
	struct mtdx_dev *parent = container_of(mdev->dev.parent,
					       struct mtdx_dev, dev);
	struct mtdx_block_data *mbd = mtdx_get_drvdata(mdev);
	int rc, disk_id;
	u64 limit = BLK_BOUNCE_HIGH;
	unsigned long capacity;

	parent->get_param(parent, MTDX_PARAM_DMA_MASK, &limit);

	if (!ida_pre_get(&mtdx_block_disk_ida, GFP_KERNEL))
		return -ENOMEM;

	mutex_lock(&mtdx_block_disk_lock);
	rc = ida_get_new(&mtdx_block_disk_ida, &disk_id);
	mutex_unlock(&mtdx_block_disk_lock);

	if (rc)
		return rc;

	if ((disk_id << MTDX_BLOCK_PART_SHIFT) > MINORMASK) {
		rc = -ENOSPC;
		goto out_release_id;
	}

	mbd->disk = alloc_disk(1 << MTDX_BLOCK_PART_SHIFT);
	if (!mbd->disk) {
		rc = -ENOMEM;
		goto out_release_id;
	}

	mbd->queue = blk_init_queue(mtdx_block_submit_req, &mbd->q_lock);
	if (!mbd->queue) {
		rc = -ENOMEM;
		goto out_put_disk;
	}

	mbd->queue->queuedata = mdev;
	blk_queue_prep_rq(mbd->queue, mtdx_block_prepare_req);

	blk_queue_bounce_limit(mbd->queue, limit);
	blk_queue_max_sectors(mbd->queue, MTDX_BLOCK_MAX_PAGES);
	blk_queue_max_phys_segments(mbd->queue, MTDX_BLOCK_MAX_SEGS);
	blk_queue_max_hw_segments(mbd->queue, MTDX_BLOCK_MAX_SEGS);
	blk_queue_max_segment_size(mbd->queue,
				   MTDX_BLOCK_MAX_PAGES * mbd->geo.page_size);
	blk_queue_hardsect_size(mbd->queue, mbd->geo.page_size);

	mbd->disk->major = major;
	mbd->disk->first_minor = disk_id << MTDX_BLOCK_PART_SHIFT;
	mbd->disk->fops = &mtdx_block_bdops;
	mbd->usage_count = 1;
	mbd->disk->private_data = mbd;
	mbd->disk->queue = mbd->queue;
	mbd->disk->driverfs_dev = &mdev->dev;

	{
		char d_name[DEVICE_ID_SIZE] = {};

		rc = parent->get_param(parent, MTDX_PARAM_DEV_SUFFIX, &d_name);

		if (rc || !strlen(d_name))
			snprintf(mbd->disk->disk_name, DEVICE_ID_SIZE,
				 "mtdxblk%d", disk_id);
		else
			snprintf(mbd->disk->disk_name, DEVICE_ID_SIZE,
				 "mtdxblk%s%d", d_name, disk_id);
	}

	capacity = mbd->geo.log_block_cnt;
	capacity *= mbd->geo.page_cnt;
	capacity *= mbd->geo.page_size;
	capacity >>= 9;

	set_capacity(mbd->disk, capacity);
	dev_dbg(&mdev->dev, "mdev %p, capacity set %ld\n", mdev, capacity);

	add_disk(mbd->disk);
	return 0;

out_put_disk:
	put_disk(mbd->disk);
out_release_id:
	mutex_lock(&mtdx_block_disk_lock);
	ida_remove(&mtdx_block_disk_ida, disk_id);
	mutex_unlock(&mtdx_block_disk_lock);
	return rc;
}

static int mtdx_block_probe(struct mtdx_dev *mdev)
{
	struct mtdx_dev *parent = container_of(mdev->dev.parent,
					       struct mtdx_dev, dev);
	struct mtdx_block_data *mbd = kzalloc(sizeof(struct mtdx_block_data),
					      GFP_KERNEL);
	int rc;

	if (!mbd)
		return -ENOMEM;

	spin_lock_init(&mbd->q_lock);
	mtdx_set_drvdata(mdev, mbd);
	mbd->mdev = mdev;

	rc = parent->get_param(parent, MTDX_PARAM_GEO, &mbd->geo);
	if (rc)
		goto err_out;

	parent->get_param(parent, MTDX_PARAM_HD_GEO, &mbd->hd_geo);

	mbd->peb_size = mbd->geo.page_cnt * mbd->geo.page_size;
	mdev->get_request = mtdx_block_get_request;
	mdev->end_request = mtdx_block_end_request;

	rc = mtdx_block_init_disk(mdev);
	dev_dbg(&mdev->dev, "init_disk %d\n", rc);
	if (!rc)
		return 0;

err_out:
	mtdx_set_drvdata(mdev, NULL);
	kfree(mbd);
	return rc;
}

static void mtdx_block_remove(struct mtdx_dev *mdev)
{
	struct mtdx_block_data *mbd = mtdx_get_drvdata(mdev);
	unsigned long flags;

	del_gendisk(mbd->disk);
	dev_dbg(&mdev->dev, "mtdx block del disk\n");
	spin_lock_irqsave(&mbd->q_lock, flags);
	mbd->eject = 1;
	blk_start_queue(mbd->queue);
	spin_unlock_irqrestore(&mbd->q_lock, flags);

	blk_cleanup_queue(mbd->queue);
	mbd->queue = NULL;

	mutex_lock(&mtdx_block_disk_lock);
	mbd->mdev = NULL;
	mutex_unlock(&mtdx_block_disk_lock);

	mtdx_block_disk_release(mbd->disk);
	mtdx_set_drvdata(mdev, NULL);
	dev_dbg(&mdev->dev, "mtdx block remove\n");
}

#ifdef CONFIG_PM

static int mtdx_block_suspend(struct mtdx_dev *mdev, pm_message_t state)
{
	struct mtdx_block_data *mbd = mtdx_get_drvdata(mdev);
	unsigned long flags;

	spin_lock_irqsave(&mbd->q_lock, flags);
	blk_stop_queue(mbd->queue);
	spin_unlock_irqrestore(&mbd->q_lock, flags);

	return 0;
}

static int mtdx_block_resume(struct mtdx_dev *mdev)
{
	struct mtdx_block_data *mbd = mtdx_get_drvdata(mdev);
	unsigned long flags;

	spin_lock_irqsave(&mbd->q_lock, flags);
	blk_start_queue(mbd->queue);
	spin_unlock_irqrestore(&mbd->q_lock, flags);

	return 0;
}

#else

#define mtdx_block_suspend NULL
#define mtdx_block_resume NULL

#endif /* CONFIG_PM */

static struct mtdx_device_id mtdx_block_id_tbl[] = {
	{ MTDX_WMODE_NONE, MTDX_WMODE_PAGE, MTDX_RMODE_NONE,
	  MTDX_RMODE_PAGE, MTDX_TYPE_ADAPTER, MTDX_ID_ADAPTER_BLKDEV },
	{}
};

static struct mtdx_driver mtdx_block_driver = {
	.driver = {
		.name  = DRIVER_NAME,
		.owner = THIS_MODULE
	},
	.id_table = mtdx_block_id_tbl,
	.probe    = mtdx_block_probe,
	.remove   = mtdx_block_remove,
	.suspend  = mtdx_block_suspend,
	.resume   = mtdx_block_resume
};

static int __init mtdx_block_init(void)
{
	int rc = -ENOMEM;

	rc = register_blkdev(major, DRIVER_NAME);
	if (rc < 0) {
		printk(KERN_ERR DRIVER_NAME ": failed to register "
		       "major %d, error %d\n", major, rc);
		return rc;
	}
	if (!major)
		major = rc;

	rc = mtdx_register_driver(&mtdx_block_driver);
	if (rc)
		unregister_blkdev(major, DRIVER_NAME);

	return rc;
}

static void __exit mtdx_block_exit(void)
{
	mtdx_unregister_driver(&mtdx_block_driver);
	unregister_blkdev(major, DRIVER_NAME);
}

module_init(mtdx_block_init);
module_exit(mtdx_block_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("MTDX block device adapter");
MODULE_DEVICE_TABLE(mtdx, mtdx_block_id_tbl);
