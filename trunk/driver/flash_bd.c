#include "flash_bd.h"

struct block_node {
	struct rb_node node;
	unsigned int   address;
	unsigned long  page_map[];
};

struct flash_zone {
	unsigned int   free_cnt;
	unsigned int   first_free;
	unsigned int   last_free;
	unsigned int   *block_table;
	unsigned long  *block_map;
	struct rb_root useful_blocks;
};

struct flash_bd {
	struct flash_bd_info;
	sector_t              size;
	unsigned int          block_ssize;
	unsigned int          zone_bsize;
	struct flash_zone     zone[];
};

static struct block_node* flash_bd_find_useful(struct flash_zone *fbz,
					       unsigned int phy_block)
{
	struct rb_node *n = fbz->useful_blocks.rb_node;
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

static struct block_node* flash_bd_alloc_useful(struct flash_zone *fbz,
						unsigned int phy_block,
						unsigned int page_cnt)
{
	struct rb_node **p = &fbz->useful_blocks.rb_node;
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
		     + BITS_TO_LONGS(page_cnt) * sizeof(unsigned long),
		     GFP_KERNEL);
	if (!rv)
		return NULL;

	rb_link_node(&rv->node, q, p);
	rb_insert_color(&rv->node, &fbz->useful_blocks);
	return rv;
}

struct flash_bd* flash_bd_init(sector_t size, struct flash_bd_info *fbi)
{
	unsigned int cnt, b_cnt, b_size;
	struct flash_bd *rv = kzalloc(sizeof(struct flash_bd)
				      + sizeof(struct flash_zone)
					* info->zone_cnt, GFP_KERNEL);
	if (!rv)
		return NULL;

	rv->zone_cnt = fbi->zone_cnt;
	rv->zone_ssize = fbi->zone_ssize;
	rv->block_cnt = fbi->block_cnt;
	rv->page_cnt = fbi->page_cnt;
	rv->page_size = fbi->page_size;
	rv->size = size;
	rv->block_ssize = (rv->page_cnt * rv->page_size) / 512;
	rv->zone_bsize = rv->zone_ssize / rv->block_ssize;

	b_size = rv->zone

	for (cnt = 0; cnt < rv->zone_cnt; ++cnt) {
		rv->zone[cnt].block_table = kmalloc(sizeof(unsigned int)
						    * rv->zone_bsize,
						    GFP_KERNEL);
		if (!rv->zone[cnt].block_table)
			goto err_out;

		for (b_cnt = 0; b_cnt < rv->zone_bsize; ++b_cnt)
			rv->zone[cnt].block_table[b_cnt] = FLASH_BD_INVALID;

		rv->zone[cnt].block_map = kzalloc(BITS_TO_LONGS(rv->block_cnt)
						  * sizeof(unsigned long),
						  GFP_KERNEL);
		if (!rv->zone[cnt].block_map)
			goto err_out;

		idr_init(&(rv->zone[cnt].useful_blocks));
	}

	return rv;
err_out:
	flash_bd_destroy(rv);
	return NULL;
}

void flash_bd_destroy(struct flash_bd *fbd)
{
	if (!fbd)
		return;

	for (cnt = 0; cnt < rv->zone_cnt; ++cnt) {
		kfree(rv->zone[cnt].block_table);
		kfree(rv->zone[cnt].block_map);
		idr_destroy(&(rv->zone[cnt].useful_blocks));
	}

	kfree(fbd);
}

static int flash_bd_make_useful(struct flash_bd *fbd, unsigned int zone,
				unsigned int phy_block, int dirty)
{
	unsigned long *page_map = kmalloc(BITS_TO_LONGS(fbd->page_cnt)
					  * sizeof(unsigned long), GFP_KERNEL);
	int rc = 0;

	if (!page_map)
		return -ENOMEM;

	memset(page_map, dirty ? 0xff : 0, BITS_TO_LONGS(fbd->page_cnt)
					   * sizeof(unsigned long));

	rc = idr_pre_get(&fbd->zone[zone].useful_blocks, GFP_KERNEL);

	if (!rc)
		rc = idr_get_new(&fbd->zone[zone].useful_blocks, page_map,
				 &phy_block);

	if (rc)
		kfree(page_map);

	return rc;
}

static void flash_bd_remove_useful(struct flash_bd *fbd, unsigned int zone,
				   unsigned int phy_block)
{
	unsigned long *page_map = idr_find(&fbd->zone[zone].useful_blocks,
					   phy_block);
	kfree(page_map);
	idr_remove(&fbd->zone[zone].useful_blocks, phy_block);
}

int flash_bd_add_block(struct flash_bd *fbd, unsigned int zone,
		       unsigned int *phy_block, unsigned int *log_block)
{
	int rc = 0;

	if (zone >= fbd->zone_cnt
	    || *phy_block >= fbd->block_cnt)
		return -EINVAL;

	if (test_bit(*phy_block, fbd->zone[zone].block_map)) {
		if (*log_block != FLASH_BD_INVALID) {
			if (*phy_block
			    != fbd->zone[zone].block_table[*log_block]) {
				*phy_block = fbd->zone[zone]
						  .block_table[*log_block];
				rc = -EEXIST;
			}
		} else {
			for (rc = 0; rc < fbd->zone_bsize; ++rc) {
				if (fbd->zone[zone].block_table[rc]
				    == *phy_block) {
					*log_block = rc;
					break;
				}
			}
			rc = -EEXIST;
		}
	}

	if (rc)
		return rc;

	if (*log_block != FLASH_BD_INVALID) {
		if (*log_block >= fbd->zone_bsize)
			return -EINVAL;

		fbd->zone[zone].block_table[*log_block] = *phy_block;
		set_bit(*phy_block, fbd->zone[zone].block_map);
	} else {
		
	}
}

int flash_bd_mark_bad(struct flash_bd *fbd, unsigned int zone,
		      unsigned int phy_block)
{
	unsigned int cnt;

	if (zone >= fbd->zone_cnt
	    || phy_block >= fbd->block_cnt)
		return -EINVAL;

	if (test_bit(phy_block, fbd->zone[zone].block_map)) {
		for (cnt = 0; cnt < fbd->zone_bsize; ++cnt)
			if (fbd->zone[zone].block_table[cnt] == phy_block) {
				fbd->zone[zone]
				     .block_table[cnt] = FLASH_BD_INVALID;
				break;
			}
	} else
		set_bit(phy_block, fbd->zone[zone].block_map);

	flash_bd_remove_useful(fbd, zone, phy_block);
	return 0;
}

