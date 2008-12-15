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
	struct mtdx_peb_alloc mpa;
	unsigned long         zone_map[]; /* 0 - use map A, 1 - use map B */
};

/*
 * For each zone, allocator will get an empty block nearest to randomly
 * selected position in selected map, and put a returned block into it's
 * position in the unselected map (the selectors are in zone_map). When there
 * are no more free blocks in the selected map, selection is reversed and
 * search is retried.
 */

static unsigned int rand_peb_alloc_find_circ(unsigned long *map,
					     unsigned int min_pos,
					     unsigned int max_pos,
					     unsigned int s_pos)
{
	unsigned long n_pos = find_next_zero_bit(map, max_pos, s_pos);
	unsigned int rv = MTDX_INVALID_BLOCK;

	if (n_pos != max_pos)
		rv = n_pos;
	else if (s_pos) {
		n_pos = find_next_zero_bit(map, s_pos, min_pos);
		if (n_pos != s_pos)
			rv = n_pos;
	}

	return rv;
}

static unsigned int rand_peb_alloc_get(struct mtdx_peb_alloc *bal,
				       unsigned int zone, int *dirty)
{
	struct rand_peb_alloc *rb = container_of(bal, struct rand_peb_alloc,
						 mpa);
	unsigned long *c_map;
	unsigned int f_pos, c_pos, rand_pos;
	int c_stat;

	unsigned int zone_min = mtdx_geo_zone_to_phy(bal->geo, zone, 0);
	unsigned int zone_max = mtdx_geo_zone_to_phy(bal->geo, zone + 1, 0);

	if (zone_max == MTDX_INVALID_BLOCK)
		zone_max = bal->geo->phy_block_cnt;

	/* Is random of any help here? */
	rand_pos = (random32() % (zone_max - zone_min)) + zone_min;

	if (rand_pos > bal->geo->phy_block_cnt)
		rand_pos = bal->geo->phy_block_cnt - 1;

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

	while (c_stat != *dirty) {
		c_pos++;
		if (c_pos == zone_max)
			c_pos = zone_min;

		c_pos = rand_peb_alloc_find_circ(c_map, zone_min, zone_max,
						 c_pos);

		if (c_pos != MTDX_INVALID_BLOCK) {
			c_stat = test_bit(c_pos, rb->erase_map);

			if (c_pos == f_pos)
				break;
		} else {
			c_stat = test_bit(f_pos, rb->erase_map);
			c_pos = f_pos;
			break;
		}
	}

	c_stat = test_bit(c_pos, rb->erase_map);
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
	unsigned int z_off;
	unsigned int zone = mtdx_geo_phy_to_zone(bal->geo, peb, &z_off);

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

static void rand_peb_alloc_reset(struct mtdx_peb_alloc *bal, unsigned int zone)
{
	struct rand_peb_alloc *rb = container_of(bal, struct rand_peb_alloc,
						 mpa);

	if (zone == MTDX_PEB_ALLOC_ALL) {
		bitmap_fill(rb->map_a, bal->geo->phy_block_cnt);
		bitmap_fill(rb->map_b, bal->geo->phy_block_cnt);
		bitmap_fill(rb->erase_map, bal->geo->phy_block_cnt);
		bitmap_zero(rb->zone_map, rb->mpa.geo->zone_cnt);
	} else {
		unsigned int z_off = mtdx_geo_zone_to_phy(bal->geo, zone, 0);
		unsigned int z_cnt = mtdx_geo_zone_to_phy(bal->geo, zone + 1,
							  0);
		if (z_cnt == MTDX_INVALID_BLOCK)
			z_cnt = bal->geo->phy_block_cnt;

		z_cnt -= z_off;

		bitmap_set_region(rb->map_a, z_off, z_cnt);
		bitmap_set_region(rb->map_b, z_off, z_cnt);
		bitmap_set_region(rb->erase_map, z_off, z_cnt);
		clear_bit(zone, rb->zone_map);
	}
}

static void rand_peb_alloc_free(struct mtdx_peb_alloc *bal)
{
	struct rand_peb_alloc *rb = container_of(bal, struct rand_peb_alloc,
						 mpa);
	kfree(rb->map_a);
	kfree(rb->map_b);
	kfree(rb->erase_map);

	kfree(rb);
}

struct mtdx_peb_alloc *mtdx_rand_peb_alloc(const struct mtdx_geo *geo)
{
	struct rand_peb_alloc *rb = kzalloc(sizeof(struct rand_peb_alloc)
					    + BITS_TO_LONGS(geo->zone_cnt)
					      * sizeof(unsigned long),
					    GFP_KERNEL);
	unsigned int z_off;

	if (!rb)
		return NULL;

	rb->mpa.geo = geo;
	rb->mpa.get_peb = rand_peb_alloc_get;
	rb->mpa.put_peb = rand_peb_alloc_put;
	rb->mpa.reset = rand_peb_alloc_reset;
	rb->mpa.free = rand_peb_alloc_free;

	rb->map_a = kmalloc(BITS_TO_LONGS(geo->phy_block_cnt)
			    * sizeof(unsigned long), GFP_KERNEL);
	rb->map_b = kmalloc(BITS_TO_LONGS(geo->phy_block_cnt)
			    * sizeof(unsigned long), GFP_KERNEL);
	rb->erase_map = kmalloc(BITS_TO_LONGS(geo->phy_block_cnt)
				* sizeof(unsigned long), GFP_KERNEL);

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
