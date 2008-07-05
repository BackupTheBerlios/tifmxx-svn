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
#include "mtdx_attr.h"
#include <linux/err.h>
#include <linux/hdreg.h>
#include <linux/sched.h>
#include <linux/wait.h>

#define DRIVER_NAME "ms_block"

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

static int ms_block_attr_date_verify(struct mtdx_attr *attr,
				     unsigned int offset,
				     long param)
{
	unsigned char val[8];
	const char days[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

	if (__mtdx_attr_get_byte_range(attr, &val, offset, sizeof(val))
	    != sizeof(val))
		return -E2BIG;

	if (val[0] != 0x80) {
		if (((char)val[0] > 48) || ((char)val[0] < -48))
			return -EINVAL;
	}

	if (val[3] != 0xff) {
		if (!val[3] || (val[3] > 12))
			return -EINVAL;
	}

	if (val[4] != 0xff) {
		if (!val[4] || (val[4] > days[val[3] - 1]))
			return -EINVAL;
	}

	if (val[5] != 0xff) {
		if (val[5] > 23)
			return -EINVAL;
	}

	if (val[6] != 0xff) {
		if (val[6] > 59)
			return -EINVAL;
	}

	if (val[7] != 0xff) {
		if (val[7] > 59)
			return -EINVAL;
	}

	return 8;
}

static char *ms_block_attr_date_print(struct mtdx_attr *attr,
				      unsigned int offset,
				      unsigned int size, long param)
{
	unsigned char val[8];
	char tz_str[7]; /* +xx:xx */
	char year_str[5], date_str[5][3];
	int v1;

	if (size != 8)
		return NULL;

	if (__mtdx_attr_get_byte_range(attr, &val, offset, size) != size)
		return NULL;

	if (val[0] == 0x80)
		strcpy(tz_str, "+??");
	else {
		int v2;
		v1 = v2 = val[0];
		v1 /= 4;
		if (v2 < 0)
			v2 = -v2;
		v2 = (v2 % 4) * 15;
		snprintf(tz_str, sizeof(tz_str), "%+d:%d", v1, v2);
	}

	v1 = (val[1] << 8) + val[2];
	if (v1 == 0xffff)
		strcpy(year_str, "xxxx");
	else
		snprintf(year_str, sizeof(year_str), "%04d", v1);

	for (v1 = 0; v1 < 5; ++v1) {
		if (val[v1 + 3] == 0xff)
			strcpy(date_str[v1], "xx");
		else
			snprintf(date_str[v1], 3, "%02d", val[v1 + 3]);
	}

	return kasprintf(GFP_KERNEL, "GMT%s %s-%s-%s %s:%s:%s", tz_str,
			 year_str, date_str[0], date_str[1], date_str[2],
			 date_str[3], date_str[4]);
}

struct mtdx_attr_value ms_block_boot_attr_values[] = {
	{"Memory Stick class", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Format unique value 1", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Block size", 2, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Number of blocks", 2, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Number of effective blocks", 2, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Page size", 2, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Extra Data Area size", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Format unique value 2", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Assembly date and time", 0, ms_block_attr_date_verify,
	 ms_block_attr_date_print},
	{"Format unique value 3", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Manufacturer area", 3, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Assembly manufacturer code", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Assembly model code", 3, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Memory manufacturer code", 2, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Memory device code", 2, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Implemented capacity", 2, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Format unique value 4", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Format unique value 5", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"VCC", 1, mtdx_attr_value_range_verify, mtdx_attr_value_be_num_print},
	{"VPP", 1, mtdx_attr_value_range_verify, mtdx_attr_value_be_num_print},
	{"Controller number", 2, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Controller function", 2, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{NULL, 9, mtdx_attr_value_range_verify, NULL},
	{"Parallel-Transfer supporting", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Format unique value 6", 2, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Format type", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Memory Stick application", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Device type", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{NULL, 22, mtdx_attr_value_range_verify, NULL},
	{"Format unique value 7", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{"Format unique value 8", 1, mtdx_attr_value_range_verify,
	 mtdx_attr_value_be_num_print},
	{NULL, 15, mtdx_attr_value_range_verify, NULL},
	{NULL, 0, NULL, NULL}
};

struct ms_block_idi {
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
	struct memstick_dev      *card;
	unsigned int             caps; 
	struct mtdx_dev          *mdev;
	spinlock_t               lock;

	unsigned char            system;
	unsigned char            read_only:1,
				 active:1,
				 stopped:1;
	unsigned char            cmd_flags;
#define MS_BLOCK_FLG_DATA     0x01
#define MS_BLOCK_FLG_EXTRA    0x02
#define MS_BLOCK_FLG_OV       0x04
#define MS_BLOCK_FLG_PAGE_INC 0x08
#define MS_BLOCK_FLG_COPY     0x10

	struct {
		unsigned int     phy_block;
		struct mtdx_attr *attr;
	} boot_blocks[2];

	unsigned int             *bad_blocks;
	struct mtdx_attr_value   *bad_blocks_val;
	struct mtdx_dev_geo      geo;
	struct hd_geometry       hd_geo;

	struct ms_extra_data_register extra;

	wait_queue_head_t        req_wq;
	struct mtdx_dev          *req_dev;
	struct mtdx_request      *req_in;
	int                      (*mrq_handler)(struct memstick_dev *card,
						struct memstick_request **mrq);

	enum memstick_command    cmd;
	struct scatterlist       req_sg;
	unsigned int             sg_offset;
	unsigned int             dst_page;
	unsigned int             src_page;
	unsigned int             page_count;
	unsigned int             t_count;
	int                      trans_err;
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

#define MS_BLOCK_CORRECTABLE (MEMSTICK_STATUS1_FGER | MEMSTICK_STATUS1_EXER \
			      | MEMSTICK_STATUS1_DTER)

#define MS_BLOCK_UNCORRECTABLE (MEMSTICK_STATUS1_UCFG | MEMSTICK_STATUS1_UCEX \
				| MEMSTICK_STATUS1_UCDT)

static int ms_block_complete_req(struct memstick_dev *card, int error);
static int h_ms_block_write_param(struct memstick_dev *card,
				  struct memstick_request **mrq);
static int h_ms_block_set_extra_addr_w(struct memstick_dev *card,
				       struct memstick_request **mrq);
static int h_ms_block_cmd_get_int(struct memstick_dev *card,
				  struct memstick_request **mrq);
static int h_ms_block_set_param_addr_init(struct memstick_dev *card,
					  struct memstick_request **mrq);
static int h_ms_block_set_extra_addr_r(struct memstick_dev *card,
				       struct memstick_request **mrq);
static int h_ms_block_trans_data(struct memstick_dev *card,
				 struct memstick_request **mrq);
static int h_ms_block_write_extra_multi(struct memstick_dev *card,
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

/* Expected callback activation sequence for reading:
 * 1. set_param_addr_init
 * 2. write_param
 * 3. set_cmd
 * 4. cmd_get_int
 * 5-1 (extra)
 * 5-1-1. set_extra_addr_r
 * 5-1-2. read_extra
 * 5-2 (error)
 * 5-2-1. set_stat_addr_r
 * 5-2-2. read_status ?-> 5-1
 * 5-3 (end, page_inc)
 * 5-3-1. set_cmd
 * 5-3-2. cmd_get_int
 * 6. read_data
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
 * 4. cmd_get_int
 *
 *
 * Expected callback activation sequence for writing:
 * 1. set_param_addr_init
 * 2. write_param
 * 3-1 (extra)
 * 3-1-1. set_extra_addr_w (extra)
 * 3-1-2. write_extra_single (!page_inc)
 * 4. set_cmd
 * 5. cmd_get_int
 * 6-1
 * 6-1-1. set_extra_addr_w
 * 6-1-2. write_extra_multi (page_inc)
 * 7. write_data
 * 8-1 (page_inc)
 * 8-1-1. data_get_int
 * 8-1-1. -> 6
 * 8-2 (!page_inc)
 * 8-2-1. set_param_addr
 * 8-2-2. -> 2
 * 8-3 (end)
 * 8-3-1. -> 4
 *
 *
 * Expected callback activation sequence for copying:
 * 1. write_param
 * 2. set_cmd
 * 3. cmd_get_int
 * 4-1 read_status
 * 5. -> 1
 */

static int h_ms_block_internal_req_init(struct memstick_dev *card,
					struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	*mrq = &card->current_mrq;
	card->next_request = msb->mrq_handler;
	return 0;
}

static int h_ms_block_req_init(struct memstick_dev *card,
			       struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	*mrq = &card->current_mrq;

	if (!msb->req_in && msb->req_dev)
		msb->req_in = msb->req_dev->get_request(msb->req_dev);

	if (!msb->req_in) {
		*mrq = NULL;
		complete_all(&card->mrq_complete);
		return -EAGAIN;
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

static int h_ms_block_default(struct memstick_dev *card,
			      struct memstick_request **mrq)
{
	return ms_block_complete_req(card, (*mrq)->error);
}

static int ms_block_set_req_data(struct ms_block_data *msb,
				 struct memstick_request *mrq)
{
	enum memstick_tpc tpc;
	struct scatterlist sg;

	if (msb->cmd == MS_CMD_BLOCK_READ)
		tpc = MS_TPC_READ_LONG_DATA;
	else if (msb->cmd == MS_CMD_BLOCK_WRITE)
		tpc = MS_TPC_WRITE_LONG_DATA;
	else
		return -EINVAL;

	if (msb->page_count - msb->t_count) {
		if ((msb->req_sg.length - msb->sg_offset)
		    < msb->geo.page_size) {
			mrq->error = mtdx_get_data_buf_sg(msb->req_in,
							  &msb->req_sg);
			msb->sg_offset = 0;
			if (mrq->error)
				return mrq->error;
		}
		memcpy(&sg, &msb->req_sg, sizeof(sg));
		sg.offset += msb->sg_offset;
		sg.length = msb->geo.page_size;
		memstick_init_req_sg(mrq, tpc, &sg);
	}

	return 0;
}

static int ms_block_complete_multi(struct memstick_dev *card,
				   struct memstick_request **mrq,
				   int error)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if (msb->cmd_flags & MS_BLOCK_FLG_PAGE_INC) {
		msb->trans_err = error;
		msb->cmd = MS_CMD_BLOCK_END;
		memstick_init_req(*mrq, MS_TPC_SET_CMD, &msb->cmd, 1);
		card->next_request = h_ms_block_cmd_get_int;
		return 0;
	} else
		return ms_block_complete_req(card, error);
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
			return 0;
		}

	if (msb->cmd_flags & MS_BLOCK_FLG_COPY) {
		struct ms_param_register param = {
			.system = msb->system,
			.block_address_msb = (msb->req_in->phy_block >> 16)
					     & 0xff,
			.block_address = cpu_to_be16(msb->req_in->phy_block
						     & 0xffff),
			.cp = MEMSTICK_CP_PAGE | MEMSTICK_CP_EXTRA,
			.page_address = msb->dst_page
		};
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

	if ((msb->cmd_flags & MS_BLOCK_FLG_PAGE_INC)
	    && (msb->cmd_flags & MS_BLOCK_FLG_EXTRA)) {
		msb->mrq_handler = h_ms_block_write_extra_multi;
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
			.block_address_msb = (msb->req_in->src.phy_block >> 16)
					      & 0xff,
			.block_address = cpu_to_be16(msb->req_in->src.phy_block
						     & 0xffff),
			.cp = MEMSTICK_CP_PAGE | MEMSTICK_CP_EXTRA,
			.page_address = msb->src_page + 1
		};

		if (msb->t_count == msb->page_count)
			return ms_block_complete_req(card, 0);

		msb->src_page++;
		msb->dst_page++;
		msb->t_count++;
		msb->cmd = MS_CMD_BLOCK_READ;

		memstick_init_req(*mrq, MS_TPC_WRITE_REG, &param,
				  sizeof(param));
		card->next_request = h_ms_block_write_param;
		return 0;
	}

	return ms_block_complete_multi(card, mrq, -EINVAL);
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
			msb->req_in->phy_block, msb->dst_page,
			status->reserved, status->interrupt, status->status0,
			status->status1);
		return ms_block_complete_multi(card, mrq, -EFAULT);
	}

	if (status->status1 & MS_BLOCK_CORRECTABLE)
		dev_warn(&card->dev, "Correctable error reading %x:%x, "
			 "status: %02x, %02x, %02x, %02x\n",
			 msb->req_in->phy_block, msb->dst_page,
			 status->reserved, status->interrupt, status->status0,
			 status->status1);

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
		.block_address_msb = (msb->req_in->src.phy_block >> 16) & 0xff,
		.block_address = cpu_to_be16(msb->req_in->src.phy_block
					     & 0xffff),
		.cp = MEMSTICK_CP_PAGE | MEMSTICK_CP_EXTRA,
		.page_address = msb->dst_page
	};

	if ((*mrq)->error)
		return ms_block_complete_req(card, (*mrq)->error);

	ms_block_reg_addr_set(card, &ms_block_r_stat_w_param);
	if (!MS_BLOCK_FLG_DATA)
		param.cp &= ~MEMSTICK_CP_PAGE;

	if (!MS_BLOCK_FLG_EXTRA)
		param.cp &= ~MEMSTICK_CP_EXTRA;

	memstick_init_req(*mrq, MS_TPC_WRITE_REG, &param, sizeof(param));
	card->next_request = h_ms_block_write_param;
	return 0;
}

static int h_ms_block_read_extra(struct memstick_dev *card,
				 struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	char *oob_buf;

	if ((*mrq)->error)
		return ms_block_complete_multi(card, mrq, (*mrq)->error);
	
	oob_buf = mtdx_get_oob_buf(msb->req_in);
	if (!oob_buf)
		return ms_block_complete_multi(card, mrq, -ENOMEM);

	memcpy(oob_buf, (*mrq)->data, sizeof(struct ms_extra_data_register));

	if (msb->cmd_flags & MS_BLOCK_FLG_DATA) {
		msb->trans_err = ms_block_set_req_data(msb, *mrq);

		if (msb->trans_err)
			return ms_block_complete_multi(card, mrq,
						       msb->trans_err);

		card->next_request = h_ms_block_trans_data;
		return 0;
	}

	if (msb->cmd_flags & MS_BLOCK_FLG_PAGE_INC)
		return ms_block_complete_multi(card, mrq, -EINVAL);
	else {
		msb->dst_page++;
		msb->t_count++;
		if (msb->t_count == msb->page_count)
			return ms_block_complete_multi(card, mrq, 0);
		else {
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
	card->next_request = h_ms_block_read_extra;
	return 0;
}

static int h_ms_block_write_extra_multi(struct memstick_dev *card,
					struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if ((*mrq)->error)
		return ms_block_complete_multi(card, mrq, (*mrq)->error);

	if (msb->cmd_flags & MS_BLOCK_FLG_DATA) {
		msb->trans_err = ms_block_set_req_data(msb, *mrq);

		if (msb->trans_err)
			return ms_block_complete_multi(card, mrq,
						       msb->trans_err);

		card->next_request = h_ms_block_trans_data;
		return 0;
	}

	return ms_block_complete_multi(card, mrq, -EINVAL);
}

static int h_ms_block_data_get_int(struct memstick_dev *card,
				   struct memstick_request **mrq)
{
	unsigned char int_reg = (*mrq)->data[0];
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if (!(*mrq)->error) {
		if (int_reg & MEMSTICK_INT_CMDNAK)
			return ms_block_complete_multi(card, mrq, -EIO);
		else if (int_reg & MEMSTICK_INT_ERR)
			(*mrq)->error = -EFAULT;
	} else
		return ms_block_complete_multi(card, mrq, (*mrq)->error);

	if (msb->cmd == MS_CMD_BLOCK_READ) {
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

		return ms_block_setup_read(card, mrq);
	} else if (msb->cmd == MS_CMD_BLOCK_WRITE) {
		if (int_reg & MEMSTICK_INT_ERR)
			return ms_block_complete_multi(card, mrq, -EFAULT);

		return ms_block_setup_write(card, mrq);
	} else
		return ms_block_complete_multi(card, mrq, -EINVAL);
}

static int h_ms_block_trans_data(struct memstick_dev *card,
				 struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if ((*mrq)->error)
		return ms_block_complete_multi(card, mrq, (*mrq)->error);

	msb->dst_page++;
	msb->t_count++;

	if (msb->t_count == msb->page_count)
		return ms_block_complete_multi(card, mrq, 0);

	if (msb->cmd_flags & MS_BLOCK_FLG_PAGE_INC) {
		memstick_init_req(*mrq, MS_TPC_GET_INT, NULL, 1);

		if (msb->caps & MEMSTICK_CAP_AUTO_GET_INT)
			return h_ms_block_data_get_int(card, mrq);
		else {
			card->next_request = h_ms_block_data_get_int;
			return 0;
		}
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

static int h_ms_block_cmd_get_int(struct memstick_dev *card,
				  struct memstick_request **mrq)
{
	unsigned char int_reg = (*mrq)->data[0];
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if (!(*mrq)->error) {
		if (int_reg & MEMSTICK_INT_CMDNAK) {
			(*mrq)->error = -EIO;
			return ms_block_complete_req(card, (*mrq)->error);
		} else if (int_reg & MEMSTICK_INT_ERR)
			(*mrq)->error = -EFAULT;
	} else
		return ms_block_complete_req(card, (*mrq)->error);

	switch (msb->cmd) {
	case MS_CMD_BLOCK_END:
		if (msb->trans_err)
			(*mrq)->error = msb->trans_err;

		return ms_block_complete_req(card, (*mrq)->error);
	case MS_CMD_BLOCK_READ:
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

		return ms_block_setup_read(card, mrq);
	case MS_CMD_BLOCK_WRITE:
		if (int_reg & MEMSTICK_INT_ERR)
			return ms_block_complete_multi(card, mrq, -EFAULT);

		return ms_block_setup_write(card, mrq);
	default: /* other memstick commands - erase, reset, etc */
		return ms_block_complete_req(card, (*mrq)->error);
	};
}

static int h_ms_block_set_cmd(struct memstick_dev *card,
			      struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if ((*mrq)->error)
		return ms_block_complete_req(card, (*mrq)->error);

	if (msb->caps & MEMSTICK_CAP_AUTO_GET_INT) {
		(*mrq)->data[0] = (*mrq)->int_reg;
		return h_ms_block_cmd_get_int(card, mrq);
	}

	memstick_init_req(*mrq, MS_TPC_GET_INT, NULL, 1);
	card->next_request = h_ms_block_cmd_get_int;

	return 0;
}

static int h_ms_block_write_extra_single(struct memstick_dev *card,
					 struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if ((*mrq)->error)
		return ms_block_complete_req(card, (*mrq)->error);

	memstick_init_req(&card->current_mrq, MS_TPC_SET_CMD,
			  &msb->cmd, 1);
	card->next_request = h_ms_block_set_cmd;

	return 0;	
}

static int h_ms_block_set_extra_addr_w(struct memstick_dev *card,
				       struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if ((*mrq)->error)
		return ms_block_complete_req(card, (*mrq)->error);

	ms_block_reg_addr_set(card, &ms_block_r_stat_w_extra);
	if (!(msb->cmd_flags & MS_BLOCK_FLG_PAGE_INC)) {
		if (msb->cmd_flags & MS_BLOCK_FLG_OV) {
			struct ms_extra_data_register extra = {0xff, 0xff,
							       0xffff};
			switch (msb->req_in->cmd) {
			case MTDX_CMD_SELECT:
				extra.overwrite_flag
				&= ~MEMSTICK_OVERWRITE_UDST;
				break;
			case MTDX_CMD_INVALIDATE:
				extra.overwrite_flag
				&= ~MEMSTICK_OVERWRITE_BKST;
				break;
			default:
				(*mrq)->error = -EINVAL;
				return ms_block_complete_req(card,
							     (*mrq)->error);
			}
			memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG,
					  &extra, sizeof(extra));
		} else {
			char *oob_buf = mtdx_get_oob_buf(msb->req_in);

			if (!oob_buf)
				return ms_block_complete_multi(card, mrq,
							       -ENOMEM);

			memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG,
					  oob_buf,
					  sizeof(struct
						 ms_extra_data_register));
		}
		card->next_request = msb->mrq_handler;

		return 0;
	}

	memstick_init_req(&card->current_mrq, MS_TPC_SET_CMD,
			  &msb->cmd, 1);
	card->next_request = h_ms_block_set_cmd;

	return 0;
}

static int h_ms_block_write_param(struct memstick_dev *card,
				  struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	if ((*mrq)->error)
		return ms_block_complete_req(card, (*mrq)->error);

	if ((msb->cmd == MS_CMD_BLOCK_WRITE)
	    && (msb->cmd_flags & MS_BLOCK_FLG_EXTRA)) {
		memstick_init_req(*mrq, MS_TPC_SET_RW_REG_ADRS,
				  &ms_block_r_stat_w_extra,
				  sizeof(ms_block_r_stat_w_extra));

		msb->mrq_handler = h_ms_block_write_extra_single;
		if (ms_block_reg_addr_cmp(card, &ms_block_r_stat_w_extra)) {
			card->next_request = h_ms_block_set_extra_addr_w;
			return 0;
		} else
			return h_ms_block_set_extra_addr_w(card, mrq);
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
		.block_address_msb = (msb->req_in->phy_block >> 16) & 0xff,
		.block_address = cpu_to_be16(msb->req_in->phy_block & 0xffff),
		.cp = MEMSTICK_CP_BLOCK,
		.page_address = msb->req_in->offset / msb->geo.page_size
	};

	if ((*mrq)->error)
		return ms_block_complete_req(card, (*mrq)->error);

	ms_block_reg_addr_set(card, &ms_block_r_stat_w_param);

	msb->cmd_flags = 0;
	msb->sg_offset = 0;
	msb->dst_page = param.page_address;
	msb->page_count = msb->req_in->length / msb->geo.page_size;
	msb->t_count = 0;
	msb->trans_err = 0;

	switch (msb->req_in->cmd) {
	case MTDX_CMD_READ:
		msb->cmd_flags = MS_BLOCK_FLG_EXTRA | MS_BLOCK_FLG_DATA
				 | MS_BLOCK_FLG_PAGE_INC;
		msb->cmd = MS_CMD_BLOCK_READ;
		(*mrq)->error = mtdx_get_data_buf_sg(msb->req_in, &msb->req_sg);
		break;
	case MTDX_CMD_READ_DATA:
		msb->cmd_flags = MS_BLOCK_FLG_DATA | MS_BLOCK_FLG_PAGE_INC;
		msb->cmd = MS_CMD_BLOCK_READ;
		(*mrq)->error = mtdx_get_data_buf_sg(msb->req_in, &msb->req_sg);
		break;
	case MTDX_CMD_READ_OOB:
		msb->cmd_flags = MS_BLOCK_FLG_EXTRA;
		msb->cmd = MS_CMD_BLOCK_READ;
		param.cp = MEMSTICK_CP_EXTRA;
		break;
	case MTDX_CMD_ERASE:
		param.page_address = 0;
		msb->cmd = MS_CMD_BLOCK_ERASE;
		break;
	case MTDX_CMD_WRITE:
		msb->cmd_flags = MS_BLOCK_FLG_EXTRA | MS_BLOCK_FLG_DATA
				 | MS_BLOCK_FLG_PAGE_INC;
		msb->cmd = MS_CMD_BLOCK_WRITE;
		(*mrq)->error = mtdx_get_data_buf_sg(msb->req_in, &msb->req_sg);
		break;
	case MTDX_CMD_WRITE_DATA:
		msb->cmd_flags = MS_BLOCK_FLG_DATA | MS_BLOCK_FLG_PAGE_INC;
		msb->cmd = MS_CMD_BLOCK_WRITE;
		(*mrq)->error = mtdx_get_data_buf_sg(msb->req_in, &msb->req_sg);
		break;
	case MTDX_CMD_WRITE_OOB:
		msb->cmd_flags = MS_BLOCK_FLG_EXTRA;
		msb->cmd = MS_CMD_BLOCK_WRITE;
		param.cp = MEMSTICK_CP_EXTRA;
		break;
	case MTDX_CMD_SELECT:
		msb->cmd_flags = MS_BLOCK_FLG_OV | MS_BLOCK_FLG_EXTRA;
		msb->cmd = MS_CMD_BLOCK_WRITE;
		param.cp = MEMSTICK_CP_OVERWRITE;
		break;
	case MTDX_CMD_INVALIDATE:
		msb->cmd_flags = MS_BLOCK_FLG_OV | MS_BLOCK_FLG_EXTRA;
		msb->cmd = MS_CMD_BLOCK_WRITE;
		param.cp = MEMSTICK_CP_OVERWRITE;
		break;
	case MTDX_CMD_COPY:
		msb->cmd_flags = MS_BLOCK_FLG_COPY;
		msb->cmd = MS_CMD_BLOCK_READ;
		param.block_address_msb = (msb->req_in->src.phy_block >> 16)
					   & 0xff;
		param.block_address = cpu_to_be16(msb->req_in->src.phy_block
						  & 0xffff);
		param.cp = MEMSTICK_CP_PAGE | MEMSTICK_CP_EXTRA;
		param.page_address = msb->req_in->src.offset
				     / msb->geo.page_size;
		msb->src_page = param.page_address;
		break;
	default:
		(*mrq)->error = -EINVAL;
	};

	if ((*mrq)->error)
		return ms_block_complete_req(card, (*mrq)->error);

	memstick_init_req(*mrq, MS_TPC_WRITE_REG, &param, sizeof(param));
	card->next_request = h_ms_block_write_param;
	return 0;
}

static int ms_block_complete_req(struct memstick_dev *card, int error)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned long flags;

	spin_lock_irqsave(&msb->lock, flags);
	dev_dbg(&card->dev, "complete %p, %d\n", msb->req_in, error);

	if (msb->req_in) {
		/* Nothing to do - not really an error */
		if (error == -EAGAIN)
			error = 0;

		mtdx_complete_request(msb->req_in, error, msb->t_count);
		msb->req_in = NULL;
		card->next_request = h_ms_block_req_init;

		msb->req_in = msb->req_dev->get_request(msb->req_dev);

		if (!msb->req_in)
			error = -EAGAIN;
	} else if (!error)
		error = -EAGAIN;

	if (!error)
		memstick_new_req(card->host);
	else {
		if (msb->req_dev) {
			msb->req_dev = NULL;
			wake_up(&msb->req_wq);
		}

		complete_all(&card->mrq_complete);
	}

	spin_unlock_irqrestore(&msb->lock, flags);
	return error;
}

static int ms_block_switch_to_parallel(struct memstick_dev *card)
{
	struct memstick_host *host = card->host;
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_param_register param = {
		.system = msb->system | MEMSTICK_SYS_PAM,
		.block_address_msb = 0,
		.block_address = 0,
		.cp = MEMSTICK_CP_BLOCK,
		.page_address = 0
	};

	card->next_request = h_ms_block_internal_req_init;
	msb->mrq_handler = h_ms_block_default;
	memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG, &param,
			  sizeof(param));
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);

	if (card->current_mrq.error)
		return card->current_mrq.error;

	msb->system |= MEMSTICK_SYS_PAM;
	host->set_param(host, MEMSTICK_INTERFACE, MEMSTICK_PAR4);

	card->next_request = h_ms_block_internal_req_init;
	msb->mrq_handler = h_ms_block_default;
	memstick_init_req(&card->current_mrq, MS_TPC_GET_INT, NULL, 1);
	memstick_new_req(host);
	wait_for_completion(&card->mrq_complete);

	if (card->current_mrq.error) {
		msb->system &= ~MEMSTICK_SYS_PAM;
		host->set_param(host, MEMSTICK_INTERFACE, MEMSTICK_SERIAL);
		return -EFAULT;
	}

	return 0;
}

unsigned int ms_block_find_boot_block(struct memstick_dev *card,
				      struct ms_block_boot_header *header,
				      unsigned int start_addr)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned int b_cnt;

	struct mtdx_request m_req = {
		.src_dev = msb->mdev,
		.cmd = MTDX_CMD_READ,
		.log_block = MTDX_INVALID_BLOCK,
		.phy_block = MTDX_INVALID_BLOCK,
		.offset = 0,
		.length = sizeof(struct ms_block_boot_header)
	};

	for (b_cnt = start_addr; b_cnt < MS_BLOCK_MAX_BOOT_ADDR; ++b_cnt) {
		m_req.phy_block = b_cnt;
		msb->req_in = &m_req;
		sg_set_buf(&msb->req_sg, header, m_req.length);
		memset(&msb->extra, 0xff, sizeof(msb->extra));

		card->next_request = h_ms_block_req_init;
		memstick_new_req(card->host);
		wait_for_completion(&card->mrq_complete);

		if (msb->trans_err)
			return msb->trans_err;

		if (msb->t_count != m_req.length)
			return -EIO;

		if (!(msb->extra.overwrite_flag & MEMSTICK_OVERWRITE_BKST))
			continue;

		if (!(msb->extra.overwrite_flag
		      & (MEMSTICK_OVERWRITE_PGST0 | MEMSTICK_OVERWRITE_PGST1)))
			continue;

		if (be16_to_cpu(header->block_id) == MS_BLOCK_ID_BOOT)
			return b_cnt;
	}

	return MTDX_INVALID_BLOCK;
}

static int ms_block_init_card(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct memstick_host *host = card->host;
	struct ms_block_boot_header *header;
	int rc;
	enum memstick_command cmd = MS_CMD_RESET;

	msb->boot_blocks[0].phy_block = MTDX_INVALID_BLOCK;
	msb->boot_blocks[1].phy_block = MTDX_INVALID_BLOCK;
	msb->geo.page_size = sizeof(struct ms_block_boot_header);
	msb->system = MEMSTICK_SYS_BAMD;

	card->next_request = h_ms_block_internal_req_init;;
	msb->mrq_handler = h_ms_block_default;
	memstick_init_req(&card->current_mrq, MS_TPC_SET_CMD, &cmd, 1);
	card->current_mrq.need_card_int = 0;
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);

	if (card->current_mrq.error)
		return -ENODEV;

	if (ms_block_reg_addr_cmp(card, &ms_block_r_stat_w_param)) {
		card->next_request = h_ms_block_internal_req_init;;
		msb->mrq_handler = h_ms_block_default;
		memstick_init_req(&card->current_mrq, MS_TPC_SET_RW_REG_ADRS,
				  &ms_block_r_stat_w_param,
				  sizeof(ms_block_r_stat_w_param));
		memstick_new_req(card->host);
		wait_for_completion(&card->mrq_complete);
		if (card->current_mrq.error)
			return card->current_mrq.error;

		ms_block_reg_addr_set(card, &ms_block_r_stat_w_param);
	}

	if (host->caps & MEMSTICK_CAP_PAR4) {
		if (ms_block_switch_to_parallel(card))
			printk(KERN_WARNING "%s: could not switch to "
			       "parallel interface\n", card->dev.bus_id);
	}

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

	header = kzalloc(sizeof(struct ms_block_boot_header), GFP_KERNEL);
	if (!header)
		return -ENOMEM;
 
	msb->boot_blocks[0].phy_block = ms_block_find_boot_block(card, header,
								 0);

	if (msb->boot_blocks[0].phy_block == MTDX_INVALID_BLOCK) {
		rc = -EFAULT;
		goto err_out;
	}

	msb->geo.zone_cnt_log = 9;
	msb->geo.log_block_cnt
		= be16_to_cpu(header->info.number_of_effective_blocks);
	msb->geo.phy_block_cnt = be16_to_cpu(header->info.number_of_blocks);
	msb->geo.page_size = be16_to_cpu(header->info.page_size);
	msb->geo.page_cnt = be16_to_cpu(header->info.block_size)
			    / msb->geo.page_size;
	msb->geo.oob_size = sizeof(struct ms_extra_data_register);

	msb->boot_blocks[1].phy_block
		= ms_block_find_boot_block(card, header,
					   msb->boot_blocks[0].phy_block + 1);

err_out:
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

static int ms_block_wake_next(struct ms_block_data *msb)
{
	int rc = 0;
	unsigned long flags;

	spin_lock_irqsave(&msb->lock, flags);
	if (!msb->active || !(msb->stopped || msb->req_dev))
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
		if (!msb->active || msb->stopped)
			break;

		if (!msb->req_dev) {
			msb->stopped = 1;
			break;
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
	if (!msb->active)
		wake_up_all(&msb->req_wq);
	else if (msb->req_dev)
		wake_up(&msb->req_wq);
out:
	spin_unlock_irqrestore(&msb->lock, flags);
}

static int ms_block_mtdx_dummy_new_request(struct mtdx_dev *this_dev,
					   struct mtdx_dev *req_dev)
{
	return -ENODEV;
}

static int ms_block_mtdx_new_request(struct mtdx_dev *this_dev,
				     struct mtdx_dev *req_dev)
{
	struct memstick_dev *card = container_of(this_dev->dev.parent,
						 struct memstick_dev,
						 dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);
	int rc = 0;
	unsigned long flags;

	spin_lock_irqsave(&msb->lock, flags);
	while (1) {
		if (!msb->active) {
			rc = -ENODEV;
			break;
		}

		if (!(msb->stopped || msb->req_dev)) {
			msb->req_dev = req_dev;
			break;
		} else {
			spin_unlock_irqrestore(&msb->lock, flags);
			rc = wait_event_interruptible(msb->req_wq,
						      ms_block_wake_next(msb));
			spin_lock_irqsave(&msb->lock, flags);
		}
	}

	if (!rc) {
		card->next_request = h_ms_block_req_init;
		memstick_new_req(card->host);
	}

	spin_unlock_irqrestore(&msb->lock, flags);
	return rc;
}

static int ms_block_sysfs_register(struct memstick_dev *card)
{
#warning Implement!!!
	return 0;
}

static void ms_block_sysfs_unregister(struct memstick_dev *card)
{
#warning Implement!!!
}

static int ms_block_mtdx_oob_to_info(struct mtdx_dev *this_dev,
				     struct mtdx_page_info *p_info,
				     void *oob)
{
	struct memstick_dev *card = container_of(this_dev->dev.parent,
						 struct memstick_dev,
						 dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);
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
	else if (p_info->phy_block != MTDX_INVALID_BLOCK) {
		if ((p_info->phy_block == msb->boot_blocks[0].phy_block)
		     || (p_info->phy_block == msb->boot_blocks[1].phy_block))
				p_info->status = MTDX_PAGE_RESERVED;
		else {
			int cnt;

			if (!msb->bad_blocks)
				return 0;

			for (cnt = 0;
			     msb->bad_blocks[cnt] != MTDX_INVALID_BLOCK;
			     ++cnt) {
				if (msb->bad_blocks[cnt] == p_info->phy_block) {
					p_info->status = MTDX_PAGE_INVALID;
					break;
				}
			}
		}
	}
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
	struct memstick_dev *card = container_of(this_dev->dev.parent,
						 struct memstick_dev,
						 dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);

	switch (param) {
	case MTDX_PARAM_GEO: {
		memcpy(val, &msb->geo, sizeof(msb->geo));
		return 0;
	}
	case MTDX_PARAM_HD_GEO: {
		memcpy(val, &msb->hd_geo, sizeof(msb->hd_geo));
/*
		struct hd_geometry *geo = val;

		geo->heads = msb->idi.current_logical_heads;
		geo->sectors = msb->idi.current_sectors_per_track;
		geo->cylinders = msb->idi.current_logical_cylinders;
*/
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

static void ms_block_end_request(struct mtdx_request *req, int error,
				 unsigned int count)
{
	struct memstick_dev *card = container_of(req->src_dev->dev.parent,
						 struct memstick_dev,
						 dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);

	msb->trans_err = error;
	msb->t_count = count;
}

static int ms_block_get_data_buf_sg(struct mtdx_request *req,
				    struct scatterlist *sg)
{
	return 0;
}

static char* ms_block_get_oob_buf(struct mtdx_request *req)
{
	struct memstick_dev *card = container_of(req->src_dev->dev.parent,
						 struct memstick_dev,
						 dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);

	return (char *)&msb->extra;
}

static int ms_block_probe(struct memstick_dev *card)
{
	const struct mtdx_device_id c_id = {
		MTDX_WMODE_PAGE_INC,
		MTDX_WMODE_NONE,
		MTDX_RMODE_MPAGE_BLK,
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
	init_waitqueue_head(&msb->req_wq);
	msb->stopped = 1;

	rc = ms_block_init_card(card);
	if (rc)
		goto err_out_free;

	card->check = ms_block_check_card;
	card->stop = ms_block_stop;
	card->start = ms_block_start;

	msb->mdev = mtdx_alloc_dev(&card->dev, &c_id);
	if (!msb->mdev)
		goto err_out_free;

	msb->mdev->new_request = ms_block_mtdx_new_request;
	msb->mdev->get_request = ms_block_get_request;
	msb->mdev->end_request = ms_block_end_request;
	msb->mdev->get_data_buf_sg = ms_block_get_data_buf_sg;
	msb->mdev->get_oob_buf = ms_block_get_oob_buf;

	msb->mdev->oob_to_info = ms_block_mtdx_oob_to_info;
	msb->mdev->info_to_oob = ms_block_mtdx_info_to_oob;
	msb->mdev->get_param = ms_block_mtdx_get_param;

	rc = device_register(&msb->mdev->dev);
	if (rc) {
		__mtdx_free_dev(msb->mdev);
		goto err_out_free;
	}

	card->start(card);

	rc = ms_block_attr_register(card);
	if (!rc)
		return 0;

	device_unregister(&msb->mdev->dev);	
err_out_free:
	memstick_set_drvdata(card, NULL);
	kfree(msb);
	return rc;
}

static void ms_block_remove(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	unsigned long flags;

	msb->mdev->new_request = ms_block_mtdx_dummy_new_request;

	spin_lock_irqsave(&msb->lock, flags);
	msb->active = 0;
	wake_up_all(&msb->req_wq);
	spin_unlock_irqrestore(&msb->lock, flags);

	while (waitqueue_active(&msb->req_wq))
		msleep(1);

	ms_block_attr_unregister(card);
	device_unregister(&msb->mdev->dev);

	memstick_set_drvdata(card, NULL);
}

#ifdef CONFIG_PM

static int ms_block_suspend(struct memstick_dev *card, pm_message_t state)
{
//	struct ms_block_data *msb = memstick_get_drvdata(card);

#warning Implement!!!
	return 0;
}

static int ms_block_resume(struct memstick_dev *card)
{
//	struct ms_block_data *msb = memstick_get_drvdata(card);
	int rc = 0;

#warning Implement!!!
#ifdef CONFIG_MEMSTICK_UNSAFE_RESUME


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
