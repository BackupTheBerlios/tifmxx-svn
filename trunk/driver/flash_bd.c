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
#include <linux/bitrev.h>
#include <asm/div64.h>

#define READ  0
#define WRITE 1

struct block_node {
	struct rb_node node;
	unsigned int   address;
	unsigned int   page_off;
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
	unsigned int       p_line_size;
	char               *p_line;
};

/* For each block two bits are maintained:
 * Block status   erase_map  data_map
 *  unallocated       0          0
 *  clean             1          0
 *  semi-filled       1          1
 *  full              0          1
 *
 * For semi-filled blocks, useful_blocks contains offsets past last page
 * written.
 */

static int h_flash_bd_read(struct flash_bd *fbd, struct flash_bd_request *req);
static int h_flash_bd_write(struct flash_bd *fbd, struct flash_bd_request *req);
static int h_flash_bd_write_inc(struct flash_bd *fbd,
				struct flash_bd_request *req);
static int h_flash_bd_erase_src(struct flash_bd *fbd,
				struct flash_bd_request *req);
static int h_flash_bd_erase_tmp(struct flash_bd *fbd,
				struct flash_bd_request *req);
static int h_flash_bd_erase_dst(struct flash_bd *fbd,
				struct flash_bd_request *req);


/*** Block map manipulators ***/

static struct block_node* flash_bd_find_useful(struct flash_bd *fbd,
					       unsigned int phy_block)
{
	struct rb_node *n = fbd->useful_blocks.rb_node;
	struct block_node *rv;

	if (phy_block == FLASH_BD_INVALID)
		return NULL;

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

	rv = kzalloc(sizeof(struct block_node), GFP_KERNEL);
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
	b->page_off = 0;

	if (flash_bd_add_useful(fbd, b)) {
		b->node.rb_right = fbd->retired_nodes;
		fbd->retired_nodes = &b->node;
		return -EEXIST;
	}

	fbd->c_block = b;

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
	if (!fbd->c_block || (fbd->c_block->address != phy_block)) {
		fbd->c_block = flash_bd_find_useful(fbd, phy_block);

		if (!fbd->c_block)
			return;
	}

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

static void flash_bd_mark_pages(struct flash_bd *fbd, unsigned int phy_block,
				unsigned int page_cnt)
{
	if (phy_block == FLASH_BD_INVALID)
		return;

	if (!fbd->c_block || fbd->c_block->address != phy_block)
		fbd->c_block = flash_bd_find_useful(fbd, phy_block);

	if (!fbd->c_block) {
		clear_bit(phy_block, fbd->erase_map);
		return;
	}

	fbd->c_block->page_off += page_cnt;
	if (fbd->c_block->page_off >= fbd->page_cnt) {
		clear_bit(phy_block, fbd->erase_map);
		flash_bd_retire_useful(fbd, phy_block);
	}
}

static unsigned int flash_bd_clean_pages(struct flash_bd *fbd,
					 unsigned int phy_block)
{
	if (phy_block == FLASH_BD_INVALID)
		return 0;

	if (!fbd->c_block || fbd->c_block->address != phy_block)
		fbd->c_block = flash_bd_find_useful(fbd, phy_block);

	if (!fbd->c_block) {
		if (test_bit(phy_block, fbd->erase_map))
			return fbd->page_cnt;
		else
			return 0;
	}

	return fbd->page_cnt - fbd->c_block->page_off;
}

static int flash_bd_can_merge(struct flash_bd *fbd, unsigned int phy_block,
			      unsigned int page)
{
	if (phy_block == FLASH_BD_INVALID)
		return -1;

	if (!test_bit(phy_block, fbd->data_map)
	    || !test_bit(phy_block, fbd->erase_map))
			return -1;

	if (!fbd->c_block || fbd->c_block->address != phy_block)
		fbd->c_block = flash_bd_find_useful(fbd, phy_block);

	if (!fbd->c_block)
		return -1;

	if (page >= fbd->c_block->page_off)
		return page - fbd->c_block->page_off;
	else
		return -1;
}

/*** Management functions ***/

struct flash_bd* flash_bd_init(unsigned int zone_cnt,
			       unsigned int phy_block_cnt,
			       unsigned int log_block_cnt,
			       unsigned int page_cnt,
			       unsigned int page_size)

{
	unsigned int cnt;
	struct flash_bd *fbd = kzalloc(sizeof(struct flash_bd), GFP_KERNEL);

	if (!fbd)
		return NULL;

	fbd->zone_cnt = zone_cnt;
	fbd->phy_block_cnt = phy_block_cnt;
	fbd->log_block_cnt = log_block_cnt;
	fbd->page_cnt = page_cnt;
	fbd->page_size = page_size;
	fbd->block_size = page_size * page_cnt;

	fbd->block_addr_bits = fls(fbd->phy_block_cnt - 1);
	if ((1 << fbd->block_addr_bits) < fbd->phy_block_cnt)
		fbd->block_addr_bits++;

	fbd->p_line_size = fls(page_cnt - 1);
	if ((1 << fbd->p_line_size) < page_cnt)
		fbd->p_line_size++;

	fbd->p_line_size = (fbd->p_line_size + 3) / 4;
	cnt = (fbd->block_addr_bits + 3) / 4;

	if (fbd->zone_cnt < 2)
		/* <log_block> <phy_block> <mode> <page_off> */
		fbd->p_line_size += 2 * cnt + 7;
	else {
		/* <zone>: <log_block> <phy_block> <mode> <page_off> */
		fbd->p_line_size += 2 * cnt;
		cnt = fls(fbd->zone_cnt - 1);
		if ((1 << cnt) < fbd->zone_cnt)
			cnt++;

		cnt = (cnt + 3) / 4;
		fbd->p_line_size += cnt + 9;
	}

	fbd->p_line = kmalloc(fbd->p_line_size, GFP_KERNEL);
	if (!fbd->p_line)
		goto err_out;

	fbd->free_cnt = kmalloc(sizeof(unsigned int) * fbd->zone_cnt,
				GFP_KERNEL);
	if (!fbd->free_cnt)
		goto err_out;

	for (cnt = 0; cnt < fbd->zone_cnt; ++cnt)
		fbd->free_cnt[cnt] = fbd->phy_block_cnt;

	fbd->block_table = kmalloc(sizeof(unsigned int)
				   * (fbd->zone_cnt << fbd->block_addr_bits),
				   GFP_KERNEL);
	if (!fbd->block_table)
		goto err_out;

	for (cnt = 0; cnt < (fbd->zone_cnt << fbd->block_addr_bits); ++cnt)
		fbd->block_table[cnt] = FLASH_BD_INVALID;

	fbd->erase_map = kzalloc(BITS_TO_LONGS(fbd->zone_cnt
					       << fbd->block_addr_bits)
				 * sizeof(unsigned long), GFP_KERNEL);
	if (!fbd->erase_map)
		goto err_out;

	fbd->data_map = kzalloc(BITS_TO_LONGS(fbd->zone_cnt
					      << fbd->block_addr_bits)
				* sizeof(unsigned long), GFP_KERNEL);
	if (!fbd->data_map)
		goto err_out;

	return fbd;

err_out:
	flash_bd_destroy(fbd);
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

	kfree(fbd->p_line);
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

unsigned int flash_bd_get_physical(struct flash_bd *fbd, unsigned int zone,
				   unsigned int log_block)
{
	if (log_block == FLASH_BD_INVALID
	    || log_block >= fbd->log_block_cnt)
		return FLASH_BD_INVALID;

	log_block |= zone << fbd->block_addr_bits;

	return fbd->block_table[log_block] == FLASH_BD_INVALID
	       ? FLASH_BD_INVALID
	       : (fbd->block_table[log_block]
		  & ((1 << fbd->block_addr_bits) - 1));
}
EXPORT_SYMBOL(flash_bd_get_physical);

int flash_bd_set_empty(struct flash_bd *fbd, unsigned int zone,
		       unsigned int phy_block, int erased)
{
	unsigned int log_block;

	if (phy_block == FLASH_BD_INVALID
	    || phy_block >= fbd->phy_block_cnt)
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
		if (fbd->free_cnt[zone])
			fbd->free_cnt[zone]--;
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
	if (!fbd->cmd_handler)
		return fbd->last_error;

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
		p = fbd->retired_nodes->rb_right->rb_right;
		fbd->retired_nodes->rb_right->rb_right = NULL;
		while (p) {
			q = p->rb_right;
			kfree(rb_entry(p, struct block_node, node));
			p = q;
		}
	}

	return fbd->t_count;
}
EXPORT_SYMBOL(flash_bd_end);

static void flash_bd_print_line(struct flash_bd *fbd, unsigned int log_block)
{
	unsigned int zone = log_block / fbd->log_block_cnt;
	unsigned int phy_block;
	unsigned int line_pos = 0, b_fmt;
	struct block_node *b;
	char s_fmt[] = "%08x";

	log_block = log_block % fbd->log_block_cnt;
	if (zone >= fbd->zone_cnt)
		goto fill_out;

	phy_block = flash_bd_get_physical(fbd, zone, log_block);

	if (fbd->zone_cnt > 1) {
		b_fmt = fls(fbd->zone_cnt - 1);
		if ((1 << b_fmt) < fbd->zone_cnt)
			b_fmt++;

		b_fmt = (b_fmt + 3) / 4;
		s_fmt[2] = b_fmt | 0x30;
	
		line_pos += scnprintf(fbd->p_line + line_pos,
				      fbd->p_line_size - line_pos,
				      s_fmt, zone);
		line_pos += scnprintf(fbd->p_line + line_pos,
				      fbd->p_line_size - line_pos,
				      ": ");
	}

	b_fmt = (fbd->block_addr_bits + 3) / 4;
	s_fmt[2] = b_fmt | 0x30;
	line_pos += scnprintf(fbd->p_line + line_pos,
			      fbd->p_line_size - line_pos,
			      s_fmt, log_block
				     & ((1 << fbd->block_addr_bits) - 1));
	line_pos += scnprintf(fbd->p_line + line_pos,
			      fbd->p_line_size - line_pos,
			      " ");

	if (phy_block == FLASH_BD_INVALID) {
		line_pos += scnprintf(fbd->p_line + line_pos,
				      fbd->p_line_size - line_pos,
				      "--");
		goto fill_out;
	}

	line_pos += scnprintf(fbd->p_line + line_pos,
			      fbd->p_line_size - line_pos, s_fmt, phy_block);

	b_fmt = 0;

	phy_block |= zone << fbd->block_addr_bits;

	if (test_bit(phy_block, fbd->data_map))
		b_fmt = 1;

	if (test_bit(phy_block, fbd->erase_map))
		b_fmt |= 2;

	switch (b_fmt) {
	case 0:
		line_pos += scnprintf(fbd->p_line + line_pos,
				      fbd->p_line_size - line_pos,
				      " U");
		break;
	case 1:
		line_pos += scnprintf(fbd->p_line + line_pos,
				      fbd->p_line_size - line_pos,
				      " F");
		break;
	case 2:
		line_pos += scnprintf(fbd->p_line + line_pos,
				      fbd->p_line_size - line_pos,
				      " C");
		break;
	case 3:
		line_pos += scnprintf(fbd->p_line + line_pos,
				      fbd->p_line_size - line_pos,
				      " S");
		break;
	}

	b = flash_bd_find_useful(fbd, phy_block);
	if (b) {
		b_fmt = fls(fbd->page_cnt - 1);
		if ((1 << b_fmt) < fbd->zone_cnt)
			b_fmt++;

		b_fmt = (b_fmt + 3) / 4;
		s_fmt[2] = b_fmt | 0x30;

		line_pos += scnprintf(fbd->p_line + line_pos,
				      fbd->p_line_size - line_pos,
				      " ");
		line_pos += scnprintf(fbd->p_line + line_pos,
				      fbd->p_line_size - line_pos,
				      s_fmt, b->page_off);
	}

fill_out:
	memset(fbd->p_line + line_pos, ' ', fbd->p_line_size - line_pos);
	fbd->p_line[fbd->p_line_size - 1] = '\n';
}

/**
 * flash_bd_map_size - calculate size of the complete block map text dump
 * Return number of characters needed.
 * fbd: owner of the map
 */
size_t flash_bd_map_size(struct flash_bd *fbd)
{
	return fbd->p_line_size * fbd->zone_cnt * fbd->log_block_cnt;
}
EXPORT_SYMBOL(flash_bd_map_size);

/**
 * flash_bd_read_map - read current block map as text.
 * Return number of characters written into buffer.
 * fbd: owner of the map
 * buf: where to put the printout
 * offset: offset into the text representation of the map
 * count: size of the buffer
 */
ssize_t flash_bd_read_map(struct flash_bd *fbd, char *buf, loff_t offset,
			  size_t count)
{
	unsigned int l_begin = offset / fbd->p_line_size;
	unsigned int l_end = (offset + count - 1) / fbd->p_line_size;
	ssize_t rc = 0;

	if (!count)
		return 0;

	flash_bd_print_line(fbd, l_begin);

	if (l_begin != l_end) {
		rc = offset % fbd->p_line_size;
		memcpy(buf, fbd->p_line + rc,
		       fbd->p_line_size - rc);
		rc = fbd->p_line_size - rc;
	} else {
		memcpy(buf, fbd->p_line + (offset % fbd->p_line_size), count);
		return count;
	}

	for (++l_begin; l_begin < l_end; ++l_begin) {
		flash_bd_print_line(fbd, l_begin);
		memcpy(buf + rc, fbd->p_line, fbd->p_line_size);
		rc += fbd->p_line_size;
	}

	flash_bd_print_line(fbd, l_end);
	memcpy(buf + rc, fbd->p_line, count - rc);

	return count;
}
EXPORT_SYMBOL(flash_bd_read_map);

/*** Protocol processing ***/

static unsigned int flash_bd_get_free(struct flash_bd *fbd, unsigned int zone)
{
	unsigned long r_pos;
	unsigned long pos = zone << fbd->block_addr_bits; 

	if (!fbd->free_cnt[zone])
		return FLASH_BD_INVALID;

	r_pos = random32() % fbd->free_cnt[zone];

	while (1) {
		pos = find_next_zero_bit(fbd->data_map,
					 fbd->phy_block_cnt * fbd->zone_cnt,
					 pos);
		if (pos >= ((zone + 1) << fbd->block_addr_bits))
			return FLASH_BD_INVALID;

		if (!r_pos)
			break;

		r_pos--;
		pos++;
	};

	set_bit(pos, fbd->data_map);
	if (fbd->free_cnt[zone])
		fbd->free_cnt[zone]--;

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
	req->byte_off = fbd->buf_offset - (fbd->buf_page_off * fbd->page_size);
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

	fbd->buf_page_off = fbd->buf_offset / fbd->page_size;
	fbd->buf_page_cnt = ((fbd->buf_offset + fbd->buf_count)
			     / fbd->page_size) - fbd->buf_page_off;
	if ((fbd->buf_offset + fbd->buf_count) % fbd->page_size)
		fbd->buf_page_cnt++;

	req->log_block = log_block;

	req->phy_block = flash_bd_get_physical(fbd, zone_off, log_block);

	if (req->phy_block == FLASH_BD_INVALID) {
		req->cmd = FBD_SKIP;
		req->byte_off = 0;
		req->byte_cnt = fbd->buf_count;
		fbd->req_count = fbd->buf_count;
		fbd->cmd_handler = h_flash_bd_read;
		return 0;
	}

	req->page_off = fbd->buf_page_off;
	req->page_cnt = fbd->buf_page_cnt;
	fbd->req_count = fbd->buf_page_cnt * fbd->page_size;

	if ((fbd->buf_offset % fbd->page_size)
	    || (fbd->buf_count % fbd->page_size)) {
		req->cmd = FBD_READ_TMP;
		fbd->cmd_handler = h_flash_bd_read_tmp_r;
	} else {
		req->cmd = FBD_READ;
		fbd->cmd_handler = h_flash_bd_read;
	}

	return 0;
}

static int h_flash_bd_mark_dst_bad(struct flash_bd *fbd,
				   struct flash_bd_request *req)
{
	unsigned int zone = fbd->w_dst_block >>  fbd->block_addr_bits;

	fbd->w_dst_block = flash_bd_get_free(fbd, zone);
	if (fbd->w_dst_block == FLASH_BD_INVALID)
		return -EFAULT;

	req->phy_block = fbd->w_dst_block
			 & ((1 << fbd->block_addr_bits) - 1);

	if (!test_bit(fbd->w_dst_block, fbd->erase_map)) {
		req->cmd = FBD_ERASE;
		fbd->req_count = 0;
		fbd->cmd_handler = h_flash_bd_erase_dst;
		return 0;
	} else
		return h_flash_bd_erase_dst(fbd, req);
}

static int h_flash_bd_copy_last(struct flash_bd *fbd,
				struct flash_bd_request *req)
{

	if ((fbd->req_count != fbd->last_count)
	    && !fbd->last_error)
		fbd->last_error = -EIO;

	if (!fbd->last_error) {
		flash_bd_mark_pages(fbd, fbd->w_dst_block,
				    fbd->req_count / fbd->page_size);

		req->cmd = FBD_ERASE;
		req->phy_block = fbd->w_src_block
				 & ((1 <<  fbd->block_addr_bits) - 1);
		req->page_off = 0;
		req->page_cnt = 0;
		fbd->req_count = 0;
		fbd->cmd_handler = h_flash_bd_erase_src;
		return 0;
	} else {
		fbd->block_table[fbd->w_log_block] = fbd->w_src_block;
		fbd->t_count -= fbd->buf_count;
		fbd->rem_count += fbd->buf_count;

		if (fbd->last_error == -EFAULT) {
			flash_bd_mark_used(fbd, fbd->w_dst_block);
			req->cmd = FBD_MARK_BAD;
			req->page_off = 0;
			req->page_cnt = 1;
			fbd->req_count = 0;
			fbd->cmd_handler = h_flash_bd_mark_dst_bad;
			return 0;
		} else
			return fbd->last_error;
	}
}

static int h_flash_bd_copy_first(struct flash_bd *fbd,
				 struct flash_bd_request *req)
{
	req->zone = fbd->w_log_block >> fbd->block_addr_bits;
	req->log_block = fbd->w_log_block & ((1 <<  fbd->block_addr_bits) - 1);
	req->phy_block = fbd->w_dst_block & ((1 <<  fbd->block_addr_bits) - 1);

	if (!fbd->last_error) {
		if (fbd->req_count != fbd->last_count)
			return -EIO;
	} else if (fbd->last_error == -EFAULT) {
		flash_bd_mark_used(fbd, fbd->w_dst_block);

		req->cmd = FBD_MARK_BAD;
		req->page_off = 0;
		req->page_cnt = 1;
		fbd->req_count = 0;
		fbd->cmd_handler = NULL;
		return 0;
	} else
		return fbd->last_error;

	flash_bd_mark_pages(fbd, fbd->w_dst_block, fbd->buf_page_off);

	req->page_off = fbd->buf_page_off;
	req->page_cnt = fbd->buf_page_cnt;
	fbd->req_count = req->page_cnt * fbd->page_size;

	if ((fbd->buf_offset % fbd->page_size)
	    || (fbd->buf_count % fbd->page_size))
		req->cmd = FBD_WRITE_TMP;
	else
		req->cmd = FBD_WRITE;

	fbd->cmd_handler = h_flash_bd_write_inc;
	return 0;
}

static int h_flash_bd_mark(struct flash_bd *fbd,
			   struct flash_bd_request *req)
{
	req->zone = fbd->w_log_block >> fbd->block_addr_bits;
	req->log_block = fbd->w_log_block & ((1 <<  fbd->block_addr_bits) - 1);

	if (!fbd->last_error) {
		if (fbd->req_count != fbd->last_count)
			return -EIO;
	} else if (fbd->last_error == -EFAULT) {
		flash_bd_mark_used(fbd, fbd->w_dst_block);
		req->phy_block = fbd->w_dst_block
				 & ((1 <<  fbd->block_addr_bits) - 1);
		req->cmd = FBD_MARK_BAD;
		req->page_off = 0;
		req->page_cnt = 1;
		fbd->req_count = 0;
		fbd->cmd_handler = NULL;
		return 0;
	} else
		return fbd->last_error;

	flash_bd_mark_pages(fbd, fbd->w_dst_block,
			    flash_bd_can_merge(fbd, fbd->w_dst_block,
					       fbd->buf_page_off));

	req->phy_block = fbd->w_dst_block;
	req->phy_block &= (1 << fbd->block_addr_bits) - 1;
	req->page_off = fbd->buf_page_off;
	req->page_cnt = fbd->buf_page_cnt;
	fbd->req_count = fbd->buf_page_cnt * fbd->page_size;
	fbd->cmd_handler = h_flash_bd_write_inc;

	if ((fbd->buf_offset % fbd->page_size)
	    || (fbd->buf_count % fbd->page_size))
		req->cmd = FBD_WRITE_TMP;
	else
		req->cmd = FBD_WRITE;

	return 0;
}

static int h_flash_bd_erase_dst(struct flash_bd *fbd,
				struct flash_bd_request *req)
{
	unsigned int zone = fbd->w_dst_block >>  fbd->block_addr_bits;
	unsigned int c_pages = fbd->page_cnt - fbd->buf_page_off
			       - fbd->buf_page_cnt;

	req->zone = zone;
	req->log_block = fbd->w_log_block
			 & ((1 << fbd->block_addr_bits) - 1);
	req->phy_block = fbd->w_dst_block
			 & ((1 << fbd->block_addr_bits) - 1);
	req->src.phy_block = fbd->w_src_block
			     & ((1 << fbd->block_addr_bits) - 1);

	if (fbd->last_error) {
		if (fbd->last_error == -EFAULT) {
			req->cmd = FBD_MARK_BAD;
			req->page_off = 0;
			req->page_cnt = 1;
			fbd->req_count = 0;
			fbd->cmd_handler = h_flash_bd_mark_dst_bad;
			return 0;
		} else
			return fbd->last_error;
	}

	if ((fbd->w_src_block != fbd->w_dst_block)
	    || (fbd->buf_page_cnt == fbd->page_cnt)) {

		if (fbd->w_src_block != FLASH_BD_INVALID)
			c_pages = min(flash_bd_clean_pages(fbd,
							   fbd->w_src_block),
				      c_pages);

		if (c_pages) {
			set_bit(fbd->w_dst_block, fbd->erase_map);
			flash_bd_insert_useful(fbd, fbd->w_dst_block);
		}

		if (fbd->buf_page_off) {
			req->page_off = 0;
			req->page_cnt = fbd->buf_page_off;
			fbd->req_count = req->page_cnt * fbd->page_size;
			if (fbd->w_src_block == FLASH_BD_INVALID) {
				req->cmd = FBD_MARK;
				fbd->cmd_handler = h_flash_bd_mark;
			} else {
				req->src.page_off = 0;
				req->cmd = FBD_COPY;
				fbd->cmd_handler = h_flash_bd_copy_first;
			}
		} else {
			req->page_off = fbd->buf_page_off;
			req->page_cnt = fbd->buf_page_cnt;
			fbd->req_count = req->page_cnt * fbd->page_size;

			if ((fbd->buf_offset % fbd->page_size)
			    || (fbd->buf_count % fbd->page_size))
				req->cmd = FBD_WRITE_TMP;
			else
				req->cmd = FBD_WRITE;

			fbd->cmd_handler = h_flash_bd_write_inc;
		}
	} else {
		req->cmd = FBD_WRITE_TMP;
		req->page_off = 0;
		req->page_cnt = fbd->page_cnt;
		fbd->req_count = fbd->block_size;
		fbd->cmd_handler = h_flash_bd_write_inc;
	}

	return 0;
}

static int h_flash_bd_mark_src_bad(struct flash_bd *fbd,
				   struct flash_bd_request *req)
{
	if (!fbd->rem_count) {
		req->cmd = FBD_NONE;
		return 0;
	} else
		return h_flash_bd_write(fbd, req);
}

static int h_flash_bd_erase_src(struct flash_bd *fbd,
				struct flash_bd_request *req)
{
	unsigned int zone = fbd->w_dst_block >>  fbd->block_addr_bits;

	req->zone = zone;
	req->log_block = fbd->w_log_block
			 & ((1 << fbd->block_addr_bits) - 1);
	req->phy_block = fbd->w_src_block
			 & ((1 << fbd->block_addr_bits) - 1);

	flash_bd_retire_useful(fbd, fbd->w_src_block);
	flash_bd_mark_erased(fbd, fbd->w_src_block);

	if (fbd->last_error) {
		if (fbd->last_error == -EFAULT) {
			flash_bd_mark_used(fbd, fbd->w_src_block);
			req->cmd = FBD_MARK_BAD;
			req->page_off = 0;
			req->page_cnt = 1;
			fbd->req_count = 0;
			fbd->cmd_handler = h_flash_bd_mark_src_bad;
			return 0;
		} else
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
	unsigned int c_pages;

	req->zone = fbd->w_log_block >> fbd->block_addr_bits;
	req->log_block = fbd->w_log_block & ((1 <<  fbd->block_addr_bits) - 1);

	if (!fbd->last_error) {
		if (fbd->req_count != fbd->last_count)
			return -EIO;
	} else if (fbd->last_error == -EFAULT) {
		flash_bd_mark_used(fbd, fbd->w_dst_block);
		req->phy_block = fbd->w_dst_block
				 & ((1 <<  fbd->block_addr_bits) - 1);
		req->cmd = FBD_MARK_BAD;
		req->page_off = 0;
		req->page_cnt = 1;
		fbd->req_count = 0;
		fbd->cmd_handler = NULL;
		return 0;
	} else
		return fbd->last_error;

	fbd->block_table[fbd->w_log_block] = fbd->w_dst_block;
	fbd->t_count += fbd->buf_count;
	fbd->rem_count -= fbd->buf_count;

	flash_bd_mark_pages(fbd, fbd->w_dst_block, fbd->buf_page_cnt);

	if ((fbd->w_src_block != FLASH_BD_INVALID)
	    && (fbd->w_src_block != fbd->w_dst_block)) {
		c_pages = flash_bd_clean_pages(fbd, fbd->w_dst_block);

		if (!c_pages) {
			clear_bit(fbd->w_src_block, fbd->erase_map);
			req->cmd = FBD_ERASE;
			req->phy_block = fbd->w_src_block
					 & ((1 <<  fbd->block_addr_bits) - 1);
			req->page_off = 0;
			req->page_cnt = 0;
			fbd->req_count = 0;
			fbd->cmd_handler = h_flash_bd_erase_src;
			return 0;
		} else {
			req->cmd = FBD_COPY;
			req->phy_block = fbd->w_dst_block
					 & ((1 <<  fbd->block_addr_bits) - 1);
			req->src.phy_block = fbd->w_src_block
					     & ((1 <<  fbd->block_addr_bits)
						- 1);
			req->page_off = fbd->buf_page_off + fbd->buf_page_cnt;
			req->src.page_off = req->page_off;
			req->page_cnt = flash_bd_clean_pages(fbd,
							     fbd->w_src_block);
			if (req->page_cnt < c_pages) {
				req->page_cnt = c_pages - req->page_cnt;
				fbd->req_count = req->page_cnt * fbd->page_size;
				fbd->cmd_handler = h_flash_bd_copy_last;
				return 0;
			}
		}
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
	int merge_pos;

	if (fbd->last_error)
		return fbd->last_error;

	if (fbd->req_count != fbd->last_count)
		return -EIO;

	req->zone = fbd->w_log_block >> fbd->block_addr_bits;
	req->log_block = fbd->w_log_block & ((1 <<  fbd->block_addr_bits) - 1);
	req->phy_block = fbd->w_dst_block & ((1 <<  fbd->block_addr_bits) - 1);

	merge_pos = flash_bd_can_merge(fbd, fbd->w_dst_block,
				       fbd->buf_page_off);

	if (merge_pos < 0) {
		if (test_bit(fbd->w_dst_block, fbd->erase_map)) {
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
	} else if (!merge_pos) {
		req->cmd = FBD_WRITE_TMP;
		req->page_off = fbd->buf_page_off;
		req->page_cnt = fbd->buf_page_cnt;
		fbd->req_count = req->page_cnt * fbd->page_size;
		fbd->cmd_handler = h_flash_bd_write_inc;
	} else {
		req->cmd = FBD_MARK;
		req->page_off = fbd->buf_page_off - merge_pos;
		req->page_cnt = merge_pos;
		fbd->req_count = req->page_cnt * fbd->page_size;
		fbd->cmd_handler = h_flash_bd_mark;
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
	req->byte_off = fbd->buf_offset - (fbd->buf_page_off * fbd->page_size);
	req->byte_cnt = fbd->buf_count;
	fbd->req_count = fbd->buf_count;
	fbd->cmd_handler = h_flash_bd_fill_tmp;
	return 0;
}

static int h_flash_bd_erase_tmp(struct flash_bd *fbd,
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
	req->byte_off = fbd->buf_offset - (fbd->buf_page_off * fbd->page_size);
	req->byte_cnt = fbd->buf_count;
	fbd->req_count = fbd->buf_count;
	fbd->cmd_handler = h_flash_bd_fill_tmp;

	return 0;
}

static int h_flash_bd_write(struct flash_bd *fbd,
			    struct flash_bd_request *req)
{
	unsigned long long zone_off;
	int merge_pos;

	if (fbd->last_error)
		return fbd->last_error;

	zone_off = fbd->byte_offset + fbd->t_count;
	fbd->buf_offset = do_div(zone_off, fbd->block_size);
	fbd->buf_count = min(fbd->rem_count, fbd->block_size - fbd->buf_offset);
	req->log_block = do_div(zone_off, fbd->log_block_cnt);

	fbd->buf_page_off = fbd->buf_offset / fbd->page_size;
	fbd->buf_page_cnt = ((fbd->buf_offset + fbd->buf_count)
			     / fbd->page_size) - fbd->buf_page_off;
	if ((fbd->buf_offset + fbd->buf_count) % fbd->page_size)
		fbd->buf_page_cnt++;

	req->zone = zone_off;

	if (req->zone >= fbd->zone_cnt)
		return -ENOSPC;

	fbd->w_src_block = flash_bd_get_physical(fbd, zone_off, req->log_block);
	fbd->w_log_block = req->log_block | req->zone << fbd->block_addr_bits;
	if (fbd->w_src_block != FLASH_BD_INVALID)
		fbd->w_src_block |= req->zone << fbd->block_addr_bits;
	merge_pos = flash_bd_can_merge(fbd, fbd->w_src_block,
				       fbd->buf_page_off);

	if (merge_pos < 0) {
		/* Pages must be copied or erased before writing */
		fbd->w_dst_block = flash_bd_get_free(fbd, zone_off);
		if (fbd->w_dst_block == FLASH_BD_INVALID)
			fbd->w_dst_block = fbd->w_src_block;
		if (fbd->w_dst_block == FLASH_BD_INVALID)
			return -EIO;

		if ((fbd->w_dst_block != fbd->w_src_block)
		    || (fbd->buf_count == fbd->block_size)) {

			if ((fbd->buf_offset % fbd->page_size)
			    || (fbd->buf_count % fbd->page_size)) {
				req->cmd = FBD_READ_TMP;
				req->phy_block = fbd->w_src_block;
				req->phy_block &= (1 << fbd->block_addr_bits)
						  - 1;
				req->page_off = fbd->buf_page_off;
				req->page_cnt = fbd->buf_page_cnt;
				fbd->req_count = fbd->buf_page_cnt
						 * fbd->page_size;
				fbd->cmd_handler = h_flash_bd_read_tmp_w;
			} else {
				req->phy_block = fbd->w_dst_block;
				req->phy_block &= (1 << fbd->block_addr_bits)
						  - 1;
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
			fbd->buf_page_off = 0;
			fbd->buf_page_cnt = fbd->page_cnt;
			req->cmd = FBD_READ_TMP;
			req->phy_block = fbd->w_src_block;
			req->phy_block &= (1 << fbd->block_addr_bits) - 1;
			req->page_off = 0;
			req->page_cnt = fbd->page_cnt;
			fbd->req_count = fbd->block_size;
			fbd->cmd_handler = h_flash_bd_read_tmp_w;
		}
	} else {
		fbd->w_dst_block = fbd->w_src_block;

		req->phy_block = fbd->w_dst_block;
		req->phy_block &= (1 << fbd->block_addr_bits) - 1;

		if ((fbd->buf_offset % fbd->page_size)
		    || (fbd->buf_count % fbd->page_size)) {
			req->cmd = FBD_ERASE_TMP;
			req->phy_block = FLASH_BD_INVALID;
			req->byte_off = 0;
			req->byte_cnt = fbd->buf_page_cnt * fbd->page_size;
			fbd->req_count = req->byte_cnt;
			fbd->cmd_handler = h_flash_bd_erase_tmp;
		} else {
			if (!merge_pos) {
				req->cmd = FBD_WRITE;
				req->page_off = fbd->buf_page_off;
				req->page_cnt = fbd->buf_page_cnt;
				fbd->req_count = fbd->buf_page_cnt
						 * fbd->page_size;
				fbd->cmd_handler = h_flash_bd_write_inc;
			} else {
				req->cmd = FBD_MARK;
				req->page_off = fbd->buf_page_off - merge_pos;
				req->page_cnt = merge_pos;
				fbd->req_count = 0;
				fbd->cmd_handler = h_flash_bd_mark;
			}
		}
	}

	return 0;
}

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Simple flash to block device translation layer");
MODULE_LICENSE("GPL");
