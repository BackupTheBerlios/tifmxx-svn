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
	unsigned int  b_table[];
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
	unsigned int          tmp_off;
	unsigned int          sg_length;
	unsigned int          zone_cnt;
	unsigned int          zone_block_cnt;
	unsigned int          block_size;
	unsigned int          zone_scan_pos;
	unsigned int          t_count;

	unsigned char         fill_value;
	unsigned char         dumb_copy:1,
			      track_inc:1;

	unsigned int          zone;
	unsigned int          src_block;
	unsigned int          dst_block;
	unsigned int          b_off;
	unsigned int          b_len;

	union {
		unsigned long         valid_zones;
		unsigned long         *valid_zones_ptr;
	};

	struct mtdx_page_info *sp_block_pos;
	struct list_head      special_blocks;

	int                   (*setup_req_fn)(struct ftl_simple_data *fsd);
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

static int bitmap_region_empty(unsigned long *bitmap, unsigned int offset,
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

static void bitmap_clear_region(unsigned long *bitmap, unsigned int offset,
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

static void bitmap_set_region(unsigned long *bitmap, unsigned int offset,
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

static int ftl_simple_setup_zone_scan(struct ftl_simple_data *fsd)
{
}

static int ftl_simple_get_data_buf_sg(struct mtdx_request *req,
				      struct scatterlist *sg)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(req->src_dev);

	if (!fsd->sg_length)
		return -ENOMEM;

	if (fsd->sg_off >= fsd->req_sg.length) {
		int rc = mtdx_get_data_buf_sg(fsd->req_in, &fsd->req_sg);
		if (rc)
			return rc;
		fsd->sg_off = 0;
	}

	memcpy(sg, &fsd->req_sg, sizeof(struct scatterlist));
	sg->offset += fsd->sg_off;
	sg->length = min(fsd->sg_length, sg->length - fsd->sg_off);
	fsd->sg_length -= sg->length;

	return 0;
}

static char *ftl_simple_get_oob_buf(struct mtdx_request *req)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(req->src_dev);

	return fsd->oob_buf;
}

static int ftl_simple_get_tmp_buf_sg(struct mtdx_request *req,
				     struct scatterlist *sg)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(req->src_dev);
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&fsd->lock, flags);
	if (fsd->sg_length) {
		sg_set_buf(sg, fsd->block_buf + fsd->tmp_off, fsd->sg_length);
		fsd->sg_length = 0;
	} else
		rc = -ENOMEM;

	spin_unlock_irqrestore(&fsd->lock, flags);
	return rc;
}

static int ftl_simple_can_merge(struct ftl_simple_data *fsd, unsigned int peb)
{
	unsigned int z_peb = peb & ((1U << fsd->geo.zone_size_log) - 1U);
	unsigned long *val;

	if (peb == MTDX_INVALID_BLOCK)
		return 0;

	if (!fsd->b_map
	    || !test_bit(z_peb, fsd->zones[fsd->zone]->useful_blocks))
		return 0;

	val = long_map_find(fsd->b_map, peb);
	if (!val) {
		clear_bit(z_peb, fsd->zones[fsd->zone]->useful_blocks);
		return 0;
	}

	if (fsd->track_inc)
		return *val <= (fsd->b_off / fsd->geo.page_size);
	else
		return bitmap_region_empty(val, fsd->b_off / fsd->geo.page_size,
					   fsd->b_len / fsd->geo.page_size);
}

static void ftl_simple_mark_useful(struct ftl_simple_data *fsd,
				   unsigned int peb, unsigned int offset,
				   unsigned int length)
{
	unsigned int z_peb = peb & ((1U << fsd->geo.zone_size_log) - 1U);
	unsigned long *val = long_map_insert(fsd->b_map, fsd->dst_block);

	if (!val)
		return;

	if (fsd->track_inc)
		*val = (fsd->b_off + fsd->b_len) / fsd->geo.page_size;
	else {
		bitmap_zero(val, fsd->geo.page_cnt);
		bitmap_set_region(val, fsd->b_off / fsd->geo.page_size,
				  fsd->b_len / fsd->geo.page_cnt);
	}

	set_bit(z_peb, fsd->zones[fsd->zone]->useful_blocks);
}

static int ftl_simple_do_copy_write(struct ftl_simple_data *fsd)
{
	struct mtdx_page_info p_info = {};
	int rc;

	p_info.status = MTDX_PAGE_MAPPED;
	p_info.log_block = fsd->req_out.log_block;
	p_info.phy_block = fsd->dst_block;

	rc = fsd->req_dev->info_to_oob(fsd->req_dev, fsd->oob_buf, &p_info);
	if (rc)
		return rc;

	if (fsd->b_len == fsd->block_size) {
		fsd->end_req_fn = ftl_simple_write_end;
		fsd->req_out.cmd = MTDX_CMD_WRITE;
		fsd->req_out.phy_block = fsd->dst_block;
		fsd->req_out.offset = 0;
		fsd->req_out.length = 0;
		return 0;
	}

	if (fsd->src_block == MTDX_INVALID_BLOCK) {
		if (!fsd->b_map) {
			memset(fsd->block_buf, fsd->fill_value,
			       fsd->block_size);
			if (rc = ftl_simple_bounce_data(fsd))
				return rc;
			fsd->tmp_off = 0;
			fsd->sg_length = fsd->block_size;
			fsd->end_req_fn = ftl_simple_write_tmp_end;
			fsd->req_out
			     .src_dev
			     ->get_data_buf_sg = ftl_simple_get_tmp_buf_sg;
			fsd->req_out.cmd = MTDX_CMD_WRITE;
			fsd->req_out.phy_block = fsd->dst_block;
			fsd->req_out.offset = 0;
			fsd->req_out.length = fsd->block_size;
			return 0;
		} else {
			fsd->req_out.cmd = MTDX_CMD_WRITE;
			fsd->req_out.phy_block = fsd->dst_block;
			fsd->req_out.offset = fsd->b_off;
			fsd->req_out.length = fsd->b_len;
			fsd->end_req_fn = ftl_simple_write_end;
		}
	} else if ((fsd->src_block == fsd->dst_block) || fsd->dumb_copy)
		return ftl_simple_read_first(fsd);
	else
		return ftl_simple_copy_first(fsd);
}

static int ftl_simple_setup_write(struct ftl_simple_data *fsd)
{
	unsigned int pos;
	unsigned int z_log_block;
	int rc = 0;

	if (fsd->setup_req_fn)
		return fsd->setup_req_fn(fsd);

	pos = fsd->req_in->offset + fsd->t_count;

	if (fsd->dst_block == MTDX_INVALID_BLOCK) {
		fsd->req_out.log_block = fsd->req_in->log_block;
		fsd->req_out.log_block += pos / fsd->block_size;
		fsd->zone = fsd->req_out.log_block >> fsd->geo.zone_size_log;
		fsd->b_off = pos % fsd->block_size;
		fsd->b_len = min(fsd->req_in->length - fsd->t_count,
				 fsd->block_size - fsd->req_out.offset);

		if (!test_bit(fsd->zone, ftl_simple_zone_map(fsd)))
			return ftl_simple_setup_zone_scan(fsd);

		z_log_block = fsd->req_out.log_block
			      & ((1U << fsd->geo.zone_size_log) - 1U);

		fsd->src_block = fsd->zones[fsd->zone]->b_table[z_log_block];

		if (ftl_simple_can_merge(fsd, fsd->src_block)) {
			struct mtdx_page_info p_info = {};

			p_info.status = MTDX_PAGE_MAPPED;
			p_info.log_block = fsd->req_out.log_block;
			p_info.phy_block = fsd->src_block;

			rc = fsd->req_dev->info_to_oob(fsd->req_dev,
						       fsd->oob_buf, &p_info);
			if (rc)
				return rc;

			fsd->end_req_fn = ftl_simple_write_merge_end;
			fsd->req_out.cmd = MTDX_CMD_WRITE;
			fsd->req_out.phy_block = fsd->src_block;
			fsd->req_out.offset = fsd->b_off;
			fsd->req_out.length = fsd->b_len;
			return 0;
		} else
			fsd->dst_block = mtdx_get_peb(fsd->b_alloc, fsd->zone,
						      &rc);

		if (fsd->dst_block == MTDX_INVALID_BLOCK) {
			if (fsd->src_block == MTDX_INVALID_BLOCK)
				return -EIO;

			fsd->dst_block = fsd->src_block;
		} else if (rc) {
			fsd->end_req_fn = ftl_simple_erase_dst_end;
			fsd->req_out.cmd = MTDX_CMD_ERASE;
			fsd->req_out.phy_block = fsd->dst_block;
			fsd->req_out.offset = 0;
			fsd->req_out.length = 0;
			return 0;
		}
	}

	if ((fsd->src_block != MTDX_INVALID_BLOCK)
	    && (fsd->src_block != fsd->dst_block)) {
		fsd->end_req_fn = ftl_simple_select_src_end;
		fsd->req_out.cmd = MTDX_CMD_SELECT;
		fsd->req_out.phy_block = fsd->src_block;
		fsd->req_out.offset = 0;
		fsd->req_out.length = 0;
		return 0;
	} else
		return ftl_simple_do_copy_write(fsd);
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
	unsigned int z_log_block;

try_again:
	pos = fsd->req_in->offset + fsd->t_count;
	fsd->req_out.log_block = fsd->req_in->log_block;
	fsd->req_out.log_block += pos / fsd->block_size;
	fsd->zone = fsd->req_out.log_block >> fsd->geo.zone_size_log;

	if (!test_bit(fsd->zone, ftl_simple_zone_map(fsd)))
		return ftl_simple_setup_zone_scan(fsd);

	z_log_block = fsd->req_out.log_block
		      & ((1U << fsd->geo.zone_size_log) - 1U);

	fsd->req_out.cmd = MTDX_CMD_READ_DATA;
	fsd->req_out.phy_block = fsd->zones[fsd->zone]->b_table[z_log_block];
	fsd->req_out.offset = pos % fsd->block_size;
	fsd->req_out.length = min(fsd->req_in->length - fsd->t_count,
				  fsd->block_size - fsd->req_out.offset);
	fsd->sg_length = fsd->req_out.length;

	fsd->req_out.src_dev->get_data_buf_sg = ftl_simple_get_data_buf_sg;

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
	if (!fsd->req_dev) {
		spin_unlock_irqrestore(&fsd->lock, flags);
		return NULL;
	}

	while (1) {
		if (fsd->req_in) {
			if (fsd->req_in->cmd == MTDX_CMD_READ_DATA)
				rc = ftl_simple_setup_read(fsd);
			else if (fsd->req_in->cmd == MTDX_CMD_WRITE_DATA)
				rc = fsd->b_alloc ? ftl_simple_setup_write(fsd)
						  : -EROFS;
			else
				rc = -EINVAL;

			if (!rc)
				goto out;
			else {
				mtdx_complete_request(fsd->req_in, rc,
						      fsd->t_count);
				fsd->req_in = NULL;
			}
		} else {
			rc = -EAGAIN;
			break;
		}

		fsd->req_sg.length = 0;
		fsd->sg_off = 0;
		fsd->req_in = fsd->req_dev->get_request(fsd->req_dev);
	}

	fsd->req_dev = NULL;
	atomic_long_dec(&fsd->usage_count);

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

		if (fsd->b_map
		    && (parent->id.inp_wmode == MTDX_WMODE_PAGE_PEB_INC))
			fsd->track_inc = 1;

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
	fsd->src_block = MTDX_INVALID_BLOCK;
	fsd->dst_block = MTDX_INVALID_BLOCK;
	mtdx_set_drvdata(mdev, fsd);
	mdev->new_request = ftl_simple_new_request;
	mdev->get_request = ftl_simple_get_request;
	mdev->end_request = ftl_simple_end_request;
	mdev->get_data_buf_sg = ftl_simple_get_data_buf_sg;
	mdev->get_oob_buf = ftl_simple_get_oob_buf;
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
