/*
 *  MTDX simple, dual-mode free page map
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/bitmap.h>
#include <linux/rbtree.h>
//#include <linux/spinlock.h>
#include "block_map.h"

struct block_node {
	struct rb_node node;
	unsigned int   address;
};

struct block_node_cnt {
	struct block_node node;
	unsigned int      count;
};

struct block_node_map {
	struct block_node node;
	unsigned long     map[];
};

struct mtdx_block_map {
	unsigned int       page_cnt;
	struct rb_root     useful_blocks;
	struct rb_node     *retired_nodes;
	struct block_node  *c_block;
	unsigned int       target_count;
	unsigned int       rnode_count;
	spinlock_t         lock;
	struct block_node  *(*block_alloc)(struct mtdx_block_map *map);
	void               (*block_free)(struct block_node *b);
	int                (*check_range)(struct mtdx_block_map *map,
					  unsigned int address,
					  unsigned int offset,
					  unsigned int count);
	int                (*set_range)(struct mtdx_block_map *map,
				        unsigned int address,
				        unsigned int offset,
				        unsigned int count);
	struct work_struct allocator_work;
};


static struct block_node* mtdx_block_map_find_useful(struct mtdx_block_map *map,
						     unsigned int address)
{
	struct rb_node *n;
	struct block_node *rv;

	if (map->c_block && (address == map->c_block->address))
		return map->c_block;

	n = map->useful_blocks.rb_node;

	while (n) {
		rv = rb_entry(n, struct block_node, node);

		if (address < rv->address)
			n = n->rb_left;
		else if (address > rv->address)
			n = n->rb_right;
		else
			return rv;
	}

	return NULL;
}

static int mtdx_block_map_add_useful(struct mtdx_block_map *map,
				     struct block_node *b)
{
	struct rb_node **p = &(map->useful_blocks.rb_node);
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
	rb_insert_color(&b->node, &map->useful_blocks);
	return 0;
}

static struct block_node *mtdx_block_map_get_node(struct mtdx_block_map *map)
{
	struct block_node *b;

	if (map->rnode_count < (map->target_count / 2))
		schedule_work(&map->allocator_work);

	if (!map->retired_nodes)
		return NULL;

	b = rb_entry(map->retired_nodes, struct block_node, node);

	map->retired_nodes = b->node.rb_right;
	b->node.rb_right = NULL;
	b->node.rb_left = NULL;
	map->rnode_count--;

	return b;
}

static void mtdx_block_map_put_node(struct mtdx_block_map *map,
				    struct block_node *b)
{
	if (map->c_block == b)
		map->c_block = NULL;

	rb_erase(&b->node, &map->useful_blocks);
	b->node.rb_left = NULL;
	b->node.rb_right = map->retired_nodes;
	map->retired_nodes = &b->node;
	map->rnode_count++;

	if (map->rnode_count > (2 * map->target_count))
		schedule_work(&map->allocator_work);
}

static void mtdx_block_map_free_tree(struct mtdx_block_map *map)
{
	struct rb_node *p, *q;
	struct block_node *b;

	for (p = map->useful_blocks.rb_node; p; p = q) {
		if (!p->rb_left) {
			q = p->rb_right;
			b = rb_entry(p, struct block_node, node);
			map->block_free(b);
		} else {
			q = p->rb_left;
			p->rb_left = q->rb_right;
			q->rb_right = p;
		}
	}
	map->useful_blocks.rb_node = NULL;
}

static void mtdx_block_map_async_alloc(struct work_struct *work)
{
	struct mtdx_block_map *map = container_of(work, struct mtdx_block_map,
						  allocator_work);
	struct block_node *b;
	struct rb_node *h = NULL;
	unsigned int cnt = 0;
	unsigned long flags;

	spin_lock_irqsave(&map->lock, flags);
	while (map->rnode_count > map->target_count) {
		b = rb_entry(map->retired_nodes, struct block_node,
			     node);
		map->retired_nodes = b->node.rb_right;
		b->node.rb_right = h;
		b->node.rb_left = NULL;
		h = &b->node;
		map->rnode_count--;
	};

	if (map->rnode_count < map->target_count)
		cnt = map->target_count - map->rnode_count;
	spin_unlock_irqrestore(&map->lock, flags);

	while (h) {
		b = rb_entry(h, struct block_node, node);
		h = b->node.rb_right;
		map->block_free(b);
	}

	while (cnt) {
		b = map->block_alloc(map);
		if (!b)
			break;
		b->node.rb_right = h;
		h = &b->node;
		cnt--;
	}

	if (h) {
		spin_lock_irqsave(&map->lock, flags);
		while (h) {
			b = rb_entry(h, struct block_node, node);
			h = b->node.rb_right;
			b->node.rb_right = map->retired_nodes;
			map->retired_nodes = &b->node;
		}
		spin_unlock_irqrestore(&map->lock, flags);
	}
}

static struct block_node *mtdx_block_map_alloc_rnd(struct mtdx_block_map *map)
{
	return kzalloc(sizeof(struct block_node_map)
		       + BITS_TO_LONGS(map->page_cnt) * sizeof(unsigned long),
		       GFP_KERNEL);
}

static void mtdx_block_map_free_rnd(struct block_node *b)
{
	if (b)
		kfree(container_of(b, struct block_node_map, node));
}

static int mtdx_block_map_check_rnd(struct mtdx_block_map *map,
				    unsigned int address,
				    unsigned int offset,
				    unsigned int count)
{
	struct block_node *b = mtdx_block_map_find_useful(map, address);
	struct block_node_map *bm;
	unsigned int pos;

	if (!b)
		return 0;

	bm = container_of(b, struct block_node_map, node);

	pos = find_next_zero_bit(bm->map, map->page_cnt, offset);
	if (pos != offset)
		return 0;

	pos = find_next_bit(bm->map, map->page_cnt, offset);
	if (pos < offset + count)
		return 0;

	return 1;
}

static void bitmap_set_bits(unsigned long *bitmap, unsigned int offset,
			    unsigned int count)
{
	unsigned long p1 = BIT_WORD(offset);
	unsigned long p2 = BIT_WORD(offset + count - 1);
	unsigned long mb = (~0UL) << (offset % BITS_PER_LONG);
	unsigned long me = BIT_MASK(offset + count) - 1;

	if (p1 == p2)
		bitmap[p1] |= ~(mb ^ me);
	else {
		bitmap[p1++] |= mb;

		while (p1 < p2)
			bitmap[p1++] = ~0UL;

		bitmap[p2] |= me;
	}
}

static int mtdx_block_map_set_rnd(struct mtdx_block_map *map,
				  unsigned int address,
				  unsigned int offset,
				  unsigned int count)
{
	struct block_node *b = mtdx_block_map_find_useful(map, address);
	struct block_node_map *bm;

	if (!offset && (count == map->page_cnt)) {
		if (b)
			mtdx_block_map_put_node(map, b);

		return 0;
	}

	if (!b) {
		b = mtdx_block_map_get_node(map);
		if (!b)
			return 0;

		b->address = address;
		bm = container_of(b, struct block_node_map, node);
		bitmap_zero(bm->map, map->page_cnt);
		mtdx_block_map_add_useful(map, b);
	}

	bm = container_of(b, struct block_node_map, node);
	bitmap_set_bits(bm->map, offset, count);
	if (bitmap_full(bm->map, map->page_cnt)) {
		mtdx_block_map_put_node(map, b);
		return 0;
	}
	return 1;
}

static struct block_node *mtdx_block_map_alloc_inc(struct mtdx_block_map *map)
{
	return kzalloc(sizeof(struct block_node_cnt), GFP_KERNEL);
}

static void mtdx_block_map_free_inc(struct block_node *b)
{
	if (b)
		kfree(container_of(b, struct block_node_cnt, node));
}

static int mtdx_block_map_check_inc(struct mtdx_block_map *map,
				    unsigned int address,
				    unsigned int offset,
				    unsigned int count)
{
	struct block_node *b = mtdx_block_map_find_useful(map, address);
	struct block_node_cnt *bm;

	if (!b)
		return 0;

	bm = container_of(b, struct block_node_cnt, node);

	if (offset != count)
		return 0;
	if (count > (map->page_cnt - bm->count))
		return 0;

	return 1;
}

static int mtdx_block_map_set_inc(struct mtdx_block_map *map,
				  unsigned int address,
				  unsigned int offset,
				  unsigned int count)
{
	struct block_node *b = mtdx_block_map_find_useful(map, address);
	struct block_node_cnt *bm;

	if (!offset && (count == map->page_cnt)) {
		if (b)
			mtdx_block_map_put_node(map, b);

		return 0;
	}

	if (!b) {
		b = mtdx_block_map_get_node(map);
		if (!b)
			return 0;

		b->address = address;
		bm = container_of(b, struct block_node_cnt, node);
		bm->count = 0;
		mtdx_block_map_add_useful(map, b);
	}

	bm = container_of(b, struct block_node_cnt, node);
	bm->count += count;

	if (bm->count >= map->page_cnt) {
		mtdx_block_map_put_node(map, b);
		return 0;
	}
	return 1;
}

struct mtdx_block_map *mtdx_block_map_alloc(unsigned int page_cnt,
					    unsigned int node_count,
					    enum mtdx_block_map_type map_type)
{
	struct mtdx_block_map *map = kzalloc(sizeof(struct mtdx_block_map),
					     GFP_KERNEL);
	if (!map)
		return NULL;

	map->page_cnt = page_cnt;
	map->target_count = node_count;
	spin_lock_init(&map->lock);
	INIT_WORK(&map->allocator_work, mtdx_block_map_async_alloc);

	switch (map_type) {
	case MTDX_BLOCK_MAP_RANDOM:
		map->block_alloc = mtdx_block_map_alloc_rnd;
		map->block_free = mtdx_block_map_free_rnd;
		map->check_range = mtdx_block_map_check_rnd;
		map->set_range = mtdx_block_map_set_rnd;
		break;
	case MTDX_BLOCK_MAP_INCREMENTAL:
		map->block_alloc = mtdx_block_map_alloc_inc;
		map->block_free = mtdx_block_map_free_inc;
		map->check_range = mtdx_block_map_check_inc;
		map->set_range = mtdx_block_map_set_inc;
		break;
	default:
		kfree(map);
		map = NULL;
	};

	if (map)
		mtdx_block_map_async_alloc(&map->allocator_work);

	return map;
}
EXPORT_SYMBOL(mtdx_block_map_alloc);

void mtdx_block_map_free(struct mtdx_block_map *map)
{
	struct block_node *b;

	if (!map)
		return;

	mtdx_block_map_free_tree(map);

	while (map->retired_nodes) {
		b = rb_entry(map->retired_nodes, struct block_node, node);
		map->retired_nodes = b->node.rb_right;
		map->block_free(b);
	}
	kfree(map);
}
EXPORT_SYMBOL(mtdx_block_map_free);

int mtdx_block_map_set_range(struct mtdx_block_map *map, unsigned int address,
			     unsigned int offset, unsigned int count)
{
	int rc;
	unsigned long flags;

	spin_lock_irqsave(&map->lock, flags);
	rc = map->set_range(map, address, offset, count);
	spin_unlock_irqrestore(&map->lock, flags);
	return rc;
}
EXPORT_SYMBOL(mtdx_block_map_set_range);

int mtdx_block_map_check_range(struct mtdx_block_map *map, unsigned int address,
			       unsigned int offset, unsigned int count)
{
	int rc;
	unsigned long flags;

	spin_lock_irqsave(&map->lock, flags);
	rc = map->check_range(map, address, offset, count);
	spin_unlock_irqrestore(&map->lock, flags);
	return rc;
}
EXPORT_SYMBOL(mtdx_block_map_check_range);

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Simple dual-mode free page map");
MODULE_LICENSE("GPL");
