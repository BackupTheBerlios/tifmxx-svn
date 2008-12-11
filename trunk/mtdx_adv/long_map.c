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
#include <linux/workqueue.h>
#include <linux/err.h>
#include "long_map.h"

static int extra = 0;
module_param(extra, int, 0644);

struct map_node {
	struct rb_node node;
	unsigned long  key;
	char           data[];
};

struct long_map {
	struct rb_root     useful_blocks;
	struct rb_node     *retired_nodes;
	struct map_node    *c_block;
	unsigned int       rnode_count;
	spinlock_t         lock;
	unsigned long      param;
	long_map_alloc_t   *alloc_fn;
	long_map_free_t    *free_fn;
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
		else {
			map->c_block = rv;
			return rv;
		}
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

	if (!map->retired_nodes)
		return ERR_PTR(-ENOMEM);

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

struct long_map *long_map_create(long_map_alloc_t *alloc_fn,
				 long_map_free_t *free_fn, unsigned long param)
{
	struct long_map *map = kzalloc(sizeof(struct long_map), GFP_KERNEL);

	if (!map)
		return NULL;

	spin_lock_init(&map->lock);

	map->param = param;
	map->alloc_fn = alloc_fn;
	map->free_fn = free_fn;

	if (alloc_fn && !free_fn)
		map->free_fn = long_map_default_free;

	return map;
}
EXPORT_SYMBOL(long_map_create);

int long_map_prealloc(struct long_map *map, unsigned int count)
{
	struct map_node *b;
	struct rb_node *h;
	unsigned int r_cnt;
	unsigned long flags;

	if (!map)
		return -EINVAL;

	while (1) {
		spin_lock_irqsave(&map->lock, flags);
		if (map->rnode_count < count)
			r_cnt = count - map->rnode_count;
		else
			r_cnt = 0;
		spin_unlock_irqrestore(&map->lock, flags);

		if (!r_cnt)
			return 0;

		h = NULL;

		while (r_cnt) {
			if (!map->alloc_fn) {
				b = kzalloc(sizeof(struct map_node)
					    + map->param, GFP_KERNEL);
                		if (!b)
					goto undo_last;
			} else {
				b = kzalloc(sizeof(struct map_node)
					    + sizeof(void *), GFP_KERNEL);
                		if (!b)
					goto undo_last;

				*(void **)b->data = map->alloc_fn(map->param);
				if (!*(void **)b->data) {
					kfree(b);
					goto undo_last;
				}
			}

			b->node.rb_right = h;
			h = &b->node;
			r_cnt--;
		}

		spin_lock_irqsave(&map->lock, flags);
		while (h) {
			b = rb_entry(h, struct map_node, node);
			h = b->node.rb_right;
			b->node.rb_right = map->retired_nodes;
			map->retired_nodes = &b->node;
			map->rnode_count++;
		}
		spin_unlock_irqrestore(&map->lock, flags);
	}
undo_last:
	while (h) {
		b = rb_entry(h, struct map_node, node);
		h = b->node.rb_right;
		if (map->free_fn)
			map->free_fn(*(void **)b->data, map->param);

		kfree(b);
	}
	return -ENOMEM;
}
EXPORT_SYMBOL(long_map_prealloc);

void long_map_destroy(struct long_map *map)
{
	struct map_node *b;

	if (!map)
		return;

	map->c_block = NULL;
	long_map_put_tree(map);

	while (map->retired_nodes) {
		b = rb_entry(map->retired_nodes, struct map_node, node);
		map->retired_nodes = b->node.rb_right;
		if (map->free_fn)
			map->free_fn(*(void **)b->data, map->param);

		kfree(b);
	}
	kfree(map);
}
EXPORT_SYMBOL(long_map_destroy);

void *long_map_get(struct long_map *map, unsigned long key)
{
	struct map_node *b;
	unsigned long flags;
	void *rv = NULL;

	if (!map)
		return NULL;

	spin_lock_irqsave(&map->lock, flags);
	b = long_map_find_useful(map, key);
	if (b) {
		if (map->alloc_fn)
			rv = *(void **)b->data;
		else
			rv = b->data;
	}
	spin_unlock_irqrestore(&map->lock, flags);
	return rv;
}
EXPORT_SYMBOL(long_map_get);

void *long_map_insert(struct long_map *map, unsigned long key)
{
	struct map_node *b;
	unsigned long flags;
	void *rv = NULL;

	if (!map)
		return NULL;

	spin_lock_irqsave(&map->lock, flags);
	b = long_map_get_node(map);

	if (!IS_ERR(b)) {
		b->key = key;
		rv = ERR_PTR(long_map_add_useful(map, b));

		if (!IS_ERR(rv)) {
			if (map->alloc_fn)
				rv = *(void **)b->data;
			else
				rv = b->data;

			map->c_block = b;
		} else {
			long_map_put_node(map, b);
			rv = NULL;
		}
	} else
		rv = NULL;

	spin_unlock_irqrestore(&map->lock, flags);
	return rv;
}
EXPORT_SYMBOL(long_map_insert);

void long_map_move(struct long_map *map, unsigned long dst_key,
		   unsigned long src_key)
{
	struct map_node *b;
	unsigned long flags;

	if (!map || dst_key == src_key)
		return;

	spin_lock_irqsave(&map->lock, flags);
	b = long_map_find_useful(map, src_key);
	if (b) {
		rb_erase(&b->node, &map->useful_blocks);
		b->key = dst_key;
		if (long_map_add_useful(map, b))
			long_map_put_node(map, b);
	}
	spin_unlock_irqrestore(&map->lock, flags);
}
EXPORT_SYMBOL(long_map_move);

void long_map_erase(struct long_map *map, unsigned long key)
{
	struct map_node *b;
	unsigned long flags;

	if (!map)
		return;

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

	if (!map)
		return;

	spin_lock_irqsave(&map->lock, flags);
	map->c_block = NULL;
	long_map_put_tree(map);
	spin_unlock_irqrestore(&map->lock, flags);
}
EXPORT_SYMBOL(long_map_clear);

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Simple dual-mode free page map");
MODULE_LICENSE("GPL");
