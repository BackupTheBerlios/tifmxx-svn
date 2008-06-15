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
#include <linux/err.h>
#include <linux/hdreg.h>

#define DRIVER_NAME "ms_block"

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

struct ms_idi {
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
	struct mtdx_dev          *mdev;
	struct mtdx_dev          *fdev;
	spinlock_t               lock;

	unsigned char            system;
	unsigned char            read_only:1,
				 active:1,
				 stopped:1,
				 skip_extra:1,
				 auto_page_inc:1,
				 page_copy:1,
				 format_media:1;

	struct ms_boot_attr_info boot_attr;
	char                     cis[256];
	struct ms_idi            idi;
	unsigned int             boot_blocks[2];
	unsigned int             *bad_blocks;
	unsigned int             page_size;

	struct ms_extra_data_register current_extra;
	struct mtdx_request      *req_in;
	int                      (*mrq_handler)(struct memstick_dev *card,
						struct memstick_request **mrq);

	enum memstick_command    cmd;
	unsigned int             page_offset;
	unsigned int             src_offset;
	unsigned int             page_count;
	unsigned int             t_count;
};

static int ms_block_complete_req(struct memstick_dev *card, int error);

static int h_ms_block_req_init(struct memstick_dev *card,
			       struct memstick_request **mrq)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	*mrq = &card->current_mrq;
	card->next_request = msb->mrq_handler;
	return 0;
}

static int h_ms_block_default(struct memstick_dev *card,
			      struct memstick_request **mrq)
{
	return ms_block_complete_req(card, (*mrq)->error);
}

static int h_ms_block_default_bad(struct memstick_dev *card,
				  struct memstick_request **mrq)
{
	return -ENXIO;
}

static int h_ms_block_write_param(struct memstick_dev *card,
				  struct memstick_request **mrq)
{
	if ((*mrq)->error)
		return ms_block_complete_req(card, (*mrq)->error);

	if (msb->cmd == MS_CMD_BLOCK_WRITE) {
#warning set rw reg addr to point to extra data
#warning set command
#warning wait for int / get int
#warning write extra reg
#warning write page data -> goto wait int
#warning set command BLOCK_END
#warning wait for int / get int / check CED
	} else {
		memstick_init_req(&card->current_mrq, MS_TPC_SET_CMD,
				  &msb->cmd, 1);
		card->next_request = h_ms_block_set_cmd;

	}
	return 0;
}

static int ms_block_setup_request(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct ms_param_register param = {
		.system = msb->system,
		.block_address_msb = (msb->req_in->phy_block >> 16) & 0xff,
		.block_address = cpu_to_be16(msb->req_in->phy_block & 0xffff),
		.cp = MEMSTICK_CP_BLOCK,
		.page_address = msb->req_in->offset / msb->page_size
	};

	msb->page_offset = param.page_address;
	msb->page_count = msb->req_in->length / msb->page_size;
	msb->t_count = 0;
	msb->skip_extra = 0;
	msb->auto_page_inc = 0;
	msb->page_copy = 0;

	switch (msb->req_in->cmd) {
	case MTDX_CMD_READ:
		msb->auto_page_inc = 1;
		msb->cmd = MS_CMD_BLOCK_READ;
		break;
	case MTDX_CMD_READ_DATA:
		msb->skip_extra = 1;
		msb->auto_page_inc = 1;
		msb->cmd = MS_CMD_BLOCK_READ;
		break;
	case MTDX_CMD_READ_OOB:
		msb->cmd = MS_CMD_BLOCK_READ;
		param.cp = MEMSTICK_CP_EXTRA;
		break;
	case MTDX_CMD_ERASE:
		param.page_address = 0;
		msb->cmd = MS_CMD_BLOCK_ERASE;
		break;
	case MTDX_CMD_WRITE:
		msb->auto_page_inc = 1;
		msb->cmd = MS_CMD_BLOCK_WRITE;
		break;
	case MTDX_CMD_WRITE_DATA:
		msb->skip_extra = 1;
		msb->auto_page_inc = 1;
		msb->cmd = MS_CMD_BLOCK_WRITE;
		break;
	case MTDX_CMD_WRITE_OOB:
		msb->cmd = MS_CMD_BLOCK_WRITE;
		param.cp = MEMSTICK_CP_EXTRA;
		break;
	case MTDX_CMD_INVALIDATE:
		msb->cmd = MS_CMD_BLOCK_WRITE;
		param.cp = MEMSTICK_CP_OVERWRITE;
		break;
	case MTDX_CMD_COPY:
		msb->page_copy = 1;
		msb->cmd = MS_CMD_BLOCK_READ;
		param.cp = MEMSTICK_CP_PAGE;
		msb->src_offset = msb->req_in.src.offset / msb->page_size;
		break;
	default:
		return -EINVAL;
	};

	card->next_request = h_ms_block_req_init;
	msb->mrq_handler = h_ms_block_write_param;
	memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG,
			  &param, sizeof(param));
	memstick_new_req(card->host);

	return 0;
}

static int ms_block_complete_req(struct memstick_dev *card, int error)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	int chunk, cnt;
	unsigned int t_len = 0;
	unsigned long flags;

	spin_lock_irqsave(&msb->lock, flags);
	dev_dbg(&card->dev, "complete %p, %d\n", msb->req_in, error);

	if (msb->req_in) {
		/* Nothing to do - not really an error */
		if (error == -EAGAIN)
			error = 0;

		mtdx_complete_request(msb->req_in, error, msb->t_count);

		if (msb->fdev->get_request)
			msb->req_in = msb->fdev->get_request(msb->fdev);

		if (!msb->req_in)
			error = -EAGAIN;

#warning Ugh!!!

	}

	card->next_request = h_ms_block_default_bad;
	complete_all(&card->mrq_complete);
out:
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

	card->next_request = h_ms_block_req_init;
	msb->mrq_handler = h_ms_block_default;
	memstick_init_req(&card->current_mrq, MS_TPC_WRITE_REG, &param,
			  sizeof(param));
	memstick_new_req(card->host);
	wait_for_completion(&card->mrq_complete);

	if (card->current_mrq.error)
		return card->current_mrq.error;

	msb->system |= MEMSTICK_SYS_PAM;
	host->set_param(host, MEMSTICK_INTERFACE, MEMSTICK_PAR4);

	card->next_request = h_ms_block_req_init;
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

static int ms_block_init_card(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);
	struct memstick_host *host = card->host;
	char *buf;
	struct ms_boot_page *boot_pages[2];
	int rc;
	unsigned char t_val = MS_CMD_RESET;

	msb->boot_blocks[0] = MTDX_INVALID_BLOCK;
	msb->boot_blocks[1] = MTDX_INVALID_BLOCK;
	msb->page_size = sizeof(struct ms_boot_page);
	msb->system = MEMSTICK_SYS_BAMD;

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

	if (host->caps & MEMSTICK_CAP_PAR4) {
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
	if (rc || boot_blocks[0] == MTDX_INVALID_BLOCK)
		goto out_free_buf;

	memcpy(&msb->boot_attr, &boot_pages[0]->attr, sizeof(msb->boot_attr));

	ms_block_mark_used(msb, boot_blocks[0]);
	if (boot_blocks[1] != MTDX_INVALID_BLOCK)
		ms_block_mark_used(msb, boot_blocks[1]);

	rc = ms_block_fetch_bad_blocks(card, boot_pages[0],
				       boot_blocks[0]);

	if (rc && boot_blocks[1] != MTDX_INVALID_BLOCK) {
		memcpy(&msb->boot_attr, &boot_pages[1]->attr,
		       sizeof(msb->boot_attr));
		boot_blocks[0] = MTDX_INVALID_BLOCK;
		rc = ms_block_fetch_bad_blocks(card, boot_pages[1],
					       boot_blocks[1]);
	}

	if (rc)
		goto out_free_buf;

	if (boot_blocks[0] != MTDX_INVALID_BLOCK) {
		rc = ms_block_fetch_cis_idi(card, boot_pages[0],
					    boot_blocks[0]);

		if (rc && boot_blocks[1] != MTDX_INVALID_BLOCK) {
			memcpy(&msb->boot_attr, &boot_pages[1]->attr,
			       sizeof(msb->boot_attr));
			rc = ms_block_fetch_cis_idi(card, boot_pages[1],
						    boot_blocks[1]);
		}
	} else if (boot_blocks[1] != MTDX_INVALID_BLOCK)
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
#warning Implement!!!
	return 0;
}

static void ms_block_stop(struct memstick_dev *card)
{
#warning Implement!!!
}

static void ms_block_start(struct memstick_dev *card)
{
#warning Implement!!!
}

static void ms_block_new_mtdx_request(struct mtdx_dev *this_dev)
{
#warning Implement!!!
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

static int ms_block_oob_to_info(struct mtdx_dev *this_dev,
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
		if ((p_info->phy_block == msb->boot_blocks[0])
		     || (p_info->phy_block == msb->boot_blocks[1]))
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

static int ms_block_info_to_oob(struct mtdx_dev *this_dev,
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

static int ms_block_get_param(struct mtdx_dev *this_dev, enum mtdx_param param,
			      void *val)
{
	struct memstick_dev *card = container_of(this_dev->dev.parent,
						 struct memstick_dev,
						 dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);

	switch (param) {
	case MTDX_PARAM_GEO: {
		struct mtdx_dev_geo *geo = val;

		geo->zone_cnt_log = 9;
		geo->log_block_cnt
			= msb->boot_attr.number_of_effective_blocks;
		geo->phy_block_cnt
			= msb->boot_attr.number_of_blocks;
		geo->page_size = msb->boot_attr.page_size;
		geo->page_cnt = msb->boot_attr.block_size / geo->page_size;
		geo->oob_size = sizeof(struct ms_extra_data_register);
		return 0;
	}
	case MTDX_PARAM_HD_GEO: {
		struct hd_geometry *geo = val;

		geo->heads = msb->idi.current_logical_heads;
		geo->sectors = msb->idi.current_sectors_per_track;
		geo->cylinders = msb->idi.current_logical_cylinders;
		return 0;
	}
	default:
		return -EINVAL;
	}
}

static int ms_block_probe(struct memstick_dev *card)
{
	struct ms_block_data *msb;
	struct mtdx_device_id c_id = {MTDX_ROLE_MTD, MTDX_WPOLICY_PAGE_INC,
				      MTDX_ID_DEV_MEMORYSTICK};
	int rc = 0;

	msb = kzalloc(sizeof(struct ms_block_data), GFP_KERNEL);
	if (!msb)
		return -ENOMEM;

	memstick_set_drvdata(card, msb);
	msb->card = card;
	spin_lock_init(&msb->lock);

	rc = ms_block_init_card(card);
	if (rc)
		goto err_out_free;

	card->check = ms_block_check_card;
	card->stop = ms_block_stop;
	card->start = ms_block_start;

	if (msb->read_only)
		c_id.w_policy = MTDX_WPOLICY_NONE;

	msb->mdev = mtdx_create_dev(&card->dev, &c_id);
	if (IS_ERR(msb->mdev)) {
		rc = PTR_ERR(msb->mdev);
		msb->mdev = NULL;
		goto err_out_free;
	}

	msb->mdev->new_request = ms_block_new_mtdx_request;
	msb->mdev->oob_to_info = ms_block_oob_to_info;
	msb->mdev->info_to_oob = ms_block_info_to_oob;
	msb->mdev->get_param = ms_block_get_param;

	rc = ms_block_sysfs_register(card);
	if (rc)
		goto err_out_unregister;

	c_id.role = MTDX_ROLE_FTL;
	c_id.id = MTDX_ID_FTL_DUMB;
	msb->fdev = mtdx_create_child(msb->mdev, 0, &c_id);

	if (IS_ERR(msb->fdev)) {
		rc = PTR_ERR(msb->fdev);
		msb->fdev = NULL;
		goto err_out_sysfs_unregister;
	}
	return 0;

err_out_sysfs_unregister:
	ms_block_sysfs_unregister(card);
err_out_unregister:
	device_unregister(&msb->mdev->dev);
err_out_free:
	memstick_set_drvdata(card, NULL);
	kfree(msb);
	return rc;
}

static void ms_block_remove(struct memstick_dev *card)
{
	struct ms_block_data *msb = memstick_get_drvdata(card);

	ms_block_sysfs_unregister(card);

	if (msb->fdev)
		device_unregister(&msb->fdev->dev);

	if (msb->mdev)
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
