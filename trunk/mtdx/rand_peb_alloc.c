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
	unsigned long         *map_a;
	unsigned long         *map_b;
	unsigned long         *erase_map; /* erase status of block        */
	union {                           /* 0 - use map A, 1 - use map B */
		unsigned long zone_map;
		unsigned long *zone_map_ptr;
	};
	unsigned int          zone_cnt;
	struct mtdx_peb_alloc mpa;
};

/*
 * For each zone, allocator will get an empty block nearest to randomly
 * selected position in selected map, and put a returned block into it's
 * position in the unselected map (the selectors are in zone_map). When there
 * are no more free blocks in the selected map, selection is reversed and
 * search is retried.
 */

static unsigned long *zone_map(struct rand_peb_alloc *rb)
{
	if (rb->zone_cnt <= BITS_PER_LONG)
		return &rb->zone_map;
	else
		return rb->zone_map_ptr;
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
	struct rand_peb_alloc *rb = container_of(bal, struct rand_peb_alloc,
						 mpa);
	unsigned long *c_map;
	unsigned int f_pos, c_pos;
	int c_stat;

	unsigned int zone_min = zone << bal->zone_size_log;
	unsigned int zone_max = (zone + 1) << bal->zone_size_log;
	/* Is random of any help here? */
	unsigned int rand_pos = (((1 << bal->zone_size_log) - 1)
				 & random32()) + zone_min;

	if (zone_max > bal->block_cnt)
		zone_max = bal->block_cnt;
	if (rand_pos > bal->block_cnt)
		rand_pos = bal->block_cnt - 1;

	c_map = test_bit(zone, zone_map(rb)) ? rb->map_b : rb->map_a;

	f_pos = rand_peb_alloc_find_circ(c_map, zone_min, zone_max,
					 rand_pos);

	if (f_pos == MTDX_INVALID_BLOCK) {
		c_map = test_bit(zone, zone_map(rb)) ? rb->map_a : rb->map_b;
		change_bit(zone, zone_map(rb));

		f_pos = rand_peb_alloc_find_circ(c_map, zone_min,
						 zone_max, rand_pos);

		if (f_pos == MTDX_INVALID_BLOCK)
			return f_pos;
	}

	c_stat = test_bit(f_pos, rb->erase_map);
	c_pos = f_pos;

	while (c_stat != *dirty) {
		c_pos++;
		if (c_pos == zone_max)
			c_pos = zone_min;

		if (c_pos == f_pos)
			break;

		c_pos = rand_peb_alloc_find_circ(c_map, zone_min, zone_max,
						 c_pos);
		c_stat = test_bit(f_pos, rb->erase_map);
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
	struct rand_peb_alloc *rb = container_of(bal, struct rand_peb_alloc,
						 mpa);
	unsigned int zone = peb >> bal->zone_size_log;

	if (test_bit(zone, zone_map(rb))) {
		set_bit(peb, rb->map_b);
		clear_bit(peb, rb->map_a);
	} else {
		set_bit(peb, rb->map_a);
		clear_bit(peb, rb->map_b);
	}

	if (!dirty)
		clear_bit(peb, rb->erase_map);
}

static void rand_peb_alloc_reset(struct mtdx_peb_alloc *bal, unsigned int zone)
{
	struct rand_peb_alloc *rb = container_of(bal, struct rand_peb_alloc,
						 mpa);

	if (zone == MTDX_PEB_ALLOC_ALL) {
		bitmap_fill(rb->map_a, bal->block_cnt);
		bitmap_fill(rb->map_b, bal->block_cnt);
		bitmap_fill(rb->erase_map, bal->block_cnt);
		bitmap_zero(zone_map(rb), rb->zone_cnt);
	} else {
		unsigned int z_off = zone << bal->zone_size_log;
		unsigned int z_cnt = 1 << bal->zone_size_log;

		bitmap_set_region(rb->map_a, z_off, z_cnt);
		bitmap_set_region(rb->map_b, z_off, z_cnt);
		bitmap_set_region(rb->erase_map, z_off, z_cnt);
		clear_bit(zone, zone_map(rb));
	}
}

static void rand_peb_alloc_free(struct mtdx_peb_alloc *bal)
{
	struct rand_peb_alloc *rb = container_of(bal, struct rand_peb_alloc,
						 mpa);
	kfree(rb->map_a);
	kfree(rb->map_b);
	kfree(rb->erase_map);
	if (rb->zone_cnt > BITS_PER_LONG)
		kfree(rb->zone_map_ptr);

	kfree(rb);
}

struct mtdx_peb_alloc *mtdx_rand_peb_alloc(unsigned int zone_size_log,
					      unsigned int block_cnt)
{
	struct rand_peb_alloc *rb = kzalloc(sizeof(struct rand_peb_alloc),
					    GFP_KERNEL);

	if (!rb)
		return NULL;

	rb->zone_cnt = block_cnt >> zone_size_log;
	if (!rb->zone_cnt)
		rb->zone_cnt = 1;

	rb->mpa.zone_size_log = zone_size_log;
	rb->mpa.block_cnt = block_cnt;
	rb->mpa.get_peb = rand_peb_alloc_get;
	rb->mpa.put_peb = rand_peb_alloc_put;
	rb->mpa.reset = rand_peb_alloc_reset;
	rb->mpa.free = rand_peb_alloc_free;

	rb->map_a = kmalloc(BITS_TO_LONGS(block_cnt)
			    * sizeof(unsigned long), GFP_KERNEL);
	rb->map_b = kmalloc(BITS_TO_LONGS(block_cnt)
			    * sizeof(unsigned long), GFP_KERNEL);
	rb->erase_map = kmalloc(BITS_TO_LONGS(block_cnt)
				* sizeof(unsigned long), GFP_KERNEL);

	if (rb->zone_cnt > BITS_PER_LONG) {
		rb->zone_map_ptr = kmalloc(BITS_TO_LONGS(rb->zone_cnt)
					   * sizeof(unsigned long),
					   GFP_KERNEL);
		if (!rb->zone_map_ptr)
			goto err_out;
	}

	if (!rb->map_a || !rb->map_b || !rb->erase_map)
		goto err_out;

	rand_peb_alloc_reset(&rb->mpa, MTDX_PEB_ALLOC_ALL);

	return &rb->mpa;
err_out:
	rand_peb_alloc_free(&rb->mpa);
	return NULL;
}
EXPORT_SYMBOL(mtdx_rand_peb_alloc);

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Simple randomizing block allocator");
MODULE_LICENSE("GPL");
