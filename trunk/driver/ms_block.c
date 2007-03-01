/*
 *  ms_block.c - Sony MemoryStick (legacy) storage support
 *
 *  Copyright (C) 2007 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Special thanks to Carlos Corbacho for providing various MemoryStick cards
 * that made this driver possible.
 *
 */

#include <linux/blkdev.h>
#include "linux/memstick.h"

#define DRIVER_NAME "ms_block"
#define DRIVER_VERSION "0.1"

static int major = 0;
module_param(major, int, 0644);

struct block_map_node
{
	unsigned int key;
	unsigned int val;
	struct rb_node node;
};

struct ms_block_data
{
	struct rb_root bad_blocks;
	struct rb_root rel_blocks;
	char data[512];
};

static struct block_map_node *ms_block_map_find(struct rb_root *map,
						unsigned int key)
{
	struct rb_node *n = map->rb_node;
	struct block_map_node *v_node = NULL;

	while (n) {
		v_node = rb_entry(n, struct block_map_node, node);
		if (key < v_node->key)
			n = n->rb_left;
		else if (key > v_node->key)
			n = n->rb_right;
		else
			return v_node;
	}

	return NULL;
}

static struct block_map_node *ms_block_map_insert(struct rb_root *map,
						  unsigned int key,
						  unsigned int val)
{
	struct block_map_node *v_node = NULL, *n_node = NULL;
	struct rb_node **p = &map->rb_node;
	struct rb_node *q = NULL;

	n_node = kzalloc(sizeof(struct block_map_node), GFP_KERNEL);
	if (!n_node)
		return ERR_PTR(-ENOMEM);

	n_node->key = key;
	n_node->val = val;

	while (*p) {
		q = *p;
		v_node = rb_entry(q, struct block_map_node, node);

		if (key < v_node->key)
			p = &(*p)->rb_left;
		else if (key > v_node->key)
			p = &(*p)->rb_right;
		else
			return v_node;
	}
	rb_link_node(&n_node->node, q, p);
	rb_insert_color(&n_node->node, map);
	return NULL;
}

static int ms_block_map_replace(struct rb_root *map, unsigned int key,
				unsigned int val)
{
	struct block_map_node *v_node = ms_block_map_insert(map, key, val);
	struct block_map_node *n_node;

	if (!v_node || IS_ERR(v_node))
		return PTR_ERR(v_node);

	n_node = kzalloc(sizeof(struct block_map_node), GFP_KERNEL);
	if (!n_node)
		return -ENOMEM;

	rb_replace_node(&v_node->node, &n_node->node, map);
	kfree(v_node);
	return 0;
}

static void ms_block_map_destroy(struct rb_root *map)
{
	struct rb_node *p, *q;

	for (p = map->rb_node; p != NULL; p = q) {
		if (!p->rb_left) {
			q = p->rb_right;
			kfree(rb_entry(p, struct block_map_node, node));
		} else {
			q = p->rb_left;
			p->rb_left = q->rb_right;
			q->rb_right = p;
		}
	}
}

static int ms_block_mark_bad(struct ms_block_data *msb, unsigned int block)
{
	unsigned int block_bit = 1 << (block & 0x1f);
	struct block_map_node *v_node;

	block >>= 5;
	v_node = ms_block_map_find(&msb->bad_blocks, block);
	if (v_node)
		v_node->val |= block_bit;
	else
		v_node = ms_block_map_insert(&msb->bad_blocks, block,
					     block_bit);

	if (IS_ERR(v_node))
		return PTR_ERR(v_node);

	return 0;
}

memstick_error_t ms_init_card(struct memstick_dev *card)
{
	struct memstick_host *host = card->host;
	memstick_error_t rc;

	mutex_lock(&host->lock);
	rc = memstick_set_cmd(host, MS_CMD_RESET, 0);
	if (rc)
		goto out;

	rc = memstick_set_rw_reg_adrs(host, 0, 31, 0, 31); 
	if (rc)
		goto out;

	

out:
	mutex_unlock(&host->lock);
	return rc;
}

static int ms_block_probe(struct memstick_dev *card)
{
	//struct memstick_host *host = card->host;
	//memstick_error_t err;
	struct ms_block_data *msb;

	msb = kzalloc(sizeof(struct ms_block_data), GFP_KERNEL);
	if (!msb)
		return -ENOMEM;

	memstick_set_drvdata(card, msb);


	return 0;
}

static void ms_block_remove(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	ms_block_map_destroy(&msb->bad_blocks);
	ms_block_map_destroy(&msb->rel_blocks);
}

#ifdef CONFIG_PM

static int ms_block_suspend(struct memstick_dev *card, pm_message_t state)
{
	return 0;
}

static int ms_block_resume(struct memstick_dev *card)
{
	return 0;
}

#else

#define ms_block_suspend NULL
#define ms_block_resume NULL

#endif /* CONFIG_PM */

static struct memstick_device_id ms_block_id_tbl[] = {
	{MEMSTICK_TYPE_LEGACY, MEMSTICK_CATEGORY_STORAGE,
	 MEMSTICK_CLASS_GENERIC},
	{}
};

static struct memstick_driver ms_block_driver = {
	.driver = {
		.name  = DRIVER_NAME,
		.owner = THIS_MODULE
	},
	.id_table = ms_block_id_tbl,
	.probe    = ms_block_probe,
	.remove   = ms_block_remove,
	.suspend  = ms_block_suspend,
	.resume   = ms_block_resume
};

static int __init ms_block_init(void)
{
	int rc = -ENOMEM;

//	rc = register_blkdev(major, "ms");
//	if (rc < 0) {
//		printk(KERN_WARNING DRIVER_NAME ": failed to register "
//		       "major %d, error %d\n", major, rc);
//		return rc;
//	}
//	if (!major)
//		major = rc;

	rc = memstick_register_driver(&ms_block_driver);
//	if (rc)
//		unregister_blkdev(major, "ms");
	return rc;
}

static void __exit ms_block_exit(void)
{
	memstick_unregister_driver(&ms_block_driver);
//	unregister_blkdev(major, "ms");
}

module_init(ms_block_init);
module_exit(ms_block_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Sony MemoryStick block device driver");
MODULE_DEVICE_TABLE(memstick, ms_block_id_tbl);
MODULE_VERSION(DRIVER_VERSION);
