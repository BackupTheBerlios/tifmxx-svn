/*
 *  MTDX simplistic random block allocator
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "peb_alloc.h"
#include "mtdx_common.h"
#include <linux/module.h>
#include <linux/random.h>
#include <linux/bitmap.h>

struct rand_peb_alloc {
	unsigned long *map_a;
	unsigned long *map_b;
	unsigned long *erase_map; /* erase status of block, 1 - needs erase */
	unsigned long *zone_map;  /* 0 - use map A, 1 - use map B           */
};

/*
 * For each zone, allocator will get an empty block nearest to randomly
 * selected position in selected map, and put a returned block into it's
 * position in the unselected map (the selectors are in zone_map). When there
 * are no more free blocks in the selected map, selection is reversed and
 * search is retried.
 */

static inline
struct rand_peb_alloc *rand_peb_alloc_priv(struct mtdx_peb_alloc *bal)
{
	return (struct rand_peb_alloc *)bal->private;
}

static unsigned int rand_peb_alloc_find_circ(unsigned long *map,
					     unsigned int min_pos,
					     unsigned int max_pos,
					     unsigned int s_pos)
{
	unsigned long n_pos = find_next_zero_bit(map, max_pos, s_pos);
	unsigned int rv = MTDX_INVALID_BLOCK;

	if (n_pos == max_pos) {
		n_pos = find_next_zero_bit(map, s_pos, min_pos);
		if (n_pos != s_pos)
			rv = n_pos;
	} else
		rv = n_pos;

	return rv;
}

static unsigned int rand_peb_alloc_get(struct mtdx_peb_alloc *bal,
				       unsigned int zone, int *dirty)
{
	struct rand_peb_alloc *rb = rand_peb_alloc_priv(bal);
	unsigned long *c_map;
	unsigned int f_pos, c_pos;
	int c_stat;

	unsigned int zone_min = zone << bal->block_addr_bits;
	unsigned int zone_max = (zone + 1) << bal->block_addr_bits;
	/* Is random of any help here? */
	unsigned int rand_pos = (((1 << bal->block_addr_bits) - 1)
				 & random32()) + zone_min;

	c_map = test_bit(zone, rb->zone_map) ? rb->map_b : rb->map_a;

	f_pos = rand_peb_alloc_find_circ(c_map, zone_min, zone_max,
					 rand_pos);

	if (f_pos == MTDX_INVALID_BLOCK) {
		c_map = test_bit(zone, rb->zone_map) ? rb->map_a : rb->map_b;
		change_bit(zone, rb->zone_map);

		f_pos = rand_peb_alloc_find_circ(c_map, zone_min,
						 zone_max, rand_pos);

		if (f_pos == MTDX_INVALID_BLOCK)
			return f_pos;
	}

	c_stat = test_bit(f_pos, rb->erase_map);
	c_pos = f_pos;

	if (c_stat != *dirty) {
		c_pos++;
		if (c_pos == zone_max)
			c_pos = zone_min;

		while ((c_stat != (*dirty)) && (c_pos != f_pos)) {
			c_pos = rand_peb_alloc_find_circ(c_map, zone_min,
							 zone_max, c_pos);
			c_stat = test_bit(f_pos, rb->erase_map);
		}
	}

	*dirty = c_stat;
	set_bit(c_pos, rb->map_a);
	set_bit(c_pos, rb->map_b);
	set_bit(c_pos, rb->erase_map);
	return c_pos;
}

static void rand_peb_alloc_put(struct mtdx_peb_alloc *bal, unsigned int peb,
			       int dirty)
{
	struct rand_peb_alloc *rb = rand_peb_alloc_priv(bal);
	unsigned int zone = peb >> bal->block_addr_bits;

	if (test_bit(zone, rb->zone_map)) {
		set_bit(peb, rb->map_b);
		clear_bit(peb, rb->map_a);
	} else {
		set_bit(peb, rb->map_a);
		clear_bit(peb, rb->map_b);
	}

	if (!dirty)
		clear_bit(peb, rb->erase_map);
}

static void rand_peb_alloc_free(struct mtdx_peb_alloc *bal)
{
	struct rand_peb_alloc *rb = rand_peb_alloc_priv(bal);
	kfree(rb->map_a);
	kfree(rb->map_b);
	kfree(rb->erase_map);
	kfree(rb->zone_map);
}

struct mtdx_peb_alloc *mtdx_random_alloc_init(unsigned int zone_cnt,
					      unsigned int block_addr_bits)
{
	struct mtdx_peb_alloc *bal = kzalloc(sizeof(struct mtdx_peb_alloc)
					     + sizeof(struct rand_peb_alloc),
					     GFP_KERNEL);
	struct rand_peb_alloc *rb;

	if (!bal)
		return NULL;

	bal->zone_cnt = zone_cnt;
	bal->block_addr_bits = block_addr_bits;
	bal->get_peb = rand_peb_alloc_get;
	bal->put_peb = rand_peb_alloc_put;
	bal->free = rand_peb_alloc_free;

	rb = rand_peb_alloc_priv(bal);

	rb->map_a = kmalloc(BITS_TO_LONGS(zone_cnt << block_addr_bits)
			    * sizeof(unsigned long), GFP_KERNEL);
	rb->map_b = kmalloc(BITS_TO_LONGS(zone_cnt << block_addr_bits)
			    * sizeof(unsigned long), GFP_KERNEL);
	rb->erase_map = kmalloc(BITS_TO_LONGS(zone_cnt << block_addr_bits)
				* sizeof(unsigned long), GFP_KERNEL);
	rb->zone_map = kzalloc(BITS_TO_LONGS(zone_cnt) * sizeof(unsigned long),
			       GFP_KERNEL);

	if (!rb->map_a || !rb->map_b || !rb->erase_map || !rb->zone_map)
		goto err_out;

	bitmap_fill(rb->map_a, bal->zone_cnt << block_addr_bits);
	bitmap_fill(rb->map_b, bal->zone_cnt << block_addr_bits);
	bitmap_fill(rb->erase_map, bal->zone_cnt << block_addr_bits);

	return bal;
err_out:
	rand_peb_alloc_free(bal);
	kfree(bal);
	return NULL;
}
EXPORT_SYMBOL(mtdx_random_alloc_init);

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Simple randomizing block allocator");
MODULE_LICENSE("GPL");