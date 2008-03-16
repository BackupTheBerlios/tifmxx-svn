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

#define PAGES_PER_NODE 128

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
	unsigned int       block_off_mask;

	unsigned int       *free_cnt;
	unsigned int       *block_table;
	unsigned long      *erase_map;
	unsigned long      *data_map;

	struct rb_root     useful_blocks;
	struct rb_node     *filled_blocks;

	unsigned int       data_dir:1,
			   active:1;
	enum flash_bd_cmd  current_cmd;
	unsigned long long byte_offset;
	unsigned int       t_count;
	unsigned int       rem_count;

	unsigned int       req_count;
	unsigned int       src_block;
	unsigned int       dst_block;
	unsigned int       src_page;
	unsigned int       pb_off;
	unsigned int       pb_cnt;

	unsigned int       buf_offset;
	unsigned int       buf_count;

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



static struct block_node* flash_bd_find_useful(struct flash_bd *fbd,
					       unsigned int phy_block)
{
	struct rb_node *n = fbd->useful_blocks.rb_node;
	struct block_node *rv;

	phy_block -= phy_block & fbd->block_off_mask;

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
		     + BITS_TO_LONGS(fbd->page_cnt * (fbd->block_off_mask + 1))
		       * sizeof(unsigned long),
		     GFP_KERNEL);
	return rv;
}

static int flash_bd_add_useful(struct flash_bd *fbd, struct block_node *b)
{
	struct rb_node **p = &(fbd->useful_blocks.rb_node);
	struct rb_node *q;
	struct block_node *cb;

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

static void flash_bd_erase_useful(struct flash_bd *fbd, unsigned int phy_block)
{
	struct block_node *b = flash_bd_find_useful(fbd, phy_block);

	if (b) {
		rb_erase(&b->node, &fbd->useful_blocks);
		kfree(b);
	}
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

	cnt = PAGES_PER_NODE / rv->page_cnt;
	rv->block_off_mask = fls(cnt - 1);
	if ((1 << rv->block_off_mask) < cnt)
		rv->block_off_mask++;

	rv->block_off_mask = (1 << rv->block_off_mask) - 1;

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

	while (fbd->filled_blocks) {
		b = rb_entry(fbd->filled_blocks, struct block_node, node);
		fbd->filled_blocks = b->node.rb_right;
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
		flash_bd_erase_useful(fbd, phy_block);
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
		flash_bd_erase_useful(fbd, phy_block);

	clear_bit(phy_block, fbd->erase_map);
	return 0;
}
EXPORT_SYMBOL(flash_bd_set_full);

static unsigned int flash_bd_get_free(struct flash_bd *fbd, unsigned int zone)
{
	unsigned long r_pos = (random32() % fbd->free_cnt[zone]) + 1;
	unsigned long pos = zone << fbd->block_addr_bits; 

	do {
		pos = find_next_zero_bit(fbd->data_map, fbd->phy_block_cnt,
					 pos);
		if (pos >= ((zone + 1) << fbd->block_addr_bits))
			return FLASH_BD_INVALID;

		r_pos--;
	} while (r_pos);

	return pos;
}

static int flash_bd_can_merge(struct flash_bd *fbd, unsigned int phy_block,
			      unsigned int page, unsigned int count)
{
	struct block_node *b;
	unsigned int cnt;

	if (phy_block == FLASH_BD_INVALID)
		return 0;

	b = flash_bd_find_useful(fbd, phy_block);
	if (!b)
		return 0;

	page += (phy_block & fbd->block_off_mask) * (fbd->block_off_mask + 1);

	for (cnt = page; cnt < page + count; ++cnt) {
		if (test_bit(cnt, b->page_map))
			return 0;
	}

	return 1;
}

int flash_bd_start_writing(struct flash_bd *fbd, unsigned long long offset,
			   unsigned int count)
{
	struct block_node *b;

	if (fbd->active)
		return -EBUSY;

	fbd->data_dir = WRITE;
	fbd->byte_offset = offset;
	fbd->t_count = 0;
	fbd->rem_count = count;
	fbd->req_count = 0;
	fbd->dst_block = FLASH_BD_INVALID;
	fbd->buf_offset = 0;
	fbd->buf_count = 0;

	/* always keep two useful block node entries, just in case */

	if (!fbd->filled_blocks) {
		b = flash_bd_alloc_useful(fbd);
		if (!b)
			return -ENOMEM;

		fbd->filled_blocks = &b->node;
	}

	if (!fbd->filled_blocks->rb_right) {
		b = flash_bd_alloc_useful(fbd);
		if (!b)
			return -ENOMEM;

		b->node.rb_right = fbd->filled_blocks;
		fbd->filled_blocks = &b->node;
	}

	fbd->active = 1;

	return 0;
}


static int h_flash_bd_add_bytes(struct flash_bd *fbd,
				struct flash_bd_request *req)
{
	fbd->t_count += fbd->last_count;
	fbd->rem_count -= fbd->last_count;

	if (!fbd->rem_count) {
		req->cmd = FBD_NONE;
		return 0;
	} else if (fbd->last_error) {
		return fbd->last_error;
	} else {
		return fbd->data_dir == READ
		       ? h_flash_bd_read(fbd, req)
		       : h_flash_bd_write(fbd, req);
	}
}

static int h_flash_bd_add_pages(struct flash_bd *fbd,
				struct flash_bd_request *req)
{
	fbd->t_count += fbd->last_count * fbd->page_size;
	fbd->rem_count -= fbd->last_count * fbd->page_size;

	if (!fbd->rem_count) {
		req->cmd = FBD_NONE;
		return 0;
	} else if (fbd->last_error) {
		return fbd->last_error;
	} else {
		return fbd->data_dir == READ
		       ? h_flash_bd_read(fbd, req)
		       : h_flash_bd_write(fbd, req);
	}
}

static int h_flash_bd_flush(struct flash_bd *fbd, struct flash_bd_request *req)
{
	if (fbd->last_error && fbd->last_count < 1)
		return fbd->last_error;

	req->cmd = FBD_FLUSH_TMP;
	req->zone = FLASH_BD_INVALID;
	req->log_block = FLASH_BD_INVALID;
	req->phy_block = FLASH_BD_INVALID;
	req->byte_off = fbd->buf_offset;
	req->byte_cnt = fbd->buf_count;
	fbd->cmd_handler = h_flash_bd_add_bytes;
	return 0;
}

static int h_flash_bd_read(struct flash_bd *fbd, struct flash_bd_request *req)
{
	unsigned long long zone_off = fbd->byte_offset + fbd->t_count;
	unsigned int block_off = do_div(zone_off, fbd->block_size); 
	unsigned int log_block = do_div(zone_off, fbd->log_block_cnt);

	fbd->buf_count = fbd->block_size - block_off;
	req->zone = zone_off;

	if (req->zone >= fbd->zone_cnt)
		return -ENOSPC;

	req->log_block = log_block;
	log_block |= req->zone << fbd->block_addr_bits;
	req->phy_block = flash_bd_get_physical(fbd, log_block);
	if (req->phy_block != FLASH_BD_INVALID)
		req->phy_block &= (1 << fbd->block_addr_bits) - 1;

	req->page_off = block_off / fbd->page_size;
	fbd->buf_offset = block_off % fbd->page_size;
	req->page_cnt = 0;

	if (fbd->buf_count > fbd->rem_count)
		fbd->buf_count = fbd->rem_count;

	if (fbd->buf_offset) {
		fbd->buf_count = fbd->page_size - fbd->buf_offset;

		if (fbd->buf_count > fbd->rem_count)
			fbd->buf_count = fbd->rem_count;
	} else
		req->page_cnt = fbd->buf_count / fbd->page_size;

	if (req->phy_block == FLASH_BD_INVALID) {
		if (!req->page_cnt) {
			fbd->buf_offset = 0;
			return h_flash_bd_flush(fbd, req);
		} else {
			req->cmd = FBD_SKIP;
		}
	} else {
		if (!req->page_cnt) {
			req->page_cnt = 1;
			req->cmd = FBD_READ_TMP;
			fbd->buf_offset = block_off % fbd->page_size;
			fbd->cmd_handler = h_flash_bd_flush;
			return 0;
		} else {
			req->cmd = FBD_READ;
		}
	}

	fbd->cmd_handler = h_flash_bd_add_pages;

	return 0;
}

int flash_bd_start_reading(struct flash_bd *fbd, unsigned long long offset,
			   unsigned int count)
{
	if (fbd->active)
		return -EBUSY;

	fbd->active = 1;
	fbd->data_dir = READ;
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

static int h_flash_bd_write(struct flash_bd *fbd,
			    struct flash_bd_request *req)
{
	unsigned long long zone_off = fbd->byte_offset + fbd->t_count;
	unsigned int block_off = do_div(zone_off, fbd->block_size); 
	unsigned int log_block = do_div(zone_off, fbd->log_block_cnt);
	unsigned int block_sz = fbd->block_size - block_off;

	if (block_sz > fbd->rem_count)
		block_sz = fbd->rem_count; 

	req->zone = zone_off;
	if (req->zone >= fbd->zone_cnt)
		return -ENOSPC;

	req->log_block = log_block;
	log_block |= req->zone << fbd->block_addr_bits;
	req->phy_block = flash_bd_get_physical(fbd, log_block);
	if (req->phy_block != FLASH_BD_INVALID)
		req->phy_block &= (1 << fbd->block_addr_bits) - 1;
 
	req->page_off = block_off / fbd->page_size;

	if (!fbd->buf_count) {
		if ((block_off % fbd->page_size)
		    || (block_sz % fbd->page_size)) {
			fbd->buf_offset = block_off;
			fbd->buf_count = block_sz;
			req->cmd = FBD_FILL_TMP;
			req->byte_off = block_off;
			req->byte_cnt = block_sz;
			return 0;
		}
	}

	fbd->buf_offset = block_off;
	fbd->buf_count = fbd->block_size - block_off;

	if (fbd->buf_count > fbd->rem_count)
		fbd->buf_count = fbd->rem_count;

	req->byte_cnt = fbd->buf_count + block_off % fbd->page_size;
	if (req->byte_cnt % fbd->page_size)
		req->page_cnt = req->byte_cnt / fbd->page_size + 1;
	else
		req->page_cnt /= fbd->page_size;

	if ((req->page_cnt * fbd->page_size) > fbd->buf_offset) {
		req->cmd = FBD_FILL_TMP;
		req->byte_cnt = fbd->buf_count;
		
	}

	if (req->dst.phy_block != FLASH_BD_INVALID) {

	} else if (flash_bd_can_merge(fbd, req->phy_block, req->page_off,
				      req->page_cnt)) {
		if ((req->page_cnt * fbd->page_size) > block_sz) {
			/* buffered write */
		} else {
			/* direct write */

		} 
	} else {
		
	}

	if (req->phy_block == FLASH_BD_INVALID) {
		req->phy_block = flash_bd_get_free(fbd, req->zone);
		if (req->phy_block == FLASH_BD_INVALID)
			return -ENOSPC;
		fbd->block_table[log_block] = req->phy_block;
		req->phy_block &= (1 << fbd->block_addr_bits) - 1;
	}

	if (!test_bit(req->phy_block, fbd->erase_map)) {
	}
}

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
	fbd->active = 0;
	return fbd->t_count;
}
EXPORT_SYMBOL(flash_bd_end);

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Simple flash to block device translation layer");
MODULE_LICENSE("GPL");
