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
#include <linux/sched.h>
#include <linux/wait.h>
#include "mtdx_common.h"
#include "peb_alloc.h"
#include "long_map.h"
#include "sg_bounce.h"

#define DRIVER_NAME "ftl_simple"
#define fsd_dev(fsd) (fsd->mdev->dev)

#undef dev_dbg
#define dev_dbg dev_emerg

#define FUNC_START_DBG(fsd) dev_dbg(&fsd_dev(fsd), "%s begin\n", __func__)

struct zone_data {
	unsigned long *useful_blocks;
	unsigned int  b_table[];
};

struct ftl_simple_data;

typedef int (req_fn_t)(struct ftl_simple_data *fsd);

#define FTL_SIMPLE_MAX_REQ_FN 10

struct ftl_simple_data {
	struct mtdx_dev       *mdev;
	spinlock_t            lock;
	struct mtdx_dev_queue c_queue;

	/* Device geometry */
	struct mtdx_geo       geo;
	unsigned int          zone_cnt;
	unsigned int          zone_phy_cnt;
	unsigned int          block_size;

	/* Incoming request */
	struct mtdx_dev       *req_dev;
	struct mtdx_request   *req_in;

	/* Outgoing request */
	struct mtdx_request   req_out;
	unsigned int          t_count;
	int                   dst_error;
	int                   src_error;

	/* Processing modifiers */
	unsigned int          dumb_copy:1,
			      track_inc:1,
			      clean_dst:1,
			      req_active:1;

	/* Current request address */
	unsigned int          zone;
	unsigned int          z_log_block;
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
					    unsigned int count);
	unsigned char         *oob_buf;
	struct mtdx_oob       req_oob;
	unsigned char         *block_buf;
	struct mtdx_data_iter req_data;
	struct zone_data      *zones[];
};

static char *ftl_simple_dst_oob(struct ftl_simple_data *fsd)
{
	return fsd->oob_buf;
}

static char *ftl_simple_src_oob(struct ftl_simple_data *fsd)
{
	return fsd->oob_buf + fsd->geo.oob_size;
}

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

static void ftl_simple_complete_req(struct ftl_simple_data *fsd)
{
	ftl_simple_pop_all_req_fn(fsd);

	fsd->req_dev->end_request(fsd->req_dev, fsd->req_in, fsd->t_count,
				  fsd->dst_error == -EAGAIN
				  ? 0 : fsd->dst_error,
				  fsd->src_error);

	fsd->req_in = NULL;
	fsd->t_count = 0;
	fsd->dst_error = 0;
	fsd->src_error = 0;
}

static void ftl_simple_end_abort(struct ftl_simple_data *fsd, int free_dst)
{
	FUNC_START_DBG(fsd);
	if (free_dst && (fsd->dst_block != fsd->src_block)) {
		fsd->b_alloc->put_peb(fsd->b_alloc, fsd->dst_block,
				      fsd->clean_dst == 1);
		fsd->dst_block = MTDX_INVALID_BLOCK;
	}

	ftl_simple_complete_req(fsd);
}

static void ftl_simple_end_resolve(struct ftl_simple_data *fsd,
				   unsigned int count)
{
	struct mtdx_dev *parent = container_of(fsd->mdev->dev.parent,
					       struct mtdx_dev, dev);
	unsigned int max_block = (fsd->zone + 1) << fsd->geo.zone_size_log;
	unsigned int zone, z_log_block;
	struct mtdx_page_info p_info = {};

	p_info.phy_block = fsd->conflict_pos;

	if (!fsd->dst_error)
		fsd->dst_error = parent->oob_to_info(parent, &p_info,
						     fsd->oob_buf);

	if (!fsd->dst_error) {
		zone = mtdx_geo_log_to_zone(&fsd->geo, p_info.log_block,
					    &z_log_block);
		if (zone != fsd->zone)
			fsd->dst_error = -ERANGE;
	}

	if (!fsd->dst_error) {
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
	mtdx_oob_init(&fsd->req_oob, fsd->oob_buf, 1, fsd->geo.oob_size);

	fsd->req_out.cmd = MTDX_CMD_READ;
	fsd->req_out.phy.b_addr = fsd->conflict_pos;
	fsd->req_out.phy.offset = 0;
	fsd->req_out.length = 0;
	fsd->req_out.req_data = NULL;
	fsd->req_out.req_oob = &fsd->req_oob;
	fsd->end_req_fn = ftl_simple_end_resolve;
	return 0;

}

static void ftl_simple_end_lookup_block(struct ftl_simple_data *fsd,
					unsigned int count)
{
	struct mtdx_dev *parent = container_of(fsd->mdev->dev.parent,
					       struct mtdx_dev, dev);
	unsigned int max_block = (fsd->zone + 1) << fsd->geo.zone_size_log;
	unsigned int zone, z_log_block;
	struct mtdx_page_info p_info = {};

	p_info.phy_block = fsd->zone_scan_pos;

	if (!fsd->dst_error)
		fsd->dst_error = parent->oob_to_info(parent, &p_info,
						     fsd->oob_buf);
	else if (fsd->dst_error == -EFAULT) {
		/* Uncorrectable error reading block */
		p_info.status = MTDX_PAGE_FAILURE;
		p_info.log_block = MTDX_INVALID_BLOCK;
		fsd->dst_error = 0;
	}

	zone = fsd->geo.log_to_zone(&fsd->geo, p_info.log_block, &z_log_block);
	if (zone != fsd->zone) {
		p_info.log_block = MTDX_INVALID_BLOCK;
		p_info.status = MTDX_PAGE_UNMAPPED;
	}

	if (!fsd->dst_error) {
		if (p_info.log_block != MTDX_INVALID_BLOCK)
			fsd->conflict_pos = fsd->zones[fsd->zone]
					       ->b_table[z_log_block];
		else if ((p_info.status == MTDX_PAGE_MAPPED)
			   || (p_info.status == MTDX_PAGE_SMAPPED))
			p_info.status = MTDX_PAGE_UNMAPPED;

		switch (p_info.status) {
		case MTDX_PAGE_ERASED:
			dev_dbg(&fsd_dev(fsd), "erased block %x\n",
				fsd->zone_scan_pos);
			mtdx_put_peb(fsd->b_alloc, fsd->zone_scan_pos, 0);
			break;
		case MTDX_PAGE_UNMAPPED:
			dev_dbg(&fsd_dev(fsd), "free block %x\n",
				fsd->zone_scan_pos);
			mtdx_put_peb(fsd->b_alloc, fsd->zone_scan_pos, 1);
			break;
		case MTDX_PAGE_MAPPED:
			dev_dbg(&fsd_dev(fsd), "allocated block %x\n",
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
			dev_dbg(&fsd_dev(fsd), "selected block %x\n",
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
			fsd->dst_error = -EINVAL;
		}
	}

	if (fsd->dst_error)
		ftl_simple_end_abort(fsd, 0);
	else {
		fsd->zone_scan_pos++;
		if (fsd->zone_scan_pos >= max_block) {
			ftl_simple_pop_all_req_fn(fsd);
			set_bit(fsd->zone, ftl_simple_zone_map(fsd));
			if (fsd->req_in)
				ftl_simple_setup_request(fsd);
		} else
			ftl_simple_push_req_fn(fsd, ftl_simple_lookup_block);
	}
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
	mtdx_oob_init(&fsd->req_oob, &fsd->oob_buf, 1, fsd->geo.oob_size);
	fsd->req_out.cmd = MTDX_CMD_READ;
	fsd->req_out.phy.b_addr = fsd->zone_scan_pos;
	fsd->req_out.phy.offset = 0;
	fsd->req_out.length = fsd->geo.page_size;
	fsd->req_out.req_data = NULL;
	fsd->req_out.req_oob = &fsd->req_oob;
	fsd->end_req_fn = ftl_simple_end_lookup_block;
	return 0;
}

static void ftl_simple_clear_useful(struct ftl_simple_data *fsd)
{
	unsigned int b_block = fsd->zone << fsd->geo.zone_size_log;
	unsigned int pos = find_first_bit(fsd->zones[fsd->zone]->useful_blocks,
					  fsd->zone_phy_cnt);

	while (pos < fsd->zone_phy_cnt) {
		long_map_erase(fsd->b_map, b_block + pos);
		clear_bit(pos, fsd->zones[fsd->zone]->useful_blocks);
		pos = find_next_bit(fsd->zones[fsd->zone]->useful_blocks,
				    fsd->zone_phy_cnt, pos);
	}
}

static void ftl_simple_make_useful(struct ftl_simple_data *fsd,
				   unsigned int peb)
{
	unsigned int z_peb = peb & ((1U << fsd->geo.zone_size_log) - 1U);

	if (!fsd->b_map)
		return;

	fsd->src_bmap_ref = long_map_insert(fsd->b_map, peb);

	if (!fsd->src_bmap_ref)
		return;

	if (fsd->track_inc)
		*(fsd->src_bmap_ref) = 0;
	else
		bitmap_zero(fsd->src_bmap_ref, fsd->geo.page_cnt);

	set_bit(z_peb, fsd->zones[peb >> fsd->geo.zone_size_log]
			  ->useful_blocks);
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

static int ftl_simple_setup_zone_scan(struct ftl_simple_data *fsd)
{
	unsigned int max_block = (fsd->zone + 1) << fsd->geo.zone_size_log;
	unsigned int cnt;
	struct mtdx_page_info *p_info;

	dev_dbg(&fsd_dev(fsd), "scanning zone %x\n", fsd->zone);
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

static void ftl_simple_advance(struct ftl_simple_data *fsd)
{
	unsigned int z_dst_block = fsd->dst_block
				   & ((1U << fsd->geo.zone_size_log) - 1U);
	unsigned long *dst_bmap_ref;
	unsigned int tmp_off;

	FUNC_START_DBG(fsd);

	fsd->t_count += fsd->b_len;
	if (fsd->dst_block != fsd->src_block)
		fsd->zones[fsd->zone]->b_table[fsd->z_log_block]
			= fsd->dst_block;

	if (!fsd->src_bmap_ref)
		goto out;

	tmp_off = fsd->b_off + fsd->b_len;

	if (fsd->track_inc) {
		tmp_off /= fsd->geo.page_size;

		if (*(fsd->src_bmap_ref) < tmp_off)
			*(fsd->src_bmap_ref) = tmp_off;

		if (!*(fsd->src_bmap_ref)
		    || (*(fsd->src_bmap_ref) >= fsd->block_size))
			ftl_simple_drop_useful(fsd, fsd->src_block);
		else if (fsd->dst_block != fsd->src_block) {
			dst_bmap_ref = long_map_insert(fsd->b_map,
						       fsd->dst_block);
			if (dst_bmap_ref) {
				*dst_bmap_ref = *(fsd->src_bmap_ref);
				set_bit(z_dst_block, fsd->zones[fsd->zone]
							->useful_blocks);
			}
			ftl_simple_drop_useful(fsd, fsd->src_block);
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

		if (tmp_off < fsd->block_size) {
			if (!ftl_simple_can_merge(fsd, tmp_off,
						  fsd->block_size
						  - tmp_off))
				bitmap_set_region(fsd->src_bmap_ref,
						  p_off + p_len,
						  fsd->geo.page_cnt - p_off
						  - p_len);
		}

		bitmap_set_region(fsd->src_bmap_ref, p_off, p_len);

		if (bitmap_full(fsd->src_bmap_ref, fsd->geo.page_cnt)
		    || bitmap_empty(fsd->src_bmap_ref, fsd->geo.page_cnt))
			ftl_simple_drop_useful(fsd, fsd->src_block);
		else if (fsd->src_block != fsd->dst_block) {
			dst_bmap_ref = long_map_insert(fsd->b_map,
						       fsd->dst_block);
			if (dst_bmap_ref) {
				memcpy(dst_bmap_ref, fsd->src_bmap_ref,
				       BITS_TO_LONGS(fsd->geo.page_cnt)
				       * sizeof(unsigned long));

				set_bit(z_dst_block, fsd->zones[fsd->zone]
							->useful_blocks);
			}
			ftl_simple_drop_useful(fsd, fsd->src_block);
		}
	}
out:
	dev_dbg(&fsd_dev(fsd), "advance out %x, req %x\n", fsd->t_count,
		fsd->req_in->length);
	if (fsd->t_count >=  fsd->req_in->length)
		ftl_simple_complete_req(fsd);
}

static void ftl_simple_end_invalidate_dst(struct ftl_simple_data *fsd,
					  unsigned int count)
{
	ftl_simple_end_abort(fsd, 0);
}

static int ftl_simple_invalidate_dst(struct ftl_simple_data *fsd)
{
	struct mtdx_dev *parent = container_of(fsd->mdev->dev.parent,
					       struct mtdx_dev, dev);
	struct mtdx_page_info p_info = {
		.status = MTDX_PAGE_INVALID,
		.log_block = MTDX_INVALID_BLOCK,
		.phy_block = fsd->dst_block,
		.page_offset = 0
	};
	int rc;

	FUNC_START_DBG(fsd);

	rc = parent->info_to_oob(parent, ftl_simple_dst_oob(fsd), &p_info);
	if (rc)
		return rc;

	mtdx_oob_init(&fsd->req_oob, ftl_simple_dst_oob(fsd), 1,
		      fsd->geo.oob_size);

	fsd->req_out.cmd = MTDX_CMD_WRITE;
	fsd->req_out.phy.b_addr = fsd->dst_block;
	fsd->req_out.phy.offset = 0;
	fsd->req_out.length = fsd->geo.page_size;
	fsd->req_out.req_data = NULL;
	fsd->req_out.req_oob = &fsd->req_oob;
	fsd->end_req_fn = ftl_simple_end_invalidate_dst;
	return 0;
}

static void ftl_simple_end_erase_src(struct ftl_simple_data *fsd,
				     unsigned int count)
{
	if (!fsd->dst_error)
		fsd->b_alloc->put_peb(fsd->b_alloc, fsd->src_block, 1);
}

static int ftl_simple_erase_src(struct ftl_simple_data *fsd)
{
	FUNC_START_DBG(fsd);
	fsd->req_out.cmd = MTDX_CMD_ERASE;
	fsd->req_out.phy.b_addr = fsd->src_block;
	fsd->req_out.phy.offset = 0;
	fsd->req_out.length = 0;
	fsd->req_out.req_data = NULL;
	fsd->req_out.req_oob = NULL;
	fsd->end_req_fn = ftl_simple_end_erase_src;
	return 0;
}

static void ftl_simple_end_erase_dst(struct ftl_simple_data *fsd,
				     unsigned int count)
{
	if (fsd->dst_error) {
		ftl_simple_pop_all_req_fn(fsd);
		ftl_simple_push_req_fn(fsd, ftl_simple_invalidate_dst);
		fsd->src_error = fsd->dst_error;
	} else
		fsd->clean_dst = 1;
}

static int ftl_simple_erase_dst(struct ftl_simple_data *fsd)
{
	FUNC_START_DBG(fsd);
	fsd->req_out.cmd = MTDX_CMD_ERASE;
	fsd->req_out.phy.b_addr = fsd->dst_block;
	fsd->req_out.phy.offset = 0;
	fsd->req_out.length = 0;
	fsd->req_out.req_data = NULL;
	fsd->req_out.req_oob = NULL;
	fsd->end_req_fn = ftl_simple_end_erase_dst;
	return 0;
}

static int ftl_simple_select_src(struct ftl_simple_data *fsd)
{
	FUNC_START_DBG(fsd);

	mtdx_oob_init(&fsd->req_oob, ftl_simple_src_oob(fsd), 1,
		      fsd->geo.oob_size);

	fsd->req_out.cmd = MTDX_CMD_WRITE;
	fsd->req_out.phy.b_addr = fsd->src_block;
	fsd->req_out.phy.offset = 0;
	fsd->req_out.length = fsd->geo.page_size;
	fsd->req_out.req_data = NULL;
	fsd->req_out.req_oob = &fsd->req_oob;
	fsd->end_req_fn = ftl_simple_end_invalidate_dst;
	return 0;
}

static void ftl_simple_end_merge_data(struct ftl_simple_data *fsd,
				      unsigned int count)
{
	if ((count != fsd->b_len) && !fsd->dst_error)
		fsd->dst_error = -EIO;

	if (!fsd->dst_error)
		ftl_simple_advance(fsd);
	else
		ftl_simple_end_abort(fsd, 0);
}

static int ftl_simple_merge_data(struct ftl_simple_data *fsd)
{
	FUNC_START_DBG(fsd);

	mtdx_oob_init(&fsd->req_oob, ftl_simple_dst_oob(fsd), 1,
		      fsd->geo.oob_size);

	fsd->req_out.cmd = MTDX_CMD_WRITE;
	fsd->req_out.phy.b_addr = fsd->dst_block;
	fsd->req_out.phy.offset = fsd->b_off;
	fsd->req_out.length = fsd->b_len;
	fsd->req_out.req_data = fsd->req_in->req_data;
	fsd->req_out.req_oob = &fsd->req_oob;
	fsd->end_req_fn = ftl_simple_end_merge_data;
	return 0;
}

static void ftl_simple_end_write_data(struct ftl_simple_data *fsd,
				      unsigned int count)
{
	if ((count != fsd->b_len) && !fsd->dst_error)
		fsd->dst_error = -EIO;

	fsd->clean_dst = 0;

	if (fsd->dst_error)
		ftl_simple_end_abort(fsd, fsd->src_error);
}

static int ftl_simple_write_data(struct ftl_simple_data *fsd)
{
	FUNC_START_DBG(fsd);

	mtdx_oob_init(&fsd->req_oob, ftl_simple_dst_oob(fsd), 1,
		      fsd->geo.oob_size);

	fsd->req_out.cmd = MTDX_CMD_WRITE;
	fsd->req_out.phy.b_addr = fsd->dst_block;
	fsd->req_out.phy.offset = fsd->b_off;
	fsd->req_out.length = fsd->b_len;
	fsd->req_out.req_data = fsd->req_in->req_data;
	fsd->req_out.req_oob = &fsd->req_oob;
	fsd->end_req_fn = ftl_simple_end_write_data;
	return 0;
}

static void ftl_simple_end_copy_last(struct ftl_simple_data *fsd,
				     unsigned int count)
{
	unsigned int e_len = fsd->block_size - fsd->b_off - fsd->b_len;

	if ((count != e_len) && !fsd->dst_error)
		fsd->dst_error = -EIO;

	if (fsd->src_error) {
		if (fsd->src_bmap_ref) {
			ftl_simple_drop_useful(fsd, fsd->src_block);
			fsd->src_bmap_ref = NULL;
		}
	} else if (fsd->dst_error) {
		ftl_simple_pop_all_req_fn(fsd);
		ftl_simple_push_req_fn(fsd, ftl_simple_invalidate_dst);
		fsd->src_error = fsd->dst_error;
		return;
	}

	if (!fsd->dst_error)
		fsd->dst_error = fsd->src_error;

	fsd->clean_dst = 0;

	if (fsd->dst_error)
		ftl_simple_end_abort(fsd, fsd->src_error);
	else
		ftl_simple_advance(fsd);
}

static int ftl_simple_submit_last(struct ftl_simple_data *fsd,
				  enum mtdx_command cmd)
{
	unsigned int tmp_off = fsd->b_off + fsd->b_len;

	fsd->req_out.length = fsd->block_size - tmp_off;

	if (!fsd->req_out.length
	    || ftl_simple_can_merge(fsd, tmp_off, fsd->req_out.length)) {
		ftl_simple_advance(fsd);
		return -EAGAIN;
	}

	fsd->req_out.cmd = cmd;
	fsd->req_out.phy.b_addr = fsd->dst_block;
	fsd->req_out.phy.offset = tmp_off;

	mtdx_data_iter_init_buf(&fsd->req_data, fsd->block_buf + tmp_off,
				fsd->req_out.length);
	fsd->req_out.req_data = &fsd->req_data;

	mtdx_oob_init(&fsd->req_oob, ftl_simple_dst_oob(fsd), 1,
		      fsd->geo.oob_size);
	fsd->req_out.req_oob = &fsd->req_oob;

	fsd->end_req_fn = ftl_simple_end_copy_last;
	return 0;
}

static int ftl_simple_write_last(struct ftl_simple_data *fsd)
{
	FUNC_START_DBG(fsd);
	return ftl_simple_submit_last(fsd, MTDX_CMD_WRITE);
}

static int ftl_simple_copy_last(struct ftl_simple_data *fsd)
{
	FUNC_START_DBG(fsd);
	return ftl_simple_submit_last(fsd, MTDX_CMD_COPY);
}

static void ftl_simple_end_copy_first(struct ftl_simple_data *fsd,
				      unsigned int count)
{
	unsigned int e_count = fsd->b_off;

	if (ftl_simple_can_merge(fsd, 0, fsd->b_off))
		e_count = fsd->geo.page_size;

	if ((count != e_count) && !fsd->dst_error)
		fsd->dst_error = -EIO;

	if (fsd->src_error) {
		if (fsd->src_bmap_ref) {
			ftl_simple_drop_useful(fsd, fsd->src_block);
			fsd->src_bmap_ref = NULL;
		}
	} else if (fsd->dst_error) {
		ftl_simple_pop_all_req_fn(fsd);
		ftl_simple_push_req_fn(fsd, ftl_simple_invalidate_dst);
		fsd->src_error = fsd->dst_error;
		return;
	}

	if (!fsd->dst_error)
		fsd->dst_error = fsd->src_error;

	fsd->clean_dst = 0;

	if (fsd->dst_error)
		ftl_simple_end_abort(fsd, fsd->src_error);
}

static int ftl_simple_write_first(struct ftl_simple_data *fsd)
{
	FUNC_START_DBG(fsd);
	if (!fsd->b_off)
		return -EAGAIN;

	fsd->req_out.cmd = MTDX_CMD_WRITE;
	fsd->req_out.phy.b_addr = fsd->dst_block;
	fsd->req_out.phy.offset = 0;
	fsd->req_out.length = fsd->b_off;


	if (ftl_simple_can_merge(fsd, 0, fsd->b_off)) {
		fsd->req_out.length = fsd->geo.page_size;
		memset(fsd->block_buf, fsd->geo.fill_value,
		       fsd->req_out.length);
	}

	mtdx_data_iter_init_buf(&fsd->req_data, fsd->block_buf,
				fsd->req_out.length);
	fsd->req_out.req_data = &fsd->req_data;

	mtdx_oob_init(&fsd->req_oob, ftl_simple_dst_oob(fsd), 1,
		      fsd->geo.oob_size);
	fsd->req_out.req_oob = &fsd->req_oob;

	fsd->end_req_fn = ftl_simple_end_copy_first;
	return 0;
}

static int ftl_simple_copy_first(struct ftl_simple_data *fsd)
{
	FUNC_START_DBG(fsd);
	if (!fsd->b_off)
		return -EAGAIN;

	if (ftl_simple_can_merge(fsd, 0, fsd->b_off))
		return ftl_simple_write_first(fsd);

	fsd->req_out.cmd = MTDX_CMD_COPY;
	fsd->req_out.phy.b_addr = fsd->dst_block;
	fsd->req_out.phy.offset = 0;
	fsd->req_out.length = fsd->b_off;

	memset(fsd->block_buf, fsd->geo.fill_value, fsd->req_out.length);
	mtdx_data_iter_init_buf(&fsd->req_data, fsd->block_buf,
				fsd->req_out.length);
	fsd->req_out.req_data = &fsd->req_data;

	mtdx_oob_init(&fsd->req_oob, ftl_simple_dst_oob(fsd), 1,
		      fsd->geo.oob_size);
	fsd->req_out.req_oob = &fsd->req_oob;

	fsd->end_req_fn = ftl_simple_end_copy_first;
	return 0;
}

static void ftl_simple_end_read_last(struct ftl_simple_data *fsd,
				      unsigned int count)
{
	unsigned int e_len = fsd->block_size - fsd->b_off - fsd->b_len;

	if ((count != e_len) && !fsd->dst_error)
		fsd->dst_error = -EIO;

	if (fsd->dst_error) {
		if (fsd->src_bmap_ref) {
			ftl_simple_drop_useful(fsd, fsd->src_block);
			fsd->src_bmap_ref = NULL;
		}

		ftl_simple_end_abort(fsd, 1);
	}
}

static int ftl_simple_read_last(struct ftl_simple_data *fsd)
{
	unsigned int tmp_off = fsd->b_off + fsd->b_len;

	FUNC_START_DBG(fsd);
	fsd->req_out.length = fsd->block_size - tmp_off;

	if (!fsd->req_out.length)
		return -EAGAIN;

	if (ftl_simple_can_merge(fsd, tmp_off, fsd->req_out.length))
		return -EAGAIN;

	fsd->req_out.cmd = MTDX_CMD_READ;
	fsd->req_out.phy.b_addr = fsd->src_block;
	fsd->req_out.phy.offset = tmp_off;

	mtdx_data_iter_init_buf(&fsd->req_data, fsd->block_buf + tmp_off,
				fsd->req_out.length);
	fsd->req_out.req_data = &fsd->req_data;
	fsd->req_out.req_oob = NULL;
	fsd->end_req_fn = ftl_simple_end_read_last;
	return 0;
}

static void ftl_simple_end_read_first(struct ftl_simple_data *fsd,
				      unsigned int count)
{
	if ((count != fsd->b_off) && !fsd->dst_error)
		fsd->dst_error = -EIO;

	if (fsd->dst_error) {
		if (fsd->src_bmap_ref) {
			ftl_simple_drop_useful(fsd, fsd->src_block);
			fsd->src_bmap_ref = NULL;
		}

		ftl_simple_end_abort(fsd, 1);
	}
}

static int ftl_simple_read_first(struct ftl_simple_data *fsd)
{
	FUNC_START_DBG(fsd);
	if (!fsd->b_off)
		return -EAGAIN;

	if (ftl_simple_can_merge(fsd, 0, fsd->b_off))
		return -EAGAIN;

	fsd->req_out.cmd = MTDX_CMD_READ;
	fsd->req_out.phy.b_addr = fsd->src_block;
	fsd->req_out.phy.offset = 0;
	fsd->req_out.length = fsd->b_off;

	mtdx_data_iter_init_buf(&fsd->req_data, fsd->block_buf,
				fsd->req_out.length);
	fsd->req_out.req_data = &fsd->req_data;
	fsd->req_out.req_oob = NULL;

	fsd->end_req_fn = ftl_simple_end_read_first;
	return 0;
}

static void ftl_simple_set_address(struct ftl_simple_data *fsd)
{
	unsigned int pos = fsd->req_in->phy.offset + fsd->t_count;

	fsd->req_out.logical = fsd->req_in->logical;
	fsd->req_out.logical += pos / fsd->block_size;
	fsd->zone = fsd->geo.log_to_zone(&fsd->geo, fsd->req_out.logical,
					 &fsd->z_log_block);
	fsd->b_off = pos % fsd->block_size;
	fsd->b_len = min(fsd->req_in->length - fsd->t_count,
			 fsd->block_size - fsd->b_off);
}

static int ftl_simple_setup_write(struct ftl_simple_data *fsd)
{
	struct mtdx_dev *parent = container_of(fsd->mdev->dev.parent,
					       struct mtdx_dev, dev);
	unsigned int z_src_block, tmp_off;
	struct mtdx_page_info p_info = {};
	int rc = 0;

	ftl_simple_set_address(fsd);

	if (!test_bit(fsd->zone, ftl_simple_zone_map(fsd)))
		return ftl_simple_setup_zone_scan(fsd);

	dev_dbg(&fsd_dev(fsd), "setup write - log %x, %x:%x\n",
		fsd->req_out.logical, fsd->b_off, fsd->b_len);

	fsd->src_block = fsd->zones[fsd->zone]->b_table[fsd->z_log_block];
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
		dev_dbg(&fsd_dev(fsd), "merging into block %x\n",
			fsd->src_block);
	} else {
		fsd->dst_block = mtdx_get_peb(fsd->b_alloc, fsd->zone, &rc);
		dev_dbg(&fsd_dev(fsd), "allocating new block %x\n",
			fsd->dst_block);

		if (fsd->dst_block == MTDX_INVALID_BLOCK) {
			dev_dbg(&fsd_dev(fsd), "no new block, current %x\n",
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
				dev_dbg(&fsd_dev(fsd), "dump copy, src %x\n",
					fsd->src_block);
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
				dev_dbg(&fsd_dev(fsd), "fast copy, src %x\n",
					fsd->src_block);
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
			if (fsd->b_len == fsd->block_size) {
				dev_dbg(&fsd_dev(fsd), "full block write\n");
				ftl_simple_push_req_fn(fsd,
						       ftl_simple_merge_data);
			} else {
				dev_dbg(&fsd_dev(fsd), "partial block write\n");
				ftl_simple_make_useful(fsd, fsd->dst_block);

				if (fsd->b_off)
					memset(fsd->block_buf,
					       fsd->geo.fill_value,
					       fsd->b_off);

				tmp_off = fsd->b_off + fsd->b_len;
				if (tmp_off < fsd->block_size)
					memset(fsd->block_buf + tmp_off,
					       fsd->geo.fill_value,
					       fsd->block_size - tmp_off);

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
	p_info.log_block = fsd->req_out.logical;
	p_info.phy_block = fsd->dst_block;
	p_info.page_offset = 0;
	rc = parent->info_to_oob(parent, ftl_simple_dst_oob(fsd), &p_info);

	if ((fsd->src_block != fsd->dst_block)
	    && (fsd->src_block != MTDX_INVALID_BLOCK)
	    && !rc) {
		p_info.status = MTDX_PAGE_SMAPPED;
		p_info.phy_block = fsd->src_block;
		rc = parent->info_to_oob(parent, ftl_simple_src_oob(fsd),
					 &p_info);
	}

	dev_dbg(&fsd_dev(fsd), "setup write %d\n", rc);
	return rc;
}

static int ftl_simple_fill_data(struct ftl_simple_data *fsd)
{
	FUNC_START_DBG(fsd);
	mtdx_data_iter_fill(fsd->req_in->req_data, fsd->geo.fill_value,
			    fsd->b_len);

	fsd->t_count += fsd->b_len;

	if (fsd->t_count >= fsd->req_in->length)
		ftl_simple_complete_req(fsd);

	return 0;
}

static void ftl_simple_end_read_data(struct ftl_simple_data *fsd,
				     unsigned int count)
{
	FUNC_START_DBG(fsd);
	if (!fsd->dst_error && count != fsd->b_len)
		fsd->dst_error = -EIO;

	fsd->t_count += count;

	if (fsd->dst_error || (fsd->t_count >= fsd->req_in->length))
		ftl_simple_complete_req(fsd);
}

static int ftl_simple_read_data(struct ftl_simple_data *fsd)
{
	FUNC_START_DBG(fsd);
	fsd->req_out.cmd = MTDX_CMD_READ;
	fsd->req_out.phy.b_addr = fsd->src_block;
	fsd->req_out.phy.offset = fsd->b_off;
	fsd->req_out.length = fsd->b_len;
	fsd->req_out.req_data = fsd->req_in->req_data;
	fsd->req_out.req_oob = NULL;
	fsd->end_req_fn = ftl_simple_end_read_data;
	return 0;
}

static int ftl_simple_setup_read(struct ftl_simple_data *fsd)
{
	ftl_simple_set_address(fsd);

	if (!test_bit(fsd->zone, ftl_simple_zone_map(fsd)))
		return ftl_simple_setup_zone_scan(fsd);

	fsd->src_block = fsd->zones[fsd->zone]->b_table[fsd->z_log_block];

	if (fsd->src_block != MTDX_INVALID_BLOCK)
		ftl_simple_push_req_fn(fsd, ftl_simple_read_data);
	else
		ftl_simple_push_req_fn(fsd, ftl_simple_fill_data);

	return 0;
}

static void ftl_simple_end_request(struct mtdx_dev *this_dev,
				   struct mtdx_request *req,
				   unsigned int count,
				   int dst_error, int src_error)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(this_dev);
	unsigned long flags;

	spin_lock_irqsave(&fsd->lock, flags);
	fsd->dst_error = dst_error;
	fsd->src_error = src_error;

	if (fsd->end_req_fn)
		fsd->end_req_fn(fsd, count);

	fsd->req_active = 0;
	spin_unlock_irqrestore(&fsd->lock, flags);
}

static int ftl_simple_setup_request(struct ftl_simple_data *fsd)
{
	int rc = 0;

	if (fsd->req_in->cmd == MTDX_CMD_READ)
		rc = ftl_simple_setup_read(fsd);
	else if (fsd->req_in->cmd == MTDX_CMD_WRITE) {
		if (fsd->b_alloc)
			rc = ftl_simple_setup_write(fsd);
		else
			rc = -EROFS;
	} else
		rc = -EINVAL;

	return rc;
}

static struct mtdx_request *ftl_simple_get_request(struct mtdx_dev *this_dev)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(this_dev);
	req_fn_t *req_fn;
	unsigned long flags;
	int rc;

	dev_dbg(&this_dev->dev, "ftl get request\n");
	spin_lock_irqsave(&fsd->lock, flags);

	while (1) {
		rc = 0;
		dev_dbg(&this_dev->dev, "ftl request loop\n");
		while ((req_fn = ftl_simple_pop_req_fn(fsd))) {
			rc = (*req_fn)(fsd);
			if (!rc)
				goto out;
			else if (rc == -EAGAIN)
				rc = 0;
		}

		dev_dbg(&this_dev->dev, "processing stopped %d, %p\n", rc,
			fsd->req_in);
		ftl_simple_pop_all_req_fn(fsd);

		if (fsd->req_in) {
			if (!rc)
				rc = ftl_simple_setup_request(fsd);

			if (rc) {
				fsd->dst_error = rc;
				ftl_simple_complete_req(fsd);
			}
		}

		if (!fsd->req_in) {
			if (fsd->req_dev)
				fsd->req_in = fsd->req_dev
						 ->get_request(fsd->req_dev);
			if (fsd->req_in)
				continue;

			put_device(&fsd->req_dev->dev);
			fsd->req_dev = mtdx_dev_queue_pop_front(&fsd->c_queue);

			if (!fsd->req_dev) {
				if (!rc)
					rc = -EAGAIN;

				break;
			}
		}
	}
out:
	if (!rc)
		fsd->req_active = 1;
	spin_unlock_irqrestore(&fsd->lock, flags);

	return !rc ? &fsd->req_out : NULL;
}

static void ftl_simple_dummy_new_request(struct mtdx_dev *this_dev,
					 struct mtdx_dev *req_dev)
{
	return;
}

static void ftl_simple_new_request(struct mtdx_dev *this_dev,
				   struct mtdx_dev *req_dev)
{
	struct mtdx_dev *parent = container_of(this_dev->dev.parent,
					       struct mtdx_dev, dev);
	struct ftl_simple_data *fsd = mtdx_get_drvdata(this_dev);

	get_device(&req_dev->dev);
	mtdx_dev_queue_push_back(&fsd->c_queue, req_dev);

	parent->new_request(parent, this_dev);
}

static int ftl_simple_get_param(struct mtdx_dev *this_dev,
				enum mtdx_param param, void *val)
{
	struct ftl_simple_data *fsd = mtdx_get_drvdata(this_dev);

	switch (param) {
	case MTDX_PARAM_GEO: {
		struct mtdx_geo *geo = (struct mtdx_geo *)val;
		memset(geo, 0, sizeof(struct mtdx_geo));
		geo->zone_size_log = sizeof(unsigned int) * 8;
		geo->log_block_cnt = fsd->geo.log_block_cnt;
		geo->page_cnt = fsd->geo.page_cnt;
		geo->page_size = fsd->geo.page_size;
		return 0;
	}
	case MTDX_PARAM_SPECIAL_BLOCKS:
	/* What about them in FTLs? */
		return -EINVAL;
	case MTDX_PARAM_HD_GEO:
	/* Really, we should make something up instead of blindly relying on
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
		struct mtdx_geo geo;
		unsigned int zone_cnt;

		rc = parent->get_param(parent, MTDX_PARAM_GEO, &geo);
		if (rc)
			return rc;

		zone_cnt = mtdx_geo_phy_to_zone(&geo, geo.phy_block_cnt - 1,
						NULL) + 1;

		fsd = kzalloc(sizeof(struct ftl_simple_data)
			      + sizeof(struct zone_data *) * zone_cnt,
			      GFP_KERNEL);
		if (!fsd)
			return -ENOMEM;

		fsd->zone_cnt = zone_cnt;
		memcpy(&fsd->geo, &geo, sizeof(geo));
	}

	fsd->zone_phy_cnt = min(fsd->geo.phy_block_cnt,
				1U << fsd->geo.zone_size_log);
	fsd->block_size = fsd->geo.page_cnt * fsd->geo.page_size;
	dev_dbg(&mdev->dev, "zone_cnt %x, zone_phy_cnt %x, oob_size %x\n",
		fsd->zone_cnt, fsd->zone_phy_cnt, fsd->geo.oob_size);

	spin_lock_init(&fsd->lock);
	mtdx_dev_queue_init(&fsd->c_queue);

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

	fsd->oob_buf = kmalloc(fsd->geo.oob_size * 2, GFP_KERNEL);
	if (!fsd->oob_buf) {
		rc = -ENOMEM;
		goto err_out;
	}

	parent->get_param(parent, MTDX_PARAM_READ_ONLY, &rc);

	if (!rc) {
		unsigned int r_cnt = max(fsd->geo.phy_block_cnt
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
					 + fsd->zone_phy_cnt
					   * sizeof(unsigned int),
					 GFP_KERNEL);
		if (!fsd->zones[rc]) {
			rc = -ENOMEM;
			goto err_out;
		}

		if (fsd->b_map) {
			fsd->zones[rc]->useful_blocks
				= kzalloc(BITS_TO_LONGS(fsd->zone_phy_cnt)
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

	fsd->mdev = mdev;
	fsd->src_block = MTDX_INVALID_BLOCK;
	fsd->dst_block = MTDX_INVALID_BLOCK;
	mtdx_set_drvdata(mdev, fsd);
	mdev->new_request = ftl_simple_new_request;
	mdev->get_request = ftl_simple_get_request;
	mdev->end_request = ftl_simple_end_request;
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
	struct mtdx_dev *c_dev;
	unsigned long flags;

	mdev->new_request = ftl_simple_dummy_new_request;

	do {
		c_dev = mtdx_dev_queue_pop_front(&fsd->c_queue);
		if (c_dev)
			put_device(&c_dev->dev);
	} while (c_dev);

	/* Wait for last client to finish. */
	spin_lock_irqsave(&fsd->lock, flags);
	while (fsd->req_in) {
		spin_unlock_irqrestore(&fsd->lock, flags);
		msleep_interruptible(1);
		spin_lock_irqsave(&fsd->lock, flags);
	}
	spin_unlock_irqrestore(&fsd->lock, flags);

	mtdx_drop_children(mdev);
	mtdx_set_drvdata(mdev, NULL);
	ftl_simple_free(fsd);
	dev_dbg(&mdev->dev, "ftl_simple removed\n");
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