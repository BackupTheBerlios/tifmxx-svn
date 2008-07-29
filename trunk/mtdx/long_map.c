/*
 *  Stl Map<long, long>-like structure mapping longs to arbitrary objects
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/rbtree.h>
#include "long_map.h"

struct map_node {
	struct rb_node node;
	unsigned long  key;
	union {
		unsigned long  val;
		void           *val_ptr;
	};
};

struct long_map {
	unsigned int       page_cnt;
	struct rb_root     useful_blocks;
	struct rb_node     *retired_nodes;
	struct map_node    *c_block;
	unsigned int       target_count;
	unsigned int       rnode_count;
	spinlock_t         lock;
	unsigned long      param;
	long_map_alloc_t   *alloc_fn;
	long_map_free_t    *free_fn;

	struct work_struct allocator_work;
};

static void long_map_default_free(void *val_ptr, unsigned long param)
{
	kfree(val_ptr);
}

static struct map_node* long_map_find_useful(struct long_map *map,
					     unsigned long key)
{
	struct rb_node *n;
	struct map_node *rv;

	if (map->c_block && (key == map->c_block->key))
		return map->c_block;

	n = map->useful_blocks.rb_node;

	while (n) {
		rv = rb_entry(n, struct map_node, node);

		if (key < rv->key)
			n = n->rb_left;
		else if (key > rv->key)
			n = n->rb_right;
		else
			return rv;
	}

	return NULL;
}

static int long_map_add_useful(struct long_map *map, struct map_node *b)
{
	struct rb_node **p = &(map->useful_blocks.rb_node);
	struct rb_node *q = NULL;
	struct map_node *cb = NULL;

	while (*p) {
		q = *p;
		cb = rb_entry(q, struct map_node, node);

		if (b->key < cb->key)
			p = &(*p)->rb_left;
		else if (b->key > cb->key)
			p = &(*p)->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&b->node, q, p);
	rb_insert_color(&b->node, &map->useful_blocks);
	return 0;
}

static struct map_node *long_map_get_node(struct long_map *map)
{
	struct map_node *b;

	if (map->rnode_count < (map->target_count / 2))
		schedule_work(&map->allocator_work);

	if (!map->retired_nodes)
		return NULL;

	b = rb_entry(map->retired_nodes, struct map_node, node);

	map->retired_nodes = b->node.rb_right;
	b->node.rb_right = NULL;
	b->node.rb_left = NULL;
	map->rnode_count--;

	return b;
}

static void long_map_put_node(struct long_map *map, struct map_node *b)
{
	if (map->c_block == b)
		map->c_block = NULL;

	b->node.rb_left = NULL;
	b->node.rb_right = map->retired_nodes;
	map->retired_nodes = &b->node;
	map->rnode_count++;

	if (map->rnode_count > (2 * map->target_count))
		schedule_work(&map->allocator_work);
}

static void long_map_put_tree(struct long_map *map)
{
	struct rb_node *p, *q;
	struct map_node *b;

	for (p = map->useful_blocks.rb_node; p; p = q) {
		if (!p->rb_left) {
			q = p->rb_right;
			b = rb_entry(p, struct map_node, node);
			b->node.rb_left = NULL;
			b->node.rb_right = map->retired_nodes;
			map->retired_nodes = &b->node;
			map->rnode_count++;
		} else {
			q = p->rb_left;
			p->rb_left = q->rb_right;
			q->rb_right = p;
		}
	}
	map->useful_blocks.rb_node = NULL;
}

static void long_map_async_alloc(struct work_struct *work)
{
	struct long_map *map = container_of(work, struct long_map,
						  allocator_work);
	struct map_node *b;
	struct rb_node *h = NULL;
	unsigned int cnt = 0;
	unsigned long flags;

	spin_lock_irqsave(&map->lock, flags);
	while (map->rnode_count > map->target_count) {
		b = rb_entry(map->retired_nodes, struct map_node,
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
		b = rb_entry(h, struct map_node, node);
		h = b->node.rb_right;
		if (map->free_fn)
			map->free_fn(b->val_ptr, map->param);

		kfree(b);
	}

	while (cnt) {
		b = kzalloc(sizeof(struct map_node), GFP_KERNEL);
		if (!b)
			break;

		if (map->alloc_fn) {
			b->val_ptr = map->alloc_fn(map->param);
			if (!b->val_ptr) {
				kfree(b);
				break;
			}
		}

		b->node.rb_right = h;
		h = &b->node;
		cnt--;
	}

	if (h) {
		spin_lock_irqsave(&map->lock, flags);
		while (h) {
			b = rb_entry(h, struct map_node, node);
			h = b->node.rb_right;
			b->node.rb_right = map->retired_nodes;
			map->retired_nodes = &b->node;
		}
		spin_unlock_irqrestore(&map->lock, flags);
	}
}

struct long_map *long_map_create(unsigned int nr, long_map_alloc_t *alloc_fn,
				 long_map_free_t *free_fn, unsigned long param)
{
	struct long_map *map = kzalloc(sizeof(struct long_map), GFP_KERNEL);

	if (!map)
		return NULL;

	map->target_count = nr;
	spin_lock_init(&map->lock);
	INIT_WORK(&map->allocator_work, long_map_async_alloc);

	map->param = param;
	map->alloc_fn = alloc_fn;
	map->free_fn = free_fn;

	if (alloc_fn && !free_fn)
		map->free_fn = long_map_default_free;

	if (map)
		long_map_async_alloc(&map->allocator_work);

	return map;
}
EXPORT_SYMBOL(long_map_create);

void long_map_destroy(struct long_map *map)
{
	struct map_node *b;

	if (!map)
		return;

	long_map_put_tree(map);

	while (map->retired_nodes) {
		b = rb_entry(map->retired_nodes, struct map_node, node);
		map->retired_nodes = b->node.rb_right;
		if (map->free_fn)
			map->free_fn(b->val_ptr, map->param);

		kfree(b);
	}
	kfree(map);
}
EXPORT_SYMBOL(long_map_destroy);

unsigned long *long_map_find(struct long_map *map, unsigned long key)
{
	struct map_node *b;
	unsigned long flags;
	unsigned long *rv = NULL;

	spin_lock_irqsave(&map->lock, flags);
	b = long_map_find_useful(map, key);
	if (b) {
		if (map->alloc_fn)
			rv = b->val_ptr;
		else
			rv = &b->val;
	}
	spin_unlock_irqrestore(&map->lock, flags);
	return rv;
}
EXPORT_SYMBOL(long_map_find);

unsigned long *long_map_insert(struct long_map *map, unsigned long key)
{
	struct map_node *b;
	unsigned long flags;
	unsigned long *rv = NULL;

	spin_lock_irqsave(&map->lock, flags);
	b = long_map_get_node(map);
	b->key = key;

	if (b) {
		if (long_map_add_useful(map, b))
			long_map_put_node(map, b);
		else {
			if (map->alloc_fn)
				rv = b->val_ptr;
			else
				rv = &b->val;
		}
	}
	spin_unlock_irqrestore(&map->lock, flags);
	return rv;
}
EXPORT_SYMBOL(long_map_insert);

void long_map_erase(struct long_map *map, unsigned long key)
{
	struct map_node *b;
	unsigned long flags;

	spin_lock_irqsave(&map->lock, flags);
	b = long_map_find_useful(map, key);
	if (b) {
		rb_erase(&b->node, &map->useful_blocks);
		long_map_put_node(map, b);
	}
	spin_unlock_irqrestore(&map->lock, flags);
}
EXPORT_SYMBOL(long_map_erase);

void long_map_clear(struct long_map *map)
{
	unsigned long flags;

	spin_lock_irqsave(&map->lock, flags);
	long_map_put_tree(map);

	if (map->rnode_count > (2 * map->target_count))
		schedule_work(&map->allocator_work);

	spin_unlock_irqrestore(&map->lock, flags);
}
EXPORT_SYMBOL(long_map_clear);

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Simple dual-mode free page map");
MODULE_LICENSE("GPL");
