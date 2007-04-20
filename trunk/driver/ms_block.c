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

#define MEMSTICK_MAX_BOOT_BLOCK 0x000c

#define MEMSTICK_CP_PAGE        0x0020

#define MEMSTICK_INT_CMDNAK     0x0001

#define MEMSTICK_INT_ERR        0x0040

#define MEMSTICK_STATUS_UCFG    0x0001
#define MEMSTICK_STATUS_UCEX    0x0004
#define MEMSTICK_STATUS_UCDT    0x0010

struct ms_boot_header {
	unsigned short block_id;
	unsigned short format_reserved;
	unsigned char  reserved0[184];
	unsigned char  data_entry;
	unsigned char  reserved1[179];
} __attribute__((packed));

struct ms_system_item {
	unsigned int  start_addr;
	unsigned int  data_size;
	unsigned char data_type_id;
	unsigned char reserved[3];
} __attribute__((packed));

struct ms_system_entry {
	struct ms_system_item disabled_block;
	struct ms_system_item cis_idi;
	unsigned char         reserved[24];
} __attribute__((packed));

struct ms_boot_attr_info {
	unsigned char      memorystick_class;
	unsigned char      format_unique_value1;
	unsigned short     block_size;
	unsigned short     number_of_blocks;
	unsigned short     number_of_effective_blocks;
	unsigned short     page_size;
	unsigned char      extra_data_size;
	unsigned char      format_unique_value2;
	unsigned long long assembly_time;
	unsigned char      format_unique_value3;
	unsigned char      serial_number[3];
	unsigned char      assembly_manufacturer_code;
	unsigned char      assembly_model_code[3];
	unsigned short     memory_mamufacturer_code;
	unsigned short     memory_device_code;
	unsigned short     implemented_capacity;
	unsigned char      format_unique_value4[2];
	unsigned char      vcc;
	unsigned char      vpp;
	unsigned short     controller_number;
	unsigned short     controller_function;
	unsigned char      reserved0[9];
	unsigned char      transfer_supporting;
	unsigned short     format_unique_value5;
	unsigned char      format_type;
	unsigned char      memorystick_application;
	unsigned char      device_type;
	unsigned char      reserved1[22];
	unsigned char      format_uniqure_value6[2];
	unsigned char      reserved2[15];
} __attribute__((packed));

struct ms_boot_page {
	struct ms_boot_header    header;
	struct ms_system_entry   entry;
	struct ms_boot_attr_info attr;
} __attribute__((packed));

struct ms_cis_idi {
	unsigned short general_config;
	unsigned short logical_cylinders;
	unsigned short reserved0;
	unsigned short logical_heads;
	unsigned short track_size;
	unsigned short sector_size;
	unsigned short sectors_per_track;
	unsigned short msw;
	unsigned short lsw;
	unsigned short reserved1;
	unsigned char  serial_number[20];
	unsigned short buffer_type;
	unsigned short buffer_size_increments;
	unsigned short long_command_ecc;
	unsigned char  firmware_version;
	unsigned char  model_name;
	unsigned short reserved2;
	unsigned short pio_mode_number;
	unsigned short dma_mode_number;
	unsigned short field_validity;
	unsigned short current_logical_cylinders;
	unsigned short current_logical_heads;
	unsigned short current_sectors_per_track;
	unsigned int   current_sector_capacity;
	unsigned short mutiple_sector_setting;
	unsigned int   addressable_sectors;
	unsigned short single_word_dma;
	unsigned short multi_word_dma;
	unsigned char  reserved3[128];
} __attribute__((packed));

struct block_map_node {
	unsigned int key;
	unsigned int val;
	struct rb_node node;
};

struct ms_block_data {
	struct rb_root      bad_blocks;
	struct rb_root      rel_blocks;
	struct ms_boot_page boot_page;
	struct ms_cis_idi   cis_idi;
	struct ms_register  reg;
};

static void ms_block_fix_boot_page_endianness(struct ms_boot_page *p)
{
	p->header.block_id = be16_to_cpu(p->header.block_id);
	p->header.format_reserved = be16_to_cpu(p->header.format_reserved);
	p->entry.disabled_block.start_addr
		= be32_to_cpu(p->entry.disabled_block.start_addr);
	p->entry.disabled_block.data_size
		= be32_to_cpu(p->entry.disabled_block.data_size);
	p->entry.cis_idi.start_addr
		= be32_to_cpu(p->entry.cis_idi.start_addr);
	p->entry.cis_idi.data_size
		= be32_to_cpu(p->entry.cis_idi.data_size);
	p->attr.block_size = be16_to_cpu(p->attr.block_size);
	p->attr.number_of_blocks = be16_to_cpu(p->attr.number_of_blocks);
	p->attr.number_of_effective_blocks
		= be16_to_cpu(p->attr.number_of_effective_blocks);
	p->attr.page_size = be16_to_cpu(p->attr.page_size);
	p->attr.assembly_time = be64_to_cpu(p->attr.assembly_time);
	p->attr.memory_mamufacturer_code
		= be16_to_cpu(p->attr.memory_mamufacturer_code);
	p->attr.memory_device_code = be16_to_cpu(p->attr.memory_device_code);
	p->attr.implemented_capacity
		= be16_to_cpu(p->attr.implemented_capacity);
	p->attr.controller_number = be16_to_cpu(p->attr.controller_number);
	p->attr.controller_function = be16_to_cpu(p->attr.controller_function);
}

static void ms_block_fix_cis_idi_endianness(struct ms_cis_idi *p)
{
	p->general_config = be16_to_cpu(p->general_config);
	p->logical_cylinders
		= be16_to_cpu(p->logical_cylinders);
	p->logical_heads = be16_to_cpu(p->logical_heads);
	p->track_size = be16_to_cpu(p->track_size);
	p->sector_size = be16_to_cpu(p->sector_size);
	p->sectors_per_track
		= be16_to_cpu(p->sectors_per_track);
	p->msw = be16_to_cpu(p->msw);
	p->lsw = be16_to_cpu(p->lsw);
	p->buffer_type = be16_to_cpu(p->buffer_type);
	p->buffer_size_increments
		= be16_to_cpu(p->buffer_size_increments);
	p->long_command_ecc = be16_to_cpu(p->long_command_ecc);
	p->pio_mode_number = be16_to_cpu(p->pio_mode_number);
	p->dma_mode_number = be16_to_cpu(p->dma_mode_number);
	p->field_validity = be16_to_cpu(p->field_validity);
	p->current_logical_cylinders
		= be16_to_cpu(p->current_logical_cylinders);
	p->current_logical_heads
		= be16_to_cpu(p->current_logical_heads);
	p->current_sectors_per_track
		= be16_to_cpu(p->current_sectors_per_track);
	p->current_sector_capacity
		= be32_to_cpu(p->current_sector_capacity);
	p->mutiple_sector_setting
		= be16_to_cpu(p->mutiple_sector_setting);
	p->addressable_sectors
		= be32_to_cpu(p->addressable_sectors);
	p->single_word_dma = be16_to_cpu(p->single_word_dma);
	p->multi_word_dma = be16_to_cpu(p->multi_word_dma);
}

static ssize_t ms_boot_attr_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ms_block_data *msb
		= memstick_get_drvdata(container_of(dev, struct memstick_dev,
						    dev));
	struct ms_boot_attr_info *ms_attr = &msb->boot_page.attr;

	sprintf(buf, "class: %x\n", ms_attr->memorystick_class);
	sprintf(buf, "block_size: %x\n", ms_attr->block_size);
	sprintf(buf, "number_of_blocks: %x\n", ms_attr->number_of_blocks);
	sprintf(buf, "number_of_effective_blocks: %x\n",
		ms_attr->number_of_effective_blocks);
	sprintf(buf, "page_size: %x\n", ms_attr->page_size);
	sprintf(buf, "extra_data_size: %x\n", ms_attr->extra_data_size);
	sprintf(buf, "assembly_time: %llx\n", ms_attr->assembly_time);
	sprintf(buf, "serial_number: %02x%02x%02x\n",
		ms_attr->serial_number[0],
		ms_attr->serial_number[1],
		ms_attr->serial_number[2]);
	sprintf(buf, "assembly_manufacturer_code: %x\n",
		ms_attr->assembly_manufacturer_code);
	sprintf(buf, "assembly_model_code: %02x%02x%02x\n",
		ms_attr->assembly_model_code[0],
		ms_attr->assembly_model_code[1],
		ms_attr->assembly_model_code[2]);
	sprintf(buf, "memory_mamufacturer_code: %x\n",
		ms_attr->memory_mamufacturer_code);
	sprintf(buf, "memory_device_code: %x\n",
		ms_attr->memory_device_code);
	sprintf(buf, "implemented_capacity: %x\n",
		ms_attr->implemented_capacity);
	sprintf(buf, "vcc: %x\n", ms_attr->vcc);
	sprintf(buf, "vpp: %x\n", ms_attr->vpp);
	sprintf(buf, "controller_number: %x\n", ms_attr->controller_number);
	sprintf(buf, "controller_function: %x\n",
		ms_attr->controller_function);
	sprintf(buf, "transfer_supporting: %x\n",ms_attr->transfer_supporting);
	sprintf(buf, "format_type: %x\n", ms_attr->format_type);
	sprintf(buf, "memorystick_application: %x\n",
		ms_attr->memorystick_application);
	sprintf(buf, "device_type: %x\n", ms_attr->device_type);
}

static ssize_t ms_cis_idi_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ms_block_data *msb
		= memstick_get_drvdata(container_of(dev, struct memstick_dev,
						    dev));
	struct ms_cis_idi *ms_cis = &msb->cis_idi;
	int cnt;

	sprintf(buf, "general_config: %x\n", ms_cis->general_config);
	sprintf(buf, "logical_cylinders: %x\n", ms_cis->logical_cylinders);
	sprintf(buf, "logical_heads: %x\n", ms_cis->logical_heads);
	sprintf(buf, "track_size: %x\n", ms_cis->track_size);
	sprintf(buf, "sector_size: %x\n", ms_cis->sector_size);
	sprintf(buf, "sectors_per_track: %x\n", ms_cis->sectors_per_track);
	sprintf(buf, "msw: %x\n", ms_cis->msw);
	sprintf(buf, "lsw: %x\n", ms_cis->lsw);
	sprintf(buf, "serial_number: ");
	for (cnt = 0; cnt < 20; cnt++)
		sprintf(buf, "%02x", ms_cis->serial_number[cnt]);
	sprintf(buf, "\n");
	sprintf(buf, "buffer_type: %x\n", ms_cis->buffer_type);
	sprintf(buf, "buffer_size_increments: %x\n", ms_cis->buffer_size_increments);
	sprintf(buf, "long_command_ecc: %x\n", ms_cis->long_command_ecc);
	sprintf(buf, "firmware_version: %x\n", ms_cis->firmware_version);
	sprintf(buf, "model_name: %x\n", ms_cis->model_name);
	sprintf(buf, "pio_mode_number: %x\n", ms_cis->pio_mode_number);
	sprintf(buf, "dma_mode_number: %x\n", ms_cis->dma_mode_number);
	sprintf(buf, "field_validity: %x\n", ms_cis->field_validity);
	sprintf(buf, "current_logical_cylinders: %x\n", ms_cis->current_logical_cylinders);
	sprintf(buf, "current_logical_heads: %x\n", ms_cis->current_logical_heads);
	sprintf(buf, "current_sectors_per_track: %x\n", ms_cis->current_sectors_per_track);
	sprintf(buf, "current_sector_capacity: %x\n", ms_cis->current_sector_capacity);
	sprintf(buf, "mutiple_sector_setting: %x\n", ms_cis->mutiple_sector_setting);
	sprintf(buf, "addressable_sectors: %x\n", ms_cis->addressable_sectors);
	sprintf(buf, "single_word_dma: %x\n", ms_cis->single_word_dma);
	sprintf(buf, "multi_word_dma: %x\n", ms_cis->multi_word_dma);
}

static struct device_attribute ms_block_sysfs_attr = {
	__ATTR(boot_attr, S_IRUGO, ms_boot_attr_show, NULL),
	__ATTR(cis_idi, S_IRUGO, ms_cis_idi_show, NULL),
	__ATTR_NULL
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

static memstick_error_t ms_block_read_page(struct memstick_dev *card,
					   struct scatterlist *sg,
					   unsigned int block,
					   unsigned int page)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct memstick_host *host = card->host;
	memstick_error_t err;

	msb->reg.param.system = 0x80;
	msb->reg.param.block_address[0] = (block >> 16) & 0xff;
	msb->reg.param.block_address[1] = (block >> 8) & 0xff;
	msb->reg.param.block_address[2] = block & 0xff;
	msb->reg.param.cp = MEMSTICK_CP_PAGE;
	msb->reg.param.page_address = page;
	
	err = memstick_write_reg(host, &msb->reg, 16, 6);
	if (err)
		return err;
	err = memstick_set_cmd(host, MS_CMD_BLOCK_READ, 1);
	if (err)
		return err;
	err = memstick_read_reg(host, &msb->reg, 0, 32);
	if (msb->reg.status.interrupt & MEMSTICK_INT_CMDNAK) {
		err = MEMSTICK_ERR_BADMEDIA;
		return err;
	}

	if (msb->reg.status.interrupt & MEMSTICK_INT_ERR) {
		if (msb->reg.status.status1
		    & (MEMSTICK_STATUS_UCDT | MEMSTICK_STATUS_UCEX
		       | MEMSTICK_STATUS_UCFG)) {
			err = MEMSTICK_ERR_BADMEDIA;
			return err;
		}
	}

	err = memstick_read_long_data(host, sg, 1);

	return MEMSTICK_ERR_NONE;
}

int ms_init_card(struct memstick_dev *card)
{
	struct memstick_host *host = card->host;
	memstick_error_t err;
	int cnt, bb_cnt = 0, rc = 0;
	unsigned char *boot_blocks;

	boot_blocks = kmalloc(2 * MEMSTICK_PAGE_SIZE, GFP_KERNEL);
	if (!boot_blocks)
		return -ENOMEM;

	mutex_lock(&host->lock);
	err = memstick_set_cmd(host, MS_CMD_RESET, 0);
	if (err) {
		rc = -ENODEV;
		goto out;
	}

	err = memstick_set_rw_reg_adrs(host, 0, 31, 0, 31); 
	if (err) {
		rc = -ENODEV;
		goto out;
	}

	for (cnt = 0; cnt < MEMSTICK_MAX_BOOT_BLOCK; cnt++) {
		err = ms_block_read_page(cnt, 0, 0x22, 1);
		if (!err) {
			memstick_read_reg();
			if (bb_cnt >= 2)
				break;
		}
	}

	if (bb_cnt) {
	}

	kfree(boot_blocks);


	if((rc = MediaModel())) return rc;
	if((rc = GetCHS())) return rc;
	if((rc = InitializeLUT())) return rc;
	if((rc = sub21DA0())) return rc;
	mbWriteProtected = (ms_regs.status.status0 & 1);
	while(!(rc = MakeLUT() && !var_x170) {};
	if(rc) return rc;
	if(mbParallelInterface) SwitchToParallelIF();
	vara_2 = 1;	

	rc = device_create_file(&card->dev, &ms_block_sysfs_attr);
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

	device_remove_file(&card->dev, &ms_block_sysfs_attr);
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
	{MEMSTICK_TYPE_LEGACY, MEMSTICK_CATEGORY_LEGACY,
	 MEMSTICK_CLASS_LEGACY},
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
