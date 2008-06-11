/*
 *  MTDX legacy Sony Memorystick support
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "../driver/linux/memstick.h"
#include "mtdx_common.h"
#include <linux/err.h>

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
	struct memstick_dev           *card;
	struct mtdx_dev               *mdev;
	struct mtdx_dev               *fdev;
	struct mtdx_dev_geo           geo;

	unsigned char                 system;
	unsigned char                 read_only:1,
				      active:1,
				      has_request:1,
				      format_media:1;

	struct ms_boot_attr_info      boot_attr;
	struct ms_cis_idi             cis_idi;

	struct ms_extra_data_register current_extra;
};


static int ms_block_init_card(struct memstick_dev *card)
{
#warning Implement!!!
	return 0;
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

static void ms_block_new_mtd_request(struct mtdx_dev *this_dev,
				     struct mtdx_dev *src_dev)
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

static int ms_block_get_param(struct mtdx_dev *this_dev, enum mtdx_param param,
			      void *val)
{
	struct memstick_dev *card = container_of(this_dev->dev.parent,
						 struct memstick_dev,
						 dev);
	struct ms_block_data *msb = memstick_get_drvdata(card);

	switch (param) {
	case MTDX_PARAM_GEO:
		memcpy(val, &msb->geo, sizeof(struct mtdx_dev_geo));
		return 0;
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

	msb->mdev->new_request = ms_block_new_mtd_request;
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
