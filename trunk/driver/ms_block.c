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
#include <linux/idr.h>
#include <linux/hdreg.h>
#include <linux/kthread.h>
#include <linux/random.h>
#include "linux/memstick.h"

#define DRIVER_NAME "ms_block"
#define DRIVER_VERSION "0.2"

static int major = 0;
static int unsafe_resume = 0;
module_param(major, int, 0644);
module_param(unsafe_resume, int, 0644);

#define MS_BLOCK_MAX_SEGS      32
#define MS_BLOCK_MAX_PAGES     ((2 << 16) - 1)

#define MS_BLOCK_MAX_BOOT_ADDR 0x000c
#define MS_BLOCK_BOOT_ID       0x0001
#define MS_BLOCK_INVALID       0xffff

#define MS_BLOCK_MAP_LINE_SZ   16

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

enum write_state {
	NOTHING = 0,
	GET_BLOCK,
	DEL_DST,
	DEL_SRC,
	COPY_PAGES,
	WRITE_PAGES,
	WRITE_EXTRA,
	SET_TABLE
};

struct ms_block_data {
	struct memstick_dev      *card;
	unsigned int             usage_count;
	unsigned int             *block_map;
	unsigned short           *block_lut;

	struct gendisk           *disk;
	request_queue_t          *queue;
	spinlock_t               q_lock;
	wait_queue_head_t        q_wait;
	struct task_struct       *q_thread;

	unsigned short           block_count;
	unsigned short           log_block_count;
	unsigned short           page_size;
	unsigned char            block_psize;
	unsigned char            system;
	unsigned char            read_only:1,
				 active:1,
				 has_request:1,
				 physical_src:1;

	struct ms_boot_attr_info boot_attr;
	struct ms_cis_idi        cis_idi;

	struct bin_attribute     dev_attr_logical_block_map;
	struct bin_attribute     dev_attr_physical_block_map;

	int                      (*mrq_handler)(struct memstick_dev *card,
						struct memstick_request **mrq);

	struct scatterlist       req_sg[MS_BLOCK_MAX_SEGS];
	unsigned short           seg_cnt;
	unsigned short           current_seg;
	unsigned short           current_page;
	struct ms_extra_data_register current_extra;
	unsigned short           src_block;
	unsigned short           dst_block;
	unsigned short           total_page_cnt;
	unsigned char            page_off;
	unsigned char            page_cnt;
	unsigned char            copy_pos;
	enum write_state         w_state;
};

static DEFINE_IDR(ms_block_disk_idr);
static DEFINE_MUTEX(ms_block_disk_lock);

/*** Lookup ***/

static void ms_block_mark_used(struct ms_block_data *msb,
			       unsigned short phy_block)
{
	unsigned int block_bit = 1U << (phy_block & 0x1f);

	if (phy_block < msb->block_count) {
		phy_block >>= 5;
		msb->block_map[phy_block] |= block_bit;
	}
}

static void ms_block_mark_unused(struct ms_block_data *msb,
				 unsigned short phy_block)
{
	unsigned int block_bit = 1U << (phy_block & 0x1f);

	if (phy_block < msb->block_count) {
		phy_block >>= 5;
		msb->block_map[phy_block] &= ~block_bit;
	}
}

static int ms_block_used(struct ms_block_data *msb, unsigned short phy_block)
{
	unsigned int block_bit = 1U << (phy_block & 0x1f);

	if (phy_block < msb->block_count) {
		phy_block >>= 5;
		return msb->block_map[phy_block] & block_bit ? 1 : 0;
	} else
		return 1;
}

static unsigned short ms_block_physical(struct ms_block_data *msb,
					unsigned short block)
{
	if (block < msb->log_block_count)
		return msb->block_lut[block];
	else
		return MS_BLOCK_INVALID;
}

static unsigned short ms_block_get_unused(struct ms_block_data *msb)
{
	unsigned short b_cnt, rc;
	unsigned int t_val, r_pos = random32();

	r_pos %= msb->block_count;
	b_cnt = r_pos >> 5;
	t_val = msb->block_map[b_cnt] | ((1 << (r_pos & 0x1f)) - 1);
	if (t_val != 0xffffffffU) {
		rc = ffz(t_val) + (b_cnt << 5);
		if (rc < msb->block_count)
			return rc;
	}

	for (++b_cnt; b_cnt != (r_pos >> 5); ++b_cnt) {
		if (b_cnt > (msb->block_count >> 5))
			b_cnt = 0;
		if (msb->block_map[b_cnt] != 0xffffffffU) {
			rc = ffz(msb->block_map[b_cnt]) + (b_cnt << 5);
			if (rc < msb->block_count)
				return rc;
		}
	}

	return MS_BLOCK_INVALID;
}

/*** Block device ***/

static int ms_block_bd_open(struct inode *inode, struct file *filp)
{
	struct gendisk *disk = inode->i_bdev->bd_disk;
	struct ms_block_data *msb = disk->private_data;
	int rc = -ENXIO;

	mutex_lock(&ms_block_disk_lock);
	
	printk(KERN_INFO "disk open\n");
	if (msb && msb->card) {
		msb->usage_count++;
		if ((filp->f_mode & FMODE_WRITE) && msb->read_only)
			rc = -EROFS;
		else
			rc = 0;
	}

	mutex_unlock(&ms_block_disk_lock);

	return rc;
}

static int ms_block_disk_release(struct gendisk *disk)
{
	struct ms_block_data *msb = disk->private_data;
	int disk_id = disk->first_minor >> MEMSTICK_PART_SHIFT;

	mutex_lock(&ms_block_disk_lock);

	printk(KERN_INFO "disk release\n");

	if (msb->usage_count) {
		msb->usage_count--;
		if (!msb->usage_count) {
			kfree(msb);
			disk->private_data = NULL;
			idr_remove(&ms_block_disk_idr, disk_id);
			put_disk(disk);	
		}
	}

	mutex_unlock(&ms_block_disk_lock);

	return 0;
}

static int ms_block_bd_release(struct inode *inode, struct file *filp)
{
	struct gendisk *disk = inode->i_bdev->bd_disk;
	return ms_block_disk_release(disk);
}

static int ms_block_bd_getgeo(struct block_device *bdev,
			      struct hd_geometry *geo)
{
	struct ms_block_data *msb = bdev->bd_disk->private_data;

	geo->heads = msb->cis_idi.current_logical_heads;
	geo->sectors = msb->cis_idi.current_sectors_per_track;
	geo->cylinders = msb->cis_idi.current_logical_cylinders;

	return 0;
}

static struct block_device_operations ms_block_bdops = {
	.open    = ms_block_bd_open,
	.release = ms_block_bd_release,
	.getgeo  = ms_block_bd_getgeo,
	.owner   = THIS_MODULE
};

/*** Information ***/ 

static ssize_t ms_boot_attr_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct ms_block_data *msb
		= memstick_get_drvdata(container_of(dev, struct memstick_dev,
						    dev));
	struct ms_boot_attr_info *ms_attr = &msb->boot_attr;
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

static ssize_t ms_block_log_block_map_read(struct kobject *kobj, char *buf,
					   loff_t offset, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct memstick_dev *card = container_of(dev, struct memstick_dev, dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);
	loff_t line_low = offset / MS_BLOCK_MAP_LINE_SZ;
	loff_t line_high = (offset + count - 1) / MS_BLOCK_MAP_LINE_SZ;
	char line[MS_BLOCK_MAP_LINE_SZ + 1];
	unsigned short p_addr;
	ssize_t rv = 0;

	if (!count)
		return 0;

	offset -= line_low * MS_BLOCK_MAP_LINE_SZ;

	mutex_lock(&card->host->lock);
	do {
		p_addr = ms_block_physical(msb, line_low);
		if (p_addr == MS_BLOCK_INVALID)
			snprintf(line, MS_BLOCK_MAP_LINE_SZ + 1,
				 "%04x   unmapped\n", (unsigned short)line_low);
		else
			snprintf(line, MS_BLOCK_MAP_LINE_SZ + 1,
				 "%04x       %04x\n", (unsigned short)line_low,
				 p_addr);

		if ((MS_BLOCK_MAP_LINE_SZ - offset) >= count) {
			memcpy(buf + rv, line + offset, count);
			rv += count;
			break;
		} 

		memcpy(buf + rv, line + offset, MS_BLOCK_MAP_LINE_SZ - offset);
		rv += MS_BLOCK_MAP_LINE_SZ - offset;
		count -= MS_BLOCK_MAP_LINE_SZ - offset;
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
	loff_t line_low = offset / MS_BLOCK_MAP_LINE_SZ;
	loff_t line_high = (offset + count - 1) / MS_BLOCK_MAP_LINE_SZ;
	char line[MS_BLOCK_MAP_LINE_SZ + 1];
	unsigned short p_addr, l_addr;
	unsigned short *p_map;
	ssize_t rv = 0;

	if (!count)
		return 0;

	p_map = kmalloc((line_high - line_low + 1) * 2, GFP_KERNEL);
	if (!p_map)
		return -ENOMEM;

	for (p_addr = 0; p_addr < (line_high - line_low + 1); p_addr++)
		p_map[p_addr] = MS_BLOCK_INVALID;

	offset -= line_low * MS_BLOCK_MAP_LINE_SZ;

	mutex_lock(&card->host->lock);
	for (l_addr = 0; l_addr < msb->log_block_count; l_addr++) {
		p_addr = ms_block_physical(msb, l_addr);
		if (p_addr >= line_low && p_addr <= line_high)
			p_map[p_addr - line_low] = l_addr;
			
	}

	p_addr = line_low;
	do {
		l_addr = p_map[p_addr - line_low];
		if (l_addr != MS_BLOCK_INVALID)
			snprintf(line, MS_BLOCK_MAP_LINE_SZ + 1,
				 "%04x       %04x\n", p_addr, l_addr);
		else if (ms_block_used(msb, p_addr))
			snprintf(line, MS_BLOCK_MAP_LINE_SZ + 1,
				 "%04x   disabled\n", p_addr);
		else
			snprintf(line, MS_BLOCK_MAP_LINE_SZ + 1,
				 "%04x   unmapped\n", p_addr);

		if ((MS_BLOCK_MAP_LINE_SZ - offset) >= count) {
			memcpy(buf + rv, line + offset, count);
			rv += count;
			break;
		}

		memcpy(buf + rv, line + offset, MS_BLOCK_MAP_LINE_SZ - offset);
		rv += MS_BLOCK_MAP_LINE_SZ - offset;
		count -= MS_BLOCK_MAP_LINE_SZ - offset;
		offset = 0;
		p_addr++;
	} while (p_addr <= line_high);
	mutex_unlock(&card->host->lock);

	kfree(p_map);
	return rv;
}


/*** Protocol handlers ***/

static int h_ms_block_copy_read(struct memstick_dev *card,
				struct memstick_request **mrq);
static int h_ms_block_erase(struct memstick_dev *card,
			    struct memstick_request **mrq);
static int h_ms_block_write_next_state(struct memstick_dev *card,
				       struct memstick_request **mrq);
static int h_ms_block_set_extra(struct memstick_dev *card,
				struct memstick_request **mrq);

static int h_ms_block_default(struct memstick_dev *card,
			      struct memstick_request **mrq)
{
	if ((*mrq)->error) {
		complete(&card->mrq_complete);
		return (*mrq)->error;
	} else
		return h_ms_block_write_next_state(card, mrq);
}

static int h_ms_block_write_single(struct memstick_dev *card,
				   struct memstick_request **mrq)
{
	unsigned char t_val = MS_CMD_BLOCK_WRITE;

	if ((*mrq)->error) {
		complete(&card->mrq_complete);
		return (*mrq)->error;
	}

	switch ((*mrq)->tpc) {
	case MS_TPC_WRITE_REG:
		memstick_init_req(*mrq, MS_TPC_SET_CMD, &t_val, 1);
		(*mrq)->get_int_reg = 1;
		break;
	case MS_TPC_SET_CMD:
		t_val = (*mrq)->int_reg;
		memstick_init_req(*mrq, MS_TPC_GET_INT, NULL, 1);
		if (card->host->caps & MEMSTICK_CAP_AUTO_GET_INT)
			goto has_int_reg;
		break;
	case MS_TPC_GET_INT:
		t_val = (*mrq)->data[0];
has_int_reg:
		if (t_val & MEMSTICK_INT_CMDNAK)
			(*mrq)->error = -EFAULT;
		else if (t_val & MEMSTICK_INT_ERR)
			(*mrq)->error = -EROFS;

		if ((*mrq)->error) {
			complete(&card->mrq_complete);
			return (*mrq)->error;
		}

		if (t_val & MEMSTICK_INT_CED)
			return h_ms_block_write_next_state(card, mrq);

		break;
	default:
		BUG();
	}
	return 0;
}

static int h_ms_block_copy_write(struct memstick_dev *card,
				 struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_param_register param;
	unsigned short src_phy_block;
	unsigned char t_val = MS_CMD_BLOCK_WRITE;

	if ((*mrq)->error) {
		complete(&card->mrq_complete);
		return (*mrq)->error;
	}

	switch ((*mrq)->tpc) {
	case MS_TPC_WRITE_REG:
		memstick_init_req(*mrq, MS_TPC_SET_CMD, &t_val, 1);
		(*mrq)->get_int_reg = 1;
		break;
	case MS_TPC_SET_CMD:
		t_val = (*mrq)->int_reg;
		memstick_init_req(*mrq, MS_TPC_GET_INT, NULL, 1);
		if (card->host->caps & MEMSTICK_CAP_AUTO_GET_INT)
			goto has_int_reg;
		break;
	case MS_TPC_GET_INT:
		t_val = (*mrq)->data[0];
has_int_reg:
		if (t_val & MEMSTICK_INT_CMDNAK)
			(*mrq)->error = -EFAULT;
		else if (t_val & MEMSTICK_INT_ERR)
			(*mrq)->error = -EROFS;

		if ((*mrq)->error) {
			complete(&card->mrq_complete);
			return (*mrq)->error;
		}

		if (t_val & MEMSTICK_INT_CED) {
			msb->copy_pos++;
			if (msb->copy_pos == msb->page_off)
				msb->copy_pos = msb->page_off + msb->page_cnt;
			if (msb->copy_pos == msb->block_psize)
				return h_ms_block_write_next_state(card, mrq);

			src_phy_block = msb->src_block;
			if (!msb->physical_src)
				src_phy_block = ms_block_physical(msb,
								  src_phy_block);

			card->next_request = h_ms_block_copy_read;
			param = (struct ms_param_register){
				.system = msb->system,
				.block_address_msb = 0,
				.block_address = cpu_to_be16(src_phy_block),
				.cp = MEMSTICK_CP_PAGE,
				.page_address = msb->copy_pos
			};
			memstick_init_req(*mrq, MS_TPC_WRITE_REG,
					  &param, sizeof(param));
		}
		break;
	default:
		BUG();
	}
	return 0;
}

static int h_ms_block_copy_read(struct memstick_dev *card,
				struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_status_register *status;
	struct ms_param_register param;
	unsigned char t_val = MS_CMD_BLOCK_READ;

	if ((*mrq)->error) {
		complete(&card->mrq_complete);
		return (*mrq)->error;
	}

	switch ((*mrq)->tpc) {
	case MS_TPC_WRITE_REG:
		memstick_init_req(*mrq, MS_TPC_SET_CMD, &t_val, 1);
		break;
	case MS_TPC_SET_CMD:
		memstick_init_req(*mrq, MS_TPC_READ_REG, NULL,
				  sizeof(struct ms_status_register));
		break;
	case MS_TPC_READ_REG:
		status = (struct ms_status_register*)card->current_mrq.data;

		if (status->interrupt & MEMSTICK_INT_CMDNAK)
			(*mrq)->error = -EFAULT;

		if ((status->interrupt & MEMSTICK_INT_ERR)
		    && (status->status1
			& (MEMSTICK_STATUS1_UCFG | MEMSTICK_STATUS1_UCEX
			   | MEMSTICK_STATUS1_UCDT)))
			(*mrq)->error = -EFAULT;

		if ((*mrq)->error) {
			complete(&card->mrq_complete);
			return (*mrq)->error;
		}

		card->next_request = h_ms_block_copy_write;
		param = (struct ms_param_register){
			.system = msb->system,
			.block_address_msb = 0,
			.block_address = cpu_to_be16(msb->dst_block),
			.cp = MEMSTICK_CP_PAGE,
			.page_address = msb->copy_pos
		};
		memstick_init_req(*mrq, MS_TPC_WRITE_REG,
				  &param, sizeof(param));
		break;
	default:
		BUG();
	};

	return 0;
}

static int h_ms_block_write_pages(struct memstick_dev *card,
				  struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_param_register param;
	struct scatterlist t_sg = { 0 };
	size_t t_offset;
	unsigned char t_val = MS_CMD_BLOCK_WRITE;

	if ((*mrq)->error) {
		complete(&card->mrq_complete);
		return (*mrq)->error;
	}

	switch ((*mrq)->tpc) {
	case MS_TPC_WRITE_REG:
		t_offset = msb->req_sg[msb->current_seg].offset;
		t_offset += msb->current_page * msb->page_size;
		t_sg.page = nth_page(msb->req_sg[msb->current_seg].page,
				     t_offset >> PAGE_SHIFT);
		t_sg.offset = offset_in_page(t_offset);
		t_sg.length = msb->page_size;
		memstick_init_req_sg(*mrq, MS_TPC_WRITE_LONG_DATA, &t_sg);
		break;
	case MS_TPC_WRITE_LONG_DATA:
		memstick_init_req(*mrq, MS_TPC_SET_CMD, &t_val, 1);
		(*mrq)->get_int_reg = 1;
		break;
	case MS_TPC_SET_CMD:
		t_val = (*mrq)->int_reg;
		memstick_init_req(*mrq, MS_TPC_GET_INT, NULL, 1);
		if (card->host->caps & MEMSTICK_CAP_AUTO_GET_INT)
			goto has_int_reg;
		break;
	case MS_TPC_GET_INT:
		t_val = (*mrq)->data[0];
has_int_reg:
		if (t_val & MEMSTICK_INT_CMDNAK)
			(*mrq)->error = -EFAULT;
		else if (t_val & MEMSTICK_INT_ERR)
			(*mrq)->error = -EROFS;

		if ((*mrq)->error) {
			complete(&card->mrq_complete);
			return (*mrq)->error;
		}

		if (t_val & MEMSTICK_INT_CED) {
			msb->current_page++;

			if (msb->current_page
			    == (msb->req_sg[msb->current_seg].length
				/ msb->page_size)) {
				msb->current_page = 0;
				msb->current_seg++;
			}

			msb->copy_pos++;
			if (msb->copy_pos == (msb->page_off + msb->page_cnt))
				return h_ms_block_write_next_state(card, mrq);

			param = (struct ms_param_register){
				.system = msb->system,
				.block_address_msb = 0,
				.block_address = cpu_to_be16(msb->dst_block),
				.cp = MEMSTICK_CP_PAGE,
				.page_address = msb->copy_pos
			};
			memstick_init_req(*mrq, MS_TPC_WRITE_REG,
					  &param, sizeof(param));
		}
		break;
	default:
		BUG();
	}
	return 0;
}

static int h_ms_block_write_next_state(struct memstick_dev *card,
				       struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned short r_pages, seg_pos, src_phy_block;
	struct ms_param_register param;

	switch(msb->w_state) {
	case NOTHING:
		break;
	case GET_BLOCK:
get_next_block:
		if (msb->current_seg == msb->seg_cnt)
			break;

		r_pages = msb->req_sg[msb->current_seg].length
			  / msb->page_size - msb->current_page;
		msb->page_cnt = msb->block_psize - msb->page_off;

		for (seg_pos = msb->current_seg + 1; seg_pos < msb->seg_cnt;
		     seg_pos++) {
			r_pages += msb->req_sg[seg_pos].length
				   / msb->page_size;

			if (r_pages >= msb->page_cnt)
				break;
		}

		if (!r_pages)
			break;

		if (r_pages < msb->page_cnt)
			msb->page_cnt = r_pages;
		msb->dst_block = ms_block_get_unused(msb);
		if (msb->dst_block == MS_BLOCK_INVALID) {
			(*mrq)->error = -EFAULT;
			break;
		}
		card->next_request = h_ms_block_erase;
		param = (struct ms_param_register){
			.system = msb->system,
			.block_address_msb = 0,
			.block_address = cpu_to_be16(msb->dst_block),
			.cp = MEMSTICK_CP_BLOCK,
			.page_address = 0
		};
		msb->w_state = DEL_DST;

		memstick_init_req(*mrq, MS_TPC_WRITE_REG, &param,
				  sizeof(param));
		dev_dbg(&card->dev, "GET_BLOCK - erase %x\n", msb->dst_block);
		return 0;
	case DEL_DST:
		if (msb->page_off) {
			msb->copy_pos = 0;
			msb->w_state = COPY_PAGES;
		} else if (msb->page_cnt < msb->block_psize) {
			msb->copy_pos = msb->page_cnt;
			msb->w_state = COPY_PAGES;
		} else
			msb->w_state = WRITE_PAGES;

		src_phy_block = ms_block_physical(msb, msb->src_block);
		if (src_phy_block == MS_BLOCK_INVALID)
			msb->w_state = WRITE_PAGES;

		dev_dbg(&card->dev, "DEL_DST - copy_pos %x, "
			"page_off %x, src_block %x, "
			"src_phy_block %x\n", msb->copy_pos,
			msb->page_off, msb->src_block, src_phy_block);

		if (msb->w_state == COPY_PAGES) {
			card->next_request = h_ms_block_copy_read;
			param = (struct ms_param_register){
				.system = msb->system,
				.block_address_msb = 0,
				.block_address = cpu_to_be16(src_phy_block),
				.cp = MEMSTICK_CP_PAGE,
				.page_address = msb->copy_pos
			};
			memstick_init_req(*mrq, MS_TPC_WRITE_REG,
					  &param, sizeof(param));
			return 0;
		}
		/* Deliberate fall-through */
	case COPY_PAGES:
		msb->w_state = WRITE_PAGES;
		card->next_request = h_ms_block_write_pages;
		msb->copy_pos = msb->page_off;
		param = (struct ms_param_register){
			.system = msb->system,
			.block_address_msb = 0,
			.block_address = cpu_to_be16(msb->dst_block),
			.cp = MEMSTICK_CP_PAGE,
			.page_address = msb->copy_pos
		};
		memstick_init_req(*mrq, MS_TPC_WRITE_REG,
				  &param, sizeof(param));
		dev_dbg(&card->dev, "COPY_PAGES - copy_pos %x, page_off %x\n",
			msb->copy_pos, msb->page_off);
		return 0;
	case WRITE_PAGES:
		msb->current_extra = (struct ms_extra_data_register){
			.overwrite_flag = 0xf8,
			.management_flag = 0xff,
			.logical_address = cpu_to_be16(msb->src_block)
		};
		msb->w_state = WRITE_EXTRA;
		card->next_request = h_ms_block_set_extra;
		card->reg_addr = (struct ms_register_addr){
			card->reg_addr.r_offset,
			card->reg_addr.r_length,
			offsetof(struct ms_register, extra_data),
			sizeof(struct ms_extra_data_register),
		};
		memstick_init_req(*mrq, MS_TPC_SET_RW_REG_ADRS,
				  &card->reg_addr, sizeof(card->reg_addr));
		dev_dbg(&card->dev, "WRITE_PAGES\n");
		return 0;
	case WRITE_EXTRA:
		card->next_request = h_ms_block_write_single;
		param = (struct ms_param_register){
			.system = msb->system,
			.block_address_msb = 0,
			.block_address = cpu_to_be16(msb->dst_block),
			.cp = MEMSTICK_CP_EXTRA,
			.page_address = 0
		};
		msb->w_state = SET_TABLE;

		memstick_init_req(*mrq, MS_TPC_WRITE_REG,
				  &param, sizeof(param));
		dev_dbg(&card->dev, "WRITE_EXTRA - dst_block %x\n",
			msb->dst_block);
		return 0;
	case SET_TABLE:
		msb->total_page_cnt += msb->page_cnt;
		src_phy_block = ms_block_physical(msb, msb->src_block);
		msb->block_lut[msb->src_block] = msb->dst_block;
		ms_block_mark_used(msb, msb->dst_block);

		dev_dbg(&card->dev, "SET_TABLE - src_block %x, "
			"src_phy_block %x, dst_block %x\n", msb->src_block,
			src_phy_block, msb->dst_block);

		if (src_phy_block != MS_BLOCK_INVALID) {
			ms_block_mark_unused(msb, src_phy_block);
			card->next_request = h_ms_block_erase;
			param = (struct ms_param_register){
				.system = msb->system,
				.block_address_msb = 0,
				.block_address = cpu_to_be16(src_phy_block),
				.cp = MEMSTICK_CP_BLOCK,
				.page_address = 0
			};
			msb->w_state = DEL_SRC;

			memstick_init_req(*mrq, MS_TPC_WRITE_REG,
					  &param, sizeof(param));
			return 0;
		}
		/* Deliberate fall-through */
	case DEL_SRC:
		msb->page_off += msb->page_cnt;
		if (msb->page_off == msb->block_psize) {
			msb->page_off = 0;
			msb->src_block++;
		}
		goto get_next_block;
	default:
		BUG();
	}

	complete(&card->mrq_complete);
	return -EAGAIN;
}

static int h_ms_block_req_init(struct memstick_dev *card,
			       struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	*mrq = &card->current_mrq;
	card->next_request = msb->mrq_handler;
	return 0;
}


static int h_ms_block_get_ro(struct memstick_dev *card,
			     struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if ((*mrq)->error) {
		complete(&card->mrq_complete);
		return (*mrq)->error;
	}

	if (((struct ms_status_register*)(*mrq)->data)->status0
	    & MEMSTICK_STATUS0_WP)
		msb->read_only = 1;
	else
		msb->read_only = 0;

	complete(&card->mrq_complete);
	return -EAGAIN;
}

static int h_ms_block_read_pages(struct memstick_dev *card,
				 struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_status_register *status;
	struct scatterlist t_sg = { 0 };
	size_t t_offset;
	unsigned short src_phy_block;
	unsigned char t_val = MS_CMD_BLOCK_READ;
	struct ms_param_register param = {
		.system = msb->system,
		.block_address_msb = 0,
		.block_address = 0,
		.cp = MEMSTICK_CP_PAGE,
		.page_address = 0
	};

	if ((*mrq)->error) {
		complete(&card->mrq_complete);
		return (*mrq)->error;
	}

	switch ((*mrq)->tpc) {
	case MS_TPC_WRITE_REG:
		memstick_init_req(*mrq, MS_TPC_SET_CMD, &t_val,
				  1);
		return 0;
	case MS_TPC_SET_CMD:
		memstick_init_req(*mrq, MS_TPC_READ_REG, NULL,
				  sizeof(struct ms_status_register));
		return 0;
	case MS_TPC_READ_REG:
		status = (struct ms_status_register*)card->current_mrq.data;

		if (status->interrupt & MEMSTICK_INT_CMDNAK)
			(*mrq)->error = -EFAULT;

		if ((status->interrupt & MEMSTICK_INT_ERR)
		    && (status->status1
			& (MEMSTICK_STATUS1_UCFG | MEMSTICK_STATUS1_UCEX
			   | MEMSTICK_STATUS1_UCDT)))
			(*mrq)->error = -EFAULT;

		if ((*mrq)->error) {
			complete(&card->mrq_complete);
			return (*mrq)->error;
		}

		t_offset = msb->req_sg[msb->current_seg].offset;
		t_offset += msb->current_page * msb->page_size;
		t_sg.page = nth_page(msb->req_sg[msb->current_seg].page,
				     t_offset >> PAGE_SHIFT);
		t_sg.offset = offset_in_page(t_offset);
		t_sg.length = msb->page_size;
		memstick_init_req_sg(*mrq, MS_TPC_READ_LONG_DATA, &t_sg);
		return 0;
	case MS_TPC_READ_LONG_DATA:
skip_page:
		msb->total_page_cnt++;
		msb->current_page++;

		if (msb->current_page
		    == (msb->req_sg[msb->current_seg].length
			/ msb->page_size)) {
			msb->current_page = 0;
			msb->current_seg++;

			if (msb->current_seg == msb->seg_cnt) {
				complete(&card->mrq_complete);
				return -EAGAIN;
			}
		}
		msb->page_off++;
		if (msb->page_off == msb->block_psize) {
			msb->page_off = 0; 
			msb->src_block++;
		}

		src_phy_block = msb->src_block;
		if (!msb->physical_src)
			src_phy_block = ms_block_physical(msb, src_phy_block);

		/* unmapped blocks are considered undefined, yet legal */
		if (src_phy_block == MS_BLOCK_INVALID)
			goto skip_page;

		param.block_address = cpu_to_be16(src_phy_block);
		param.page_address = msb->page_off;

		memstick_init_req(*mrq, MS_TPC_WRITE_REG, &param,
				  sizeof(param));
		return 0;
	default:
		BUG();
	}
}

static int h_ms_block_get_extra(struct memstick_dev *card,
				struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_register_addr reg_addr = {
		0,
		sizeof(struct ms_status_register),
		card->reg_addr.w_offset,
		card->reg_addr.w_length
	};

	if ((*mrq)->error) {
		complete(&card->mrq_complete);
		return (*mrq)->error;
	}

	switch ((*mrq)->tpc) {
	case MS_TPC_SET_RW_REG_ADRS:
		memstick_init_req(*mrq, MS_TPC_READ_REG, NULL,
				  sizeof(struct ms_extra_data_register));
		return 0; 
	case MS_TPC_READ_REG:
		msb->current_extra
			= *((struct ms_extra_data_register*)((*mrq)->data));
		memstick_init_req(*mrq, MS_TPC_SET_RW_REG_ADRS, &reg_addr,
				  sizeof(reg_addr));
		card->next_request = msb->mrq_handler;
		card->reg_addr = reg_addr;
		return 0;
	default:
		BUG();
	}
}

static int h_ms_block_set_extra(struct memstick_dev *card,
				struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_register_addr reg_addr = {
		card->reg_addr.r_offset,
		card->reg_addr.r_length,
		offsetof(struct ms_register, param),
		sizeof(struct ms_param_register)
	};

	if ((*mrq)->error) {
		complete(&card->mrq_complete);
		return (*mrq)->error;
	}

	switch ((*mrq)->tpc) {
	case MS_TPC_SET_RW_REG_ADRS:
		memstick_init_req(*mrq, MS_TPC_WRITE_REG, &msb->current_extra,
				  sizeof(struct ms_extra_data_register));
		break; 
	case MS_TPC_WRITE_REG:
		msb->current_extra
			= *((struct ms_extra_data_register*)((*mrq)->data));
		memstick_init_req(*mrq, MS_TPC_SET_RW_REG_ADRS, &reg_addr,
				  sizeof(reg_addr));
		card->next_request = h_ms_block_default;
		card->reg_addr = reg_addr;
		break;
	default:
		BUG();
	}
	return 0;
}

static int h_ms_block_read_extra(struct memstick_dev *card,
				 struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_status_register *status;
	unsigned char t_val = MS_CMD_BLOCK_READ;
	struct ms_register_addr reg_addr = {
		offsetof(struct ms_register, extra_data),
		sizeof(struct ms_extra_data_register),
		card->reg_addr.w_offset,
		card->reg_addr.w_length
	};


	if ((*mrq)->error) {
		complete(&card->mrq_complete);
		return (*mrq)->error;
	}

	switch ((*mrq)->tpc) {
	case MS_TPC_WRITE_REG:
		memstick_init_req(*mrq, MS_TPC_SET_CMD, &t_val,
				  1);
		return 0;
	case MS_TPC_SET_CMD:
		memstick_init_req(*mrq, MS_TPC_READ_REG, NULL,
				  sizeof(struct ms_status_register));
		return 0;
	case MS_TPC_READ_REG:
		status = (struct ms_status_register*)card->current_mrq.data;

		if (status->interrupt & MEMSTICK_INT_CMDNAK)
			(*mrq)->error = -EFAULT;

		if ((status->interrupt & MEMSTICK_INT_ERR)
		    && (status->status1
			& (MEMSTICK_STATUS1_UCFG | MEMSTICK_STATUS1_UCEX
			   | MEMSTICK_STATUS1_UCDT)))
			(*mrq)->error = -EFAULT;

		if ((*mrq)->error) {
			complete(&card->mrq_complete);
			return (*mrq)->error;
		}

		memstick_init_req(*mrq, MS_TPC_SET_RW_REG_ADRS, &reg_addr,
				  sizeof(reg_addr));
		card->reg_addr = reg_addr;
		msb->mrq_handler = h_ms_block_read_extra;
		card->next_request = h_ms_block_get_extra;
		return 0;
	case MS_TPC_SET_RW_REG_ADRS:
		complete(&card->mrq_complete);
		return -EAGAIN;
	default:
		BUG();
	}
}

static int h_ms_block_erase(struct memstick_dev *card,
			    struct memstick_request **mrq)
{
	unsigned char t_val = MS_CMD_BLOCK_ERASE;

	if ((*mrq)->error) {
		complete(&card->mrq_complete);
		return (*mrq)->error;
	}

	switch ((*mrq)->tpc) {
	case MS_TPC_WRITE_REG:
		memstick_init_req(*mrq, MS_TPC_SET_CMD, &t_val,
				  1);
		(*mrq)->get_int_reg = 1;
		break;
	case MS_TPC_SET_CMD:
		t_val = (*mrq)->int_reg;
		memstick_init_req(*mrq, MS_TPC_GET_INT, NULL, 1);
		if (card->host->caps & MEMSTICK_CAP_AUTO_GET_INT)
			goto has_int_reg;
		break;
	case MS_TPC_GET_INT:
		t_val = (*mrq)->data[0];
has_int_reg:
		if (t_val & MEMSTICK_INT_CMDNAK)
			(*mrq)->error = -EFAULT;
		else if (t_val & MEMSTICK_INT_ERR)
			(*mrq)->error = -EROFS;

		if ((*mrq)->error) {
			complete(&card->mrq_complete);
			return (*mrq)->error;
		}

		if (t_val & MEMSTICK_INT_CED)
			return h_ms_block_write_next_state(card, mrq);

		break;
	default:
		BUG();
	}
	return 0;
}

/*** Data transfer ***/

static int ms_block_write_req(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct memstick_request *mrq = &card->current_mrq;
	struct ms_param_register param;
	
	enum write_state w_state;
	int rc = 0, t_rc = 0;

	msb->total_page_cnt = 0;
	msb->w_state = GET_BLOCK;
	rc = h_ms_block_write_next_state(card, &mrq);

	if (!rc) {
		memstick_new_req(card->host);
		wait_for_completion(&card->mrq_complete);
		rc = card->current_mrq.error;
	}

	w_state = msb->w_state;
	msb->w_state = NOTHING;

	/* write error - dst_block should be bad */
	if ((rc == -EROFS) && (w_state != DEL_SRC)) {
		msb->current_extra = (struct ms_extra_data_register){
			.overwrite_flag = 0xf8 & (~MEMSTICK_OVERWRITE_BLOCK),
			.management_flag = 0xff,
			.logical_address = MS_BLOCK_INVALID
		};
		msb->mrq_handler = h_ms_block_set_extra;
		card->next_request = h_ms_block_req_init;
		card->reg_addr = (struct ms_register_addr){
			card->reg_addr.r_offset,
			card->reg_addr.r_length,
			offsetof(struct ms_register, extra_data),
			sizeof(struct ms_extra_data_register)
		};
		memstick_init_req(mrq, MS_TPC_SET_RW_REG_ADRS,
				  &card->reg_addr, sizeof(card->reg_addr));
		memstick_new_req(card->host);
		wait_for_completion(&card->mrq_complete);
		t_rc = card->current_mrq.error;

		if (!t_rc) {
			msb->mrq_handler = h_ms_block_write_single;
			card->next_request = h_ms_block_req_init;
			param = (struct ms_param_register){
				.system = msb->system,
				.block_address_msb = 0,
				.block_address = cpu_to_be16(msb->dst_block),
				.cp = MEMSTICK_CP_EXTRA,
				.page_address = 0
			};
			memstick_init_req(mrq, MS_TPC_WRITE_REG, &param,
					  sizeof(param));
			memstick_new_req(card->host);
			wait_for_completion(&card->mrq_complete);
			t_rc = card->current_mrq.error;
		}
	}

	if (t_rc) {
		card->reg_addr = (struct ms_register_addr){
			offsetof(struct ms_register, status),
			sizeof(struct ms_status_register),
			offsetof(struct ms_register, param),
			sizeof(struct ms_param_register)
		};
		if (memstick_set_rw_addr(card))
			return -EIO;
	}

	return msb->total_page_cnt ? 0 : rc;
}

static int ms_block_read_req(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned short src_phy_block = ms_block_physical(msb, msb->src_block);
	struct ms_param_register param = {
		.system = msb->system,
		.block_address_msb = 0,
		.block_address = cpu_to_be16(src_phy_block),
		.cp = MEMSTICK_CP_PAGE,
		.page_address = msb->page_off
	}; 

	msb->total_page_cnt = 0;

	/* unmapped blocks are considered undefined, yet legal */
	while (src_phy_block == MS_BLOCK_INVALID) {
		msb->total_page_cnt++;
		msb->current_page++;

		if (msb->current_page
		    == (msb->req_sg[msb->current_seg].length
			/ msb->page_size)) {
			msb->current_page = 0;
			msb->current_seg++;

			if (msb->current_seg == msb->seg_cnt)
				goto out;
		}
		msb->page_off++;
		if (msb->page_off == msb->block_psize) {
			msb->page_off = 0; 
			msb->src_block++;
		}

		src_phy_block = ms_block_physical(msb, msb->src_block);
	}
	param.block_address = cpu_to_be16(src_phy_block);
	param.page_address = msb->page_off;

	card->next_request = h_ms_block_req_init;
	msb->mrq_handler = h_ms_block_read_pages;

	memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG,
			  (unsigned char*)&param, sizeof(param));
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);
out:
	return msb->total_page_cnt ? 0 : card->current_mrq.error;
}

static void ms_block_process_request(struct memstick_dev *card,
				     struct request *req)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	int rc, chunk;
	sector_t t_sec;
	unsigned long flags;

	do {
		msb->seg_cnt = blk_rq_map_sg(req->q, req, msb->req_sg);

		if (msb->seg_cnt) {
			t_sec = req->sector;
			msb->page_off = sector_div(t_sec, msb->block_psize
							  * (msb->page_size
							     >> 9));
			msb->src_block = (size_t)t_sec;
			msb->physical_src = 0;
			msb->current_seg = 0;
			msb->current_page = 0;

			if (rq_data_dir(req) == READ)
				rc = ms_block_read_req(card);
			else
				rc = ms_block_write_req(card);
		} else
			rc = -EFAULT;

		spin_lock_irqsave(&msb->q_lock, flags);
		if (!rc)
			chunk = end_that_request_chunk(req, 1,
						       msb->total_page_cnt
						       * msb->page_size);
		else
			chunk = end_that_request_first(req, rc,
						       req->current_nr_sectors);

		dev_dbg(&card->dev, "end chunk %d, %d\n", rc, chunk);
		if (!chunk) {
			add_disk_randomness(req->rq_disk);
			blkdev_dequeue_request(req);
			end_that_request_last(req, rc > 0 ? 1 : rc);
		}
		spin_unlock_irqrestore(&msb->q_lock, flags);
	} while (chunk);

}

static int ms_block_has_request(struct ms_block_data *msb)
{
	int rc = 0;
	unsigned long flags;

	spin_lock_irqsave(&msb->q_lock, flags);
	if (kthread_should_stop() || msb->has_request)
		rc = 1;
	spin_unlock_irqrestore(&msb->q_lock, flags);
	return rc;
}

static int ms_block_queue_thread(void *data)
{
	struct memstick_dev *card = data;
	struct memstick_host *host = card->host;
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct request *req = NULL;
	unsigned long flags;

	current->flags |= PF_NOFREEZE;

	while (1) {
		wait_event(msb->q_wait, ms_block_has_request(msb));

		spin_lock_irqsave(&msb->q_lock, flags);
		req = elv_next_request(msb->queue);
		if (!req) {
			msb->has_request = 0;
			if (kthread_should_stop()) {
				spin_unlock_irqrestore(&msb->q_lock, flags);
				break;
			}
		} else
			msb->has_request = 1;
		spin_unlock_irqrestore(&msb->q_lock, flags);

		if (req) {
			mutex_lock(&host->lock);
			ms_block_process_request(card, req);
			mutex_unlock(&host->lock);
		}
	}
	return 0;
}

static void ms_block_request(request_queue_t *q)
{
	struct memstick_dev *card = q->queuedata;
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct request *req = NULL;

	if (!msb->q_thread) {
		for (req = elv_next_request(q); req;
		     req = elv_next_request(q)) {
			while (end_that_request_chunk(req, -ENODEV,
						      req->current_nr_sectors
						      << 9)) {}
			end_that_request_last(req, -ENODEV);
		}
	} else {
		msb->has_request = 1;
		wake_up_all(&msb->q_wait);
	}
}

/*** Initialization ***/

static int ms_block_erase(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_param_register param = {
		.system = msb->system,
		.block_address_msb = 0,
		.block_address = cpu_to_be16(msb->dst_block),
		.cp = MEMSTICK_CP_BLOCK,
		.page_address = 0
	};

	msb->mrq_handler = h_ms_block_erase;
	card->next_request = h_ms_block_req_init;
	memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG, &param,
			  sizeof(param));
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);
	return card->current_mrq.error;
}

static int ms_block_read_page_extra(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_param_register param = {
		.system = msb->system,
		.block_address_msb = 0,
		.block_address = cpu_to_be16(msb->src_block),
		.cp = MEMSTICK_CP_EXTRA,
		.page_address = msb->page_off
	};
	int rc;

	msb->mrq_handler = h_ms_block_read_extra;
	card->next_request = h_ms_block_req_init;
	memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG, &param,
			  sizeof(param));
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);
	rc = card->current_mrq.error;
	if (rc) {
		card->reg_addr = (struct ms_register_addr){
			offsetof(struct ms_register, status),
			sizeof(struct ms_status_register),
			offsetof(struct ms_register, param),
			sizeof(struct ms_param_register)
		};
		memstick_set_rw_addr(card);
	}
	return rc;
}

static int ms_block_fill_lut(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned short b_cnt, l_addr, p_addr;
	int rc;

	dev_dbg(&card->dev, "scanning %x blocks\n", msb->block_count);

	msb->page_off = 0;
	for (b_cnt = 0; b_cnt < msb->block_count; b_cnt++) {
		if (ms_block_used(msb, b_cnt))
			continue;

		msb->src_block = b_cnt;
		rc = ms_block_read_page_extra(card);

		if (rc && (rc != -EFAULT))
			return rc;

		if (rc) {
			dev_dbg(&card->dev, "failed read extra, block %x\n",
				b_cnt);
			ms_block_mark_used(msb, b_cnt);
			continue;
		}

		if (!(msb->current_extra.overwrite_flag
		      & MEMSTICK_OVERWRITE_BLOCK)) {
			dev_dbg(&card->dev, "overwrite unset, block %x\n",
				b_cnt);
			ms_block_mark_used(msb, b_cnt);
			continue;
		}

		if(!(msb->current_extra.management_flag
		     & MEMSTICK_MANAGEMENT_TRANS_TABLE)) {
			dev_dbg(&card->dev, "trans table unset, block %x\n",
				b_cnt);

			if (!msb->read_only) {
				msb->dst_block = b_cnt;
				rc = ms_block_erase(card);
				if (rc != -EFAULT)
					return rc;
			}
		}

		l_addr = be16_to_cpu(msb->current_extra.logical_address);
		if (MS_BLOCK_INVALID == l_addr)
			continue;

		p_addr = ms_block_physical(msb, l_addr);
		if (MS_BLOCK_INVALID != p_addr) {
			printk(KERN_WARNING "%s: logical block %d is dual-"
			       "mapped (%d, %d)\n", card->dev.bus_id, l_addr,
			       p_addr, b_cnt);
			if (!(msb->current_extra.overwrite_flag
			      & MEMSTICK_OVERWRITE_UPDATA)) {
				if (!msb->read_only) {
					msb->dst_block = b_cnt;
					rc = ms_block_erase(card);
					if (rc != -EFAULT)
						return rc;
					printk(KERN_WARNING "%s: resolving "
					       "logical block %d to physical "
					       "%d\n", card->dev.bus_id,
					       l_addr, p_addr);
				}
			} else {
				msb->src_block = p_addr;
				rc = ms_block_read_page_extra(card);
				if (rc)
					return rc;

				if (!(msb->current_extra.overwrite_flag
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

				if (!msb->read_only) {
					msb->dst_block = p_addr;
					rc = ms_block_erase(card);
					if (rc != -EFAULT)
						return rc;
				}

				ms_block_mark_unused(msb, p_addr);
			}
		} else if (l_addr < msb->log_block_count) {
			msb->block_lut[l_addr] = b_cnt;
			ms_block_mark_used(msb, b_cnt);
		}
	}

	return 0;
}

static int ms_block_read_physical(struct memstick_dev *card,
				  unsigned char *buffer,
				  unsigned short page_count)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_param_register param = {
		.system = msb->system,
		.block_address_msb = 0,
		.block_address = cpu_to_be16(msb->src_block),
		.cp = MEMSTICK_CP_PAGE,
		.page_address = msb->page_off
	};

	sg_init_one(&msb->req_sg[0], buffer, page_count * msb->page_size);
	msb->physical_src = 1;
	msb->seg_cnt = 1;
	msb->current_seg = 0;
	msb->current_page = 0;

	card->next_request = h_ms_block_req_init;
	msb->mrq_handler = h_ms_block_read_pages;

	memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG,
			  &param, sizeof(param));
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);

	return card->current_mrq.error;
}

static int ms_block_fetch_cis_idi(struct memstick_dev *card,
				  struct ms_boot_page *boot_page,
				  unsigned short block)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned int page_off, page_cnt;
	char *buf;
	int rc = 0;

	page_off = boot_page->entry.cis_idi.start_addr;
	page_off += sizeof(*boot_page);
	page_cnt = page_off + boot_page->entry.cis_idi.data_size - 1;
	page_off /= msb->page_size;
	page_cnt /= msb->page_size;

	if (boot_page->entry.cis_idi.data_size
	    < (sizeof(msb->cis_idi) + 0x100))
		return -ENODEV;

	page_cnt = page_cnt - page_off + 1;
	buf = kmalloc(page_cnt * msb->page_size, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;

	msb->src_block = block;
	msb->page_off = page_off;
	rc = ms_block_read_physical(card, buf, page_cnt);
	if (rc)
		goto out_free_buf;

	page_off = boot_page->entry.cis_idi.start_addr;
	page_off += sizeof(*boot_page);
	page_off %= msb->page_size;
	page_off += 0x100;

	memcpy(&msb->cis_idi, buf + page_off, sizeof(msb->cis_idi));
	
	rc = 0;

out_free_buf:
	kfree(buf);
	return rc;
}

static int ms_block_fetch_bad_blocks(struct memstick_dev *card,
				     struct ms_boot_page *boot_page,
				     unsigned short block)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned int page_off, page_cnt;
	unsigned short b_block;
	char *buf;
	int rc = 0;

	page_off = boot_page->entry.disabled_block.start_addr;
	page_off += sizeof(*boot_page);
	page_cnt = page_off + boot_page->entry.disabled_block.data_size
		   - 1;
	page_off /= msb->page_size;
	page_cnt /= msb->page_size;

	if (!boot_page->entry.disabled_block.data_size)
		return 0;

	page_cnt = page_cnt - page_off + 1;
	buf = kmalloc(page_cnt * msb->page_size, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;

	msb->src_block = block;
	msb->page_off = page_off;
	rc = ms_block_read_physical(card, buf, page_cnt);
	if (rc)
		goto out_free_buf;


	page_off = boot_page->entry.disabled_block.start_addr;
	page_off += sizeof(*boot_page);
	page_off %= msb->page_size;

	for (page_cnt = 0;
	     page_cnt < boot_page->entry.disabled_block.data_size;
	     page_cnt += 2) {
		b_block = be16_to_cpu(*(unsigned short*)(buf + page_off
							 + page_cnt));

		ms_block_mark_used(msb, b_block);
		printk(KERN_INFO "%s: physical block %d is disabled\n",
		       card->dev.bus_id, b_block);
	}

out_free_buf:
	kfree(buf);
	return rc;
}

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

static int ms_block_find_boot_blocks(struct memstick_dev *card,
				     struct ms_boot_page **buf,
				     unsigned short *boot_blocks)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned int b_cnt, bb_cnt = 0;
	int rc = 0;

	boot_blocks[0] = MS_BLOCK_INVALID;
	boot_blocks[1] = MS_BLOCK_INVALID;

	for (b_cnt = 0; b_cnt < MS_BLOCK_MAX_BOOT_ADDR; b_cnt++) {
		msb->src_block = b_cnt;
		msb->page_off = 0;
		rc = ms_block_read_physical(card, (unsigned char*)buf[bb_cnt],
					    1);

		if (rc)
			break;

		if (be16_to_cpu(buf[b_cnt]->header.block_id)
		    == MS_BLOCK_BOOT_ID) {
			ms_block_fix_boot_page_endianness(buf[b_cnt]);
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

static int ms_block_switch_to_parallel(struct memstick_dev *card)
{
	struct memstick_host *host = card->host;
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_param_register param = {
		.system = 0x88,
		.block_address_msb = 0,
		.block_address = 0,
		.cp = MEMSTICK_CP_BLOCK,
		.page_address = 0
	};

	card->next_request = h_ms_block_req_init;
	msb->mrq_handler = h_ms_block_default;
	memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG, &param,
			  sizeof(param));
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);

	if (card->current_mrq.error)
		return card->current_mrq.error;

	msb->system = 0x88;
	host->ios.interface = MEMSTICK_PARALLEL;
	host->set_ios(host, &host->ios);

	card->next_request = h_ms_block_req_init;
	msb->mrq_handler = h_ms_block_default;
	memstick_init_req(&card->current_mrq, MS_TPC_GET_INT, NULL, 1);
	memstick_new_req(host);
	wait_for_completion(&card->mrq_complete);

	if (card->current_mrq.error) {
		msb->system = 0x80;
		host->ios.interface = MEMSTICK_SERIAL;
		host->set_ios(host, &host->ios);
		return -EFAULT;
	}

	return 0;
}

static int ms_block_init_card(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct memstick_host *host = card->host;
	char *buf;
	struct ms_boot_page *boot_pages[2];
	unsigned short boot_blocks[2];
	int rc;
	unsigned char t_val = MS_CMD_RESET;

	msb->page_size = sizeof(struct ms_boot_page);
	msb->system = 0x80;

	card->next_request = h_ms_block_req_init;
	msb->mrq_handler = h_ms_block_default;
	memstick_init_req(&card->current_mrq, MS_TPC_SET_CMD, &t_val, 1);
	card->current_mrq.need_card_int = 0;
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);

	if (card->current_mrq.error)
		return -ENODEV;

	card->reg_addr = (struct ms_register_addr){
		offsetof(struct ms_register, status),
		sizeof(struct ms_status_register),
		offsetof(struct ms_register, param),
		sizeof(struct ms_param_register)
	};

	if (memstick_set_rw_addr(card))
		return -EIO;

	if (host->caps & MEMSTICK_CAP_PARALLEL) {
		if (ms_block_switch_to_parallel(card))
			printk(KERN_WARNING "%s: could not switch to "
			       "parallel interface\n", card->dev.bus_id);
	}

	buf = kmalloc(2 * msb->page_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
 	boot_pages[0] = (struct ms_boot_page*)buf;
	boot_pages[1] = (struct ms_boot_page*)(buf + msb->page_size);
 
	rc = ms_block_find_boot_blocks(card, boot_pages, boot_blocks);
	if (rc || boot_blocks[0] == MS_BLOCK_INVALID)
		goto out_free_buf;

	memcpy(&msb->boot_attr, &boot_pages[0]->attr, sizeof(msb->boot_attr));
	msb->block_count = msb->boot_attr.number_of_blocks;
	msb->log_block_count = msb->boot_attr.number_of_effective_blocks - 4;
	msb->page_size = msb->boot_attr.page_size;
	msb->block_psize = (msb->boot_attr.block_size << 10) / msb->page_size;
	msb->block_map = kzalloc(4 * ((msb->block_count >> 5) + 1), GFP_KERNEL);
	msb->block_lut = kmalloc(2 * msb->log_block_count, GFP_KERNEL);

	if (!msb->block_map || !msb->block_lut) {
		rc = -ENOMEM;
		goto out_free_buf;
	}

	for (rc = 0; rc < msb->block_count; rc++)
		msb->block_lut[rc] = MS_BLOCK_INVALID;

	ms_block_mark_used(msb, boot_blocks[0]);
	if (boot_blocks[1] != MS_BLOCK_INVALID)
		ms_block_mark_used(msb, boot_blocks[1]);

	rc = ms_block_fetch_bad_blocks(card, boot_pages[0],
				       boot_blocks[0]);

	if (rc && boot_blocks[1] != MS_BLOCK_INVALID) {
		memcpy(&msb->boot_attr, &boot_pages[1]->attr,
		       sizeof(msb->boot_attr));
		boot_blocks[0] = MS_BLOCK_INVALID;
		rc = ms_block_fetch_bad_blocks(card, boot_pages[1],
					       boot_blocks[1]);
	}

	if (rc)
		goto out_free_buf;

	if (boot_blocks[0] != MS_BLOCK_INVALID) {
		rc = ms_block_fetch_cis_idi(card, boot_pages[0],
					    boot_blocks[0]);

		if (rc && boot_blocks[1] != MS_BLOCK_INVALID) {
			memcpy(&msb->boot_attr, &boot_pages[1]->attr,
			       sizeof(msb->boot_attr));
			rc = ms_block_fetch_cis_idi(card, boot_pages[1],
						    boot_blocks[1]);
		}
	} else if (boot_blocks[1] != MS_BLOCK_INVALID)
		rc = ms_block_fetch_cis_idi(card, boot_pages[1],
					    boot_blocks[1]);

	if (rc)
		goto out_free_buf;

	card->next_request = h_ms_block_req_init;
	msb->mrq_handler = h_ms_block_get_ro;
	memstick_init_req(&card->current_mrq, MS_TPC_READ_REG, NULL,
			  sizeof(struct ms_status_register));
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);
	rc = card->current_mrq.error;

out_free_buf:
	kfree(buf);
	return rc;
}

static int ms_block_check_card(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	return (msb->active == 1);
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
		.size = MS_BLOCK_MAP_LINE_SZ * msb->log_block_count,
		.read = ms_block_log_block_map_read
	};
	msb->dev_attr_physical_block_map = (struct bin_attribute){
		.attr = {
			.name = "physical_block_map",
			.mode = S_IRUGO,
			.owner = THIS_MODULE
		},
		.size = MS_BLOCK_MAP_LINE_SZ * msb->block_count,
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

static int ms_block_init_disk(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct memstick_host *host = card->host;
	int rc, disk_id;
	u64 limit = BLK_BOUNCE_HIGH;
	unsigned long capacity;

	if (host->cdev.dev->dma_mask && *(host->cdev.dev->dma_mask))
		limit = *(host->cdev.dev->dma_mask);

	if (!idr_pre_get(&ms_block_disk_idr, GFP_KERNEL))
		return -ENOMEM;

	mutex_lock(&ms_block_disk_lock);
	rc = idr_get_new(&ms_block_disk_idr, card, &disk_id);
	mutex_unlock(&ms_block_disk_lock);

	if (rc)
		return rc;

	if ((disk_id << MEMSTICK_PART_SHIFT) > 255) {
		rc = -ENOSPC;
		goto out_release_id;
	}

	msb->disk = alloc_disk(1 << MEMSTICK_PART_SHIFT);
	if (!msb->disk) {
		rc = -ENOMEM;
		goto out_release_id;
	}

	spin_lock_init(&msb->q_lock);
	init_waitqueue_head(&msb->q_wait);

	msb->queue = blk_init_queue(ms_block_request, &msb->q_lock);
	if (!msb->queue) {
		rc = -ENOMEM;
		goto out_put_disk;
	}

	msb->queue->queuedata = card;

	blk_queue_bounce_limit(msb->queue, limit);
	blk_queue_max_sectors(msb->queue, MS_BLOCK_MAX_PAGES);
	blk_queue_max_phys_segments(msb->queue, MS_BLOCK_MAX_SEGS);
	blk_queue_max_hw_segments(msb->queue, MS_BLOCK_MAX_SEGS);
	blk_queue_max_segment_size(msb->queue,
				   MS_BLOCK_MAX_PAGES * msb->page_size);

	msb->disk->major = major;
	msb->disk->first_minor = disk_id << MEMSTICK_PART_SHIFT;
	msb->disk->fops = &ms_block_bdops;
	msb->usage_count = 1;
	msb->disk->private_data = msb;
	msb->disk->queue = msb->queue;
	msb->disk->driverfs_dev = &card->dev;

	sprintf(msb->disk->disk_name, "msblk%d", disk_id);

	blk_queue_hardsect_size(msb->queue, msb->page_size);

	capacity = msb->log_block_count;
	capacity *= msb->block_psize;
	capacity *= msb->page_size >> 9;
	set_capacity(msb->disk, capacity);
	dev_dbg(&card->dev, "capacity set %ld\n", capacity);
	msb->q_thread = kthread_run(ms_block_queue_thread, card,
				    DRIVER_NAME"d");
	if (IS_ERR(msb->q_thread))
		goto out_put_disk;

	mutex_unlock(&host->lock);
	add_disk(msb->disk);
	mutex_lock(&host->lock);
	msb->active = 1;
	return 0;

out_put_disk:
	put_disk(msb->disk);
out_release_id:
	mutex_lock(&ms_block_disk_lock);
	idr_remove(&ms_block_disk_idr, disk_id);
	mutex_unlock(&ms_block_disk_lock);
	return rc;
}

static void ms_block_data_clear(struct ms_block_data *msb)
{
	kfree(msb->block_map);
	kfree(msb->block_lut);
	msb->card = NULL;
}

static int ms_block_probe(struct memstick_dev *card)
{
	struct ms_block_data *msb;
	int rc = 0;

	msb = kzalloc(sizeof(struct ms_block_data), GFP_KERNEL);
	if (!msb)
		return -ENOMEM;

	memstick_set_drvdata(card, msb);
	msb->card = card;

	rc = ms_block_init_card(card);
	if (!rc)
		rc = ms_block_fill_lut(card);

	if (rc)
		goto out_free;

	rc = device_create_file(&card->dev, &dev_attr_boot_attr);
	if (rc)
		goto out_free;

	rc = device_create_file(&card->dev, &dev_attr_cis_idi);
	if (rc)
		goto out_remove_boot_attr;

	rc = ms_block_create_rel_table_attr(card);
	if (rc)
		goto out_remove_cis_idi;

	rc = ms_block_init_disk(card);
	if (!rc) {
		card->check = ms_block_check_card;
		return 0;
	}


	sysfs_remove_bin_file(&card->dev.kobj,
			      &msb->dev_attr_physical_block_map);
	sysfs_remove_bin_file(&card->dev.kobj,
			      &msb->dev_attr_logical_block_map);
out_remove_cis_idi:
	device_remove_file(&card->dev, &dev_attr_cis_idi);
out_remove_boot_attr:
	device_remove_file(&card->dev, &dev_attr_boot_attr);
out_free:
	memstick_set_drvdata(card, NULL);
	ms_block_data_clear(msb);
	kfree(msb);
	return rc;
}

static void ms_block_remove(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct task_struct *q_thread = NULL;
	unsigned long flags;

	del_gendisk(msb->disk);
	spin_lock_irqsave(&msb->q_lock, flags);
	q_thread = msb->q_thread;
	msb->q_thread = NULL;
	msb->active = 0;
	spin_unlock_irqrestore(&msb->q_lock, flags);

	if (q_thread) {
		mutex_unlock(&card->host->lock);
		kthread_stop(q_thread);
		mutex_lock(&card->host->lock);
	}


	blk_cleanup_queue(msb->queue);

	sysfs_remove_bin_file(&card->dev.kobj,
			      &msb->dev_attr_physical_block_map);
	sysfs_remove_bin_file(&card->dev.kobj,
			      &msb->dev_attr_logical_block_map);
	device_remove_file(&card->dev, &dev_attr_cis_idi);
	device_remove_file(&card->dev, &dev_attr_boot_attr);

	mutex_lock(&ms_block_disk_lock);
	ms_block_data_clear(msb);
	mutex_unlock(&ms_block_disk_lock);

	ms_block_disk_release(msb->disk);
	memstick_set_drvdata(card, NULL);
}


#ifdef CONFIG_PM

static int ms_block_suspend(struct memstick_dev *card, pm_message_t state)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct task_struct *q_thread = NULL;
	unsigned long flags;

	spin_lock_irqsave(&msb->q_lock, flags);
	q_thread = msb->q_thread;
	msb->q_thread = NULL;
	msb->active = 0;
	blk_stop_queue(msb->queue);
	spin_unlock_irqrestore(&msb->q_lock, flags);

	if (q_thread)
		kthread_stop(q_thread);

	return 0;
}

static int ms_block_resume(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_block_data *new_msb;
	struct memstick_host *host = card->host;
	unsigned long flags;
	int rc = 0;

	if (!unsafe_resume)
		goto out_start_queue;

	mutex_lock(&host->lock);
	new_msb = kzalloc(sizeof(struct ms_block_data), GFP_KERNEL);
	if (!new_msb) {
		rc = -ENOMEM;
		goto out_unlock;
	}

	new_msb->card = card;
	memstick_set_drvdata(card, new_msb);
	if (ms_block_init_card(card))
		goto out_free;

	if (memcmp(&msb->boot_attr, &new_msb->boot_attr, sizeof(msb->boot_attr))
	    || memcmp(&msb->cis_idi, &new_msb->cis_idi, sizeof(msb->cis_idi)))
		goto out_free;

	memstick_set_drvdata(card, msb);

	msb->q_thread = kthread_run(ms_block_queue_thread,
				    card, DRIVER_NAME"d");
	if (IS_ERR(msb->q_thread))
		msb->q_thread = NULL;
	else
		msb->active = 1;

out_free:
	memstick_set_drvdata(card, msb);
	ms_block_data_clear(new_msb);
	kfree(new_msb);
out_unlock:
	mutex_unlock(&host->lock);
out_start_queue:
	spin_lock_irqsave(&msb->q_lock, flags);
	blk_start_queue(msb->queue);
	spin_unlock_irqrestore(&msb->q_lock, flags);
	return rc;
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

	rc = register_blkdev(major, DRIVER_NAME);
	if (rc < 0) {
		printk(KERN_ERR DRIVER_NAME ": failed to register "
		       "major %d, error %d\n", major, rc);
		return rc;
	}
	if (!major)
		major = rc;

	rc = memstick_register_driver(&ms_block_driver);
	if (rc)
		unregister_blkdev(major, DRIVER_NAME);
	return rc;
}

static void __exit ms_block_exit(void)
{
	memstick_unregister_driver(&ms_block_driver);
	unregister_blkdev(major, DRIVER_NAME);
	idr_destroy(&ms_block_disk_idr);
}

module_init(ms_block_init);
module_exit(ms_block_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Sony MemoryStick block device driver");
MODULE_DEVICE_TABLE(memstick, ms_block_id_tbl);
MODULE_VERSION(DRIVER_VERSION);
