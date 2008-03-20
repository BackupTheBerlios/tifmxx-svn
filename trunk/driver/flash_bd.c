/*
 *  flash_bd.c - Simple flash to block device translation layer
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "linux/flash_bd.h"
#include <linux/module.h>
#include <linux/random.h>
#include <linux/rbtree.h>
#include <asm/div64.h>

#define READ  0
#define WRITE 1

struct block_node {
	struct rb_node node;
	unsigned int   address;
	unsigned long  page_map[];
};

struct flash_bd {
	unsigned int       zone_cnt;
	unsigned int       phy_block_cnt;
	unsigned int       log_block_cnt;
	unsigned int       page_cnt;
	unsigned int       page_size;
	unsigned int       block_size;
	unsigned int       block_addr_bits;

	unsigned int       *free_cnt;
	unsigned int       *block_table;
	unsigned long      *erase_map;
	unsigned long      *data_map;

	struct rb_root     useful_blocks;
	struct rb_node     *retired_nodes;
	struct block_node  *c_block;

	enum flash_bd_cmd  current_cmd;
	unsigned long long byte_offset;
	unsigned int       t_count;
	unsigned int       rem_count;

	unsigned int       w_log_block;
	unsigned int       w_src_block;
	unsigned int       w_dst_block; 

	unsigned int       buf_offset; /* offset in the current block*/
	unsigned int       buf_page_off;
	unsigned int       buf_count;  /* count due in the current block */
	unsigned int       buf_page_cnt;
	unsigned int       req_count;  /* expected count for the current cmd*/

	unsigned int       last_count;
	int                last_error;

	int                (*cmd_handler)(struct flash_bd *fbd,
					  struct flash_bd_request *req);
};

/* For each block two bits are maintained:
 * Block status   erase_map  data_map
 *  unallocated       0          0
 *  clean             1          0
 *  semi-filled       1          1
 *  full              0          1
 *
 * For semi-filled blocks, the page map will be present in useful_blocks.
 */

static int h_flash_bd_read(struct flash_bd *fbd, struct flash_bd_request *req);
static int h_flash_bd_write(struct flash_bd *fbd, struct flash_bd_request *req);
static int h_flash_bd_write_inc(struct flash_bd *fbd,
				struct flash_bd_request *req);

/*** Bitmap helpers ***/

int bitmap_region_empty(unsigned long *bitmap, int pos, int count)
{
	int w_b, w_e, m_b, m_e, cnt;

	w_b = pos / BITS_PER_LONG;
	w_e = (pos + count) / BITS_PER_LONG;

	m_b = ~((1 << (pos % BITS_PER_LONG)) - 1);
	m_e = (1 << ((pos + count) % BITS_PER_LONG)) - 1;

	if (w_b == w_e) {
		if (bitmap[w_b] & m_b & m_e)
			return 0;
	} else {
		if (bitmap[w_b] & m_b)
			return 0;
		if (bitmap[w_e] & m_e)
			return 0;

		for (cnt = w_b + 1; cnt < w_e; ++cnt)
			if (bitmap[cnt])
				return 0;
	}
	return 1;
}

void bitmap_region_clear(unsigned long *bitmap, int pos, int count)
{
	int w_b, w_e, m_b, m_e, cnt;

	w_b = pos / BITS_PER_LONG;
	w_e = (pos + count) / BITS_PER_LONG;

	m_b = ~((1 << (pos % BITS_PER_LONG)) - 1);
	m_e = (1 << ((pos + count) % BITS_PER_LONG)) - 1;

	if (w_b == w_e) {
		bitmap[w_b] &= ~(m_b & m_e);
	} else {
		bitmap[w_b] &= ~m_b;
		bitmap[w_e] &= ~m_e;

		for (cnt = w_b + 1; cnt < w_e; ++cnt)
			bitmap[cnt] = 0UL;
	}
}

void bitmap_region_set(unsigned long *bitmap, int pos, int count)
{
	int w_b, w_e, m_b, m_e, cnt;

	w_b = pos / BITS_PER_LONG;
	w_e = (pos + count) / BITS_PER_LONG;

	m_b = ~((1 << (pos % BITS_PER_LONG)) - 1);
	m_e = (1 << ((pos + count) % BITS_PER_LONG)) - 1;

	if (w_b == w_e) {
		bitmap[w_b] |= m_b & m_e;
	} else {
		bitmap[w_b] |= m_b;
		bitmap[w_e] |= m_e;

		for (cnt = w_b + 1; cnt < w_e; ++cnt)
			bitmap[cnt] = ~0UL;
	}
}

/*** Block map manipulators ***/

static struct block_node* flash_bd_find_useful(struct flash_bd *fbd,
					       unsigned int phy_block)
{
	struct rb_node *n = fbd->useful_blocks.rb_node;
	struct block_node *rv;

	while (n) {
		rv = rb_entry(n, struct block_node, node);

		if (phy_block < rv->address)
			n = n->rb_left;
		else if (phy_block > rv->address)
			n = n->rb_right;
		else
			return rv;
	}

	return NULL;
}

static struct block_node* flash_bd_alloc_useful(struct flash_bd *fbd)
{
	struct block_node *rv;

	rv = kzalloc(sizeof(struct block_node)
		     + BITS_TO_LONGS(fbd->page_cnt) * sizeof(unsigned long),
		     GFP_KERNEL);
	return rv;
}

static int flash_bd_add_useful(struct flash_bd *fbd, struct block_node *b)
{
	struct rb_node **p = &(fbd->useful_blocks.rb_node);
	struct rb_node *q = NULL;
	struct block_node *cb = NULL;

	while (*p) {
		q = *p;
		cb = rb_entry(q, struct block_node, node);

		if (b->address < cb->address)
			p = &(*p)->rb_left;
		else if (b->address > cb->address)
			p = &(*p)->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&b->node, q, p);
	rb_insert_color(&b->node, &fbd->useful_blocks);
	return 0;
}

static int flash_bd_insert_useful(struct flash_bd *fbd, unsigned int phy_block)
{
	struct block_node *b;

	if (!fbd->retired_nodes)
		return -ENOMEM;

	b = rb_entry(fbd->retired_nodes, struct block_node, node);

	fbd->retired_nodes = b->node.rb_right;
	b->node.rb_right = NULL;
	b->address = phy_block;
	bitmap_zero(b->page_map, fbd->page_cnt);
	if (flash_bd_add_useful(fbd, b)) {
		b->node.rb_right = fbd->retired_nodes;
		fbd->retired_nodes = &b->node;
		return -EEXIST;
	}
	return 0;
}

static void flash_bd_free_all_useful(struct flash_bd *fbd)
{
	struct rb_node *p, *q;
	struct block_node *b;

	for (p = fbd->useful_blocks.rb_node; p; p = q) {
		if (!p->rb_left) {
			q = p->rb_right;
			b = rb_entry(p, struct block_node, node);
			kfree(b);
		} else {
			q = p->rb_left;
			p->rb_left = q->rb_right;
			q->rb_right = p;
		}
	}
	fbd->useful_blocks.rb_node = NULL;
}

static void flash_bd_retire_useful(struct flash_bd *fbd, unsigned int phy_block)
{
	if (fbd->c_block->address != phy_block)
		fbd->c_block = flash_bd_find_useful(fbd, phy_block);

	if (!fbd->c_block)
		return;

	rb_erase(&fbd->c_block->node, &fbd->useful_blocks);
	fbd->c_block->node.rb_left = NULL;
	fbd->c_block->node.rb_right = fbd->retired_nodes;
	fbd->retired_nodes = &fbd->c_block->node;
	fbd->c_block = NULL;
}

static void flash_bd_mark_used(struct flash_bd *fbd, unsigned int phy_block)
{
	if (!test_bit(phy_block, fbd->data_map)) {
		unsigned int zone = phy_block >> fbd->block_addr_bits;
		if (fbd->free_cnt[zone])
			fbd->free_cnt[zone]--;
	} else if (test_bit(phy_block, fbd->erase_map)) {
		flash_bd_retire_useful(fbd, phy_block);
	}

	set_bit(phy_block, fbd->data_map);
	clear_bit(phy_block, fbd->erase_map);
}

static void flash_bd_mark_erased(struct flash_bd *fbd, unsigned int phy_block)
{
	if (test_bit(phy_block, fbd->data_map)) {
		unsigned int zone = phy_block >> fbd->block_addr_bits;
		fbd->free_cnt[zone]++;
	}

	clear_bit(phy_block, fbd->data_map);
	set_bit(phy_block, fbd->erase_map);
}

static int flash_bd_can_merge(struct flash_bd *fbd, unsigned int phy_block,
			      unsigned int page, unsigned int count)
{
	if (phy_block == FLASH_BD_INVALID)
		return 0;

	if (!test_bit(phy_block, fbd->data_map)
	    || !test_bit(phy_block, fbd->erase_map))
		return 0;

	if (fbd->c_block->address != phy_block)
		fbd->c_block = flash_bd_find_useful(fbd, phy_block);

	if (!fbd->c_block)
		return 0;

	return bitmap_region_empty(fbd->c_block->page_map, page, count);
}

/*** Management functions ***/

struct flash_bd* flash_bd_init(unsigned int zone_cnt,
			       unsigned int phy_block_cnt,
			       unsigned int log_block_cnt,
			       unsigned int page_cnt,
			       unsigned int page_size)

{
	unsigned int cnt;
	struct flash_bd *rv = kzalloc(sizeof(struct flash_bd), GFP_KERNEL);
	if (!rv)
		return NULL;


	rv->zone_cnt = zone_cnt;
	rv->phy_block_cnt = phy_block_cnt;
	rv->log_block_cnt = log_block_cnt;
	rv->page_cnt = page_cnt;
	rv->page_size = page_size;
	rv->block_size = page_size * page_cnt;

	rv->block_addr_bits = fls(rv->phy_block_cnt - 1);
	if ((1 << rv->block_addr_bits) < rv->phy_block_cnt)
		rv->block_addr_bits++;

	rv->free_cnt = kmalloc(sizeof(unsigned int) * rv->zone_cnt, GFP_KERNEL);
	if (!rv->free_cnt)
		goto err_out;

	for (cnt = 0; cnt < rv->zone_cnt; ++cnt)
		rv->free_cnt[cnt] = rv->phy_block_cnt;

	rv->block_table = kmalloc(sizeof(unsigned int)
				  * (rv->zone_cnt << rv->block_addr_bits),
				  GFP_KERNEL);
	if (!rv->block_table)
		goto err_out;

	for (cnt = 0; cnt < (rv->zone_cnt << rv->block_addr_bits); ++cnt)
		rv->block_table[cnt] = FLASH_BD_INVALID;

	rv->erase_map = kzalloc(BITS_TO_LONGS(rv->zone_cnt
					      << rv->block_addr_bits)
				* sizeof(unsigned long), GFP_KERNEL);
	if (!rv->erase_map)
		goto err_out;

	rv->data_map = kzalloc(BITS_TO_LONGS(rv->zone_cnt
					     << rv->block_addr_bits)
			       * sizeof(unsigned long), GFP_KERNEL);
	if (!rv->data_map)
		goto err_out;

err_out:
	flash_bd_destroy(rv);
	return NULL;
}
EXPORT_SYMBOL(flash_bd_init);

void flash_bd_destroy(struct flash_bd *fbd)
{
	struct block_node *b;

	if (!fbd)
		return;

	kfree(fbd->free_cnt);
	kfree(fbd->block_table);
	kfree(fbd->erase_map);
	kfree(fbd->data_map);
	flash_bd_free_all_useful(fbd);

	while (fbd->retired_nodes) {
		b = rb_entry(fbd->retired_nodes, struct block_node, node);
		fbd->retired_nodes = b->node.rb_right;
		kfree(b);
	}

	kfree(fbd);
}
EXPORT_SYMBOL(flash_bd_destroy);

unsigned int flash_bd_get_logical(struct flash_bd *fbd, unsigned int phy_block)
{
	unsigned int cnt;
	unsigned int zone = phy_block >> fbd->block_addr_bits;

	if (phy_block == FLASH_BD_INVALID)
		return FLASH_BD_INVALID;

	for (cnt = zone << fbd->block_addr_bits;
	     cnt < ((zone + 1) << fbd->block_addr_bits);
	     ++cnt) {
		if (fbd->block_table[cnt] == phy_block)
			return cnt;
	}
	return FLASH_BD_INVALID;
}

unsigned int flash_bd_get_physical(struct flash_bd *fbd, unsigned int log_block)
{
	if (log_block == FLASH_BD_INVALID
	    || log_block >= (fbd->zone_cnt << fbd->block_addr_bits))
		return FLASH_BD_INVALID;

	return fbd->block_table[log_block];
}

int flash_bd_set_empty(struct flash_bd *fbd, unsigned int zone,
		       unsigned int phy_block, int erased)
{
	unsigned int log_block;

	if (phy_block >= fbd->phy_block_cnt)
		return -EINVAL;

	phy_block |= zone << fbd->block_addr_bits;

	if (test_bit(phy_block, fbd->data_map)) {
		log_block = flash_bd_get_logical(fbd, phy_block);
		if (log_block != FLASH_BD_INVALID)
			fbd->block_table[log_block] = FLASH_BD_INVALID;

		clear_bit(phy_block, fbd->data_map);
		flash_bd_retire_useful(fbd, phy_block);
		fbd->free_cnt[zone]++;
	}

	if (erased)
		set_bit(phy_block, fbd->erase_map);

	return 0;
}
EXPORT_SYMBOL(flash_bd_set_empty);

int flash_bd_set_full(struct flash_bd *fbd, unsigned int zone,
		      unsigned int phy_block, unsigned int log_block)
{
	if (phy_block == FLASH_BD_INVALID
	    || phy_block >= fbd->phy_block_cnt)
		return -EINVAL;

	phy_block |= zone << fbd->block_addr_bits;

	if (log_block == FLASH_BD_INVALID) {
		if (test_bit(phy_block, fbd->data_map)) {
			log_block = flash_bd_get_logical(fbd, phy_block);
			if (log_block != FLASH_BD_INVALID)
				fbd->block_table[log_block] = FLASH_BD_INVALID;
		}
	} else if (log_block < fbd->log_block_cnt) {
		if (test_bit(phy_block, fbd->data_map))
			return -EEXIST;

		log_block |= zone << fbd->block_addr_bits;
		fbd->block_table[log_block] = phy_block;
	} else
		return -EINVAL;


	if (!test_bit(phy_block, fbd->data_map)) {
		set_bit(phy_block, fbd->data_map);
		if (fbd->free_cnt)
			fbd->free_cnt--;
	} else
		flash_bd_retire_useful(fbd, phy_block);

	clear_bit(phy_block, fbd->erase_map);
	return 0;
}
EXPORT_SYMBOL(flash_bd_set_full);

int flash_bd_start_writing(struct flash_bd *fbd, unsigned long long offset,
			   unsigned int count)
{
	struct block_node *b;

	fbd->byte_offset = offset;
	fbd->t_count = 0;
	fbd->rem_count = count;

	/* always keep two useful block node entries, just in case */

	if (!fbd->retired_nodes) {
		b = flash_bd_alloc_useful(fbd);
		if (!b)
			return -ENOMEM;

		fbd->retired_nodes = &b->node;
	}

	if (!fbd->retired_nodes->rb_right) {
		b = flash_bd_alloc_useful(fbd);
		if (!b)
			return -ENOMEM;

		b->node.rb_right = fbd->retired_nodes;
		fbd->retired_nodes = &b->node;
	}

	fbd->cmd_handler = h_flash_bd_write;

	return 0;
}
EXPORT_SYMBOL(flash_bd_start_writing);

int flash_bd_start_reading(struct flash_bd *fbd, unsigned long long offset,
			   unsigned int count)
{
	fbd->byte_offset = offset;
	fbd->t_count = 0;
	fbd->rem_count = count;
	fbd->cmd_handler = h_flash_bd_read;
	fbd->buf_offset = 0;
	fbd->buf_count = 0;
	fbd->last_count = 0;
	fbd->last_error = 0;

	return 0;
}
EXPORT_SYMBOL(flash_bd_start_reading);

int flash_bd_next_req(struct flash_bd *fbd, struct flash_bd_request *req,
		      unsigned int count, int error)
{
	fbd->last_count = count;
	fbd->last_error = error;

	return (fbd->cmd_handler)(fbd, req);
}
EXPORT_SYMBOL(flash_bd_next_req);

unsigned int flash_bd_end(struct flash_bd *fbd)
{
	struct rb_node *p, *q;

	/* Any excess of two retired nodes can be disposed of. */
	if (fbd->retired_nodes && fbd->retired_nodes->rb_right) {
		p = fbd->retired_nodes->rb_right;
		fbd->retired_nodes->rb_right = NULL;
		while (p) {
			q = p->rb_right;
			kfree(rb_entry(p, struct block_node, node));
			p = q;
		}
	}

	return fbd->t_count;
}
EXPORT_SYMBOL(flash_bd_end);

/*** Protocol processing ***/

static unsigned int flash_bd_get_free(struct flash_bd *fbd, unsigned int zone)
{
	unsigned long r_pos = (random32() % fbd->free_cnt[zone]) + 1;
	unsigned long pos = zone << fbd->block_addr_bits; 

	do {
		pos = find_next_zero_bit(fbd->data_map,
					 fbd->phy_block_cnt * fbd->zone_cnt,
					 pos);
		if (pos >= ((zone + 1) << fbd->block_addr_bits))
			return FLASH_BD_INVALID;

		r_pos--;
	} while (r_pos);

	return pos;
}

static int h_flash_bd_read_tmp_r(struct flash_bd *fbd,
			         struct flash_bd_request *req)
{
	if (fbd->last_error)
		return fbd->last_error;

	if (fbd->req_count != fbd->last_count)
		return -EIO;

	req->cmd = FBD_FLUSH_TMP;
	req->zone = FLASH_BD_INVALID;
	req->log_block = FLASH_BD_INVALID;
	req->phy_block = FLASH_BD_INVALID;
	req->byte_off = fbd->buf_offset;
	req->byte_cnt = fbd->buf_count;
	fbd->req_count = fbd->buf_count;
	fbd->cmd_handler = h_flash_bd_read;
	return 0;
}

static int h_flash_bd_read(struct flash_bd *fbd, struct flash_bd_request *req)
{
	unsigned long long zone_off;
	unsigned int log_block;

	fbd->t_count += fbd->last_count;
	fbd->rem_count -= fbd->last_count;

	if (!fbd->rem_count) {
		req->cmd = FBD_NONE;
		return 0;
	}

	if (fbd->last_error)
		return fbd->last_error;

	zone_off = fbd->byte_offset + fbd->t_count;
	fbd->buf_offset = do_div(zone_off, fbd->block_size);
	log_block = do_div(zone_off, fbd->log_block_cnt);

	req->zone = zone_off;

	if (req->zone >= fbd->zone_cnt)
		return -ENOSPC;

	fbd->buf_count = min(fbd->rem_count, fbd->block_size - fbd->buf_offset);

	req->log_block = log_block;
	log_block |= req->zone << fbd->block_addr_bits;

	req->phy_block = flash_bd_get_physical(fbd, log_block);
	if (req->phy_block != FLASH_BD_INVALID)
		req->phy_block &= (1 << fbd->block_addr_bits) - 1;
	else {
		req->cmd = FBD_SKIP;
		req->byte_off = 0;
		req->byte_cnt = fbd->buf_count;
		fbd->req_count = fbd->buf_count;
		fbd->cmd_handler = h_flash_bd_read;
		return 0;
	}

	req->page_off = fbd->buf_offset / fbd->page_size;

	if ((fbd->buf_offset % fbd->page_size)
	    || (fbd->buf_count % fbd->page_size)) {
		req->cmd = FBD_READ_TMP;
		req->page_cnt = ((fbd->buf_offset + fbd->buf_count)
				 / fbd->page_size) - req->page_off + 1;
		fbd->req_count = req->page_cnt * fbd->page_size;
		fbd->cmd_handler = h_flash_bd_read_tmp_r;
	} else {
		req->cmd = FBD_READ;
		req->page_cnt = fbd->buf_count / fbd->page_size;
		fbd->req_count = fbd->buf_count;
		fbd->cmd_handler = h_flash_bd_read;
	}

	return 0;
}


static int h_flash_bd_clear_error(struct flash_bd *fbd,
				  struct flash_bd_request *req)
{
	/* Just pretend everything went ok. */
	fbd->last_error = 0;
	return h_flash_bd_write_inc(fbd, req);
}

static int h_flash_bd_copy(struct flash_bd *fbd,
			   struct flash_bd_request *req)
{
	/* Ignoring errors here seams to be better idea than falling */
	if (fbd->last_error)
		fbd->last_error = 0;

	req->page_cnt = fbd->page_cnt - fbd->buf_page_off - fbd->buf_page_cnt;

	if (req->page_cnt) {
		req->cmd = FBD_COPY;
		req->zone = fbd->w_log_block >> fbd->block_addr_bits;
		req->log_block = fbd->w_log_block
				 & ((1 << fbd->block_addr_bits) - 1);
		req->phy_block = fbd->w_dst_block
				 & ((1 << fbd->block_addr_bits) - 1);
		req->page_off = fbd->buf_page_off + fbd->buf_page_cnt;
		fbd->req_count = 0;
		fbd->cmd_handler = h_flash_bd_clear_error;
	} else
		return h_flash_bd_write_inc(fbd, req);

	return 0;
}

static int h_flash_bd_mark(struct flash_bd *fbd,
			   struct flash_bd_request *req)
{
	/* Ignoring errors here seams to be better idea than falling */
	if (fbd->last_error)
		fbd->last_error = 0;

	req->page_cnt = fbd->page_cnt - fbd->buf_page_off - fbd->buf_page_cnt;

	if (req->page_cnt) {
		req->cmd = FBD_MARK;
		req->zone = fbd->w_log_block >> fbd->block_addr_bits;
		req->log_block = fbd->w_log_block
				 & ((1 << fbd->block_addr_bits) - 1);
		req->phy_block = fbd->w_dst_block
				 & ((1 << fbd->block_addr_bits) - 1);
		req->page_off = fbd->buf_page_off + fbd->buf_page_cnt;
		fbd->req_count = 0;
		fbd->cmd_handler = h_flash_bd_clear_error;
	} else
		return h_flash_bd_write_inc(fbd, req);

	return 0;
}

static int h_flash_bd_write_pages(struct flash_bd *fbd,
				  struct flash_bd_request *req)
{
	if (fbd->last_count != fbd->req_count) {
		if (!fbd->last_error)
			fbd->last_error = -EIO;
	}

	if (fbd->last_error) {
		flash_bd_mark_used(fbd, fbd->w_dst_block);
		flash_bd_retire_useful(fbd, fbd->w_dst_block);
		return fbd->last_error;
	}

	req->zone = fbd->w_log_block >> fbd->block_addr_bits;
	req->log_block = fbd->w_log_block & ((1 << fbd->block_addr_bits) - 1);
	req->phy_block = fbd->w_dst_block & ((1 << fbd->block_addr_bits) - 1);
	req->src.phy_block = fbd->w_src_block
			     & ((1 << fbd->block_addr_bits) - 1);

	set_bit(fbd->w_dst_block, fbd->data_map);
	fbd->block_table[fbd->w_log_block] = fbd->w_dst_block;

	if (fbd->w_src_block == FLASH_BD_INVALID) {
		/* New physical block was allocated */
		if (fbd->free_cnt[req->zone])
			fbd->free_cnt[req->zone]--;

		if (fbd->buf_page_cnt < fbd->page_cnt) {
			/* Can't insert useful blocks entry - no worries! */
			if (flash_bd_insert_useful(fbd, fbd->w_dst_block))
				clear_bit(fbd->w_dst_block, fbd->erase_map);

			req->cmd = FBD_MARK;
			if (fbd->buf_page_off) {
				req->page_off = 0;
				req->page_cnt = fbd->buf_page_off;
				fbd->req_count = 0;
				fbd->cmd_handler = h_flash_bd_mark;
			} else {
				req->page_off = fbd->buf_page_off
						+ fbd->buf_page_cnt;
				req->page_cnt = fbd->page_cnt - req->page_off;
				fbd->req_count = 0;
				fbd->cmd_handler = h_flash_bd_clear_error;
			}
		} else {
			clear_bit(fbd->w_dst_block, fbd->erase_map);
			return h_flash_bd_write_inc(fbd, req);
		}
	} else {
		/* Physical block was replaced or merged */
		if (fbd->c_block
		    && (fbd->c_block->address == fbd->w_src_block)) {
			if (fbd->w_src_block != fbd->w_dst_block) {
				rb_erase(&fbd->c_block->node,
					 &fbd->useful_blocks);
				fbd->c_block->address = fbd->w_dst_block;
				flash_bd_add_useful(fbd, fbd->c_block);
				clear_bit(fbd->w_src_block, fbd->erase_map);
				set_bit(fbd->w_dst_block, fbd->erase_map);
			}
		} else
			clear_bit(fbd->w_dst_block, fbd->erase_map);

		if ((fbd->w_src_block != fbd->w_dst_block)
		    && (fbd->buf_page_cnt < fbd->page_cnt)) {
			req->cmd = FBD_COPY;

			if (fbd->buf_page_off) {
				req->page_off = 0;
				req->src.page_off = 0;
				req->page_cnt = fbd->buf_page_off;
				fbd->req_count = 0;
				fbd->cmd_handler = h_flash_bd_copy;
			} else {
				req->page_off = fbd->buf_page_off
						+ fbd->buf_page_cnt;
				req->src.page_off = req->page_off;
				req->page_cnt = fbd->page_cnt - req->page_off;
				fbd->req_count = 0;
				fbd->cmd_handler = h_flash_bd_clear_error;
			}
		} else
			return h_flash_bd_write_inc(fbd, req);
	}

	return 0;
}

static int h_flash_bd_erase_dst(struct flash_bd *fbd,
				struct flash_bd_request *req)
{
	unsigned int zone = fbd->w_dst_block >>  fbd->block_addr_bits;

	req->zone = zone;
	req->log_block = fbd->w_log_block
			 & ((1 << fbd->block_addr_bits) - 1);
	req->phy_block = fbd->w_dst_block
			 & ((1 << fbd->block_addr_bits) - 1);

	if (!fbd->last_error) {
		flash_bd_mark_erased(fbd, fbd->w_dst_block);
		if ((fbd->w_src_block != fbd->w_dst_block)
		    || (fbd->buf_count == fbd->block_size)) {

			req->page_off = fbd->buf_page_off;
			req->page_cnt = fbd->buf_page_cnt;
			fbd->req_count = req->page_cnt * fbd->page_size;

			if ((fbd->buf_offset % fbd->page_size)
			    || (fbd->buf_count % fbd->page_size))
				req->cmd = FBD_WRITE_TMP;
			else
				req->cmd = FBD_WRITE;
		} else {
			req->cmd = FBD_WRITE_TMP;
			req->page_off = 0;
			req->page_cnt = fbd->page_cnt;
			fbd->req_count = fbd->block_size;
		}
		fbd->cmd_handler = h_flash_bd_write_pages;
	} else if (fbd->last_error == -EFAULT) {
		flash_bd_mark_used(fbd, fbd->w_dst_block);
		fbd->w_dst_block = flash_bd_get_free(fbd, zone);
		if (fbd->w_dst_block == FLASH_BD_INVALID)
			return fbd->last_error;

		req->cmd = FBD_ERASE;
		fbd->req_count = 0;
		fbd->cmd_handler = h_flash_bd_erase_dst;
	} else
		return fbd->last_error;

	return 0;
}

static int h_flash_bd_erase_src(struct flash_bd *fbd,
				struct flash_bd_request *req)
{
	if (!fbd->last_error) {
		clear_bit(fbd->w_src_block, fbd->data_map);
		set_bit(fbd->w_src_block, fbd->erase_map);
		fbd->free_cnt[fbd->w_src_block >> fbd->block_addr_bits]++;
	} else {
		if (fbd->last_error == -EFAULT)
			fbd->last_error = 0;
		else
			return fbd->last_error;
	}

	if (!fbd->rem_count) {
		req->cmd = FBD_NONE;
		return 0;
	} else
		return h_flash_bd_write(fbd, req);

}

static int h_flash_bd_write_inc(struct flash_bd *fbd,
				struct flash_bd_request *req)
{
	if (fbd->last_error)
		return fbd->last_error;

	if (fbd->req_count != fbd->last_count)
		return -EIO;

	fbd->t_count += fbd->buf_count;
	fbd->rem_count -= fbd->buf_count;

	req->zone = fbd->w_log_block >> fbd->block_addr_bits;
	req->log_block = fbd->w_log_block & ((1 <<  fbd->block_addr_bits) - 1);
	req->phy_block = fbd->w_src_block & ((1 <<  fbd->block_addr_bits) - 1);
	req->page_off = 0;
	req->page_cnt = 0;
	fbd->req_count = 0;

	if (fbd->c_block && (fbd->c_block->address == fbd->w_dst_block)) {
		bitmap_region_set(fbd->c_block->page_map, fbd->buf_page_off,
				  fbd->buf_page_cnt);
		if (bitmap_full(fbd->c_block->page_map, fbd->page_cnt)) {
			flash_bd_retire_useful(fbd, fbd->w_dst_block);
			clear_bit(fbd->w_dst_block, fbd->erase_map);
		}
	}

	if ((fbd->w_src_block != FLASH_BD_INVALID)
	    && (fbd->w_src_block != fbd->w_dst_block)) {
		clear_bit(fbd->w_src_block, fbd->erase_map);
		req->cmd = FBD_ERASE;
		fbd->cmd_handler = h_flash_bd_erase_src;
		return 0;
	}

	if (!fbd->rem_count) {
		req->cmd = FBD_NONE;
		return 0;
	} else
		return h_flash_bd_write(fbd, req);
}

static int h_flash_bd_fill_tmp(struct flash_bd *fbd,
			       struct flash_bd_request *req)
{
	if (fbd->last_error)
		return fbd->last_error;

	if (fbd->req_count != fbd->last_count)
		return -EIO;

	req->zone = fbd->w_log_block >> fbd->block_addr_bits;
	req->log_block = fbd->w_log_block & ((1 <<  fbd->block_addr_bits) - 1);
	req->phy_block = fbd->w_dst_block & ((1 <<  fbd->block_addr_bits) - 1);

	if (flash_bd_can_merge(fbd, fbd->w_src_block, fbd->buf_page_off,
			       fbd->buf_page_cnt)) {
		req->cmd = FBD_WRITE;
		req->page_off = fbd->buf_page_off;
		req->page_cnt = fbd->buf_page_cnt;
		fbd->req_count = req->page_cnt * fbd->page_size;
		fbd->cmd_handler = h_flash_bd_write_inc;
	} else {
		req->cmd = FBD_ERASE;
		req->page_off = 0;
		req->page_cnt = 0;
		fbd->req_count = 0;
		fbd->cmd_handler = h_flash_bd_erase_dst;
	}

	return 0;
}

static int h_flash_bd_read_tmp_w(struct flash_bd *fbd,
			         struct flash_bd_request *req)
{
	if (fbd->last_error)
		return fbd->last_error;

	if (fbd->req_count != fbd->last_count)
		return -EIO;


	req->cmd = FBD_FILL_TMP;
 	req->zone = FLASH_BD_INVALID;
	req->log_block = FLASH_BD_INVALID;
	req->phy_block = FLASH_BD_INVALID;
	req->byte_off = fbd->buf_offset;
	req->byte_cnt = fbd->buf_count;
	fbd->req_count = fbd->buf_count;
	fbd->cmd_handler = h_flash_bd_fill_tmp;
	return 0;
}

static int h_flash_bd_write(struct flash_bd *fbd,
			    struct flash_bd_request *req)
{
	unsigned long long zone_off;

	if (fbd->last_error)
		return fbd->last_error;

	zone_off = fbd->byte_offset + fbd->t_count;
	fbd->buf_offset = do_div(zone_off, fbd->block_size);
	fbd->buf_count = min(fbd->rem_count, fbd->block_size - fbd->buf_offset);
	req->log_block = do_div(zone_off, fbd->log_block_cnt);

	fbd->buf_page_off = fbd->buf_offset / fbd->page_size;
	fbd->buf_page_cnt = ((fbd->buf_offset + fbd->buf_count)
			    / fbd->page_size) - fbd->buf_page_off + 1;

	req->zone = zone_off;

	if (req->zone >= fbd->zone_cnt)
		return -ENOSPC;

	fbd->w_log_block = req->log_block | req->zone << fbd->block_addr_bits;
	fbd->w_src_block = flash_bd_get_physical(fbd, fbd->w_log_block);

	if (flash_bd_can_merge(fbd, fbd->w_src_block, fbd->buf_page_off,
			       fbd->buf_page_cnt)) {
		fbd->w_dst_block = fbd->w_src_block;
		if ((fbd->buf_offset % fbd->page_size)
		    || (fbd->buf_count % fbd->page_size)) {
			req->cmd = FBD_FILL_TMP;
			req->phy_block = FLASH_BD_INVALID;
			req->byte_off = fbd->buf_offset;
			req->byte_cnt = fbd->buf_count;
			fbd->cmd_handler = h_flash_bd_fill_tmp;
		} else {
			req->cmd = FBD_WRITE;
			req->phy_block = fbd->w_src_block;
			req->phy_block &= (1 << fbd->block_addr_bits) - 1;
			req->page_off = fbd->buf_page_off;
			req->page_cnt = fbd->buf_page_cnt;
			fbd->cmd_handler = h_flash_bd_write_inc;
		}
	} else {
		fbd->w_dst_block = flash_bd_get_free(fbd, zone_off);
		if (fbd->w_dst_block == FLASH_BD_INVALID)
			fbd->w_dst_block = fbd->w_src_block;
		if (fbd->w_dst_block == FLASH_BD_INVALID)
			return -EIO;

		req->phy_block = fbd->w_dst_block;
		req->phy_block &= (1 << fbd->block_addr_bits) - 1;

		if ((fbd->w_dst_block != fbd->w_src_block)
		    || (fbd->buf_count == fbd->block_size)) {

			if ((fbd->buf_offset % fbd->page_size)
			    || (fbd->buf_count % fbd->page_size)) {
				req->cmd = FBD_READ_TMP;
				req->page_off = fbd->buf_page_off;
				req->page_cnt = fbd->buf_page_cnt;
				fbd->cmd_handler = h_flash_bd_read_tmp_w;
			} else {
				if (test_bit(fbd->w_dst_block,
					     fbd->erase_map)) {
					fbd->last_error = 0;
					fbd->last_count = 0;
					return h_flash_bd_erase_dst(fbd, req);
				} else {
					req->cmd = FBD_ERASE;
					req->page_off = 0;
					req->page_cnt = 0;
					fbd->req_count = 0;
					fbd->cmd_handler = h_flash_bd_erase_dst;
				}
			}
		} else {
			req->cmd = FBD_READ_TMP;
			req->page_off = 0;
			req->page_cnt = fbd->page_cnt;
			fbd->req_count = fbd->block_size;
			fbd->cmd_handler = h_flash_bd_read_tmp_w;
		}
	}

	return 0;
}

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Simple flash to block device translation layer");
MODULE_LICENSE("GPL");
