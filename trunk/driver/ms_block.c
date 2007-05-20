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

#define MEMSTICK_MAX_SEGS               0x0080

#define MEMSTICK_MAX_BOOT_BLOCK         0x000c

#define MEMSTICK_BOOT_BLOCK_ID          0x0001

#define MEMSTICK_INVALID_BLOCK          0xffff

#define MEMSTICK_BMAP_LINE_SZ           0x0010

struct ms_block_reg_status {
	unsigned char reserved0;
	unsigned char interrupt;
	unsigned char status0;
	unsigned char status1;
} __attribute__((packed));

static const char ms_block_cmd_block_end = MS_CMD_BLOCK_END;
static const char ms_block_cmd_reset = MS_CMD_RESET;
static const char ms_block_cmd_block_write = MS_CMD_BLOCK_WRITE;
static const char ms_block_cmd_block_erase = MS_CMD_BLOCK_ERASE;
static const char ms_block_cmd_block_read = MS_CMD_BLOCK_READ;

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

struct ms_block_data {
	unsigned char              *block_map;
	unsigned short             *block_lut;
	unsigned short             block_count;
	unsigned short             log_block_count;
	unsigned short             page_size;
	unsigned char              system;
	struct ms_boot_page        boot_page;
	struct ms_cis_idi          cis_idi;
	struct bin_attribute       dev_attr_logical_block_map;
	struct bin_attribute       dev_attr_physical_block_map;
	struct scatterlist         sg[MEMSTICK_MAX_SEGS];
	struct ms_param_register   req_param[MEMSTICK_MAX_SEGS];
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
static DEVICE_ATTR(cis_idi, S_IRUGO, ms_cis_idi_show, NULL);

static void ms_block_mark_used(struct ms_block_data *msb, unsigned short block)
{
	unsigned char block_bit = 1U << (block & 7);

	if (block < msb->block_count) {
		block >>= 3;
		msb->block_map[block] |= block_bit;
	}
}

static void ms_block_mark_unused(struct ms_block_data *msb,
				 unsigned short block)
{
	unsigned char block_bit = 1U << (block & 7);

	if (block < msb->block_count) {
		block >>= 3;
		msb->block_map[block] &= ~block_bit;
	}
}

static int ms_block_used(struct ms_block_data *msb, unsigned short block)
{
	unsigned char block_bit = 1U << (block & 7);

	if (block < msb->block_count) {
		block >>= 3;
		return msb->block_map[block] & block_bit ? 1 : 0;
	} else
		return 1;
}

static unsigned short ms_block_physical(struct ms_block_data *msb,
					unsigned short block)
{
	if (block < msb->block_count)
		return msb->block_lut[block];
	else
		return MEMSTICK_INVALID_BLOCK;
}

static ssize_t ms_block_log_block_map_read(struct kobject *kobj, char *buf,
					   loff_t offset, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct memstick_dev *card = container_of(dev, struct memstick_dev, dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);
	loff_t line_low = offset / MEMSTICK_BMAP_LINE_SZ;
	loff_t line_high = (offset + count - 1) / MEMSTICK_BMAP_LINE_SZ;
	char line[MEMSTICK_BMAP_LINE_SZ + 1];
	unsigned short p_addr;
	ssize_t rv = 0;

	if (!count)
		return 0;

	dev_dbg(&card->dev, "read log map, offset %llx, size %lx, ll %llx, "
		"lh %llx\n", offset, count, line_low, line_high);

	offset -= line_low * MEMSTICK_BMAP_LINE_SZ;

	mutex_lock(&card->host->lock);
	do {
		p_addr = ms_block_physical(msb, line_low);
		if (p_addr == MEMSTICK_INVALID_BLOCK)
			snprintf(line, MEMSTICK_BMAP_LINE_SZ + 1,
				 "%04x   unmapped\n", (unsigned short)line_low);
		else
			snprintf(line, MEMSTICK_BMAP_LINE_SZ + 1,
				 "%04x       %04x\n", (unsigned short)line_low,
				 p_addr);

		if ((MEMSTICK_BMAP_LINE_SZ - offset) >= count) {
			memcpy(buf + rv, line + offset, count);
			rv += count;
			break;
		} 

		memcpy(buf + rv, line + offset, MEMSTICK_BMAP_LINE_SZ - offset);
		rv += MEMSTICK_BMAP_LINE_SZ - offset;
		count -= MEMSTICK_BMAP_LINE_SZ - offset;
		offset = 0;
		line_low++;
	} while (line_low <= line_high);

	mutex_unlock(&card->host->lock);
	return rv;
}

static ssize_t ms_block_phys_block_map_read(struct kobject *kobj, char *buf,
					    loff_t offset, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct memstick_dev *card = container_of(dev, struct memstick_dev, dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);
	loff_t line_low = offset / MEMSTICK_BMAP_LINE_SZ;
	loff_t line_high = (offset + count - 1) / MEMSTICK_BMAP_LINE_SZ;
	char line[MEMSTICK_BMAP_LINE_SZ + 1];
	unsigned short p_addr, l_addr;
	unsigned short *p_map;
	ssize_t rv = 0;

	if (!count)
		return 0;

	p_map = kmalloc((line_high - line_low + 1) * 2, GFP_KERNEL);
	if (!p_map)
		return -ENOMEM;

	for (p_addr = 0; p_addr < (line_high - line_low + 1); p_addr++)
		p_map[p_addr] = MEMSTICK_INVALID_BLOCK;

	offset -= line_low * MEMSTICK_BMAP_LINE_SZ;

	mutex_lock(&card->host->lock);
	for (l_addr = 0; l_addr < msb->log_block_count; l_addr++) {
		p_addr = ms_block_physical(msb, l_addr);
		if (p_addr >= line_low && p_addr <= line_high)
			p_map[p_addr - line_low] = l_addr;
			
	}

	p_addr = line_low;
	do {
		l_addr = p_map[p_addr - line_low];
		if (l_addr != MEMSTICK_INVALID_BLOCK)
			snprintf(line, MEMSTICK_BMAP_LINE_SZ + 1,
				 "%04x       %04x\n", p_addr, l_addr);
		else if (ms_block_used(msb, p_addr))
			snprintf(line, MEMSTICK_BMAP_LINE_SZ + 1,
				 "%04x   disabled\n", p_addr);
		else
			snprintf(line, MEMSTICK_BMAP_LINE_SZ + 1,
				 "%04x   unmapped\n", p_addr);

		if ((MEMSTICK_BMAP_LINE_SZ - offset) >= count) {
			memcpy(buf + rv, line + offset, count);
			rv += count;
			break;
		}

		memcpy(buf + rv, line + offset, MEMSTICK_BMAP_LINE_SZ - offset);
		rv += MEMSTICK_BMAP_LINE_SZ - offset;
		count -= MEMSTICK_BMAP_LINE_SZ - offset;
		offset = 0;
		p_addr++;
	} while (p_addr <= line_high);
	mutex_unlock(&card->host->lock);

	kfree(p_map);
	return rv;
}

static int ms_block_erase(struct memstick_dev *card,
			  struct ms_block_data *msb,
			  unsigned short block)
{
	struct memstick_host *host = card->host;
	struct ms_register_addr t_addr = card->reg_addr;
	struct ms_block_reg_status stat;
	struct memstick_request req;
	struct ms_param_register param = {
		msb->system, {0, block >> 8, block & 0xff},
		MEMSTICK_CP_BLOCK, 0
	};

	int rc = 0;

	card->reg_addr = (struct ms_register_addr){
		0, sizeof(stat), 16, sizeof(param)
	};

	rc = memstick_set_rw_addr(card);

	if (rc) {
		card->reg_addr = t_addr;
		return rc;
	}

	memstick_init_req(&req, MS_TPC_WRITE_REG, (char*)(&param),
			  sizeof(param));
	memstick_queue_req(host, &req);
	memstick_get_req(host);
	if (req.error) {
		rc = -EIO;
		goto out;
	}

	memstick_init_req(&req, MS_TPC_SET_CMD,
			  (char*)&ms_block_cmd_block_erase, 1);
	memstick_queue_req(host, &req);
	memstick_get_req(host);
	if (req.error) {
		rc = -EIO;
		goto out;
	}

	memstick_init_req(&req, MS_TPC_READ_REG, (char*)(&stat), sizeof(stat));
	memstick_queue_req(host, &req);
	memstick_get_req(host);
	if (req.error) {
		rc = -EIO;
		goto out;
	}

	if (stat.interrupt & (MEMSTICK_INT_CMDNAK | MEMSTICK_INT_CED))
		rc = -EFAULT;
out:
	card->reg_addr = t_addr;

	if (!rc)
		rc = memstick_set_rw_addr(card);
	else
		memstick_set_rw_addr(card);

	return rc;
}

static int ms_block_read_page(struct memstick_dev *card,
			      struct ms_block_data *msb,
			      char *buf,
			      unsigned short block,
			      unsigned char page)
{
	struct memstick_host *host = card->host;
	struct ms_param_register param = {
		msb->system, {0, block >> 8, block & 0xff},
		MEMSTICK_CP_PAGE, page
	};
	struct memstick_request req;
	struct ms_block_reg_status stat;
	struct scatterlist sg;

	sg_init_one(&sg, buf, msb->page_size);

	memstick_init_req(&req, MS_TPC_WRITE_REG, (char*)(&param),
			  sizeof(param));
	memstick_queue_req(host, &req);
	memstick_get_req(host);
	if (req.error)
		return -EIO;

	memstick_init_req(&req, MS_TPC_SET_CMD,
			  (char*)&ms_block_cmd_block_read, 1);
	memstick_queue_req(host, &req);
	memstick_get_req(host);
	if (req.error)
		return -EIO;

	memstick_init_req(&req, MS_TPC_READ_REG, (char*)(&stat), sizeof(stat));
	memstick_queue_req(host, &req);
	memstick_get_req(host);
	if (req.error)
		return -EIO;

	if (stat.interrupt & MEMSTICK_INT_CMDNAK)
		return -EFAULT;

	if (stat.interrupt & MEMSTICK_INT_ERR
	    && (stat.status1 & (MEMSTICK_STATUS1_UCFG
			        | MEMSTICK_STATUS1_UCEX
			        | MEMSTICK_STATUS1_UCDT)))
		return -EFAULT;

	memstick_init_req_sg(&req, MS_TPC_READ_LONG_DATA, &sg);
	memstick_queue_req(host, &req);
	memstick_get_req(host);
	if (req.error)
		return -EIO;

	return 0;
}

static int ms_block_find_boot_blocks(struct memstick_dev *card,
				     struct ms_block_data *msb,
				     char **buf, unsigned short *boot_blocks)
{
	struct ms_boot_page *b_page;
	unsigned int b_cnt, bb_cnt = 0;
	int rc = 0;

	boot_blocks[0] = MEMSTICK_INVALID_BLOCK;
	boot_blocks[1] = MEMSTICK_INVALID_BLOCK;

	for (b_cnt = 0; b_cnt < MEMSTICK_MAX_BOOT_BLOCK; b_cnt++) {
		rc = ms_block_read_page(card, msb, buf[bb_cnt], b_cnt, 0);
		if (rc)
			break;

		b_page = (struct ms_boot_page*)buf[b_cnt];
		ms_block_fix_boot_page_endianness(b_page);
		if (b_page->header.block_id == MEMSTICK_BOOT_BLOCK_ID) {
			boot_blocks[bb_cnt] = b_cnt;
			bb_cnt++;
			if (bb_cnt == 2)
				break;
		}
	}

	if (!bb_cnt)
		return -EIO;

	return rc;
}


static int ms_block_fetch_bad_blocks(struct memstick_dev *card,
				     struct ms_block_data *msb,
				     unsigned short block)
{
	unsigned int start_page, page_cnt;
	unsigned short b_block;
	char *buf;
	int rc = 0, cnt;

	start_page = msb->boot_page.entry.disabled_block.start_addr;
	start_page += sizeof(msb->boot_page);
	page_cnt = start_page + msb->boot_page.entry.disabled_block.data_size
		   - 1;
	start_page /= msb->page_size;
	page_cnt /= msb->page_size;

	if (!msb->boot_page.entry.disabled_block.data_size)
		return 0;

	page_cnt = page_cnt - start_page + 1;
	buf = kmalloc(page_cnt * msb->page_size, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;

	for (cnt = 0; cnt < page_cnt; cnt++) {
		rc = ms_block_read_page(card, msb, buf + cnt * msb->page_size,
					block, start_page + cnt);
		if (rc)
			goto out_free_buf;
	}

	start_page = msb->boot_page.entry.disabled_block.start_addr;
	start_page += sizeof(msb->boot_page);
	start_page %= msb->page_size;

	for (page_cnt = 0;
	     page_cnt < msb->boot_page.entry.disabled_block.data_size;
	     page_cnt += 2) {
		b_block = be16_to_cpu(*(unsigned short*)(buf + start_page
							 + page_cnt));

		ms_block_mark_used(msb, b_block);
		printk(KERN_INFO "%s: physical block %d is disabled\n",
		       card->dev.bus_id, b_block);
	}

out_free_buf:
	kfree(buf);
	return rc;
}

static int ms_block_fetch_cis_idi(struct memstick_dev *card,
				  struct ms_block_data *msb,
				  unsigned short block)
{
	unsigned int start_page, page_cnt;
	char *buf;
	int rc = 0, cnt;

	start_page = msb->boot_page.entry.cis_idi.start_addr;
	start_page += sizeof(msb->boot_page);
	page_cnt = start_page + msb->boot_page.entry.cis_idi.data_size - 1;
	start_page /= msb->page_size;
	page_cnt /= msb->page_size;

	if (msb->boot_page.entry.cis_idi.data_size
	    < (sizeof(msb->cis_idi) + 0x100))
		return -ENODEV;

	page_cnt = page_cnt - start_page + 1;
	buf = kmalloc(page_cnt * msb->page_size, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;

	for (cnt = 0; cnt < page_cnt; cnt++) {
		rc = ms_block_read_page(card, msb, buf + cnt * msb->page_size,
					block, start_page + cnt);
		if (rc)
			goto out_free_buf;
	}

	start_page = msb->boot_page.entry.cis_idi.start_addr;
	start_page += sizeof(msb->boot_page);
	start_page %= msb->page_size;
	start_page += 0x100;

	memcpy(&msb->cis_idi, buf + start_page, sizeof(msb->cis_idi));
	
	rc = 0;

out_free_buf:
	kfree(buf);
	return rc;
}

static int ms_block_read_page_extra(struct memstick_dev *card,
				    struct ms_block_data *msb,
				    unsigned short block,
				    unsigned char page,
				    struct ms_register *reg)
{
	struct memstick_host *host = card->host;
	struct ms_param_register param = {
		msb->system, { 0, block >> 8, block & 0xff },
		MEMSTICK_CP_EXTRA, page
	};
	struct memstick_request req;

	memstick_init_req(&req, MS_TPC_WRITE_REG, (char*)(&param),
			  sizeof(param));
	memstick_queue_req(host, &req);
	memstick_get_req(host);
	if (req.error)
		return -EIO;

	memstick_init_req(&req, MS_TPC_SET_CMD,
			  (char*)&ms_block_cmd_block_read, 1);
	memstick_queue_req(host, &req);
	memstick_get_req(host);
	if (req.error)
		return -EIO;

	memstick_init_req(&req, MS_TPC_READ_REG, (char*)reg, sizeof(*reg) - 1);
	memstick_queue_req(host, &req);
	memstick_get_req(host);
	if (req.error)
		return -EIO;


	if (reg->status.interrupt & MEMSTICK_INT_CMDNAK)
		return -EFAULT;

	if ((reg->status.interrupt & MEMSTICK_INT_ERR)
	    && (reg->status.status1
		& (MEMSTICK_STATUS1_UCFG | MEMSTICK_STATUS1_UCEX
		   | MEMSTICK_STATUS1_UCDT)))
		return -EFAULT;

	return 0;
}

static int ms_block_fill_lut(struct memstick_dev *card,
			     struct ms_block_data *msb)
{
	struct ms_register_addr t_addr = card->reg_addr;
	struct ms_register reg;
	unsigned short b_cnt, l_addr, p_addr;
	int rc, wp = 0;

	card->reg_addr = (struct ms_register_addr){
		0, sizeof(reg) - 1, 16, sizeof(struct ms_param_register)
	};

	rc = memstick_set_rw_addr(card);
	if (rc)
		return rc;

	dev_dbg(&card->dev, "scanning %x blocks\n", msb->block_count);

	for (b_cnt = 0; b_cnt < msb->block_count; b_cnt++) {
		if (ms_block_used(msb, b_cnt))
			continue;

		rc = ms_block_read_page_extra(card, msb, b_cnt, 0, &reg);

		if (rc && rc != -EFAULT)
			return rc;

		if (rc) {
			dev_dbg(&card->dev, "failed read extra, block %x\n",
				b_cnt);
			ms_block_mark_used(msb, b_cnt);
			continue;
		}

		wp = reg.status.status0 & MEMSTICK_STATUS0_WP;

		if (!(reg.extra_data.overwrite_flag
		      & MEMSTICK_OVERWRITE_BLOCK)) {
			dev_dbg(&card->dev, "overwrite unset, block %x\n",
				b_cnt);
			ms_block_mark_used(msb, b_cnt);
			continue;
		}

		if(!(reg.extra_data.management_flag
		     & MEMSTICK_MANAGEMENT_TRANS_TABLE)) {
			dev_dbg(&card->dev, "trans table unset, block %x\n",
				b_cnt);

			if (!wp) {
				rc = ms_block_erase(card, msb, b_cnt);
				if (rc != -EFAULT)
					return rc;
			}
		}

		l_addr = be16_to_cpu(reg.extra_data.logical_address);
		if (MEMSTICK_INVALID_BLOCK == l_addr)
			continue;

		p_addr = ms_block_physical(msb, l_addr);
		if (MEMSTICK_INVALID_BLOCK != p_addr) {
			printk(KERN_WARNING "%s: logical block %d is dual-"
			       "mapped (%d, %d)\n", card->dev.bus_id, l_addr,
			       p_addr, b_cnt);
			if (!(reg.extra_data.overwrite_flag
			      & MEMSTICK_OVERWRITE_UPDATA)) {
				if (!wp) {
					rc = ms_block_erase(card, msb, b_cnt);
					if (rc != -EFAULT)
						return rc;
					printk(KERN_WARNING "%s: resolving "
					       "logical block %d to physical "
					       "%d\n", card->dev.bus_id,
					       l_addr, p_addr);
				}
			} else {
				rc = ms_block_read_page_extra(card, msb, p_addr,
							      0, &reg);
				if (rc)
					return rc;

				if (!(reg.extra_data.overwrite_flag
				      & MEMSTICK_OVERWRITE_UPDATA)) {
					msb->block_lut[l_addr] = b_cnt;
					ms_block_mark_used(msb, b_cnt);
					printk(KERN_WARNING "%s: resolving "
					       "logical block %d to physical "
					       "%d\n", card->dev.bus_id,
					       l_addr, b_cnt);
				} else {
					printk(KERN_WARNING "%s: resolving "
					       "logical block %d to physical "
					       "%d\n", card->dev.bus_id,
					       l_addr, p_addr);
					p_addr = b_cnt;
				}

				if (!wp) {
					rc = ms_block_erase(card, msb, p_addr);
					if (rc != -EFAULT)
						return rc;
				}

				ms_block_mark_unused(msb, p_addr);
			}
		} else if (l_addr < msb->block_count) {
			msb->block_lut[l_addr] = b_cnt;
			ms_block_mark_used(msb, b_cnt);
		}
	}

	card->reg_addr = t_addr;
	rc = memstick_set_rw_addr(card);
	if (rc)
		return rc;

	return wp ? -EROFS : 0;
}

static int ms_block_switch_to_parallel(struct memstick_dev *card,
				       struct ms_block_data *msb)
{
	struct memstick_host *host = card->host;
	struct ms_register_addr t_addr = card->reg_addr;
	struct ms_param_register param = {0x88, { 0 }, 0, 0};
	struct memstick_request req;
	int rc;

	card->reg_addr = (struct ms_register_addr){
		0, sizeof(struct ms_register) - 1, 16, sizeof(param)
	};

	rc = memstick_set_rw_addr(card);
	if (rc) {
		card->reg_addr = t_addr;
		return rc;
	}

	memstick_init_req(&req, MS_TPC_WRITE_REG, (char*)(&param),
			  sizeof(param));
	memstick_queue_req(host, &req);
	memstick_get_req(host);
	if (!req.error) {
		msb->system = 0x88;
		host->ios.interface = MEMSTICK_PARALLEL;
		host->set_ios(host, &host->ios);
	}

	card->reg_addr = t_addr;
	rc = memstick_set_rw_addr(card);
	if (rc) {
		msb->system = 0x80;
		host->ios.interface = MEMSTICK_SERIAL;
		host->set_ios(host, &host->ios);
		if (memstick_set_rw_addr(card))
			rc = -EIO;
		else
			rc = -EFAULT;
	}
	return rc;
}

static int ms_block_init_card(struct memstick_dev *card,
			      struct ms_block_data *msb)
{
	struct memstick_host *host = card->host;
	struct ms_register_addr reg_addr = {
		0, sizeof(struct ms_block_reg_status),
		16, sizeof(struct ms_param_register)
	};
	char *buf;
	char *buf_ptr[2];
	unsigned short boot_blocks[2];
	int rc;

	msb->page_size = sizeof(struct ms_boot_page);
	msb->system = 0x80;

	memstick_init_req(&msb->req[0], MS_TPC_SET_CMD,
			  (char*)(&ms_block_cmd_reset), 1);
	msb->req[0].need_card_int = 0;
	memstick_queue_req(host, &msb->req[0]);
	memstick_get_req(host);

	if (msb->req[0].error)
		return -ENODEV;

	if (host->caps & MEMSTICK_CAP_PARALLEL) {
		rc = ms_block_switch_to_parallel(card, msb);
		if (rc == -EFAULT) {
			printk(KERN_WARNING "%s: could not switch to "
			       "parallel interface\n", card->dev.bus_id);
			rc = 0;
		} else if (rc)
			return rc;
	}

	card->reg_addr = reg_addr;
	rc = memstick_set_rw_addr(card);
	if (rc)
		return rc;

	dev_dbg(&card->dev, "page size %d\n", msb->page_size);
	buf = kmalloc(2 * msb->page_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
 	buf_ptr[0] = buf;
	buf_ptr[1] = buf + msb->page_size;
 
	rc = ms_block_find_boot_blocks(card, msb, buf_ptr, boot_blocks);
	if (rc || boot_blocks[0] == MEMSTICK_INVALID_BLOCK)
		goto out_free_buf;

	memcpy(&msb->boot_page, buf_ptr[0], sizeof(msb->boot_page));
	msb->block_count = msb->boot_page.attr.number_of_blocks;
	msb->log_block_count = msb->boot_page.attr.number_of_effective_blocks;
	msb->page_size = msb->boot_page.attr.page_size;
	msb->block_map = kzalloc(msb->block_count / 8 + 1, GFP_KERNEL);
	msb->block_lut = kmalloc(2 * msb->block_count, GFP_KERNEL);

	if (!msb->block_map || !msb->block_lut) {
		rc = -ENOMEM;
		goto out_free_buf;
	}

	for (rc = 0; rc < msb->block_count; rc++)
		msb->block_lut[rc] = MEMSTICK_INVALID_BLOCK;

	ms_block_mark_used(msb, boot_blocks[0]);
	if (boot_blocks[1] != MEMSTICK_INVALID_BLOCK)
		ms_block_mark_used(msb, boot_blocks[1]);

	rc = ms_block_fetch_bad_blocks(card, msb, boot_blocks[0]);

	if (rc && boot_blocks[1] != MEMSTICK_INVALID_BLOCK) {
		memcpy(&msb->boot_page, buf_ptr[1],
		       sizeof(msb->boot_page));
		boot_blocks[0] = MEMSTICK_INVALID_BLOCK;
		rc = ms_block_fetch_bad_blocks(card, msb, boot_blocks[1]);
	}

	if (rc)
		goto out_free_buf;

	if (boot_blocks[0] != MEMSTICK_INVALID_BLOCK) {
		rc = ms_block_fetch_cis_idi(card, msb, boot_blocks[0]);

		if (rc && boot_blocks[1] != MEMSTICK_INVALID_BLOCK) {
			memcpy(&msb->boot_page, buf_ptr[1],
			       sizeof(msb->boot_page));
			rc = ms_block_fetch_cis_idi(card, msb, boot_blocks[1]);
		}
	} else if (boot_blocks[1] != MEMSTICK_INVALID_BLOCK)
		rc = ms_block_fetch_cis_idi(card, msb, boot_blocks[1]);

	if (rc)
		goto out_free_buf;

	rc = ms_block_fill_lut(card, msb);
	if (rc && rc != -EROFS)
		goto out_free_buf;



	return 0;
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

static int ms_block_create_rel_table_attr(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	int rc;

	msb->dev_attr_logical_block_map = (struct bin_attribute){
		.attr = {
			.name = "logical_block_map",
			.mode = S_IRUGO,
			.owner = THIS_MODULE
		},
		.size = MEMSTICK_BMAP_LINE_SZ * msb->log_block_count,
		.read = ms_block_log_block_map_read
	};
	msb->dev_attr_physical_block_map = (struct bin_attribute){
		.attr = {
			.name = "physical_block_map",
			.mode = S_IRUGO,
			.owner = THIS_MODULE
		},
		.size = MEMSTICK_BMAP_LINE_SZ * msb->block_count,
		.read = ms_block_phys_block_map_read
	};

	rc = sysfs_create_bin_file(&card->dev.kobj,
				   &msb->dev_attr_logical_block_map);
	if (rc)
		return rc;

	rc = sysfs_create_bin_file(&card->dev.kobj,
				   &msb->dev_attr_physical_block_map);
	if (rc) {
		sysfs_remove_bin_file(&card->dev.kobj,
				      &msb->dev_attr_logical_block_map);
		return rc;
	}

	return 0;
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

	rc = device_create_file(&card->dev, &dev_attr_cis_idi);
	if (rc)
		goto out_remove_boot_attr;

	rc = ms_block_create_rel_table_attr(card);
	if (!rc)
		return 0;

	device_remove_file(&card->dev, &dev_attr_cis_idi);
out_remove_boot_attr:
	device_remove_file(&card->dev, &dev_attr_boot_attr);
out_free:
	kfree(msb->block_map);
	kfree(msb->block_lut);
	kfree(msb);
	memstick_set_drvdata(card, NULL);
	return rc;
}

static void ms_block_remove(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	memstick_set_drvdata(card, NULL);
	sysfs_remove_bin_file(&card->dev.kobj,
			      &msb->dev_attr_physical_block_map);
	sysfs_remove_bin_file(&card->dev.kobj,
			      &msb->dev_attr_logical_block_map);
	device_remove_file(&card->dev, &dev_attr_cis_idi);
	device_remove_file(&card->dev, &dev_attr_boot_attr);
	kfree(msb->block_map);
	kfree(msb->block_lut);
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
