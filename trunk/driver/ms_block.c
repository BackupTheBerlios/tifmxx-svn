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
#include <linux/scatterlist.h>
#include "linux/memstick.h"

#define DRIVER_NAME "ms_block"
#define DRIVER_VERSION "0.1"

static int major = 0;
module_param(major, int, 0644);

#define MEMSTICK_MAX_SEGS       0x0080

#define MEMSTICK_MAX_BOOT_BLOCK 0x000c

#define MEMSTICK_CP_PAGE        0x0020
#define MEMSTICK_CP_EXTRA       0x0040
#define MEMSTICK_CP_OVERWRITE   0x0080

#define MEMSTICK_INT_CMDNAK     0x0001

#define MEMSTICK_INT_ERR        0x0040

#define MEMSTICK_STATUS_UCFG    0x0001
#define MEMSTICK_STATUS_UCEX    0x0004
#define MEMSTICK_STATUS_UCDT    0x0010

#define MEMSTICK_BOOT_BLOCK_ID  0x0001

struct ms_block_reg_param {
	struct ms_param_register      param;
	struct ms_extra_data_register extra_data;
} __attribute__((packed));

struct ms_block_reg_status {
	unsigned char reserved1;
	unsigned char interrupt;
	unsigned char status0;
	unsigned char status1;
} __attribute__((packed));

static const unsigned char ms_block_cmd_block_end = MS_CMD_BLOCK_END;
static const unsigned char ms_block_cmd_reset = MS_CMD_RESET;
static const unsigned char ms_block_cmd_block_write = MS_CMD_BLOCK_WRITE;
static const unsigned char ms_block_cmd_block_erase = MS_CMD_BLOCK_ERASE;
static const unsigned char ms_block_cmd_block_read = MS_CMD_BLOCK_READ;

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
	unsigned char      assembly_time[8];
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
	unsigned char  firmware_version[28];
	unsigned char  model_name[18];
	unsigned short reserved2[5];
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
	unsigned long key;
	unsigned long val;
	struct rb_node node;
};

struct ms_block_data {
	struct rb_root             bad_blocks;
	struct rb_root             rel_blocks;
	unsigned char              system;
	struct ms_boot_page        boot_page;
	struct ms_cis_idi          cis_idi;
	struct scatterlist         sg[MEMSTICK_MAX_SEGS];
	struct ms_block_reg_param  req_param[MEMSTICK_MAX_SEGS];
	struct ms_block_reg_status req_status[MEMSTICK_MAX_SEGS];
	struct memstick_request    req[MEMSTICK_MAX_SEGS * 4];
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
	p->attr.memory_mamufacturer_code
		= be16_to_cpu(p->attr.memory_mamufacturer_code);
	p->attr.memory_device_code = be16_to_cpu(p->attr.memory_device_code);
	p->attr.implemented_capacity
		= be16_to_cpu(p->attr.implemented_capacity);
	p->attr.controller_number = be16_to_cpu(p->attr.controller_number);
	p->attr.controller_function = be16_to_cpu(p->attr.controller_function);
}

static ssize_t ms_boot_attr_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ms_block_data *msb
		= memstick_get_drvdata(container_of(dev, struct memstick_dev,
						    dev));
	struct ms_boot_attr_info *ms_attr = &msb->boot_page.attr;
	ssize_t rc = 0;
	unsigned short as_year;

	as_year = (ms_attr->assembly_time[1] << 8) | ms_attr->assembly_time[2];

	rc += sprintf(buf, "class: %x\n", ms_attr->memorystick_class);
	rc += sprintf(buf + rc, "block_size: %x\n", ms_attr->block_size);
	rc += sprintf(buf + rc, "number_of_blocks: %x\n",
		      ms_attr->number_of_blocks);
	rc += sprintf(buf + rc, "number_of_effective_blocks: %x\n",
		      ms_attr->number_of_effective_blocks);
	rc += sprintf(buf + rc, "page_size: %x\n", ms_attr->page_size);
	rc += sprintf(buf + rc, "extra_data_size: %x\n",
		      ms_attr->extra_data_size);
	rc += sprintf(buf + rc, "assembly_time: %d %04d-%02d-%02d "
				"%02d:%02d:%02d\n",
		      ms_attr->assembly_time[0], as_year,
		      ms_attr->assembly_time[3], ms_attr->assembly_time[4],
		      ms_attr->assembly_time[5], ms_attr->assembly_time[6],
		      ms_attr->assembly_time[7]);
	rc += sprintf(buf + rc, "serial_number: %02x%02x%02x\n",
		      ms_attr->serial_number[0],
		      ms_attr->serial_number[1],
		      ms_attr->serial_number[2]);
	rc += sprintf(buf + rc, "assembly_manufacturer_code: %x\n",
		      ms_attr->assembly_manufacturer_code);
	rc += sprintf(buf + rc, "assembly_model_code: %02x%02x%02x\n",
		      ms_attr->assembly_model_code[0],
		      ms_attr->assembly_model_code[1],
		      ms_attr->assembly_model_code[2]);
	rc += sprintf(buf + rc, "memory_mamufacturer_code: %x\n",
		      ms_attr->memory_mamufacturer_code);
	rc += sprintf(buf + rc, "memory_device_code: %x\n",
		      ms_attr->memory_device_code);
	rc += sprintf(buf + rc, "implemented_capacity: %x\n",
		      ms_attr->implemented_capacity);
	rc += sprintf(buf + rc, "vcc: %x\n", ms_attr->vcc);
	rc += sprintf(buf + rc, "vpp: %x\n", ms_attr->vpp);
	rc += sprintf(buf + rc, "controller_number: %x\n",
		      ms_attr->controller_number);
	rc += sprintf(buf + rc, "controller_function: %x\n",
		      ms_attr->controller_function);
	rc += sprintf(buf + rc, "transfer_supporting: %x\n",
		      ms_attr->transfer_supporting);
	rc += sprintf(buf + rc, "format_type: %x\n", ms_attr->format_type);
	rc += sprintf(buf + rc, "memorystick_application: %x\n",
		      ms_attr->memorystick_application);
	rc += sprintf(buf + rc, "device_type: %x\n", ms_attr->device_type);
	return rc;
}

static ssize_t ms_bad_blocks_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct memstick_dev *card = container_of(dev, struct memstick_dev, dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);
	const int max_height = 32;
	struct rb_node **stack, *node;
	struct block_map_node *v_node;
	int height = 0, num_blocks, cnt;
	unsigned long block_base;
	size_t rc = 0;

	stack = kmalloc(sizeof(struct rb_node*) * max_height, GFP_KERNEL);
	if (!stack)
		return 0;

	mutex_lock(&card->host->lock);
	num_blocks = sizeof(unsigned long) * 8;
	node = msb->bad_blocks.rb_node;

	while (1) {
		while (node) {
			if (height >= max_height)
				goto out;
			stack[height++] = node;
			node = node->rb_left;
		}
		if (!height)
			break;

		node = stack[--height];
		v_node = rb_entry(node, struct block_map_node, node);
		block_base = v_node->key << (ilog2(sizeof(unsigned long)) + 3);
		for (cnt = 0; cnt < num_blocks; cnt++) {
			if ((1UL << cnt) & v_node->val) {
				rc += sprintf(buf + rc, "%lx\n",
					      block_base + cnt);
				if (PAGE_SIZE < (rc + 25)) {
					rc += sprintf(buf + rc, "...\n");
					goto out;
				}
			}
		}
		node = node->rb_right;
	}

out:
	mutex_unlock(&card->host->lock);
	kfree(stack);
	return rc;
}

static ssize_t ms_rel_blocks_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct memstick_dev *card = container_of(dev, struct memstick_dev, dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);
	const int max_height = 32;
	struct rb_node **stack, *node;
	struct block_map_node *v_node;
	int height = 0;
	size_t rc = 0;

	stack = kmalloc(sizeof(struct rb_node*) * max_height, GFP_KERNEL);
	if (!stack)
		return 0;

	mutex_lock(&card->host->lock);
	node = msb->rel_blocks.rb_node;

	while (1) {
		while (node) {
			if (height >= max_height)
				goto out;
			stack[height++] = node;
			node = node->rb_left;
		}
		if (!height)
			break;

		node = stack[--height];
		v_node = rb_entry(node, struct block_map_node, node);

		rc +=  sprintf(buf + rc, "%lx -> %lx\n", v_node->key,
			       v_node->val);
		if (PAGE_SIZE < (rc + 40)) {
			rc += sprintf(buf + rc, "...\n");
			goto out;
		}
		
		node = node->rb_right;
	}

out:
	mutex_unlock(&card->host->lock);
	kfree(stack);
	return rc;
}

static ssize_t ms_cis_idi_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ms_block_data *msb
		= memstick_get_drvdata(container_of(dev, struct memstick_dev,
						    dev));
	struct ms_cis_idi *ms_cis = &msb->cis_idi;
	int cnt;
	ssize_t rc = 0;

	rc += sprintf(buf, "general_config: %x\n", ms_cis->general_config);
	rc += sprintf(buf + rc, "logical_cylinders: %x\n",
		      ms_cis->logical_cylinders);
	rc += sprintf(buf + rc, "logical_heads: %x\n", ms_cis->logical_heads);
	rc += sprintf(buf + rc, "track_size: %x\n", ms_cis->track_size);
	rc += sprintf(buf + rc, "sector_size: %x\n", ms_cis->sector_size);
	rc += sprintf(buf + rc, "sectors_per_track: %x\n",
		      ms_cis->sectors_per_track);
	rc += sprintf(buf + rc, "msw: %x\n", ms_cis->msw);
	rc += sprintf(buf + rc, "lsw: %x\n", ms_cis->lsw);

	rc += sprintf(buf + rc, "serial_number: '");
	for (cnt = 0; cnt < 20; cnt++)
		rc += sprintf(buf + rc, "%c", ms_cis->serial_number[cnt]);
	rc += sprintf(buf + rc, "'\n");

	rc += sprintf(buf + rc, "buffer_type: %x\n", ms_cis->buffer_type);
	rc += sprintf(buf + rc, "buffer_size_increments: %x\n",
		      ms_cis->buffer_size_increments);
	rc += sprintf(buf + rc, "long_command_ecc: %x\n",
		      ms_cis->long_command_ecc);

	rc += sprintf(buf + rc, "firmware_version: '");
	for (cnt = 0; cnt < 28; cnt++)
		rc += sprintf(buf + rc, "%c", ms_cis->firmware_version[cnt]);
	rc += sprintf(buf + rc, "'\n");

	rc += sprintf(buf + rc, "model_name: '");
	for (cnt = 0; cnt < 18; cnt++)
		rc += sprintf(buf + rc, "%c", ms_cis->model_name[cnt]);
	rc += sprintf(buf + rc, "'\n");
	
	rc += sprintf(buf + rc, "pio_mode_number: %x\n",
		      ms_cis->pio_mode_number);
	rc += sprintf(buf + rc, "dma_mode_number: %x\n",
		      ms_cis->dma_mode_number);
	rc += sprintf(buf + rc, "field_validity: %x\n", ms_cis->field_validity);
	rc += sprintf(buf + rc, "current_logical_cylinders: %x\n",
		      ms_cis->current_logical_cylinders);
	rc += sprintf(buf + rc, "current_logical_heads: %x\n",
		      ms_cis->current_logical_heads);
	rc += sprintf(buf + rc, "current_sectors_per_track: %x\n",
		      ms_cis->current_sectors_per_track);
	rc += sprintf(buf + rc, "current_sector_capacity: %x\n",
		      ms_cis->current_sector_capacity);
	rc += sprintf(buf + rc, "mutiple_sector_setting: %x\n",
		      ms_cis->mutiple_sector_setting);
	rc += sprintf(buf + rc, "addressable_sectors: %x\n",
		      ms_cis->addressable_sectors);
	rc += sprintf(buf + rc, "single_word_dma: %x\n",
		      ms_cis->single_word_dma);
	rc += sprintf(buf + rc, "multi_word_dma: %x\n", ms_cis->multi_word_dma);

	return rc;
}

static DEVICE_ATTR(boot_attr, S_IRUGO, ms_boot_attr_show, NULL);
static DEVICE_ATTR(disabled_blocks, S_IRUGO, ms_bad_blocks_show, NULL);
static DEVICE_ATTR(relocated_blocks, S_IRUGO, ms_rel_blocks_show, NULL);
static DEVICE_ATTR(cis_idi, S_IRUGO, ms_cis_idi_show, NULL);

static struct block_map_node *ms_block_map_find(struct rb_root *map,
						unsigned long key)
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
						  unsigned long key,
						  unsigned long val)
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

static int ms_block_map_replace(struct rb_root *map, unsigned long key,
				unsigned long val)
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

static int ms_block_mark_bad(struct ms_block_data *msb, unsigned long block)
{
	unsigned long block_bit
		= 1UL << (block & (sizeof(unsigned long) * 8 - 1));
	struct block_map_node *v_node;

	block >>= ilog2(sizeof(unsigned long)) + 3;
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

static void ms_block_init_read_pages(struct memstick_dev *card,
				     struct ms_block_data *msb,
				     char *buf,
				     unsigned int block_addr,
				     unsigned int start_page,
				     unsigned int num_pages,
				     unsigned int page_size,
				     unsigned int *req_offset)
{
	struct memstick_host *host = card->host;
	unsigned int cnt;

	for (cnt = *req_offset; cnt < num_pages; cnt++) {
		msb->req_param[cnt].param.system = msb->system;
		msb->req_param[cnt].param.block_address[0]
			= (block_addr >> 16) & 0xff;
		msb->req_param[cnt].param.block_address[1]
			= (block_addr >> 8) & 0xff;
		msb->req_param[cnt].param.block_address[2]
			= block_addr & 0xff;
		msb->req_param[cnt].param.cp = MEMSTICK_CP_PAGE;
		msb->req_param[cnt].param.page_address
			= cnt - *req_offset + start_page;
		msb->req_param[cnt].extra_data.overwrite_flag = 0;
		msb->req_param[cnt].extra_data.management_flag = 0;
		msb->req_param[cnt].extra_data.logical_address = 0;

		sg_init_one(&msb->sg[cnt],
			    buf + ((cnt - *req_offset)* page_size),
			    page_size);

		memstick_init_req(&msb->req[4 * cnt], MS_TPC_WRITE_REG,
				  (char*)(&msb->req_param[cnt]),
				  sizeof(struct ms_block_reg_param));
		memstick_queue_req(host, &msb->req[4 * cnt]);

		memstick_init_req(&msb->req[4 * cnt + 1], MS_TPC_SET_CMD,
				  (char*)(&ms_block_cmd_block_read), 1);
		memstick_queue_req(host, &msb->req[4 * cnt + 1]);

		memstick_init_req(&msb->req[4 * cnt + 2], MS_TPC_READ_REG,
				  (char*)(&msb->req_status[cnt]),
				  sizeof(struct ms_block_reg_status));
		memstick_queue_req(host, &msb->req[4 * cnt + 2]);

		memstick_init_req_sg(&msb->req[4 * cnt + 3],
				     MS_TPC_READ_LONG_DATA, &msb->sg[cnt]);
		memstick_queue_req(host, &msb->req[4 * cnt + 3]);
	}
	*req_offset += num_pages;
}

static unsigned int ms_block_fini_pages(struct memstick_dev *card,
					struct ms_block_data *msb)
{
	struct memstick_host *host = card->host;
	struct memstick_request *req;
	struct ms_block_reg_status *st;
	int cnt = 0, page_ok = 0;

	for (req = memstick_get_req(host); req; req = memstick_get_req(host)) {
		dev_dbg(&card->dev, "req %d, err %d\n", req->tpc, req->error);
		if (req->error) {
			while (req)
				req = memstick_get_req(host);
			break;
		}
		switch (req->tpc) {
		case MS_TPC_WRITE_REG:
			break;
		case MS_TPC_SET_CMD:
			break;
		case MS_TPC_READ_REG:
			page_ok = 1;
			st = (struct ms_block_reg_status*)req->data;
			if (st->interrupt & MEMSTICK_INT_CMDNAK)
				page_ok = 0;
			if (st->interrupt & MEMSTICK_INT_ERR
			    && (st->status1 & (MEMSTICK_STATUS_UCFG
					       | MEMSTICK_STATUS_UCEX
					       | MEMSTICK_STATUS_UCDT)))
				page_ok = 0;
				
			dev_dbg(&card->dev, "interrupt %x, status %x\n", st->interrupt, st->status1);
			break;
		case MS_TPC_READ_LONG_DATA:
		case MS_TPC_WRITE_LONG_DATA:
			if (!page_ok)
				goto out;

			cnt++;
			break;

		default:
			BUG();
		}
	}

out:
	for (req = memstick_get_req(host); req; req = memstick_get_req(host)) {
	// drain request queue 
	};
	return cnt;
}

static int ms_block_find_boot_blocks(struct memstick_dev *card,
				     struct ms_block_data *msb,
				     char **buf, int *boot_blocks)
{
	struct ms_boot_page *b_page;
	unsigned int b_cnt, bb_cnt = 0;
	unsigned int req_offset;
	int rc = 0;

	boot_blocks[0] = -1;
	boot_blocks[1] = -1;

	for (b_cnt = 0; b_cnt < MEMSTICK_MAX_BOOT_BLOCK; b_cnt++) {
		req_offset = 0;
		ms_block_init_read_pages(card, msb, buf[bb_cnt], b_cnt, 0, 1,
					 sizeof(struct ms_boot_page),
					 &req_offset);
		if (ms_block_fini_pages(card, msb)) {
			b_page = (struct ms_boot_page*)buf[b_cnt];
			ms_block_fix_boot_page_endianness(b_page);
			if (b_page->header.block_id == MEMSTICK_BOOT_BLOCK_ID) {
				boot_blocks[bb_cnt] = b_cnt;
				rc = ms_block_mark_bad(msb, b_cnt);
				bb_cnt++;
				if (bb_cnt == 2)
					break;
			}
		}
	}

	if (!bb_cnt)
		return -EIO;

	return rc;
}


static int ms_block_fetch_bad_blocks(struct memstick_dev *card,
				     struct ms_block_data *msb, int block)
{
	unsigned int start_page, page_cnt;
	unsigned int req_offset = 0;
	unsigned short b_block;
	char *buf;
	int rc = 0;

	start_page = msb->boot_page.entry.disabled_block.start_addr;
	start_page += sizeof(struct ms_boot_page);
	page_cnt = start_page + msb->boot_page.entry.disabled_block.data_size
		   - 1;
	start_page /= msb->boot_page.attr.page_size;
	page_cnt /= msb->boot_page.attr.page_size;

	if (!msb->boot_page.entry.disabled_block.data_size)
		return 0;

	page_cnt = page_cnt - start_page + 1;
	buf = kmalloc(page_cnt * msb->boot_page.attr.page_size, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;

	ms_block_init_read_pages(card, msb, buf, block, start_page, page_cnt,
				 msb->boot_page.attr.page_size, &req_offset);

	if (page_cnt != ms_block_fini_pages(card, msb)) {
		rc = -EIO;
		goto out_free_buf;
	}

	start_page = msb->boot_page.entry.disabled_block.start_addr;
	start_page += sizeof(struct ms_boot_page);
	start_page %= msb->boot_page.attr.page_size;

	for (page_cnt = 0;
	     page_cnt < msb->boot_page.entry.disabled_block.data_size;
	     page_cnt += 2) {
		b_block = be16_to_cpu(*(unsigned short*)(buf + start_page
							 + page_cnt));
		rc = ms_block_mark_bad(msb, b_block);
		dev_dbg(&card->dev, "found bad block %x\n", b_block);
	}

out_free_buf:
	kfree(buf);
	return rc;
}

static int ms_block_fetch_cis_idi(struct memstick_dev *card,
				  struct ms_block_data *msb, int block)
{
	unsigned int start_page, page_cnt;
	unsigned int req_offset = 0;
	char *buf;
	int rc = 0;

	start_page = msb->boot_page.entry.cis_idi.start_addr;
	start_page += sizeof(struct ms_boot_page);
	page_cnt = start_page + msb->boot_page.entry.cis_idi.data_size - 1;
	start_page /= msb->boot_page.attr.page_size;
	page_cnt /= msb->boot_page.attr.page_size;

	dev_dbg(&card->dev, "cis_idi %x, %x\n",
		msb->boot_page.entry.cis_idi.start_addr,
		msb->boot_page.entry.cis_idi.data_size);
		
	if (msb->boot_page.entry.cis_idi.data_size
	    < (sizeof(struct ms_cis_idi) + 0x100))
		return -ENODEV;

	page_cnt = page_cnt - start_page + 1;
	buf = kmalloc(page_cnt * msb->boot_page.attr.page_size, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;

	dev_dbg(&card->dev, "cis_idi p: %x, %x, %x, %x\n",
		block, start_page, page_cnt, msb->boot_page.attr.page_size);

	ms_block_init_read_pages(card, msb, buf, block, start_page, page_cnt,
				 msb->boot_page.attr.page_size, &req_offset);

	if (page_cnt != ms_block_fini_pages(card, msb)) {
		rc = -EIO;
		goto out_free_buf;
	}

	start_page = msb->boot_page.entry.cis_idi.start_addr;
	start_page += sizeof(struct ms_boot_page);
	start_page %= msb->boot_page.attr.page_size;
	start_page += 0x100;

	memcpy(&msb->cis_idi, buf + start_page, sizeof(struct ms_cis_idi));
	//ms_block_fix_cis_idi_endianness(&msb->cis_idi);
	
	rc = 0;

out_free_buf:
	kfree(buf);
	return rc;
}

static int ms_block_init_card(struct memstick_dev *card,
			      struct ms_block_data *msb)
{
	struct memstick_host *host = card->host;
	int page_size = sizeof(struct ms_boot_page);
	char *buf = kmalloc(2 * page_size, GFP_KERNEL);
	char *buf_ptr[2] = {buf, buf + page_size};
	int boot_blocks[2];
	int rc;

	struct ms_register_addr addr = {
		0, sizeof(struct ms_block_reg_status),
		16, sizeof(struct ms_block_reg_param)
	};

	msb->system = 0x80;

	memstick_init_req(&msb->req[0], MS_TPC_SET_CMD,
			  (char*)(&ms_block_cmd_reset), 1);
	msb->req[0].need_card_int = 0;
	memstick_queue_req(host, &msb->req[0]);
	memstick_get_req(host);

	if (msb->req[0].error)
		return -ENODEV;

	memstick_init_req(&msb->req[0], MS_TPC_SET_RW_REG_ADRS,
			  (char*)(&addr), sizeof(addr));
	memstick_queue_req(host, &msb->req[0]);
	memstick_get_req(host);
	if (msb->req[0].error)
		return -ENODEV;

	if (!buf)
		return -ENOMEM;

	rc = ms_block_find_boot_blocks(card, msb, buf_ptr, boot_blocks);
	if (rc)
		goto out_free_buf;

	memcpy(&msb->boot_page, buf_ptr[0], page_size);

	rc = ms_block_fetch_bad_blocks(card, msb, boot_blocks[0]);

	if (rc && boot_blocks[1] != -1) {
		memcpy(&msb->boot_page, buf_ptr[1],
		       page_size);
		boot_blocks[0] = -1;
		rc = ms_block_fetch_bad_blocks(card, msb, boot_blocks[1]);
	}

	if (rc)
		goto out_free_buf;

	if (boot_blocks[0] != -1) {
		rc = ms_block_fetch_cis_idi(card, msb, boot_blocks[0]);

		if (rc && boot_blocks[1] != -1) {
			memcpy(&msb->boot_page, buf_ptr[1], page_size);
			rc = ms_block_fetch_cis_idi(card, msb, boot_blocks[1]);
		}
	} else if (boot_blocks[1] != -1)
		rc = ms_block_fetch_cis_idi(card, msb, boot_blocks[1]);

	if (rc)
		goto out_free_buf;


/*

	if((rc = MediaModel())) return rc;
	if((rc = GetCHS())) return rc;
	if((rc = InitializeLUT())) return rc;
	if((rc = sub21DA0())) return rc;
	mbWriteProtected = (ms_regs.status.status0 & 1);
	while(!(rc = MakeLUT() && !var_x170) {};
	if(rc) return rc;
	if(mbParallelInterface) SwitchToParallelIF();
	vara_2 = 1;	
*/

out_free_buf:
	kfree(buf);
	return rc;
}

static int ms_block_probe(struct memstick_dev *card)
{
	struct ms_block_data *msb;
	int rc = 0;

	msb = kzalloc(sizeof(struct ms_block_data), GFP_KERNEL);
	if (!msb)
		return -ENOMEM;

	mutex_lock(&card->host->lock);
	rc = ms_block_init_card(card, msb);
	mutex_unlock(&card->host->lock);

	if (rc)
		goto out_free;

	memstick_set_drvdata(card, msb);

	rc = device_create_file(&card->dev, &dev_attr_boot_attr);
	if (rc)
		goto out_free;

	rc = device_create_file(&card->dev, &dev_attr_disabled_blocks);
	if (rc)
		goto out_remove_boot_attr;

	rc = device_create_file(&card->dev, &dev_attr_relocated_blocks);
	if (rc)
		goto out_remove_disabled_blocks;

	rc = device_create_file(&card->dev, &dev_attr_cis_idi);
	if (rc)
		goto out_remove_relocated_blocks;

	return 0;

out_remove_relocated_blocks:
	device_remove_file(&card->dev, &dev_attr_relocated_blocks);
out_remove_disabled_blocks:
	device_remove_file(&card->dev, &dev_attr_disabled_blocks);
out_remove_boot_attr:
	device_remove_file(&card->dev, &dev_attr_boot_attr);
out_free:
	kfree(msb);
	memstick_set_drvdata(card, NULL);
	return rc;
}

static void ms_block_remove(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	memstick_set_drvdata(card, NULL);
	device_remove_file(&card->dev, &dev_attr_cis_idi);
	device_remove_file(&card->dev, &dev_attr_relocated_blocks);
	device_remove_file(&card->dev, &dev_attr_disabled_blocks);
	device_remove_file(&card->dev, &dev_attr_boot_attr);
	ms_block_map_destroy(&msb->bad_blocks);
	ms_block_map_destroy(&msb->rel_blocks);
	kfree(msb);
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
	{MEMSTICK_MATCH_ALL, MEMSTICK_TYPE_LEGACY, MEMSTICK_CATEGORY_STORAGE,
	 MEMSTICK_CLASS_GENERIC},
	{MEMSTICK_MATCH_ALL, MEMSTICK_TYPE_DUO, MEMSTICK_CATEGORY_STORAGE_DUO,
	 MEMSTICK_CLASS_GENERIC_DUO},
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

	rc = register_blkdev(major, "ms");
	if (rc < 0) {
		printk(KERN_ERR DRIVER_NAME ": failed to register "
		       "major %d, error %d\n", major, rc);
		return rc;
	}
	if (!major)
		major = rc;

	rc = memstick_register_driver(&ms_block_driver);
	if (rc)
		unregister_blkdev(major, "ms");
	return rc;
}

static void __exit ms_block_exit(void)
{
	memstick_unregister_driver(&ms_block_driver);
	unregister_blkdev(major, "ms");
}

module_init(ms_block_init);
module_exit(ms_block_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Sony MemoryStick block device driver");
MODULE_DEVICE_TABLE(memstick, ms_block_id_tbl);
MODULE_VERSION(DRIVER_VERSION);
