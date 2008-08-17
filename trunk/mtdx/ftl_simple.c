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

struct ftl_simple_data;

typedef int (req_fn_t)(struct ftl_simple_data *fsd);

#define FTL_SIMPLE_MAX_REQ_FN 10

struct ftl_simple_data {
	spinlock_t            lock;
	unsigned int          usage_count;

	/* Device geometry */
	struct mtdx_dev_geo   geo;
	unsigned int          zone_cnt;
	unsigned int          zone_block_cnt;
	unsigned int          block_size;

	/* Incoming request */
	struct mtdx_dev       *req_dev;
	struct mtdx_request   *req_in;
	struct scatterlist    req_sg;

	/* Outgoing request */
	struct mtdx_request   req_out;
	unsigned int          tmp_off;
	unsigned int          sg_off;
	unsigned int          sg_length;
	unsigned int          t_count;
	int                   src_error;

	/* Processing modifiers */
	unsigned int          dumb_copy:1,
			      track_inc:1,
			      clean_dst:1;

	/* Current request address */
	unsigned int          zone;
	unsigned int          src_block;
	unsigned int          dst_block;
	unsigned int          b_off;
	unsigned int          b_len;

	/* Block address translation */
	union {
		unsigned long         valid_zones;
		unsigned long         *valid_zones_ptr;
	};

	struct long_map       *b_map;
	struct mtdx_peb_alloc *b_alloc;

	unsigned int          zone_scan_pos;
	unsigned int          conflict_pos;
	struct list_head      *sp_block_pos;
	struct list_head      special_blocks;

	unsigned long         *src_bmap_ref;

	/* Request processing */
	unsigned int          req_fn_pos;
	req_fn_t              *req_fn[FTL_SIMPLE_MAX_REQ_FN];
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

static void ftl_simple_pop_all_req_fn(struct ftl_simple_data *fsd)
{
	fsd->req_fn_pos = 0;
}

static req_fn_t *ftl_simple_pop_req_fn(struct ftl_simple_data *fsd)
{
	req_fn_t *rv = NULL;

	if (fsd->req_fn_pos) {
		fsd->req_fn_pos--;
		rv = fsd->req_fn[fsd->req_fn_pos];
		fsd->req_fn[fsd->req_fn_pos] = NULL;
	}

	return rv;
}

static void ftl_simple_push_req_fn(struct ftl_simple_data *fsd,
				   req_fn_t *req_fn)
{
	BUG_ON(fsd->req_fn_pos >= FTL_SIMPLE_MAX_REQ_FN);

	fsd->req_fn[fsd->req_fn_pos++] = req_fn;
}

static int ftl_simple_lookup_block(struct ftl_simple_data *fsd);
static int ftl_simple_setup_request(struct ftl_simple_data *fsd);

static void ftl_simple_end_resolve(struct ftl_simple_data *fsd, int error,
				   unsigned int count)
{
	struct mtdx_dev *parent = container_of(fsd->req_out.src_dev->dev.parent,
					       struct mtdx_dev, dev);
	unsigned int max_block = (fsd->zone + 1) << fsd->geo.zone_size_log;
	unsigned int z_log_block;
	struct mtdx_page_info p_info = {};

	p_info.phy_block = fsd->conflict_pos;

	if (!error)
		error = parent->oob_to_info(parent, &p_info, fsd->oob_buf);

	z_log_block = p_info.log_block & ((1U << fsd->geo.zone_size_log) - 1U);

	if (!error) {
		if (p_info.status == MTDX_PAGE_SMAPPED)
			mtdx_put_peb(fsd->b_alloc, fsd->zone_scan_pos, 1);
		else {
			mtdx_put_peb(fsd->b_alloc,
				     fsd->zones[fsd->zone]
					->b_table[z_log_block], 1);
			fsd->zones[fsd->zone]->b_table[z_log_block]
				= fsd->zone_scan_pos;
		}
	}

	fsd->zone_scan_pos++;
	if (fsd->zone_scan_pos < max_block) {
		ftl_simple_pop_all_req_fn(fsd);
		set_bit(fsd->zone, ftl_simple_zone_map(fsd));
	} else
		ftl_simple_push_req_fn(fsd, ftl_simple_lookup_block);
}

static int ftl_simple_resolve(struct ftl_simple_data *fsd)
{
	memset(fsd->oob_buf, fsd->geo.fill_value, fsd->geo.oob_size);
	fsd->req_out.cmd = MTDX_CMD_READ_OOB;
	fsd->req_out.log_block = MTDX_INVALID_BLOCK;
	fsd->req_out.phy_block = fsd->conflict_pos;
	fsd->req_out.offset = 0;
	fsd->req_out.length = 0;
	fsd->end_req_fn = ftl_simple_end_resolve;
	return 0;

}

static void ftl_simple_end_lookup_block(struct ftl_simple_data *fsd, int error,
					unsigned int count)
{
	struct mtdx_dev *parent = container_of(fsd->req_out.src_dev->dev.parent,
					       struct mtdx_dev, dev);
	unsigned int max_block = (fsd->zone + 1) << fsd->geo.zone_size_log;
	unsigned int z_log_block;
	struct mtdx_page_info p_info = {};

	p_info.phy_block = fsd->zone_scan_pos;

	if (!error)
		error = parent->oob_to_info(parent, &p_info, fsd->oob_buf);

	z_log_block = p_info.log_block & ((1U << fsd->geo.zone_size_log) - 1U);

	if (!error) {
		fsd->conflict_pos = fsd->zones[fsd->zone]
				       ->b_table[z_log_block];
		switch (p_info.status) {
		case MTDX_PAGE_ERASED:
			dev_dbg(&parent->dev, "erased block %x\n",
				fsd->zone_scan_pos);
			mtdx_put_peb(fsd->b_alloc, fsd->zone_scan_pos, 0);
			break;
		case MTDX_PAGE_UNMAPPED:
			dev_dbg(&parent->dev, "free block %x\n",
				fsd->zone_scan_pos);
			mtdx_put_peb(fsd->b_alloc, fsd->zone_scan_pos, 1);
			break;
		case MTDX_PAGE_MAPPED:
			dev_dbg(&parent->dev, "allocated block %x\n",
				fsd->zone_scan_pos);
			if (fsd->conflict_pos != MTDX_INVALID_BLOCK) {
				ftl_simple_pop_all_req_fn(fsd);
				ftl_simple_push_req_fn(fsd, ftl_simple_resolve);
				return;
			} else
				fsd->zones[fsd->zone]->b_table[z_log_block]
					= fsd->zone_scan_pos;
			break;
		case MTDX_PAGE_SMAPPED:
			dev_dbg(&parent->dev, "selected block %x\n",
				fsd->zone_scan_pos);
			/* As higher address supposedly take preference over
			 * lower ones and the block is selected, conflict
			 * can be decided right now.
			 */
			fsd->zones[fsd->zone]->b_table[z_log_block]
				= fsd->zone_scan_pos;
			if (fsd->conflict_pos != MTDX_INVALID_BLOCK)
				mtdx_put_peb(fsd->b_alloc, fsd->conflict_pos,
					     1);
			break;
		case MTDX_PAGE_INVALID:
		case MTDX_PAGE_FAILURE:
		case MTDX_PAGE_RESERVED:
			dev_dbg(&parent->dev, "bad block %x\n",
				fsd->zone_scan_pos);
			break;
		default:
			error = -EINVAL;
		}
	}

	fsd->zone_scan_pos++;
	if (fsd->zone_scan_pos >= max_block) {
		ftl_simple_pop_all_req_fn(fsd);
		set_bit(fsd->zone, ftl_simple_zone_map(fsd));
		if (fsd->req_in)
			ftl_simple_setup_request(fsd);
	} else
		ftl_simple_push_req_fn(fsd, ftl_simple_lookup_block);
}

static int ftl_simple_lookup_block(struct ftl_simple_data *fsd)
{
	if (fsd->sp_block_pos) {
		struct mtdx_page_info *p_info;
		unsigned int max_block = (fsd->zone + 1)
					 << fsd->geo.zone_size_log;

		p_info  = list_entry(fsd->sp_block_pos, struct mtdx_page_info,
				     node);
		if (p_info->phy_block == fsd->zone_scan_pos) {
			fsd->sp_block_pos = p_info->node.next;

			if (fsd->sp_block_pos == &fsd->special_blocks)
				fsd->sp_block_pos = NULL;

			fsd->zone_scan_pos++;
			if (fsd->zone_scan_pos < max_block) {
				ftl_simple_pop_all_req_fn(fsd);
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_lookup_block);
			} else
				set_bit(fsd->zone, ftl_simple_zone_map(fsd));

			return -EAGAIN;
		}
	}

	memset(fsd->oob_buf, fsd->geo.fill_value, fsd->geo.oob_size);
	fsd->req_out.cmd = MTDX_CMD_READ_OOB;
	fsd->req_out.log_block = MTDX_INVALID_BLOCK;
	fsd->req_out.phy_block = fsd->zone_scan_pos;
	fsd->req_out.offset = 0;
	fsd->req_out.length = fsd->geo.page_size;
	fsd->end_req_fn = ftl_simple_end_lookup_block;
	return 0;
}

static void ftl_simple_clear_useful(struct ftl_simple_data *fsd)
{
	unsigned int b_block = fsd->zone << fsd->geo.zone_size_log;
	unsigned int pos = find_first_bit(fsd->zones[fsd->zone]->useful_blocks,
					  fsd->zone_block_cnt);

	while (pos < fsd->zone_block_cnt) {
		long_map_erase(fsd->b_map, b_block + pos);
		clear_bit(pos, fsd->zones[fsd->zone]->useful_blocks);
		pos = find_next_bit(fsd->zones[fsd->zone]->useful_blocks,
				    fsd->zone_block_cnt, pos);
	}
}

static int ftl_simple_setup_zone_scan(struct ftl_simple_data *fsd)
{
	unsigned int max_block = (fsd->zone + 1) << fsd->geo.zone_size_log;
	unsigned int cnt;
	struct mtdx_page_info *p_info;

	dev_dbg(&fsd->req_out.src_dev.dev, "scanning zone %x\n", fsd->zone);
	fsd->zone_scan_pos = fsd->zone << fsd->geo.zone_size_log;
	fsd->conflict_pos = MTDX_INVALID_BLOCK;
	mtdx_peb_alloc_reset(fsd->b_alloc, fsd->zone);
	if (fsd->b_map)
		ftl_simple_clear_useful(fsd);

	__list_for_each(fsd->sp_block_pos, &fsd->special_blocks) {
		p_info = list_entry(fsd->sp_block_pos, struct mtdx_page_info,
				    node);
		if ((p_info->phy_block >= fsd->zone_scan_pos)
		    && (p_info->phy_block < max_block))
			break;
	}

	if (fsd->sp_block_pos == &fsd->special_blocks)
		fsd->sp_block_pos = NULL;

	for (cnt = 0; cnt < (1 << fsd->geo.zone_size_log); ++cnt)
		fsd->zones[fsd->zone]->b_table[cnt] = MTDX_INVALID_BLOCK;

	ftl_simple_push_req_fn(fsd, ftl_simple_lookup_block);
	return 0;
}

static int ftl_simple_get_data_buf_sg(struct mtdx_request *req,
				      struct scatterlist *sg)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(req->src_dev);

	if (!fsd->sg_length)
		return -EAGAIN;

	if (fsd->sg_off >= fsd->req_sg.length) {
		int rc = mtdx_get_data_buf_sg(fsd->req_in, &fsd->req_sg);
		if (rc) {
			fsd->sg_length = 0;
			return rc;
		}
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

	dev_dbg(&req->src_dev->dev, "ftl get oob buf\n");
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
		rc = -EAGAIN;

	spin_unlock_irqrestore(&fsd->lock, flags);
	return rc;
}

static int ftl_simple_can_merge(struct ftl_simple_data *fsd,
				unsigned int offset, unsigned int count)
{
	if (!fsd->src_bmap_ref || !count)
		return 0;

	if (fsd->track_inc)
		return *(fsd->src_bmap_ref)
		       <= (offset / fsd->geo.page_size);
	else
		return bitmap_region_empty(fsd->src_bmap_ref,
					   offset / fsd->geo.page_size,
					   count / fsd->geo.page_size);
}

static void ftl_simple_drop_useful(struct ftl_simple_data *fsd,
				   unsigned int peb)
{
	unsigned int z_peb = peb & ((1U << fsd->geo.zone_size_log) - 1U);

	if ((peb == MTDX_INVALID_BLOCK) || !fsd->b_map)
		return;

	long_map_erase(fsd->b_map, peb);
	clear_bit(z_peb, fsd->zones[peb >> fsd->geo.zone_size_log]
			    ->useful_blocks);
}

static void ftl_simple_advance(struct ftl_simple_data *fsd)
{
	unsigned int z_log_block = fsd->req_out.log_block
				   & ((1U << fsd->geo.zone_size_log) - 1U);
	unsigned int z_dst_block = fsd->dst_block
				   & ((1U << fsd->geo.zone_size_log) - 1U);
	unsigned long *dst_bmap_ref;

	fsd->t_count += fsd->b_len;
	if (fsd->dst_block != fsd->src_block)
		fsd->zones[fsd->zone]->b_table[z_log_block] = fsd->dst_block;

	if (!fsd->src_bmap_ref)
		return;

	fsd->tmp_off = fsd->b_off + fsd->b_len;

	if (fsd->track_inc) {
		fsd->tmp_off /= fsd->geo.page_size;

		if (*fsd->src_bmap_ref < fsd->tmp_off)
			*fsd->src_bmap_ref = fsd->tmp_off;

		if (*fsd->src_bmap_ref < fsd->block_size) {
			dst_bmap_ref = long_map_insert(fsd->b_map,
						       fsd->dst_block);
			if (dst_bmap_ref) {
				*dst_bmap_ref = *fsd->src_bmap_ref;
				set_bit(z_dst_block, fsd->zones[fsd->zone]
							->useful_blocks);
			}
		}
	} else {
		unsigned int p_off = fsd->b_off / fsd->geo.page_size;
		unsigned int p_len = fsd->b_len / fsd->geo.page_size;

		if (fsd->b_off) {
			if (ftl_simple_can_merge(fsd, 0, fsd->b_off))
				set_bit(0, fsd->src_bmap_ref);
			else
				bitmap_set_region(fsd->src_bmap_ref, 0, p_off);
		}

		if (fsd->tmp_off < fsd->block_size) {
			if (!ftl_simple_can_merge(fsd, fsd->tmp_off,
						  fsd->block_size
						  - fsd->tmp_off))
				bitmap_set_region(fsd->src_bmap_ref,
						  p_off + p_len,
						  fsd->geo.page_cnt - p_off
						  - p_len);
		}

		bitmap_set_region(fsd->src_bmap_ref, p_off, p_len);

		if (!bitmap_full(fsd->src_bmap_ref, fsd->geo.page_cnt)) {
			dst_bmap_ref = long_map_insert(fsd->b_map,
						       fsd->dst_block);
			if (dst_bmap_ref) {
				memcpy(dst_bmap_ref, fsd->src_bmap_ref,
				       BITS_TO_LONGS(fsd->geo.page_cnt)
				       * sizeof(unsigned long));

				set_bit(z_dst_block, fsd->zones[fsd->zone]
							->useful_blocks);
			}
		}
	}

	ftl_simple_drop_useful(fsd, fsd->src_block);
}

static void ftl_simple_end_abort(struct ftl_simple_data *fsd, int error,
				 int free_dst)
{
	if (free_dst && (fsd->dst_block != fsd->src_block)) {
		fsd->b_alloc->put_peb(fsd->b_alloc, fsd->dst_block,
				      fsd->clean_dst == 1);
		fsd->dst_block = MTDX_INVALID_BLOCK;
	}

	mtdx_complete_request(fsd->req_in, error, fsd->t_count);
	ftl_simple_pop_all_req_fn(fsd);
	fsd->req_in = NULL;
}

static void ftl_simple_end_invalidate_dst(struct ftl_simple_data *fsd,
					  int error, unsigned int count)
{
	ftl_simple_end_abort(fsd, fsd->src_error, 0);
}

static int ftl_simple_invalidate_dst(struct ftl_simple_data *fsd)
{
	fsd->req_out.cmd = MTDX_CMD_INVALIDATE;
	fsd->req_out.phy_block = fsd->dst_block;
	fsd->req_out.offset = 0;
	fsd->req_out.length = 0;
	fsd->sg_length = 0;
	fsd->end_req_fn = ftl_simple_end_invalidate_dst;
	return 0;
}

static void ftl_simple_end_erase_src(struct ftl_simple_data *fsd, int error,
				     unsigned int count)
{
	if (!error)
		fsd->b_alloc->put_peb(fsd->b_alloc, fsd->src_block, 1);
}

static int ftl_simple_erase_src(struct ftl_simple_data *fsd)
{
	fsd->req_out.cmd = MTDX_CMD_ERASE;
	fsd->req_out.phy_block = fsd->src_block;
	fsd->req_out.offset = 0;
	fsd->req_out.length = 0;
	fsd->sg_length = 0;
	fsd->end_req_fn = ftl_simple_end_erase_src;
	return 0;
}

static void ftl_simple_end_erase_dst(struct ftl_simple_data *fsd, int error,
				     unsigned int count)
{
	if (error) {
		ftl_simple_pop_all_req_fn(fsd);
		ftl_simple_push_req_fn(fsd, ftl_simple_invalidate_dst);
		fsd->src_error = error;
	} else
		fsd->clean_dst = 1;
}

static int ftl_simple_erase_dst(struct ftl_simple_data *fsd)
{
	fsd->req_out.cmd = MTDX_CMD_ERASE;
	fsd->req_out.phy_block = fsd->dst_block;
	fsd->req_out.offset = 0;
	fsd->req_out.length = 0;
	fsd->sg_length = 0;
	fsd->end_req_fn = ftl_simple_end_erase_dst;
	return 0;
}

static int ftl_simple_select_src(struct ftl_simple_data *fsd)
{
	fsd->req_out.cmd = MTDX_CMD_SELECT;
	fsd->req_out.phy_block = fsd->src_block;
	fsd->req_out.offset = 0;
	fsd->req_out.length = 0;
	fsd->sg_length = 0;
	fsd->end_req_fn = NULL;
	return 0;
}

static void ftl_simple_end_merge_data(struct ftl_simple_data *fsd, int error,
				      unsigned int count)
{
	if ((count != fsd->b_len) && !error)
		error = -EIO;

	if (!error && fsd->src_bmap_ref) {
		if (fsd->track_inc) {
			*(fsd->src_bmap_ref) = (fsd->b_off + count)
					       / fsd->geo.page_size;
			if (*(fsd->src_bmap_ref) >= fsd->geo.page_cnt)
				ftl_simple_drop_useful(fsd, fsd->dst_block);
		} else {
			bitmap_set_region(fsd->src_bmap_ref,
					  fsd->b_off / fsd->geo.page_size,
				  	count / fsd->geo.page_size);
			if (bitmap_full(fsd->src_bmap_ref, fsd->geo.page_cnt))
				ftl_simple_drop_useful(fsd, fsd->dst_block);
		}
	}

	if (!error) {
		fsd->t_count += fsd->b_len;
		if (fsd->t_count >= fsd->req_in->length) {
			mtdx_complete_request(fsd->req_in, 0, fsd->t_count);
			ftl_simple_pop_all_req_fn(fsd);
			fsd->req_in = NULL;
		}
	} else {
		ftl_simple_pop_all_req_fn(fsd);
		ftl_simple_push_req_fn(fsd, ftl_simple_invalidate_dst);
		fsd->src_error = error;
	}
}

static int ftl_simple_merge_data(struct ftl_simple_data *fsd)
{
	fsd->req_out.cmd = MTDX_CMD_WRITE;
	fsd->req_out.phy_block = fsd->dst_block;
	fsd->req_out.offset = fsd->b_off;
	fsd->req_out.length = fsd->b_len;
	fsd->sg_length = fsd->b_len;
	fsd->req_out.src_dev->get_data_buf_sg = ftl_simple_get_data_buf_sg;
	fsd->end_req_fn = ftl_simple_end_merge_data;
	return 0;
}

static void ftl_simple_end_write_data(struct ftl_simple_data *fsd, int error,
				      unsigned int count)
{
	if ((count != fsd->b_len) && !error)
		error = -EIO;

	fsd->clean_dst = 0;

	if (error)
		ftl_simple_end_abort(fsd, error, fsd->src_error);
}

static int ftl_simple_write_data(struct ftl_simple_data *fsd)
{
	fsd->req_out.cmd = MTDX_CMD_WRITE;
	fsd->req_out.phy_block = fsd->dst_block;
	fsd->req_out.offset = fsd->b_off;
	fsd->req_out.length = fsd->b_len;
	fsd->sg_length = fsd->b_len;
	fsd->req_out.src_dev->get_data_buf_sg = ftl_simple_get_data_buf_sg;
	fsd->end_req_fn = ftl_simple_end_write_data;
	return 0;
}

static void ftl_simple_end_copy_last(struct ftl_simple_data *fsd, int error,
				     unsigned int count)
{
	unsigned int e_len = fsd->block_size - fsd->tmp_off;

	if ((count != e_len) && !error)
		error = -EIO;

	if (fsd->src_error) {
		if (fsd->src_bmap_ref) {
			ftl_simple_drop_useful(fsd, fsd->src_block);
			fsd->src_bmap_ref = NULL;
		}
	} else if (error) {
		ftl_simple_pop_all_req_fn(fsd);
		ftl_simple_push_req_fn(fsd, ftl_simple_invalidate_dst);
		fsd->src_error = error;
		return;
	}

	if (!error)
		error = fsd->src_error;

	fsd->clean_dst = 0;

	if (error)
		ftl_simple_end_abort(fsd, error, fsd->src_error);
	else
		ftl_simple_advance(fsd);
}

static int ftl_simple_write_last(struct ftl_simple_data *fsd)
{
	fsd->tmp_off = fsd->b_off + fsd->b_len;
	fsd->sg_length = fsd->block_size - fsd->tmp_off;

	if (!fsd->sg_length
	    || ftl_simple_can_merge(fsd, fsd->tmp_off, fsd->sg_length)) {
		ftl_simple_advance(fsd);
		return -EAGAIN;
	}

	fsd->req_out.cmd = MTDX_CMD_WRITE;
	fsd->req_out.phy_block = fsd->dst_block;
	fsd->req_out.offset = fsd->tmp_off;
	fsd->req_out.length = fsd->sg_length;
	fsd->req_out.src_dev->get_data_buf_sg = ftl_simple_get_tmp_buf_sg;
	fsd->end_req_fn = ftl_simple_end_copy_last;
	return 0;
}

static int ftl_simple_copy_last(struct ftl_simple_data *fsd)
{
	fsd->tmp_off = fsd->b_off + fsd->b_len;
	fsd->sg_length = fsd->block_size - fsd->tmp_off;

	if (!fsd->sg_length
	    || ftl_simple_can_merge(fsd, fsd->tmp_off, fsd->sg_length)) {
		ftl_simple_advance(fsd);
		return -EAGAIN;
	}

	fsd->req_out.cmd = MTDX_CMD_COPY;
	fsd->req_out.phy_block = fsd->dst_block;
	fsd->req_out.offset = fsd->tmp_off;
	fsd->req_out.length = fsd->sg_length;
	fsd->req_out.src_dev->get_data_buf_sg = ftl_simple_get_tmp_buf_sg;
	fsd->end_req_fn = ftl_simple_end_copy_last;
	return 0;
}

static void ftl_simple_end_copy_first(struct ftl_simple_data *fsd, int error,
				      unsigned int count)
{
	unsigned int e_count = fsd->b_off;

	if (ftl_simple_can_merge(fsd, 0, fsd->b_off))
		e_count = fsd->geo.page_size;

	if ((count != e_count) && !error)
		error = -EIO;

	if (fsd->src_error) {
		if (fsd->src_bmap_ref) {
			ftl_simple_drop_useful(fsd, fsd->src_block);
			fsd->src_bmap_ref = NULL;
		}
	} else if (error) {
		ftl_simple_pop_all_req_fn(fsd);
		ftl_simple_push_req_fn(fsd, ftl_simple_invalidate_dst);
		fsd->src_error = error;
		return;
	}

	if (!error)
		error = fsd->src_error;

	fsd->clean_dst = 0;

	if (error)
		ftl_simple_end_abort(fsd, error, fsd->src_error);
}

static int ftl_simple_write_first(struct ftl_simple_data *fsd)
{
	if (!fsd->b_off)
		return -EAGAIN;

	fsd->req_out.cmd = MTDX_CMD_WRITE;
	fsd->req_out.phy_block = fsd->dst_block;
	fsd->req_out.offset = 0;
	fsd->req_out.length = fsd->b_off;
	fsd->tmp_off = 0;

	if (ftl_simple_can_merge(fsd, 0, fsd->b_off))
		fsd->sg_length = fsd->geo.page_size;
	else
		fsd->sg_length = fsd->b_off;


	fsd->req_out.length = fsd->sg_length;
	memset(fsd->block_buf, fsd->geo.fill_value, fsd->sg_length);

	fsd->req_out.src_dev->get_data_buf_sg = ftl_simple_get_tmp_buf_sg;
	fsd->end_req_fn = ftl_simple_end_copy_first;
	return 0;
}

static int ftl_simple_copy_first(struct ftl_simple_data *fsd)
{
	if (!fsd->b_off)
		return -EAGAIN;

	fsd->req_out.cmd = MTDX_CMD_COPY;
	fsd->req_out.phy_block = fsd->dst_block;
	fsd->req_out.offset = 0;
	fsd->req_out.length = fsd->b_off;
	fsd->tmp_off = 0;
	fsd->sg_length = fsd->b_off;

	if (ftl_simple_can_merge(fsd, 0, fsd->b_off))
		return ftl_simple_write_first(fsd);

	fsd->req_out.src_dev->get_data_buf_sg = ftl_simple_get_tmp_buf_sg;
	fsd->end_req_fn = ftl_simple_end_copy_first;
	return 0;
}

static void ftl_simple_end_read_last(struct ftl_simple_data *fsd, int error,
				      unsigned int count)
{
	unsigned int e_len = fsd->block_size - fsd->tmp_off;

	if ((count != e_len) && !error)
		error = -EIO;

	if (error) {
		if (fsd->src_bmap_ref) {
			ftl_simple_drop_useful(fsd, fsd->src_block);
			fsd->src_bmap_ref = NULL;
		}

		ftl_simple_end_abort(fsd, error, 1);
	}
}

static int ftl_simple_read_last(struct ftl_simple_data *fsd)
{
	fsd->tmp_off = fsd->b_off + fsd->b_len;
	fsd->sg_length = fsd->block_size - fsd->tmp_off;

	if (!fsd->sg_length)
		return -EAGAIN;

	if (ftl_simple_can_merge(fsd, fsd->tmp_off, fsd->sg_length))
		return -EAGAIN;

	fsd->req_out.cmd = MTDX_CMD_READ_DATA;
	fsd->req_out.phy_block = fsd->src_block;
	fsd->req_out.offset = fsd->tmp_off;
	fsd->req_out.length = fsd->sg_length;
	fsd->req_out.src_dev->get_data_buf_sg = ftl_simple_get_tmp_buf_sg;
	fsd->end_req_fn = ftl_simple_end_read_last;
	return 0;
}

static void ftl_simple_end_read_first(struct ftl_simple_data *fsd, int error,
				      unsigned int count)
{
	if ((count != fsd->b_off) && !error)
		error = -EIO;

	if (error) {
		if (fsd->src_bmap_ref) {
			ftl_simple_drop_useful(fsd, fsd->src_block);
			fsd->src_bmap_ref = NULL;
		}

		ftl_simple_end_abort(fsd, error, 1);
	}
}

static int ftl_simple_read_first(struct ftl_simple_data *fsd)
{
	if (!fsd->b_off)
		return -EAGAIN;

	if (ftl_simple_can_merge(fsd, 0, fsd->b_off))
		return -EAGAIN;

	fsd->req_out.cmd = MTDX_CMD_READ_DATA;
	fsd->req_out.phy_block = fsd->src_block;
	fsd->req_out.offset = 0;
	fsd->req_out.length = fsd->b_off;
	fsd->tmp_off = 0;
	fsd->sg_length = fsd->b_off;
	fsd->req_out.src_dev->get_data_buf_sg = ftl_simple_get_tmp_buf_sg;
	fsd->end_req_fn = ftl_simple_end_read_first;
	return 0;
}

static void ftl_simple_set_address(struct ftl_simple_data *fsd)
{
	unsigned int pos = fsd->req_in->offset + fsd->t_count;

	fsd->req_out.log_block = fsd->req_in->log_block;
	fsd->req_out.log_block += pos / fsd->block_size;
	fsd->zone = fsd->req_out.log_block >> fsd->geo.zone_size_log;
	fsd->b_off = pos % fsd->block_size;
	fsd->b_len = min(fsd->req_in->length - fsd->t_count,
			 fsd->block_size - fsd->req_out.offset);
}

static int ftl_simple_setup_write(struct ftl_simple_data *fsd)
{
	struct mtdx_dev *parent = container_of(fsd->req_out.src_dev->dev.parent,
					       struct mtdx_dev, dev);
	unsigned int z_log_block, z_src_block;
	struct mtdx_page_info p_info = {};
	int rc = 0;

	ftl_simple_set_address(fsd);

	if (!test_bit(fsd->zone, ftl_simple_zone_map(fsd)))
		return ftl_simple_setup_zone_scan(fsd);

	dev_dbg(&parent->dev, "setup write - zone valid\n");
	z_log_block = fsd->req_out.log_block
		      & ((1U << fsd->geo.zone_size_log) - 1U);

	fsd->src_block = fsd->zones[fsd->zone]->b_table[z_log_block];
	z_src_block = fsd->src_block & ((1U << fsd->geo.zone_size_log) - 1U);
	fsd->clean_dst = 0;
	fsd->src_error = 0;

	if ((fsd->src_block != MTDX_INVALID_BLOCK)
	    && fsd->b_map
	    && test_bit(z_src_block, fsd->zones[fsd->zone]->useful_blocks)) {
		fsd->src_bmap_ref = long_map_find(fsd->b_map, fsd->src_block);
		if (!fsd->src_bmap_ref)
			clear_bit(z_src_block,
				  fsd->zones[fsd->zone]->useful_blocks);
	} else
		fsd->src_bmap_ref = NULL;

	if (ftl_simple_can_merge(fsd, fsd->b_off, fsd->b_len)) {
		fsd->dst_block = fsd->src_block;
		ftl_simple_push_req_fn(fsd, ftl_simple_merge_data);
		dev_dbg(&parent->dev, "merging into block %x\n",
			fsd->src_block);
	} else {
		fsd->dst_block = mtdx_get_peb(fsd->b_alloc, fsd->zone, &rc);
		dev_dbg(&parent->dev, "allocating new block %x\n",
			fsd->dst_block);

		if (fsd->dst_block == MTDX_INVALID_BLOCK) {
			dev_dbg(&parent->dev, "no new block, current %x\n",
				fsd->src_block);
			if (fsd->src_block == MTDX_INVALID_BLOCK)
				return -EIO;
			fsd->dst_block = fsd->src_block;

			ftl_simple_push_req_fn(fsd, ftl_simple_write_last);
			ftl_simple_push_req_fn(fsd, ftl_simple_write_data);
			ftl_simple_push_req_fn(fsd, ftl_simple_write_first);
			ftl_simple_push_req_fn(fsd, ftl_simple_erase_dst);
			ftl_simple_push_req_fn(fsd, ftl_simple_read_last);
			ftl_simple_push_req_fn(fsd, ftl_simple_read_first);
		} else if (fsd->src_block != MTDX_INVALID_BLOCK) {
			ftl_simple_push_req_fn(fsd, ftl_simple_erase_src);
			if (!fsd->b_map || fsd->dumb_copy) {
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_write_last);
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_write_data);
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_write_first);
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_read_last);
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_read_first);
			} else {
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_copy_last);
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_write_data);
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_copy_first);
			}

			ftl_simple_push_req_fn(fsd, ftl_simple_select_src);
			if (rc)
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_erase_dst);
			else
				fsd->clean_dst = 1;
		} else {
			/* Writing new block */
			if (fsd->b_len == fsd->block_size)
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_merge_data);
			else {
				if (fsd->b_off)
					memset(fsd->block_buf,
					       fsd->geo.fill_value,
					       fsd->b_off);

				fsd->tmp_off = fsd->b_off + fsd->b_len;
				if (fsd->tmp_off < fsd->block_size)
					memset(fsd->block_buf + fsd->tmp_off,
					       fsd->geo.fill_value,
					       fsd->block_size - fsd->tmp_off);

				ftl_simple_push_req_fn(fsd,
						       ftl_simple_write_last);
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_write_data);
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_write_first);
			}

			if (rc)
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_erase_dst);
			else
				fsd->clean_dst = 1;
		}
	}

	p_info.status = MTDX_PAGE_MAPPED;
	p_info.log_block = fsd->req_out.log_block;
	p_info.phy_block = fsd->dst_block;
	rc = parent->info_to_oob(parent, fsd->oob_buf, &p_info);
	dev_dbg(&parent->dev, "setup write %d\n", rc);
	return rc;
}

static int ftl_simple_get_copy_source(struct mtdx_request *req,
				      unsigned int *phy_block,
				      unsigned int *offset)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(req->src_dev);

	if (fsd->req_out.cmd == MTDX_CMD_COPY) {
		*phy_block = fsd->src_block;
		*offset = fsd->tmp_off;
		return 0;
	} else
		return -EINVAL;
}

static void ftl_simple_put_copy_source(struct mtdx_request *req, int src_error)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(req->src_dev);

	if (fsd->req_out.cmd == MTDX_CMD_COPY)
		fsd->src_error = src_error;
}

static int ftl_simple_fill_data(struct ftl_simple_data *fsd)
{
	unsigned int length = fsd->b_len;
	int rc = 0;

	while (length) {
		if (fsd->req_sg.length <= fsd->sg_off) {
			fsd->sg_off = 0;
			rc = mtdx_get_data_buf_sg(fsd->req_in, &fsd->req_sg);
			if (rc)
				return rc;
		}
		length -= fill_sg(&fsd->req_sg, &fsd->sg_off,
				  fsd->geo.fill_value, length);
	}

	fsd->t_count += fsd->b_len - length;
	return -EAGAIN;
}

static void ftl_simple_end_read_data(struct ftl_simple_data *fsd, int error,
				     unsigned int count)
{
	fsd->t_count += count;

	if (!error && (fsd->t_count >= fsd->req_in->length))
		mtdx_complete_request(fsd->req_in, error, fsd->t_count);
	else if (error)
		mtdx_complete_request(fsd->req_in, error, fsd->t_count);
	else
		return;

	ftl_simple_pop_all_req_fn(fsd);
	fsd->req_in = NULL;
}

static int ftl_simple_read_data(struct ftl_simple_data *fsd)
{
	fsd->req_out.cmd = MTDX_CMD_READ_DATA;
	fsd->req_out.phy_block = fsd->src_block;
	fsd->req_out.offset = fsd->b_off;
	fsd->req_out.length = fsd->b_len;
	fsd->sg_length = fsd->b_len;
	fsd->req_out.src_dev->get_data_buf_sg = ftl_simple_get_data_buf_sg;
	fsd->end_req_fn = ftl_simple_end_read_data;
	return 0;
}

static int ftl_simple_setup_read(struct ftl_simple_data *fsd)
{
	unsigned int z_log_block;

	ftl_simple_set_address(fsd);

	if (!test_bit(fsd->zone, ftl_simple_zone_map(fsd)))
		return ftl_simple_setup_zone_scan(fsd);

	z_log_block = fsd->req_out.log_block
		      & ((1U << fsd->geo.zone_size_log) - 1U);

	fsd->src_block = fsd->zones[fsd->zone]->b_table[z_log_block];

	if (fsd->src_block != MTDX_INVALID_BLOCK)
		ftl_simple_push_req_fn(fsd, ftl_simple_read_data);
	else
		ftl_simple_push_req_fn(fsd, ftl_simple_fill_data);

	return 0;
}

static void ftl_simple_end_request(struct mtdx_request *req, int error,
				   unsigned int count)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(req->src_dev);
	unsigned long flags;

	spin_lock_irqsave(&fsd->lock, flags);
	if (fsd->end_req_fn)
		fsd->end_req_fn(fsd, error, count);
	spin_unlock_irqrestore(&fsd->lock, flags);
}

static int ftl_simple_setup_request(struct ftl_simple_data *fsd)
{
	int rc = 0;

	if (fsd->req_in->cmd == MTDX_CMD_READ_DATA)
		rc = ftl_simple_setup_read(fsd);
	else if (fsd->req_in->cmd == MTDX_CMD_WRITE_DATA) {
		if (fsd->b_alloc)
			rc = ftl_simple_setup_write(fsd);
		else
			rc = -EROFS;
	} else
		rc = -EINVAL;

	if (rc) {
		mtdx_complete_request(fsd->req_in, rc, fsd->t_count);
		ftl_simple_pop_all_req_fn(fsd);
		fsd->req_in = NULL;
	}
	return rc;
}

static struct mtdx_request *ftl_simple_get_request(struct mtdx_dev *this_dev)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(this_dev);
	req_fn_t *req_fn;
	int rc;
	unsigned long flags;

	dev_dbg(&this_dev->dev, "ftl get request\n");
	spin_lock_irqsave(&fsd->lock, flags);
	if (!fsd->req_dev) {
		spin_unlock_irqrestore(&fsd->lock, flags);
		return NULL;
	}

	while (1) {
		rc = 0;
		dev_dbg(&this_dev->dev, "ftl request loop\n");
		while ((req_fn = ftl_simple_pop_req_fn(fsd))) {
			rc = (*req_fn)(fsd);
			if (!rc)
				goto out;
			else if (rc != -EAGAIN)
				break;
		}

		ftl_simple_pop_all_req_fn(fsd);

		if (fsd->req_in) {
			if (rc) {
				mtdx_complete_request(fsd->req_in, rc,
						      fsd->t_count);
				fsd->req_in = NULL;
			} else
				rc = ftl_simple_setup_request(fsd);
		} else if (!rc)
			rc = -EAGAIN; /* No current request */

		if (rc) {
			fsd->req_sg.length = 0;
			fsd->sg_off = 0;
			fsd->req_in = fsd->req_dev->get_request(fsd->req_dev);
			if (!fsd->req_in) {
				fsd->req_dev = NULL;
				fsd->usage_count--;
				break;
			}
		}
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

	spin_lock_irqsave(&fsd->lock, flags);
	fsd->usage_count++;
	rc = parent->new_request(parent, this_dev);
	dev_dbg(&this_dev->dev, "parent notified, %d\n", rc);

	if (!rc && fsd->req_dev)
		rc = -EIO;

	if (rc) {
		fsd->usage_count--;
		spin_unlock_irqrestore(&fsd->lock, flags);
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

	parent->get_param(parent, MTDX_PARAM_READ_ONLY, &rc);

	if (!rc) {
		unsigned int r_cnt = min(fsd->geo.phy_block_cnt
					 - fsd->geo.log_block_cnt, 8U);

		if (((parent->id.inp_wmode == MTDX_WMODE_PAGE_PEB)
		      && (fsd->geo.page_cnt <= BITS_PER_LONG))
		     || (parent->id.inp_wmode == MTDX_WMODE_PAGE_PEB_INC)) {
			fsd->b_map = long_map_create(r_cnt, NULL, NULL, 0);
			dev_dbg(&mdev->dev, "using incremental page "
				"tracking\n");
		} else if (parent->id.inp_wmode == MTDX_WMODE_PAGE_PEB) {
			fsd->b_map = long_map_create(r_cnt,
						     ftl_simple_alloc_pmap,
						     NULL, fsd->geo.page_cnt);
			dev_dbg(&mdev->dev, "using full page tracking\n");
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

	INIT_LIST_HEAD(&fsd->special_blocks);
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
	mdev->get_copy_source = ftl_simple_get_copy_source;
	mdev->put_copy_source = ftl_simple_put_copy_source;
	mdev->get_oob_buf = ftl_simple_get_oob_buf;
	mdev->get_param = ftl_simple_get_param;

	{
		struct mtdx_dev *cdev;
		struct mtdx_device_id c_id = {
			MTDX_WMODE_NONE, MTDX_WMODE_PAGE, MTDX_RMODE_NONE,
	  		MTDX_RMODE_PAGE, MTDX_TYPE_ADAPTER,
			MTDX_ID_ADAPTER_BLKDEV
		};

		/* Temporary hack to insert block device */
		cdev = mtdx_alloc_dev(&mdev->dev, &c_id);
		if (cdev) {
			rc = device_register(&cdev->dev);
			if (rc)
				__mtdx_free_dev(cdev);
		}
	}

	return 0;
err_out:
	ftl_simple_free(fsd);
	return rc;
}

static void ftl_simple_remove(struct mtdx_dev *mdev)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(mdev);
	unsigned long flags;

	mdev->new_request = ftl_simple_dummy_new_request;

	/* Just wait for everybody to go away! */
	while (1) {
		spin_lock_irqsave(&fsd->lock, flags);
		if (!fsd->usage_count)
			break;

		spin_unlock_irqrestore(&fsd->lock, flags);
		msleep(1);
	};
	spin_unlock_irqrestore(&fsd->lock, flags);

	mtdx_set_drvdata(mdev, NULL);
	ftl_simple_free(fsd);
}

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
	.remove   = ftl_simple_remove
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
