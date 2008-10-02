/*
 *  MTDX legacy Sony Memorystick support
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *  Copyright (C) 2008 JMicron Technology Corporation <www.jmicron.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "../driver/linux/memstick.h"
#include "mtdx_common.h"
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/kthread.h>
#include <linux/wait.h>

#define DRIVER_NAME "ms_block"

#undef dev_dbg
#define dev_dbg dev_emerg

struct ms_block_system_item {
	unsigned int  start_addr;
	unsigned int  data_size;
	unsigned char data_type_id;
#define MS_BLOCK_ENTRY_BAD_BLOCKS 0x01
#define MS_BLOCK_ENTRY_CIS_IDI    0x0a

	unsigned char reserved[3];
} __attribute__((packed));

struct ms_block_boot_attr_info {
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
	unsigned char      format_unique_value4;
	unsigned char      format_unique_value5;
	unsigned char      vcc;
	unsigned char      vpp;
	unsigned short     controller_number;
	unsigned short     controller_function;
	unsigned char      reserved0[9];
	unsigned char      transfer_supporting;
	unsigned short     format_unique_value6;
	unsigned char      format_type;
	unsigned char      memorystick_application;
	unsigned char      device_type;
	unsigned char      reserved1[22];
	unsigned char      format_uniqure_value7;
	unsigned char      format_uniqure_value8;
	unsigned char      reserved2[15];
} __attribute__((packed));

struct ms_block_boot_header {
	unsigned short                 block_id;
#define MS_BLOCK_ID_BOOT 0x0001

	unsigned short                 format_reserved;
	unsigned char                  reserved0[184];
	unsigned char                  sys_entry_cnt;
	unsigned char                  reserved1[179];
	struct ms_block_system_item    sys_entry[4];
	struct ms_block_boot_attr_info info;
} __attribute__((packed));

#define MS_BLOCK_MAX_BOOT_ADDR 12
#define MS_BLOCK_CIS_SIZE 256

struct ms_block_idi {
	unsigned short general_config;
	unsigned short logical_cylinders;
	unsigned short reserved0;
	unsigned short logical_heads;
	unsigned short track_size;
	unsigned short sector_size;
	unsigned short sectors_per_track;
	unsigned short total_sectors_msw;
	unsigned short total_sectors_lsw;
	unsigned short reserved1;
	unsigned char  serial_number[20];
	unsigned short buffer_type;
	unsigned short buffer_size_increments;
	unsigned short long_command_ecc;
	unsigned char  firmware_version[8];
	unsigned char  model_name[40];
	unsigned short reserved2;
	unsigned short dual_word;
	unsigned short dma_transfer;
	unsigned short reserved3;
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
	unsigned char  reserved4[128];
} __attribute__((packed));

struct ms_block_boot_ref {
	unsigned int           phy_block;
	unsigned int           size;
	char                   *data;
	char                   *cis_idi;
	struct hd_geometry     hd_geo;
	unsigned int           bad_blocks_cnt;
	unsigned int           *bad_blocks;
};

struct ms_block_data {
	struct memstick_dev      *card;
	unsigned int             caps;
	struct mtdx_dev          *mdev;
	spinlock_t               lock;

	unsigned char            system;
	unsigned char            read_only:1,
				 active:1,
				 stopped:1,
				 format:1;
	unsigned char            cmd_flags;
#define MS_BLOCK_FLG_DATA     0x01
#define MS_BLOCK_FLG_EXTRA    0x02
#define MS_BLOCK_FLG_PAGE_INC 0x04
#define MS_BLOCK_FLG_COPY     0x08
#define MS_BLOCK_FLG_WRITE    0x10

	struct ms_block_boot_ref boot_blocks[2];
	struct mtdx_geo          geo;
	struct task_struct       *f_thread;

	struct mtdx_dev_queue    c_queue;
	wait_queue_head_t        req_wq;
	struct mtdx_dev          *req_dev;
	struct mtdx_request      *req_in;
	int                      (*mrq_handler)(struct memstick_dev *card,
						struct memstick_request **mrq);

	enum memstick_command    cmd;
	unsigned int             dst_page;
	struct mtdx_pos          src_pos;
	unsigned int             page_count;
	unsigned int             t_count;
	int                      trans_err;
	int                      src_error;
};

static const struct ms_register_addr ms_block_r_stat_w_param = {
	.r_offset = 0,
	.r_length = sizeof(struct ms_status_register),
	.w_offset = offsetof(struct ms_register, param),
	.w_length = sizeof(struct ms_param_register)
};

static const struct ms_register_addr ms_block_r_extra_w_param = {
	.r_offset = offsetof(struct ms_register, extra_data),
	.r_length = sizeof(struct ms_extra_data_register),
	.w_offset = offsetof(struct ms_register, param),
	.w_length = sizeof(struct ms_param_register)
};

static const struct ms_register_addr ms_block_r_stat_w_extra = {
	.r_offset = 0,
	.r_length = sizeof(struct ms_status_register),
	.w_offset = offsetof(struct ms_register, extra_data),
	.w_length = sizeof(struct ms_extra_data_register)
};

static ssize_t ms_block_boot_address_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf);
static ssize_t ms_block_boot_record_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf);
static ssize_t ms_block_info_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf);
static ssize_t ms_block_defects_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf);
static ssize_t ms_block_cis_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf);
static ssize_t ms_block_idi_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf);

static struct device_attribute ms_block_boot_address[] = {
	[0 ... 1] = {
		.attr = {
			.name = "address",
			.mode = 0444
		},
		.show = ms_block_boot_address_show
	}
};

static struct device_attribute ms_block_boot_record[] = {
	[0 ... 1] = {
		.attr = {
			.name = "boot_record",
			.mode = 0444
		},
		.show = ms_block_boot_record_show
	}
};

static struct device_attribute ms_block_info[] = {
	[0 ... 1] = {
		.attr = {
			.name = "info",
			.mode = 0444
		},
		.show = ms_block_info_show
	}
};

static struct device_attribute ms_block_defects[] = {
	[0 ... 1] = {
		.attr = {
			.name = "defects",
			.mode = 0444
		},
		.show = ms_block_defects_show
	}
};

static struct device_attribute ms_block_cis[] = {
	[0 ... 1] = {
		.attr = {
			.name = "cis",
			.mode = 0444
		},
		.show = ms_block_cis_show
	}
};

static struct device_attribute ms_block_idi[] = {
	[0 ... 1] = {
		.attr = {
			.name = "idi",
			.mode = 0444
		},
		.show = ms_block_idi_show
	}
};

static struct attribute *ms_block_boot_attr[2][7] = {
	{
		&ms_block_boot_address[0].attr, &ms_block_boot_record[0].attr,
		&ms_block_info[0].attr, &ms_block_defects[0].attr,
		&ms_block_cis[0].attr, &ms_block_idi[0].attr, NULL
	},
	{
		&ms_block_boot_address[1].attr, &ms_block_boot_record[1].attr,
		&ms_block_info[1].attr, &ms_block_defects[1].attr,
		&ms_block_cis[1].attr, &ms_block_idi[1].attr, NULL
	}
};

static struct attribute_group ms_block_grp_boot0 = {
	.name = "boot_block0",
	.attrs = ms_block_boot_attr[0]
};

static struct attribute_group ms_block_grp_boot1 = {
	.name = "boot_block1",
	.attrs = ms_block_boot_attr[1]
};


static ssize_t ms_block_boot_address_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	int idx = (attr == &ms_block_boot_address[1]) ? 1 : 0;
	struct ms_block_data *msb
		= memstick_get_drvdata(container_of(dev, struct memstick_dev,
						    dev));
	return scnprintf(buf, PAGE_SIZE, "%08x",
			 msb->boot_blocks[idx].phy_block);
}

static ssize_t ms_block_boot_record_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	int idx = (attr == &ms_block_boot_record[1]) ? 1 : 0;
	struct ms_block_data *msb
		= memstick_get_drvdata(container_of(dev, struct memstick_dev,
						    dev));
	ssize_t rc = min(msb->boot_blocks[idx].size, (unsigned int)PAGE_SIZE);

	if (rc)
		memcpy(buf, msb->boot_blocks[idx].data, rc);

	return rc;
}

static ssize_t ms_block_info_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	int idx = (attr == &ms_block_info[1]) ? 1 : 0;
	struct ms_block_data *msb
		= memstick_get_drvdata(container_of(dev, struct memstick_dev,
						    dev));
	ssize_t rc = 0;
	struct ms_block_boot_attr_info *info;
	unsigned short as_year;
	int tz;

	if (!msb->boot_blocks[idx].data)
		return 0;

	info = &((struct ms_block_boot_header *)msb->boot_blocks[idx].data)
	        ->info;

	tz = info->assembly_time[0];
	if (tz & 0x80)
		tz |= (~0) << 8;

	as_year = (info->assembly_time[1] << 8) | info->assembly_time[2];

	rc += scnprintf(buf, PAGE_SIZE, "class: %x\n", info->memorystick_class);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "block_size: %x\n",
			be16_to_cpu(info->block_size));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "number_of_blocks: %x\n",
			be16_to_cpu(info->number_of_blocks));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc,
			"number_of_effective_blocks: %x\n",
			be16_to_cpu(info->number_of_effective_blocks));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "page_size: %x\n",
			be16_to_cpu(info->page_size));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "extra_data_size: %x\n",
			info->extra_data_size);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc,
			"assembly_time: %+d %04d-%02d-%02d %02d:%02d:%02d\n",
			tz, as_year, info->assembly_time[3],
			info->assembly_time[4], info->assembly_time[5],
			info->assembly_time[6], info->assembly_time[7]);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc,
			"serial_number: %02x%02x%02x\n", info->serial_number[0],
			info->serial_number[1], info->serial_number[2]);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc,
			"assembly_manufacturer_code: %x\n",
			info->assembly_manufacturer_code);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc,
			"assembly_model_code: %02x%02x%02x\n",
			info->assembly_model_code[0],
			info->assembly_model_code[1],
			info->assembly_model_code[2]);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc,
			"memory_manufacturer_code: %x\n",
			be16_to_cpu(info->memory_mamufacturer_code));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "memory_device_code: %x\n",
			be16_to_cpu(info->memory_device_code));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "implemented_capacity: %x\n",
			be16_to_cpu(info->implemented_capacity));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "vcc: %x\n", info->vcc);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "vpp: %x\n", info->vpp);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "controller_number: %x\n",
			be16_to_cpu(info->controller_number));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "controller_function: %x\n",
			be16_to_cpu(info->controller_function));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "transfer_supporting: %x\n",
			info->transfer_supporting);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "format_type: %x\n",
			info->format_type);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc,
			"memorystick_application: %x\n",
			info->memorystick_application);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "device_type: %x\n",
			info->device_type);
	return rc;
}

static ssize_t ms_block_defects_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	int idx = (attr == &ms_block_defects[1]) ? 1 : 0;
	struct ms_block_data *msb
		= memstick_get_drvdata(container_of(dev, struct memstick_dev,
						    dev));
	unsigned int cnt;
	ssize_t rc = 0;

	if (!msb->boot_blocks[idx].bad_blocks_cnt)
		return 0;

	for (cnt = 0;
	     cnt < msb->boot_blocks[idx].bad_blocks_cnt;
	     ++cnt)
		rc += scnprintf(buf + rc, PAGE_SIZE - rc, "%08x\n",
				msb->boot_blocks[idx].bad_blocks[cnt]);

	return rc;
}

static ssize_t ms_block_cis_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	int idx = (attr == &ms_block_cis[1]) ? 1 : 0;
	struct ms_block_data *msb
		= memstick_get_drvdata(container_of(dev, struct memstick_dev,
						    dev));

	if (!msb->boot_blocks[idx].cis_idi)
		return 0;

	memcpy(buf, msb->boot_blocks[idx].cis_idi, MS_BLOCK_CIS_SIZE);
	return MS_BLOCK_CIS_SIZE;
}

static ssize_t ms_block_idi_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	int idx = (attr == &ms_block_idi[1]) ? 1 : 0;
	int cnt;
	struct ms_block_data *msb
		= memstick_get_drvdata(container_of(dev, struct memstick_dev,
						    dev));
	ssize_t rc = 0;
	struct ms_block_idi *idi;

	if (!msb->boot_blocks[idx].cis_idi)
		return 0;

	idi = (struct ms_block_idi *)(msb->boot_blocks[idx].cis_idi
				      + MS_BLOCK_CIS_SIZE);

	rc += scnprintf(buf, PAGE_SIZE, "general_config: %x\n",
			le16_to_cpu(idi->general_config));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "logical_cylinders: %x\n",
			le16_to_cpu(idi->logical_cylinders));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "logical_heads: %x\n",
			le16_to_cpu(idi->logical_heads));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "track_size: %x\n",
			le16_to_cpu(idi->track_size));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "sector_size: %x\n",
			le16_to_cpu(idi->sector_size));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "sectors_per_track: %x\n",
			le16_to_cpu(idi->sectors_per_track));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "total_sectors_msw: %x\n",
			le16_to_cpu(idi->total_sectors_msw));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "total_sectors_lsw: %x\n",
			le16_to_cpu(idi->total_sectors_lsw));

	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "serial_number: '");
	for (cnt = 0; cnt < sizeof(idi->serial_number); cnt += 2)
		rc += scnprintf(buf + rc, PAGE_SIZE - rc, "%c%c",
				idi->serial_number[cnt + 1],
				idi->serial_number[cnt]);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "'\n");

	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "buffer_type: %x\n",
			le16_to_cpu(idi->buffer_type));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc,
			"buffer_size_increments: %x\n",
			le16_to_cpu(idi->buffer_size_increments));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "long_command_ecc: %x\n",
			le16_to_cpu(idi->long_command_ecc));

	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "firmware_version: '");
	for (cnt = 0; cnt < sizeof(idi->firmware_version); cnt += 2)
		rc += scnprintf(buf + rc, PAGE_SIZE - rc, "%c%c",
				idi->firmware_version[cnt + 1],
				idi->firmware_version[cnt]);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "'\n");

	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "model_name: '");
	for (cnt = 0; cnt < sizeof(idi->model_name); cnt += 2)
		rc += scnprintf(buf + rc, PAGE_SIZE - rc, "%c%c",
				idi->model_name[cnt + 1],
				idi->model_name[cnt]);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "'\n");

	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "dual_word: %x\n",
			le16_to_cpu(idi->dual_word));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "dma_transfer: %x\n",
			le16_to_cpu(idi->dma_transfer));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "pio_mode_number: %x\n",
			le16_to_cpu(idi->pio_mode_number));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "dma_mode_number: %x\n",
			le16_to_cpu(idi->dma_mode_number));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "field_validity: %x\n",
			le16_to_cpu(idi->field_validity));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc,
			"current_logical_cylinders: %x\n",
			le16_to_cpu(idi->current_logical_cylinders));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "current_logical_heads: %x\n",
			le16_to_cpu(idi->current_logical_heads));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc,
			"current_sectors_per_track: %x\n",
			le16_to_cpu(idi->current_sectors_per_track));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc,
			"current_sector_capacity: %x\n",
			le32_to_cpu(idi->current_sector_capacity));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc,
			"mutiple_sector_setting: %x\n",
			le16_to_cpu(idi->mutiple_sector_setting));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "addressable_sectors: %x\n",
			le32_to_cpu(idi->addressable_sectors));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "single_word_dma: %x\n",
			le16_to_cpu(idi->single_word_dma));
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "multi_word_dma: %x\n",
			le16_to_cpu(idi->multi_word_dma));

	return rc;
}

#define MS_BLOCK_CORRECTABLE (MEMSTICK_STATUS1_FGER | MEMSTICK_STATUS1_EXER \
			      | MEMSTICK_STATUS1_DTER)

#define MS_BLOCK_UNCORRECTABLE (MEMSTICK_STATUS1_UCFG | MEMSTICK_STATUS1_UCEX \
				| MEMSTICK_STATUS1_UCDT)

static void ms_block_complete_req(struct memstick_dev *card, int error);
static int h_ms_block_req_init(struct memstick_dev *card,
			       struct memstick_request **mrq);
static int h_ms_block_write_param(struct memstick_dev *card,
				  struct memstick_request **mrq);
static int h_ms_block_set_extra_addr_w(struct memstick_dev *card,
				       struct memstick_request **mrq);
static int h_ms_block_cmd_get_int(struct memstick_dev *card,
				  struct memstick_request **mrq);
static int h_ms_block_set_param_addr_init(struct memstick_dev *card,
					  struct memstick_request **mrq);
static int h_ms_block_set_param_addr(struct memstick_dev *card,
				     struct memstick_request **mrq);
static int h_ms_block_set_extra_addr_r(struct memstick_dev *card,
				       struct memstick_request **mrq);
static int h_ms_block_trans_data(struct memstick_dev *card,
				 struct memstick_request **mrq);
static int h_ms_block_trans_extra(struct memstick_dev *card,
				  struct memstick_request **mrq);
static int h_ms_block_data_get_int(struct memstick_dev *card,
				   struct memstick_request **mrq);

static int ms_block_reg_addr_cmp(struct memstick_dev *card,
				 const struct ms_register_addr *addr)
{
	return memcmp(&card->reg_addr, addr, sizeof(struct ms_register_addr));
}

static void ms_block_reg_addr_set(struct memstick_dev *card,
				  const struct ms_register_addr *addr)
{
	memcpy(&card->reg_addr, addr, sizeof(struct ms_register_addr));
}

static int h_ms_block_internal_req_init(struct memstick_dev *card,
					struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	*mrq = &card->current_mrq;
	card->next_request = msb->mrq_handler;
	return 0;
}

static int ms_block_format(void *data)
{
	struct memstick_dev *card = data;
	struct ms_block_data *msb = memstick_get_drvdata(card);
	LIST_HEAD(s_blocks);
	struct list_head *s_block_pos;
	struct ms_param_register param;
	unsigned long flags;
	int rc;

	msb->mdev->get_param(msb->mdev, MTDX_PARAM_SPECIAL_BLOCKS, &s_blocks);
	s_block_pos = s_blocks.next;
	msb->src_pos.b_addr = 0;

	if (ms_block_reg_addr_cmp(card, &ms_block_r_stat_w_param)) {
		ms_block_reg_addr_set(card, &ms_block_r_stat_w_param);
		rc = memstick_set_rw_addr(card);
		if (rc) {
			/* invalidate the address to avoid mistakes */
			memset(&card->reg_addr, 0, sizeof(card->reg_addr));
			goto out;
		}
	}

	while (!kthread_should_stop()
	       && (msb->src_pos.b_addr < msb->geo.phy_block_cnt)) {
		if (s_block_pos != &s_blocks) {
			if (list_entry(s_block_pos, struct mtdx_page_info, node)
			    ->phy_block == msb->src_pos.b_addr) {
				dev_info(&card->dev, "skipping block %x\n",
					 msb->src_pos.b_addr);
				msb->src_pos.b_addr++;
				s_block_pos = s_block_pos->next;
				continue;
			}
		}

		msb->cmd = MS_CMD_BLOCK_ERASE;
		param.system = msb->system;
		param.cp = MEMSTICK_CP_BLOCK;
		param.page_address = 0;
		ms_param_set_addr(&param, msb->src_pos.b_addr);
		memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG, &param,
				  sizeof(param));
		msb->mrq_handler = h_ms_block_write_param;
		card->next_request = h_ms_block_internal_req_init;
		memstick_new_req(card->host);
		wait_for_completion(&card->mrq_complete);
		if (card->current_mrq.error)
			dev_warn(&card->dev, "format: error %d erasing block "
				 "%x\n", card->current_mrq.error,
				 msb->src_pos.b_addr);

		msb->src_pos.b_addr++;
	}

out:
	dev_dbg(&card->dev, "format out\n");
	spin_lock_irqsave(&msb->lock, flags);
	card->next_request = h_ms_block_req_init;
	if (msb->f_thread) {
		msb->f_thread = NULL;
		spin_unlock_irqrestore(&msb->lock, flags);
	} else {
		spin_unlock_irqrestore(&msb->lock, flags);
		while(!kthread_should_stop())
			msleep_interruptible(1);
	}

	dev_dbg(&card->dev, "format end\n");
	return 0;
}

static ssize_t ms_block_format_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct ms_block_data *msb
		= memstick_get_drvdata(container_of(dev, struct memstick_dev,
						    dev));
	unsigned int f_pos = MTDX_INVALID_BLOCK;
	unsigned long flags;

	dev_dbg(&msb->card->dev, "format show\n");
	spin_lock_irqsave(&msb->lock, flags);
	if (msb->f_thread)
		f_pos = msb->src_pos.b_addr;
	spin_unlock_irqrestore(&msb->lock, flags);

	if (f_pos != MTDX_INVALID_BLOCK)
		return scnprintf(buf, PAGE_SIZE, "Erasing block %x of %x.\n",
				 f_pos, msb->geo.phy_block_cnt);
	else
		return scnprintf(buf, PAGE_SIZE, "Not running.\n");

	dev_dbg(&msb->card->dev, "format show end\n");
}

int ms_block_wake_next(struct ms_block_data *msb)
{
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&msb->lock, flags);
	if (!msb->req_dev && !msb->f_thread)
		rc = 1;

	if (!msb->active)
		rc = 1;
	else if (msb->stopped)
		rc = 0;
	spin_unlock_irqrestore(&msb->lock, flags);
	return rc;
}

static ssize_t ms_block_format_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct memstick_dev *card = container_of(dev, struct memstick_dev, dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct task_struct *f_thread = NULL;
	unsigned long flags;

	if ((count < 6) || strncmp(buf, "format", 6))
		return count;

	f_thread = kthread_create(ms_block_format, card, "msb_format%d",
				  card->host->id);
	if (IS_ERR(f_thread))
		return count;

	spin_lock_irqsave(&msb->lock, flags);
	while (1) {
		if (!msb->active || msb->f_thread) {
			spin_unlock_irqrestore(&msb->lock, flags);
			kthread_stop(f_thread);
			return count;
		}

		if (!(msb->stopped || msb->req_dev)) {
			msb->f_thread = f_thread;
			spin_unlock_irqrestore(&msb->lock, flags);
			break;
		}

		spin_unlock_irqrestore(&msb->lock, flags);
		wait_event(msb->req_wq, ms_block_wake_next(msb));
		spin_lock_irqsave(&msb->lock, flags);
	}

	if (f_thread)
		wake_up_process(f_thread);

	return count;
}

static DEVICE_ATTR(format, 0644, ms_block_format_show, ms_block_format_store);

/*
 * All sequences start from CLEAR_BUF command and optional SET_RW_REG_ADDR tpc.
 *
 *
 * Expected callback activation sequence for reading:
 * 1. set_param_addr_init
 * 2. write_param
 * 3. set_cmd
 * 4. cmd_get_int
 * 5-1 (extra)
 * 5-1-1. set_extra_addr_r
 * 5-1-2. trans_extra
 * 5-2 (error)
 * 5-2-1. set_stat_addr_r
 * 5-2-2. read_status ?-> 5-1
 * 5-3 (end, page_inc)
 * 5-3-1. set_cmd
 * 5-3-2. cmd_get_int
 * 6. trans_data (data)
 * 7-1 (page_inc)
 * 7-1-1. data_get_int
 * 7-1-2. -> 5
 * 7-2 (!page_inc)
 * 7-2-1. set_param_addr
 * 7-2-2. -> 2
 *
 *
 * Expected callback activation sequence for erasing:
 * 1. set_param_addr_init
 * 2. write_param
 * 3. set_cmd
 * 4. cmd_get_int -> 4
 *
 * Expected callback activation sequence for writing:
 * 1. set_param_addr_init
 * 2. write_param
 * 3. set_cmd
 * 4. cmd_get_int
 * 5-1 (extra)
 * 5-1-1. set_extra_addr_w
 * 5-1-2. trans_extra
 * 5-2 (error)
 * 6-1 (data)
 * 6-1-1. trans_data
 * 7. data_get_int
 * 8-1 (page_inc)
 * 8-1-1. -> 5
 * 8-2 (end, page_inc)
 * 8-2-1. set_cmd
 * 8-2-2. cmd_get_int
 * 8-3 (!page_inc)
 * 8-3-1. -> set_param_addr
 * 8-3-2. -> 2
 *
 *
 * Expected callback activation sequence for copying:
 * 1. set_param_addr_init
 * 2. write_param
 * 3. set_cmd
 * 4. cmd_get_int
 * 5-1 (read error)
 * 5-1-1. read_status
 * 5-1-2. copy_read (corr. err)
 * 5-1-3. copy_write (corr. err)
 * 5-2 (error)
 * 6. -> 2
 */

static int h_ms_block_clear_buf(struct memstick_dev *card,
				struct memstick_request **mrq)
{
	if ((*mrq)->error) {
		ms_block_complete_req(card, (*mrq)->error);
		return (*mrq)->error;
	}

	memstick_init_req(*mrq, MS_TPC_SET_RW_REG_ADRS,
			  &ms_block_r_stat_w_param,
			  sizeof(ms_block_r_stat_w_param));

	if (ms_block_reg_addr_cmp(card, &ms_block_r_stat_w_param))
		card->next_request = h_ms_block_set_param_addr_init;
	else
		return h_ms_block_set_param_addr_init(card, mrq);

	return 0;
}

static int h_ms_block_req_init(struct memstick_dev *card,
			       struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	int c_cmd = MS_CMD_CLEAR_BUF;
	unsigned long flags;

	*mrq = &card->current_mrq;

	spin_lock_irqsave(&msb->lock, flags);
	if (msb->f_thread || msb->stopped || !msb->active) {
		spin_unlock_irqrestore(&msb->lock, flags);
		return -EAGAIN;
	}

	do {
		if (msb->req_dev)
			msb->req_in = msb->mdev->get_request(msb->req_dev);

		if (msb->req_in)
			break;

		if (msb->req_dev) {
			put_device(&msb->req_dev->dev);
			msb->req_dev = NULL;
		}

		msb->req_dev = mtdx_dev_queue_pop_front(&msb->c_queue);
	} while (msb->req_dev);
	spin_unlock_irqrestore(&msb->lock, flags);

	memstick_reset_req(card->host);

	if (!msb->req_in) {
		*mrq = NULL;
		complete_all(&card->mrq_complete);
		return -EAGAIN;
	}

	memstick_init_req(*mrq, MS_TPC_SET_CMD, &c_cmd, 1);
	card->next_request = h_ms_block_clear_buf;
	return 0;
}

static int h_ms_block_default(struct memstick_dev *card,
			      struct memstick_request **mrq)
{
	ms_block_complete_req(card, (*mrq)->error);
	return (*mrq)->error;
}

static int ms_block_set_req_data(struct ms_block_data *msb,
				 struct memstick_request *mrq)
{
	enum memstick_tpc tpc;
	struct scatterlist sg;

	if (msb->cmd_flags & MS_BLOCK_FLG_WRITE)
		tpc = MS_TPC_WRITE_LONG_DATA;
	else
		tpc = MS_TPC_READ_LONG_DATA;

	dev_dbg(&msb->card->dev, "set req data page_count %x, t_count %x\n",
		msb->page_count, msb->t_count);

	if (msb->page_count - msb->t_count) {
		mtdx_data_iter_get_sg(msb->req_in->req_data, &sg,
				      msb->geo.page_size);
		if (sg.length != msb->geo.page_size) {
			mrq->error = -ENOMEM;
			return mrq->error;
		}

		memstick_init_req_sg(mrq, tpc, &sg);
		dev_dbg(&msb->card->dev, "req sg %p, %x:%x\n",
			sg_page(&mrq->sg), mrq->sg.offset, mrq->sg.length);
		return 0;
	}

	return -EAGAIN;
}

static int ms_block_complete_multi(struct memstick_dev *card,
				   struct memstick_request **mrq,
				   int error)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	dev_dbg(&card->dev, "complete_multi: %d\n", error);

	if (msb->cmd_flags & MS_BLOCK_FLG_COPY) {
		if (!(msb->cmd_flags & MS_BLOCK_FLG_WRITE)) {
			msb->src_error = error;
		}
	}

	if (msb->cmd_flags & MS_BLOCK_FLG_PAGE_INC) {
		msb->trans_err = error;
		msb->cmd = MS_CMD_BLOCK_END;
		memstick_init_req(*mrq, MS_TPC_SET_CMD, &msb->cmd, 1);
		card->next_request = h_ms_block_cmd_get_int;
		return 0;
	} else {
		ms_block_complete_req(card, error);
		return error;
	}
}

static int ms_block_setup_read(struct memstick_dev *card,
			       struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if (msb->cmd_flags & MS_BLOCK_FLG_EXTRA) {
		memstick_init_req(*mrq, MS_TPC_SET_RW_REG_ADRS,
				  &ms_block_r_extra_w_param,
				  sizeof(ms_block_r_extra_w_param));
		if (ms_block_reg_addr_cmp(card, &ms_block_r_extra_w_param)) {
			card->next_request = h_ms_block_set_extra_addr_r;
			return 0;
		} else
			return h_ms_block_set_extra_addr_r(card, mrq);
	}

	if (msb->cmd_flags & MS_BLOCK_FLG_DATA) {
		msb->trans_err = ms_block_set_req_data(msb, *mrq);

		if (msb->trans_err)
			return ms_block_complete_multi(card, mrq,
						       msb->trans_err);

		card->next_request = h_ms_block_trans_data;

		if ((msb->cmd_flags & MS_BLOCK_FLG_PAGE_INC)
		    || (msb->cmd_flags & MS_BLOCK_FLG_WRITE))
			(*mrq)->need_card_int = 1;

		return 0;
	}

	if (msb->cmd_flags & MS_BLOCK_FLG_COPY) {
		struct ms_param_register param = {
			.system = msb->system,
			.cp = MEMSTICK_CP_PAGE | MEMSTICK_CP_EXTRA,
			.page_address = msb->dst_page
		};
		ms_param_set_addr(&param, msb->req_in->phy.b_addr);
		msb->cmd_flags |= MS_BLOCK_FLG_WRITE;
		msb->cmd = MS_CMD_BLOCK_WRITE;
		memstick_init_req(*mrq, MS_TPC_WRITE_REG, &param,
				  sizeof(param));
		card->next_request = h_ms_block_write_param;
		return 0;
	}

	return ms_block_complete_multi(card, mrq, -EINVAL);
}

static int ms_block_setup_write(struct memstick_dev *card,
				struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if (msb->cmd_flags & MS_BLOCK_FLG_EXTRA) {
		memstick_init_req(*mrq, MS_TPC_SET_RW_REG_ADRS,
				  &ms_block_r_stat_w_extra,
				  sizeof(ms_block_r_stat_w_extra));

		if (ms_block_reg_addr_cmp(card, &ms_block_r_stat_w_extra)) {
			card->next_request = h_ms_block_set_extra_addr_w;
			return 0;
		} else
			return h_ms_block_set_extra_addr_w(card, mrq);
	}

	if (msb->cmd_flags & MS_BLOCK_FLG_DATA) {
		msb->trans_err = ms_block_set_req_data(msb, *mrq);

		if (msb->trans_err)
			return ms_block_complete_multi(card, mrq,
						       msb->trans_err);

		card->next_request = h_ms_block_trans_data;
		return 0;
	}

	if (msb->cmd_flags & MS_BLOCK_FLG_COPY) {
		struct ms_param_register param = {
			.system = msb->system,
			.cp = MEMSTICK_CP_PAGE | MEMSTICK_CP_EXTRA,
			.page_address = msb->src_pos.offset + 1
		};
		ms_param_set_addr(&param, msb->src_pos.b_addr);

		if (msb->t_count == msb->page_count) {
			ms_block_complete_req(card, 0);
			return -EAGAIN;
		}

		msb->src_pos.offset++;
		msb->dst_page++;
		msb->t_count++;
		msb->cmd_flags &= ~MS_BLOCK_FLG_WRITE;
		msb->cmd = MS_CMD_BLOCK_READ;

		memstick_init_req(*mrq, MS_TPC_WRITE_REG, &param,
				  sizeof(param));
		card->next_request = h_ms_block_write_param;
		return 0;
	}

	return ms_block_complete_multi(card, mrq, -EINVAL);
}

static int h_ms_block_copy_write(struct memstick_dev *card,
				 struct memstick_request **mrq)
{
	if ((*mrq)->error)
		return ms_block_complete_multi(card, mrq, (*mrq)->error);

	return ms_block_setup_read(card, mrq);
}

static int h_ms_block_copy_read(struct memstick_dev *card,
				struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct scatterlist sg;

	if ((*mrq)->error)
		return ms_block_complete_multi(card, mrq, (*mrq)->error);

	mtdx_data_iter_dec(msb->req_in->req_data, msb->geo.page_size);
	mtdx_data_iter_get_sg(msb->req_in->req_data, &sg, msb->geo.page_size);
	memstick_init_req_sg(*mrq, MS_TPC_WRITE_LONG_DATA, &sg);
	card->next_request = h_ms_block_copy_write;
	return 0;

}

static int h_ms_block_read_status(struct memstick_dev *card,
				  struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_status_register *status;

	if ((*mrq)->error)
		return ms_block_complete_multi(card, mrq, (*mrq)->error);

	status = (struct ms_status_register *)((*mrq)->data);

	if (status->status1 & MS_BLOCK_UNCORRECTABLE) {
		dev_err(&card->dev, "Uncorrectable error reading %x:%x, "
			"status: %02x, %02x, %02x, %02x\n",
			msb->req_in->phy.b_addr, msb->dst_page,
			status->reserved, status->interrupt, status->status0,
			status->status1);

		return ms_block_complete_multi(card, mrq, -EFAULT);
	}

	if (status->status1 & MS_BLOCK_CORRECTABLE) {
		dev_warn(&card->dev, "Correctable error reading %x:%x, "
			 "status: %02x, %02x, %02x, %02x\n",
			 msb->req_in->phy.b_addr, msb->dst_page,
			 status->reserved, status->interrupt, status->status0,
			 status->status1);

		if (msb->cmd_flags & MS_BLOCK_FLG_COPY) {
			/* We need to read and then write back data, to fix
			 * the error.
			 */
			struct scatterlist sg;

			mtdx_data_iter_get_sg(msb->req_in->req_data, &sg,
					      msb->geo.page_size);
			memstick_init_req_sg(*mrq, MS_TPC_READ_LONG_DATA, &sg);
			card->next_request = h_ms_block_copy_read;
			return 0;
		}
	}

	return ms_block_setup_read(card, mrq);
}

static int h_ms_block_set_stat_addr_r(struct memstick_dev *card,
				      struct memstick_request **mrq)
{
	if ((*mrq)->error)
		return ms_block_complete_multi(card, mrq, (*mrq)->error);

	ms_block_reg_addr_set(card, &ms_block_r_stat_w_param);

	memstick_init_req(*mrq, MS_TPC_READ_REG, NULL,
			  sizeof(struct ms_status_register));
	card->next_request = h_ms_block_read_status;
	return 0;
}

static int h_ms_block_set_param_addr(struct memstick_dev *card,
				     struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_param_register param = {
		.system = msb->system,
		.cp = MEMSTICK_CP_PAGE | MEMSTICK_CP_EXTRA,
		.page_address = msb->dst_page
	};
	ms_param_set_addr(&param, msb->req_in->phy.b_addr);

	if ((*mrq)->error) {
		ms_block_complete_req(card, (*mrq)->error);
		return (*mrq)->error;
	}

	ms_block_reg_addr_set(card, &ms_block_r_stat_w_param);
	if (!MS_BLOCK_FLG_DATA)
		param.cp &= ~MEMSTICK_CP_PAGE;

	if (!MS_BLOCK_FLG_EXTRA)
		param.cp &= ~MEMSTICK_CP_EXTRA;

	memstick_init_req(*mrq, MS_TPC_WRITE_REG, &param, sizeof(param));
	card->next_request = h_ms_block_write_param;
	return 0;
}

static int h_ms_block_trans_extra(struct memstick_dev *card,
				  struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	char *oob_buf;

	if ((*mrq)->error)
		return ms_block_complete_multi(card, mrq, (*mrq)->error);

	if (!(msb->cmd_flags & MS_BLOCK_FLG_WRITE)) {
		oob_buf = mtdx_oob_get_next(msb->req_in->req_oob);

		memcpy(oob_buf, (*mrq)->data, msb->geo.oob_size);
	}

	if (msb->cmd_flags & MS_BLOCK_FLG_DATA) {
		msb->trans_err = ms_block_set_req_data(msb, *mrq);

		if (msb->trans_err)
			return ms_block_complete_multi(card, mrq,
						       msb->trans_err);

		card->next_request = h_ms_block_trans_data;
		return 0;
	}

	msb->dst_page++;
	msb->t_count++;

	if (msb->t_count == msb->page_count)
		return ms_block_complete_multi(card, mrq, 0);
	else {
		if (msb->cmd_flags & MS_BLOCK_FLG_PAGE_INC) {
			memstick_init_req(*mrq, MS_TPC_GET_INT, NULL, 1);
			(*mrq)->need_card_int = 1;
			card->next_request = h_ms_block_data_get_int;
		} else {
			memstick_init_req(*mrq, MS_TPC_SET_RW_REG_ADRS,
					  &ms_block_r_stat_w_param,
					  sizeof(ms_block_r_stat_w_param));

			if (ms_block_reg_addr_cmp(card,
						  &ms_block_r_stat_w_param)) {
				card->next_request
					= h_ms_block_set_param_addr;
				return 0;
			} else
				return h_ms_block_set_param_addr(card, mrq);
		}
		return 0;
	}
}

static int h_ms_block_set_extra_addr_r(struct memstick_dev *card,
				       struct memstick_request **mrq)
{
	if ((*mrq)->error)
		return ms_block_complete_multi(card, mrq, (*mrq)->error);

	ms_block_reg_addr_set(card, &ms_block_r_extra_w_param);
	memstick_init_req(*mrq, MS_TPC_READ_REG, NULL,
			  sizeof(struct ms_extra_data_register));
	card->next_request = h_ms_block_trans_extra;
	return 0;
}

static int h_ms_block_data_get_int(struct memstick_dev *card,
				   struct memstick_request **mrq)
{
	unsigned char int_reg = (*mrq)->data[0];
	struct ms_block_data *msb = memstick_get_drvdata(card);

	dev_dbg(&card->dev, "data_get_int: %d, %x\n", (*mrq)->error, int_reg);

	if (!(*mrq)->error) {
		if (int_reg & MEMSTICK_INT_CMDNAK)
			return ms_block_complete_multi(card, mrq, -EIO);
		else if (int_reg & MEMSTICK_INT_ERR)
			(*mrq)->error = -EFAULT;
	} else
		return ms_block_complete_multi(card, mrq, (*mrq)->error);

	if (msb->cmd_flags & MS_BLOCK_FLG_WRITE) {
		if (int_reg & MEMSTICK_INT_ERR) {
			if (msb->dst_page)
				msb->dst_page--;

			if (msb->t_count)
				msb->t_count--;

			return ms_block_complete_multi(card, mrq, -EFAULT);
		}
	} else {
		if (int_reg & MEMSTICK_INT_ERR) {
			memstick_init_req(*mrq, MS_TPC_SET_RW_REG_ADRS,
					  &ms_block_r_stat_w_param,
					  sizeof(ms_block_r_stat_w_param));
			if (ms_block_reg_addr_cmp(card,
						  &ms_block_r_stat_w_param)) {
				card->next_request = h_ms_block_set_stat_addr_r;
				return 0;
			} else
				return h_ms_block_set_stat_addr_r(card, mrq);
		}
	}

	if (msb->t_count == msb->page_count)
		return ms_block_complete_multi(card, mrq, 0);
	else if (msb->cmd_flags & MS_BLOCK_FLG_PAGE_INC) {
		if (msb->cmd == MS_CMD_BLOCK_READ)
			return ms_block_setup_read(card, mrq);
		else
			return ms_block_setup_write(card, mrq);
	} else {
		memstick_init_req(*mrq, MS_TPC_SET_RW_REG_ADRS,
				  &ms_block_r_stat_w_param,
				  sizeof(ms_block_r_stat_w_param));
		if (ms_block_reg_addr_cmp(card, &ms_block_r_stat_w_param)) {
			card->next_request = h_ms_block_set_param_addr;
			return 0;
		} else
			return h_ms_block_set_param_addr(card, mrq);
	}
}

static int h_ms_block_trans_data(struct memstick_dev *card,
				 struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if ((*mrq)->error)
		return ms_block_complete_multi(card, mrq, (*mrq)->error);

	msb->dst_page++;
	msb->t_count++;

	memstick_init_req(*mrq, MS_TPC_GET_INT, NULL, 1);
	card->next_request = h_ms_block_data_get_int;

	if (msb->caps & MEMSTICK_CAP_AUTO_GET_INT) {
		(*mrq)->data[0] = (*mrq)->int_reg;
		return h_ms_block_data_get_int(card, mrq);
	}

	return 0;
}

static int h_ms_block_cmd_get_int(struct memstick_dev *card,
				  struct memstick_request **mrq)
{
	unsigned char int_reg = (*mrq)->data[0];
	struct ms_block_data *msb = memstick_get_drvdata(card);

	dev_dbg(&card->dev, "cmd_get_int: %d, %x\n", (*mrq)->error, int_reg);

	if ((*mrq)->error) {
		ms_block_complete_req(card, (*mrq)->error);
		return (*mrq)->error;
	}

	switch (msb->cmd) {
	case MS_CMD_BLOCK_END:
		if (msb->trans_err)
			(*mrq)->error = msb->trans_err;

		ms_block_complete_req(card, (*mrq)->error);
		return msb->trans_err;
	case MS_CMD_BLOCK_READ:
		if (int_reg & MEMSTICK_INT_CMDNAK) {
			ms_block_complete_req(card, -EIO);
			return -EIO;
		} else if (int_reg & MEMSTICK_INT_ERR) {
			memstick_init_req(*mrq, MS_TPC_SET_RW_REG_ADRS,
					  &ms_block_r_stat_w_param,
					  sizeof(ms_block_r_stat_w_param));
			if (ms_block_reg_addr_cmp(card,
						  &ms_block_r_stat_w_param)) {
				card->next_request = h_ms_block_set_stat_addr_r;
				return 0;
			} else
				return h_ms_block_set_stat_addr_r(card, mrq);
		}

		return ms_block_setup_read(card, mrq);
	case MS_CMD_BLOCK_WRITE:
		if (int_reg & MEMSTICK_INT_CMDNAK) {
			ms_block_complete_req(card, -EIO);
			return -EIO;
		} else if (int_reg & MEMSTICK_INT_ERR) {
			ms_block_complete_multi(card, mrq, -EFAULT);
			return -EFAULT;
		}

		return ms_block_setup_write(card, mrq);
	case MS_CMD_BLOCK_ERASE:
		if (int_reg & MEMSTICK_INT_CMDNAK) {
			ms_block_complete_req(card, -EIO);
			return -EIO;
		} else if (int_reg & MEMSTICK_INT_ERR) {
			ms_block_complete_req(card, -EFAULT);
			return -EFAULT;
		}

		/* retry until CED is signalled */
		if (!(int_reg & MEMSTICK_INT_CED))
			return 0;

		/* fall through */
	default: /* other memstick commands */
		ms_block_complete_req(card, (*mrq)->error);
		return (*mrq)->error;
	};
}

static int h_ms_block_set_cmd(struct memstick_dev *card,
			      struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if ((*mrq)->error) {
		ms_block_complete_req(card, (*mrq)->error);
		return (*mrq)->error;
	}

	memstick_init_req(*mrq, MS_TPC_GET_INT, NULL, 1);
	card->next_request = h_ms_block_cmd_get_int;

	if (msb->caps & MEMSTICK_CAP_AUTO_GET_INT) {
		(*mrq)->data[0] = (*mrq)->int_reg;
		return h_ms_block_cmd_get_int(card, mrq);
	}

	return 0;
}

static int h_ms_block_set_extra_addr_w(struct memstick_dev *card,
				       struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if ((*mrq)->error) {
		ms_block_complete_req(card, (*mrq)->error);
		return (*mrq)->error;
	}

	ms_block_reg_addr_set(card, &ms_block_r_stat_w_extra);

	memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG,
			  mtdx_oob_get_next(msb->req_in->req_oob),
			  sizeof(struct ms_extra_data_register));

	card->next_request = h_ms_block_trans_extra;
	return 0;
}

static int h_ms_block_write_param(struct memstick_dev *card,
				  struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if ((*mrq)->error) {
		ms_block_complete_req(card, (*mrq)->error);
		return (*mrq)->error;
	}

	memstick_init_req(&card->current_mrq, MS_TPC_SET_CMD,
			  &msb->cmd, 1);
	card->next_request = h_ms_block_set_cmd;

	return 0;
}

static int h_ms_block_set_param_addr_init(struct memstick_dev *card,
					  struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_param_register param = {
		.system = msb->system,
		.cp = MEMSTICK_CP_BLOCK,
		.page_address = msb->req_in->phy.offset / msb->geo.page_size
	};
	ms_param_set_addr(&param, msb->req_in->phy.b_addr);

	if ((*mrq)->error) {
		ms_block_complete_req(card, (*mrq)->error);
		return (*mrq)->error;
	}

	ms_block_reg_addr_set(card, &ms_block_r_stat_w_param);

	msb->dst_page = param.page_address;
	msb->page_count = msb->req_in->length / msb->geo.page_size;

	dev_dbg(&card->dev, "init request %x, %x, %x:%x\n", msb->req_in->cmd,
		msb->req_in->phy.b_addr, msb->req_in->phy.offset,
		msb->req_in->length);

	switch (msb->req_in->cmd) {
	case MTDX_CMD_READ:
		msb->cmd = MS_CMD_BLOCK_READ;
		if (msb->req_in->req_data) {
			msb->cmd_flags |= MS_BLOCK_FLG_DATA
					  | MS_BLOCK_FLG_PAGE_INC;
			if (msb->req_in->req_oob)
				msb->cmd_flags |= MS_BLOCK_FLG_EXTRA;
		} else if (msb->req_in->req_oob) {
			msb->cmd_flags |= MS_BLOCK_FLG_EXTRA;
			param.cp = MEMSTICK_CP_EXTRA;
		}
		break;
	case MTDX_CMD_ERASE:
		param.page_address = 0;
		msb->cmd = MS_CMD_BLOCK_ERASE;
		break;
	case MTDX_CMD_WRITE:
		msb->cmd = MS_CMD_BLOCK_WRITE;
		msb->cmd_flags |= MS_BLOCK_FLG_WRITE;

		if (msb->req_in->req_data) {
			msb->cmd_flags |= MS_BLOCK_FLG_DATA
					  | MS_BLOCK_FLG_PAGE_INC;
			if (msb->req_in->req_oob)
				msb->cmd_flags |= MS_BLOCK_FLG_EXTRA;
		} else if (msb->req_in->req_oob) {
			msb->cmd_flags |= MS_BLOCK_FLG_EXTRA;
			param.cp = MEMSTICK_CP_EXTRA;
		}
		break;
	case MTDX_CMD_COPY:
		msb->cmd_flags = MS_BLOCK_FLG_COPY;
		msb->cmd = MS_CMD_BLOCK_READ;
		memcpy(&msb->src_pos, &msb->req_in->copy, sizeof(msb->src_pos));

		msb->src_pos.offset /= msb->geo.page_size;

		ms_param_set_addr(&param, msb->src_pos.b_addr);
		param.cp = MEMSTICK_CP_PAGE | MEMSTICK_CP_EXTRA;
		param.page_address = msb->src_pos.offset;
		break;
	default:
		(*mrq)->error = -EINVAL;
	};

	if ((*mrq)->error) {
		ms_block_complete_req(card, (*mrq)->error);
		return (*mrq)->error;
	}

	memstick_init_req(*mrq, MS_TPC_WRITE_REG, &param, sizeof(param));
	card->next_request = h_ms_block_write_param;
	return 0;
}

static void ms_block_complete_req(struct memstick_dev *card, int error)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned long flags;

	spin_lock_irqsave(&msb->lock, flags);
	dev_dbg(&card->dev, "complete %p, %d\n", msb->req_in, error);
	msb->req_dev->end_request(msb->req_dev, msb->req_in,
				  msb->t_count * msb->geo.page_size,
				  error, msb->src_error);
	msb->req_in = NULL;
	msb->t_count = 0;
	msb->trans_err = 0;
	msb->src_error = 0;
	msb->page_count = 0;
	msb->cmd_flags = 0;
	card->next_request = h_ms_block_req_init;
	spin_unlock_irqrestore(&msb->lock, flags);
}

static int ms_block_reset_card(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	enum memstick_command cmd = MS_CMD_RESET;

	card->next_request = h_ms_block_internal_req_init;
	msb->mrq_handler = h_ms_block_default;
	memstick_init_req(&card->current_mrq, MS_TPC_SET_CMD, &cmd, 1);
	card->current_mrq.need_card_int = 0;
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);

	return card->current_mrq.error;
}

static int ms_block_switch_to_parallel(struct memstick_dev *card)
{
	struct memstick_host *host = card->host;
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_param_register param = {
		.system = msb->system | MEMSTICK_SYS_PAM,
		.block_address = { 0, 0, 0},
		.cp = MEMSTICK_CP_BLOCK,
		.page_address = 0
	};
	int rc;

	card->next_request = h_ms_block_internal_req_init;
	msb->mrq_handler = h_ms_block_default;
	memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG, &param,
			  sizeof(param));
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);
	rc = card->current_mrq.error;

	if (rc)
		return rc;

	host->set_param(host, MEMSTICK_INTERFACE, MEMSTICK_PAR4);
	msb->system = param.system;

	card->next_request = h_ms_block_internal_req_init;
	msb->mrq_handler = h_ms_block_default;
	memstick_init_req(&card->current_mrq, MS_TPC_GET_INT, NULL, 1);
	memstick_new_req(host);
	wait_for_completion(&card->mrq_complete);
	rc = card->current_mrq.error;

	if (rc) {
		dev_warn(&card->dev,
			 "interface error, trying to fall back to serial\n");
		msb->system = MEMSTICK_SYS_BAMD;
		host->set_param(host, MEMSTICK_POWER, MEMSTICK_POWER_OFF);
		msleep(10);
		host->set_param(host, MEMSTICK_POWER, MEMSTICK_POWER_ON);
		host->set_param(host, MEMSTICK_INTERFACE, MEMSTICK_SERIAL);

		rc = ms_block_reset_card(card);
		if (rc)
			return rc;

		ms_block_reg_addr_set(card, &ms_block_r_stat_w_param);
		rc = memstick_set_rw_addr(card);
	}

	return rc;
}

unsigned int ms_block_find_boot_block(struct memstick_dev *card,
				      struct ms_block_boot_header *header,
				      unsigned int start_addr)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned int b_cnt;
	struct mtdx_data_iter r_data;
	struct ms_extra_data_register extra;
	struct mtdx_oob r_oob;

	struct mtdx_request m_req = {
		.cmd = MTDX_CMD_READ,
		.logical = MTDX_INVALID_BLOCK,
		.phy = {
			.b_addr = MTDX_INVALID_BLOCK,
			.offset = 0
		},
		.length = sizeof(struct ms_block_boot_header),
		.req_data = &r_data,
		.req_oob = &r_oob
	};

	msb->req_dev = msb->mdev;
	get_device(&msb->mdev->dev);

	for (b_cnt = start_addr; b_cnt < MS_BLOCK_MAX_BOOT_ADDR; ++b_cnt) {
		m_req.phy.b_addr = b_cnt;
		msb->req_dev = msb->mdev;
		msb->req_in = &m_req;
		mtdx_data_iter_init_buf(&r_data, header, m_req.length);
		mtdx_oob_init(&r_oob, &extra, 1, sizeof(extra));
		memset(&extra, 0xff, sizeof(extra));

		dev_dbg(&card->dev, "find block %x\n", b_cnt);

		card->next_request = h_ms_block_req_init;
		memstick_new_req(card->host);
		wait_for_completion(&card->mrq_complete);

		if (msb->trans_err) {
			dev_dbg(&card->dev, "transfer error %d\n",
				msb->trans_err);
			return MTDX_INVALID_BLOCK;
		}

		if (msb->t_count != m_req.length) {
			dev_dbg(&card->dev, "got %x bytes, expected %x\n",
				msb->t_count, m_req.length);
			return MTDX_INVALID_BLOCK;
		}

		dev_dbg(&card->dev, "block %x, ov_flag %x\n",
			b_cnt, extra.overwrite_flag);

		if (!(extra.overwrite_flag & MEMSTICK_OVERWRITE_BKST))
			continue;

		if (!(extra.overwrite_flag
		      & (MEMSTICK_OVERWRITE_PGST0 | MEMSTICK_OVERWRITE_PGST1)))
			continue;

		if (be16_to_cpu(header->block_id) == MS_BLOCK_ID_BOOT)
			return b_cnt;
	}

	return MTDX_INVALID_BLOCK;
}

static int ms_block_read_boot_block(struct memstick_dev *card,
				    struct ms_block_boot_header *header,
				    struct ms_block_boot_ref *b_ref)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned int page_size = be16_to_cpu(header->info.page_size);
	unsigned int p1, p2, p_sum = 0;
	struct ms_extra_data_register *extra = NULL;
	struct mtdx_data_iter r_data;
	struct mtdx_oob r_oob;
	int rc;
	struct mtdx_request m_req = {
		.cmd = MTDX_CMD_READ,
		.logical = MTDX_INVALID_BLOCK,
		.phy = {
			.b_addr = b_ref->phy_block,
			.offset = 0
		},
		.length = 0,
		.req_data = &r_data,
		.req_oob = &r_oob
	};

	if (page_size != msb->geo.page_size) {
		rc = -EINVAL;
		goto out;
	}

	for (rc = 0; rc < header->sys_entry_cnt; ++rc) {
		p1 = be32_to_cpu(header->sys_entry[rc].start_addr);
		p2 = be32_to_cpu(header->sys_entry[rc].data_size);
		p_sum = max(p_sum, p1 + p2);
	}

	rc = 0;

	p_sum += sizeof(struct ms_block_boot_header);
	p1 = p_sum;
	p_sum /= page_size;
	if ((p_sum * page_size) < p1)
		p_sum++;

	extra = kmalloc(p_sum * msb->geo.oob_size, GFP_KERNEL);
	if (!extra) {
		rc = -ENOMEM;
		goto out;
	}
	memset(extra, msb->geo.fill_value, p_sum * msb->geo.oob_size);
	mtdx_oob_init(&r_oob, extra, p_sum, msb->geo.oob_size);

	b_ref->data = kmalloc(p_sum * page_size, GFP_KERNEL);
	if (!b_ref->data) {
		rc = -ENOMEM;
		goto out;
	}

	b_ref->size = p_sum * page_size;
	m_req.length = b_ref->size;

	get_device(&msb->mdev->dev);
	msb->req_dev = msb->mdev;
	msb->req_in = &m_req;
	mtdx_data_iter_init_buf(&r_data, b_ref->data, b_ref->size);

	dev_dbg(&card->dev, "read block addr %x, buf %p, size %x\n",
		b_ref->phy_block, b_ref->data, b_ref->size);

	card->next_request = h_ms_block_req_init;
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);

	dev_dbg(&card->dev, "end read block %d\n", msb->trans_err);
	if (msb->trans_err) {
		rc = msb->trans_err;
		goto out;
	}

	if (msb->t_count != m_req.length) {
		rc = -EIO;
		goto out;
	}

	if (!(extra[0].overwrite_flag & MEMSTICK_OVERWRITE_BKST)) {
		rc = -EFAULT;
		goto out;
	}

	if (!(extra[0].overwrite_flag
	      & (MEMSTICK_OVERWRITE_PGST0 | MEMSTICK_OVERWRITE_PGST1))) {
		rc = -EFAULT;
		goto out;
	}

out:
	if (rc) {
		kfree(b_ref->data);
		b_ref->data = NULL;
		b_ref->size = 0;
	}
	kfree(extra);

	return rc;
}

static int ms_block_get_boot_values(struct ms_block_data *msb,
				    struct ms_block_boot_ref *b_ref)
{
	int off, cnt, s_pos, rc = 0;
	struct ms_block_boot_header *header
		= (struct ms_block_boot_header *)b_ref->data;

	if (b_ref->size < sizeof(struct ms_block_boot_header)) {
		rc = -E2BIG;
		goto out;
	}

	for (s_pos = 0; s_pos < header->sys_entry_cnt; ++s_pos) {
		off = be32_to_cpu(header->sys_entry[s_pos].start_addr);
		cnt = be32_to_cpu(header->sys_entry[s_pos].data_size);
		off += sizeof(struct ms_block_boot_header);

		if (header->sys_entry[s_pos].data_type_id
		    == MS_BLOCK_ENTRY_BAD_BLOCKS) {
			unsigned short *bblk;

			if (!cnt)
				continue;

			if (off + cnt > b_ref->size) {
				rc = -E2BIG;
				break;
			}

			b_ref->bad_blocks = kmalloc((cnt / 2)
						    * sizeof(unsigned int),
						    GFP_KERNEL);

			if (!b_ref->bad_blocks)
				continue;

			cnt /= 2;
			b_ref->bad_blocks_cnt = cnt;

			bblk = (unsigned short *)(b_ref->data + off);

			for (cnt = 0; cnt < b_ref->bad_blocks_cnt; ++cnt)
				b_ref->bad_blocks[cnt] = be16_to_cpu(bblk[cnt]);
		} else if (header->sys_entry[s_pos].data_type_id
			   == MS_BLOCK_ENTRY_CIS_IDI) {
			struct ms_block_idi *idi;

			if (!cnt)
				continue;

			if ((off + cnt > b_ref->size)
			    || (cnt < (MS_BLOCK_CIS_SIZE
				       + sizeof(struct ms_block_idi)))) {
				rc = -E2BIG;
				break;
			}

			b_ref->cis_idi = b_ref->data + off;
			idi = (struct ms_block_idi *)(b_ref->cis_idi
						      + MS_BLOCK_CIS_SIZE);
			b_ref->hd_geo.heads
				= le16_to_cpu(idi->current_logical_heads);
			b_ref->hd_geo.sectors
				= le16_to_cpu(idi->current_sectors_per_track);
			b_ref->hd_geo.cylinders
				= le16_to_cpu(idi->current_logical_cylinders);
		}
	}

out:
	if (rc) {
		kfree(b_ref->data);
		kfree(b_ref->bad_blocks);
		b_ref->bad_blocks_cnt = 0;
		b_ref->data = NULL;
		b_ref->size = 0;
	}

	return rc;
}

static void ms_block_adjust_log_cnt(struct ms_block_data *msb)
{
	struct ms_block_idi *idi;
	unsigned int s_count;

	if (msb->boot_blocks[0].cis_idi)
		idi = (struct ms_block_idi *)(msb->boot_blocks[0].cis_idi
            				      + MS_BLOCK_CIS_SIZE);
	else if (msb->boot_blocks[1].cis_idi)
		idi = (struct ms_block_idi *)(msb->boot_blocks[1].cis_idi
            				      + MS_BLOCK_CIS_SIZE);
	else
		return;

	s_count = le32_to_cpu(idi->addressable_sectors);
	msb->geo.log_block_cnt = s_count / msb->geo.page_cnt;
}

/* Logical block assignment in MemoryStick is fixed and predefined */
static unsigned int ms_block_log_to_zone(struct mtdx_geo *geo,
					 unsigned int log_addr,
					 unsigned int *log_off)
{
	if (log_addr < 494) {
		*log_off = log_addr;
		return 0;
	} else {
		unsigned int zone;

		log_addr -= 494;
		zone = log_addr / 496;
		*log_off = log_addr % 496;
		return zone + 1;
	}
}

static unsigned int ms_block_zone_to_log(struct mtdx_geo *geo,
					 unsigned int zone,
					 unsigned int log_off)
{
	if (!zone) {
		if (log_off < 494)
			return log_off;
		else
			return MTDX_INVALID_BLOCK;
	} else {
		if (log_off < 496) {
			return 494 + (zone - 1) * 496 + log_off;
		} else
			return MTDX_INVALID_BLOCK;
	}
}

static int ms_block_init_card(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct memstick_host *host = card->host;
	struct ms_block_boot_header *header = NULL;
	int rc, rcx;

	msb->caps = host->caps;
	msb->boot_blocks[0].phy_block = MTDX_INVALID_BLOCK;
	msb->boot_blocks[1].phy_block = MTDX_INVALID_BLOCK;
	msb->geo.page_size = sizeof(struct ms_block_boot_header);
	msb->system = MEMSTICK_SYS_BAMD;

	rc = ms_block_reset_card(card);
	if (rc)
		return rc;

	ms_block_reg_addr_set(card, &ms_block_r_stat_w_param);
	rc = memstick_set_rw_addr(card);
	if (rc)
		return rc;

	if (host->caps & MEMSTICK_CAP_PAR4) {
		if (ms_block_switch_to_parallel(card))
			dev_warn(&card->dev, "could not switch to "
				 "parallel interface\n");
	}

	if (msb->system & MEMSTICK_SYS_PAM)
		msb->caps |= MEMSTICK_CAP_AUTO_GET_INT;

	card->next_request = h_ms_block_internal_req_init;
	msb->mrq_handler = h_ms_block_default;
	memstick_init_req(&card->current_mrq, MS_TPC_READ_REG, NULL,
			  sizeof(struct ms_status_register));
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);
	rc = card->current_mrq.error;

	if (rc)
		return rc;

	msb->read_only = (card->current_mrq.data[2] & MEMSTICK_STATUS0_WP)
			 ? 1 : 0;

	dev_dbg(&card->dev, "find boot block\n");
	header = kzalloc(sizeof(struct ms_block_boot_header), GFP_KERNEL);
	if (!header)
		return -ENOMEM;

	msb->geo.zone_size_log = 9;
	msb->geo.oob_size = sizeof(struct ms_extra_data_register);
	msb->geo.fill_value = 0xff;
	msb->geo.log_to_zone = ms_block_log_to_zone;
	msb->geo.zone_to_log = ms_block_zone_to_log;

	msb->boot_blocks[0].phy_block = ms_block_find_boot_block(card, header,
								 0);

	if (msb->boot_blocks[0].phy_block == MTDX_INVALID_BLOCK) {
		rc = -ENOENT;
		goto out;
	}

	dev_dbg(&card->dev, "first boot block: %x\n",
		msb->boot_blocks[0].phy_block);

	msb->geo.log_block_cnt
		= be16_to_cpu(header->info.number_of_effective_blocks);
	msb->geo.phy_block_cnt = be16_to_cpu(header->info.number_of_blocks);
	msb->geo.page_size = be16_to_cpu(header->info.page_size);
	msb->geo.page_cnt = (be16_to_cpu(header->info.block_size) << 10)
			    / msb->geo.page_size;

	dev_dbg(&card->dev, "read first boot block\n");
	rc = ms_block_read_boot_block(card, header, &msb->boot_blocks[0]);

	dev_dbg(&card->dev, "find another boot block\n");
	msb->boot_blocks[1].phy_block
		= ms_block_find_boot_block(card, header,
					   msb->boot_blocks[0].phy_block + 1);

	dev_dbg(&card->dev, "second boot block: %x\n",
		msb->boot_blocks[1].phy_block);

	if (msb->boot_blocks[1].phy_block != MTDX_INVALID_BLOCK)
		rcx = ms_block_read_boot_block(card, header,
					       &msb->boot_blocks[1]);
	else
		rcx = -EIO;

	dev_dbg(&card->dev, "init_card status 1 %d:%d\n", rc, rcx);

	if (!rc)
		rc = ms_block_get_boot_values(msb, &msb->boot_blocks[0]);

	if (!rcx)
		rcx = ms_block_get_boot_values(msb, &msb->boot_blocks[1]);

	dev_dbg(&card->dev, "init_card status 2 %d:%d\n", rc, rcx);
	if (!rc || !rcx) {
		ms_block_adjust_log_cnt(msb);
		rc = 0;
	}

out:
	kfree(header);
	return rc;
}

static int ms_block_check_card(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	int rc = 0;
	unsigned long flags;

	spin_lock_irqsave(&msb->lock, flags);
	if (msb->active)
		rc = 1;
	spin_unlock_irqrestore(&msb->lock, flags);

	return rc;
}

static void ms_block_stop(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned long flags;

	spin_lock_irqsave(&msb->lock, flags);
	while (1) {
		if (!msb->f_thread) {
			if (!msb->active || msb->stopped)
				break;

			if (!msb->req_dev) {
				msb->stopped = 1;
				break;
			}
		}
		spin_unlock_irqrestore(&msb->lock, flags);
		wait_event(msb->req_wq, ms_block_wake_next(msb));
		spin_lock_irqsave(&msb->lock, flags);
	}
	spin_unlock_irqrestore(&msb->lock, flags);
}

static void ms_block_start(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned long flags;

	spin_lock_irqsave(&msb->lock, flags);
	if (!msb->stopped)
		goto out;

	msb->stopped = 0;
	if (!msb->active || mtdx_dev_queue_empty(&msb->c_queue))
		wake_up_all(&msb->req_wq);
	else if (!mtdx_dev_queue_empty(&msb->c_queue)) {
		card->next_request = h_ms_block_req_init;
		memstick_new_req(card->host);
	}
out:
	spin_unlock_irqrestore(&msb->lock, flags);
}

static void ms_block_mtdx_dummy_new_request(struct mtdx_dev *this_dev,
					    struct mtdx_dev *req_dev)
{
	return;
}

static void ms_block_mtdx_new_request(struct mtdx_dev *this_dev,
				      struct mtdx_dev *req_dev)
{
	struct ms_block_data *msb = mtdx_get_drvdata(this_dev);

	get_device(&req_dev->dev);
	mtdx_dev_queue_push_back(&msb->c_queue, req_dev);
	msb->card->host->request(msb->card->host);
}

static int ms_block_sysfs_register(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	int rc;

	rc = sysfs_create_group(&card->dev.kobj, &ms_block_grp_boot0);
	if (rc)
		return rc;

	rc = sysfs_create_group(&card->dev.kobj, &ms_block_grp_boot1);
	if (rc)
		goto err_out_boot0;

	if (!msb->read_only)
		rc = device_create_file(&card->dev, &dev_attr_format);

	if (!rc)
		return 0;

	sysfs_remove_group(&card->dev.kobj, &ms_block_grp_boot1);
err_out_boot0:
	sysfs_remove_group(&card->dev.kobj, &ms_block_grp_boot0);
	return rc;
}

static int ms_block_mtdx_oob_to_info(struct mtdx_dev *this_dev,
				     struct mtdx_page_info *p_info,
				     void *oob)
{
	struct ms_extra_data_register *ms_oob = oob;

	unsigned int addr = be16_to_cpu(ms_oob->logical_address);

	if (addr == 0xffff) {
		p_info->status = MTDX_PAGE_UNMAPPED;
		p_info->log_block = MTDX_INVALID_BLOCK;
	} else {
		p_info->status = MTDX_PAGE_MAPPED;
		p_info->log_block = addr;

		if (!(ms_oob->overwrite_flag & MEMSTICK_OVERWRITE_UDST))
			p_info->status = MTDX_PAGE_SMAPPED;
	}

	if (!(ms_oob->overwrite_flag & MEMSTICK_OVERWRITE_BKST))
		p_info->status = MTDX_PAGE_INVALID;
	else if (!(ms_oob->overwrite_flag
		   & (MEMSTICK_OVERWRITE_PGST0 | MEMSTICK_OVERWRITE_PGST1)))
		p_info->status = MTDX_PAGE_FAILURE;

	return 0;
}

static int ms_block_mtdx_info_to_oob(struct mtdx_dev *this_dev,
				     void *oob,
				     struct mtdx_page_info *p_info)
{
	struct ms_extra_data_register *ms_oob = oob;

	ms_oob->overwrite_flag = 0xff;
	ms_oob->management_flag = 0xff;
	ms_oob->logical_address = 0xffff;

	switch (p_info->status) {
	case MTDX_PAGE_ERASED:
	case MTDX_PAGE_UNMAPPED:
		break;
	case MTDX_PAGE_MAPPED:
		ms_oob->logical_address = cpu_to_be16(p_info->log_block);
		break;
	case MTDX_PAGE_SMAPPED:
		ms_oob->logical_address = cpu_to_be16(p_info->log_block);
		ms_oob->overwrite_flag &= ~MEMSTICK_OVERWRITE_UDST;
		break;
	case MTDX_PAGE_INVALID:
		ms_oob->overwrite_flag &= ~MEMSTICK_OVERWRITE_BKST;
		break;
	case MTDX_PAGE_FAILURE:
		ms_oob->overwrite_flag &= ~(MEMSTICK_OVERWRITE_PGST0
					    | MEMSTICK_OVERWRITE_PGST1);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int ms_block_mtdx_get_param(struct mtdx_dev *this_dev,
				   enum mtdx_param param, void *val)
{
	struct ms_block_data *msb = mtdx_get_drvdata(this_dev);

	switch (param) {
	case MTDX_PARAM_GEO: {
		memcpy(val, &msb->geo, sizeof(msb->geo));
		return 0;
	}
	case MTDX_PARAM_HD_GEO: {
		if (msb->boot_blocks[0].size)
			memcpy(val, &msb->boot_blocks[0].hd_geo,
			       sizeof(struct hd_geometry));
		else if (msb->boot_blocks[1].size)
			 memcpy(val, &msb->boot_blocks[1].hd_geo,
				sizeof(struct hd_geometry));
		else
			return -ENOENT;

		return 0;
	}
	case MTDX_PARAM_SPECIAL_BLOCKS: {
		struct list_head *p_list = val;
		struct mtdx_page_info info;
		unsigned int cnt;
		int rc;

		info.log_block = MTDX_INVALID_BLOCK;
		info.page_offset = 0;

		if (msb->boot_blocks[0].phy_block != MTDX_INVALID_BLOCK) {
			info.status = MTDX_PAGE_RESERVED;
			info.phy_block = msb->boot_blocks[0].phy_block;
			rc = mtdx_page_list_append(p_list, &info);
			if (rc)
				return rc;
		}

		if (msb->boot_blocks[1].phy_block != MTDX_INVALID_BLOCK) {
			info.status = MTDX_PAGE_RESERVED;
			info.phy_block = msb->boot_blocks[1].phy_block;
			rc = mtdx_page_list_append(p_list, &info);
			if (rc)
				return rc;
		}

		for (cnt = 0; cnt < msb->boot_blocks[0].bad_blocks_cnt;
		     ++cnt) {
			info.status = MTDX_PAGE_INVALID;
			info.phy_block = msb->boot_blocks[0].bad_blocks[cnt];
			rc = mtdx_page_list_append(p_list, &info);
			if (rc)
				return rc;
		}

		for (cnt = 0; cnt < msb->boot_blocks[1].bad_blocks_cnt;
		     ++cnt) {
			info.status = MTDX_PAGE_INVALID;
			info.phy_block = msb->boot_blocks[1].bad_blocks[cnt];
			rc = mtdx_page_list_append(p_list, &info);
			if (rc)
				return rc;
		}

		return 0;
	}
	case MTDX_PARAM_READ_ONLY: {
		int *rv = val;
		*rv = msb->read_only ? 1 : 0;
		return 0;
	}
	case MTDX_PARAM_DEV_SUFFIX: {
		char *rv = val;
		sprintf(rv, "%d", this_dev->ord);
		return 0;
	}
	case MTDX_PARAM_DMA_MASK: {
		u64 *rv = val;
		struct memstick_host *host = msb->card->host;

		*rv = BLK_BOUNCE_HIGH;

		if (host->dev.dma_mask && *(host->dev.dma_mask))
			*rv = *(host->dev.dma_mask);

		return 0;
	}
	default:
		return -EINVAL;
	}
}

static struct mtdx_request *ms_block_get_request(struct mtdx_dev *this_dev)
{
	return NULL;
}

static void ms_block_end_request(struct mtdx_dev *this_dev,
				 struct mtdx_request *req,
				 unsigned int count,
				 int dst_error, int src_error)
{
	struct ms_block_data *msb = mtdx_get_drvdata(this_dev);

	msb->trans_err = dst_error;
	msb->src_error = src_error;
	msb->t_count = count;
}

static void ms_block_data_free(struct ms_block_data *msb)
{
	int cnt;

	if (!msb)
		return;

	for (cnt = 0; cnt < 2; ++cnt) {
		kfree(msb->boot_blocks[cnt].data);
		kfree(msb->boot_blocks[cnt].bad_blocks);
	}

	kfree(msb);
}

static int ms_block_probe(struct memstick_dev *card)
{
	const struct mtdx_device_id c_id = {
		MTDX_WMODE_PAGE_PEB_INC,
		MTDX_WMODE_NONE,
		MTDX_RMODE_PAGE_PEB,
		MTDX_RMODE_NONE,
		MTDX_TYPE_MEDIA,
		MTDX_ID_MEDIA_MEMORYSTICK
	};
	struct ms_block_data *msb = kzalloc(sizeof(struct ms_block_data),
					    GFP_KERNEL);
	int rc = 0;

	if (!msb)
		return -ENOMEM;

	memstick_set_drvdata(card, msb);
	msb->card = card;
	spin_lock_init(&msb->lock);
	mtdx_dev_queue_init(&msb->c_queue);
	init_waitqueue_head(&msb->req_wq);

	dev_dbg(&card->dev, "alloc dev\n");
	msb->mdev = mtdx_alloc_dev(&card->dev, &c_id);
	if (!msb->mdev)
		goto err_out_free;

	mtdx_set_drvdata(msb->mdev, msb);
	msb->mdev->new_request = ms_block_mtdx_new_request;
	msb->mdev->get_request = ms_block_get_request;
	msb->mdev->end_request = ms_block_end_request;

	msb->mdev->oob_to_info = ms_block_mtdx_oob_to_info;
	msb->mdev->info_to_oob = ms_block_mtdx_info_to_oob;
	msb->mdev->get_param = ms_block_mtdx_get_param;

	rc = ms_block_init_card(card);
	dev_dbg(&card->dev, "init card %d\n", rc);
	if (rc)
		goto err_out_free_mdev;

	card->check = ms_block_check_card;
	card->stop = ms_block_stop;
	card->start = ms_block_start;


	rc = device_register(&msb->mdev->dev);
	dev_dbg(&card->dev, "dev register %d\n", rc);
	if (rc)
		goto err_out_free_mdev;

	rc = ms_block_sysfs_register(card);
	dev_dbg(&card->dev, "sysfs register %d\n", rc);

	if (!rc) {
		struct mtdx_dev *cdev;
		struct mtdx_device_id c_id = {
			MTDX_WMODE_PAGE, MTDX_WMODE_PAGE_PEB_INC,
			MTDX_RMODE_PAGE, MTDX_RMODE_PAGE_PEB,
			MTDX_TYPE_FTL, MTDX_ID_FTL_SIMPLE
		};

		msb->active = 1;

		/* Temporary hack to insert ftl */
		cdev = mtdx_alloc_dev(&msb->mdev->dev, &c_id);
		if (cdev) {
			rc = device_register(&cdev->dev);
			if (rc)
				__mtdx_free_dev(cdev);
		}
		return 0;
	}

	device_unregister(&msb->mdev->dev);
	goto err_out_free;
err_out_free_mdev:
	__mtdx_free_dev(msb->mdev);
err_out_free:
	memstick_set_drvdata(card, NULL);
	ms_block_data_free(msb);
	return rc;
}

static void ms_block_remove(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct task_struct *f_thread;
	unsigned long flags;

	msb->mdev->new_request = ms_block_mtdx_dummy_new_request;

	spin_lock_irqsave(&msb->lock, flags);
	msb->active = 0;
	f_thread = msb->f_thread;
	msb->f_thread = NULL;
	spin_unlock_irqrestore(&msb->lock, flags);

	if (f_thread)
		kthread_stop(f_thread);

	dev_dbg(&card->dev, "mtdx drop\n");
	mtdx_drop_children(msb->mdev);
	dev_dbg(&card->dev, "mtdx uregister\n");
	device_unregister(&msb->mdev->dev);

	dev_dbg(&card->dev, "block data free\n");
	ms_block_data_free(msb);

	memstick_set_drvdata(card, NULL);
	dev_dbg(&card->dev, "removed\n");
}

#ifdef CONFIG_PM

static int ms_block_suspend(struct memstick_dev *card, pm_message_t state)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned long flags;

	spin_lock_irqsave(&msb->lock, flags);
	while (1) {
		if (msb->stopped) {
			msb->active = 0;
			break;
		}
		spin_unlock_irqrestore(&msb->lock, flags);
		ms_block_stop(card);
		spin_lock_irqsave(&msb->lock, flags);
	}
	spin_unlock_irqrestore(&msb->lock, flags);
	return 0;
}

static int ms_block_resume(struct memstick_dev *card)
{
	int rc = 0;
#ifdef CONFIG_MEMSTICK_UNSAFE_RESUME
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_block_data *new_msb = kzalloc(sizeof(struct ms_block_data),
						GFP_KERNEL);
	struct memstick_host *host = card->host;

	new_msb->card = card;
	spin_lock_init(&new_msb->lock);
	mtdx_dev_queue_init(&new_msb->c_queue);
	init_waitqueue_head(&new_msb->req_wq);

	mutex_lock(&host->lock);
	memstick_set_drvdata(card, new_msb);

	rc = ms_block_init_card(card);
	if (rc)
		goto out;

	if (msb->boot_blocks[0].data) {
		if (!new_msb->boot_blocks[0].data
		    || (new_msb->boot_blocks[0].size
			!= msb->boot_blocks[0].size)
		    || memcmp(new_msb->boot_blocks[0].data,
			      msb->boot_blocks[0].data,
			      msb->boot_blocks[0].size)) {
			rc = -ENODEV;
			goto out;
		}
	} else if (msb->boot_blocks[1].data) {
		if (!new_msb->boot_blocks[1].data
		    || (new_msb->boot_blocks[1].size
			!= msb->boot_blocks[1].size)
		    || memcmp(new_msb->boot_blocks[1].data,
			      msb->boot_blocks[1].data,
			      msb->boot_blocks[1].size)) {
			rc = -ENODEV;
			goto out;
		}
	} else {
		rc = -ENODEV;
		goto out;
	}

out:
	memstick_set_drvdata(card, msb);
	if (!rc) {
		msb->active = 1;
		ms_block_start(card);
	}
	ms_block_data_free(new_msb);
	mutex_unlock(&host->lock);

#endif /* CONFIG_MEMSTICK_UNSAFE_RESUME */
	return rc;
}

#else

#define ms_block_suspend NULL
#define ms_block_resume NULL

#endif /* CONFIG_PM */

static struct memstick_device_id ms_block_id_tbl[] = {
	{MEMSTICK_MATCH_ALL, MEMSTICK_TYPE_LEGACY, MEMSTICK_CATEGORY_STORAGE,
	 MEMSTICK_CLASS_FLASH},
	{MEMSTICK_MATCH_ALL, MEMSTICK_TYPE_DUO, MEMSTICK_CATEGORY_STORAGE_DUO,
	 MEMSTICK_CLASS_DUO},
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
	return memstick_register_driver(&ms_block_driver);
}

static void __exit ms_block_exit(void)
{
	memstick_unregister_driver(&ms_block_driver);
}

module_init(ms_block_init);
module_exit(ms_block_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("Sony MemoryStick block device driver");
MODULE_DEVICE_TABLE(memstick, ms_block_id_tbl);
