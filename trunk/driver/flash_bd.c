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

#include "flash_bd.h"
#include <linux/module.h>

struct block_node {
	struct rb_node node;
	unsigned int   address;
	unsigned long  page_map[];
};

struct flash_bd {
	unsigned int   phy_block_cnt;
	unsigned int   log_block_cnt;
	unsigned int   page_cnt;
	unsigned int   page_size;
	sector_t       size;
	unsigned int   block_ssize;
	unsigned int   free_cnt;
	unsigned int   *block_table;
	unsigned long  *erase_map;
	unsigned long  *data_map;
	struct rb_root useful_blocks;
	struct rb_node *filled_blocks;
	char           *block_buffer;
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

static struct block_node* flash_bd_alloc_useful(struct flash_bd *fbd,
						unsigned int zone,
						unsigned int phy_block)
{
	struct rb_node **p = &(fbd->useful_blocks.rb_node);
	struct rb_node *q;
	struct block_node *rv;

	while (*p) {
		q = *p;
		rv = rb_entry(q, struct block_node, node);

		if (phy_block < rv->address)
			p = &(*p)->rb_left;
		else if (phy_block > rv->address)
			p = &(*p)->rb_right;
		else
			return rv;
	}

	rv = kzalloc(sizeof(struct block_node)
		     + BITS_TO_LONGS(fbd->page_cnt) * sizeof(unsigned long),
		     GFP_KERNEL);
	if (!rv)
		return NULL;

	rv->address = phy_block;
	rb_link_node(&rv->node, q, p);
	rb_insert_color(&rv->node, &fbd->useful_blocks);
	return rv;
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

struct flash_bd* flash_bd_init(unsigned int phy_block_cnt,
			       unsigned int log_block_cnt,
			       unsigned int page_cnt,
			       unsigned int page_size)

{
	unsigned int cnt;
	struct flash_bd *rv = kzalloc(sizeof(struct flash_bd), GFP_KERNEL);
	if (!rv)
		return NULL;


	rv->phy_block_cnt = phy_block_cnt;
	rv->log_block_cnt = log_block_cnt;
	rv->page_cnt = page_cnt;
	rv->page_size = page_size;
	rv->block_ssize = (rv->page_cnt * rv->page_size) / 512;

	rv->block_table = kmalloc(sizeof(unsigned int) * rv->log_block_cnt,
				  GFP_KERNEL);
	if (!rv->block_table)
		goto err_out;

	for (cnt = 0; cnt < rv->log_block_cnt; ++cnt)
		rv->block_table[cnt] = FLASH_BD_INVALID;

	rv->erase_map = kzalloc(BITS_TO_LONGS(rv->phy_block_cnt)
				* sizeof(unsigned long), GFP_KERNEL);
	if (!rv->erase_map)
		goto err_out;

	rv->data_map = kzalloc(BITS_TO_LONGS(rv->phy_block_cnt)
				* sizeof(unsigned long), GFP_KERNEL);
	if (!rv->data_map)
		goto err_out;

	return rv;
err_out:
	flash_bd_destroy(rv);
	return NULL;
}

void flash_bd_destroy(struct flash_bd *fbd)
{
	struct block_node *b;

	if (!fbd)
		return;

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

unsigned int flash_bd_get_logical(struct flash_bd *fbd, unsigned int phy_block)
{
	unsigned int cnt;

	if (phy_block == FLASH_BD_INVALID
	    || phy_block >= fbd->phy_block_cnt)
		return FLASH_BD_INVALID;

	for (cnt = 0; cnt < fbd->log_block_cnt; ++cnt) {
		if (fbd->block_table[cnt] == phy_block)
			return cnt;
	}
	return FLASH_BD_INVALID;
}

unsigned int flash_bd_get_physical(struct flash_bd *fbd, unsigned int log_block)
{
	if (log_block == FLASH_BD_INVALID
	    || log_block >= fbd->log_block_cnt)
		return FLASH_BD_INVALID;

	return fbd->block_table[log_block];
}

int flash_bd_set_empty(struct flash_bd *fbd, unsigned int phy_block, int erased)
{
	unsigned int log_block;

	if (phy_block >= fbd->phy_block_cnt)
		return -EINVAL;

	if (test_bit(phy_block, fbd->data_map)) {
		log_block = flash_bd_get_logical(fbd, phy_block);
		if (log_block != FLASH_BD_INVALID)
			fbd->block_table[log_block] = FLASH_BD_INVALID;
	}

	clear_bit(phy_block, fbd->data_map);

	if (erased)
		set_bit(phy_block, fbd->erase_map);

	flash_bd_erase_useful(fbd, phy_block);
	return 0;
}

int flash_bd_set_full(struct flash_bd *fbd, unsigned int phy_block,
		      unsigned int log_block)
{
	if (phy_block == FLASH_BD_INVALID
	    || phy_block >= fbd->phy_block_cnt)
		return -EINVAL;

	if (log_block == FLASH_BD_INVALID) {
		if (test_bit(phy_block, fbd->data_map)) {
			log_block = flash_bd_get_logical(fbd, phy_block);
			if (log_block != FLASH_BD_INVALID)
				fbd->block_table[log_block] = FLASH_BD_INVALID;
		}
	} else if (log_block < fbd->log_block_cnt) {
		if (test_bit(phy_block, fbd->data_map)
		    && phy_block != fbd->block_table[log_block])
			return -EEXIST;

		fbd->block_table[log_block] = phy_block;
	} else
		return -EINVAL;


	set_bit(phy_block, fbd->data_map);
	clear_bit(phy_block, fbd->erase_map);
	flash_bd_erase_useful(fbd, phy_block);
	return 0;
}

int flash_bd_start_reading(struct flash_bd *fbd, sector_t start,
			   unsigned int count)
{
#warning Ugh!
}

int flash_bd_start_writing(struct flash_bd *fbd, sector_t start,
			   unsigned int count)
{
#warning Ugh!
}

int flash_bd_next_action(struct flash_bd *fbd, struct flash_bd_request *req,
			 int last_error)
{
#warning Ugh!
}

int flash_bd_abort(struct flash_bd *fbd)
{
#warning Ugh!
}

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Simple flash to block device translation layer");
MODULE_LICENSE("GPL");
