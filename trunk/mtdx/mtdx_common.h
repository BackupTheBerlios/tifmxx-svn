/*
 *  MTDx common defines
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef _MTDX_COMMON_H
#define _MTDX_COMMON_H

#include <linux/device.h>
#include <linux/scatterlist.h>

#define MTDX_INVALID_BLOCK 0xffffffff

struct mtdx_device_id {
	char           role;
#define MTDX_ROLE_INVALID       0x00
#define MTDX_ROLE_MTD           0x01
#define MTDX_ROLE_FTL           0x02
#define MTDX_ROLE_BLK           0x03

	char           w_policy;
#define MTDX_WPOLICY_NONE       0x00
#define MTDX_WPOLICY_PEB        0x01
#define MTDX_WPOLICY_PAGE_INC   0x02
#define MTDX_WPOLICY_PAGE       0x03
#define MTDX_WPOLICY_RAM        0x04

	unsigned short id;
#define MTDX_ID_DEV_INVALID     0x0000
#define MTDX_ID_DEV_SMARTMEDIA  0x0001
#define MTDX_ID_DEV_MEMORYSTICK 0x0002
#define MTDX_ID_FTL_DUMB        0x0003
};

struct mtdx_dev_geo {
	unsigned int zone_cnt;
	unsigned int log_block_cnt;
	unsigned int phy_block_cnt;
	unsigned int page_cnt;
	unsigned int page_size;
	unsigned int oob_size;
};

struct mtdx_dev;

enum mtdx_command {
	MTDX_CMD_NONE = 0,
	MTDX_CMD_READ,
	MTDX_CMD_WRITE,
	MTDX_CMD_ERASE,
	MTDX_CMD_COPY
};

enum mtdx_block_info {
	MTDX_BLK_UNKNOWN = 0,
	MTDX_BLK_ERASED,
	MTDX_BLK_UNMAPPED,
	MTDX_BLK_MAPPED,
	MTDX_BLK_INVALID
};

enum mtdx_param {
	MTDX_PARAM_NONE = 0,
	MTDX_PARAM_GEO
};

struct mtdx_request {
	struct mtdx_dev      *src_dev;
	enum mtdx_command    cmd;
	enum mtdx_block_info block_info;
	unsigned int         flags;
/* Request has associated data pending */
#define MTDX_REQ_DATA  1
/* log_block and block_info fields must be set from/to oob */
#define MTDX_REQ_TRANS 2
	unsigned int         log_block;
	unsigned int         phy_block;
	unsigned int         offset;
	unsigned int         length;
	struct {
		unsigned int phy_block;
		unsigned int offset;
	} src;
};

struct mtdx_dev {
	struct mtdx_device_id id;
	unsigned int          ord;

	void                 (*new_request)(struct mtdx_dev *this_dev,
					    struct mtdx_dev *src_dev);
	struct mtdx_request  *(*get_request)(struct mtdx_dev *this_dev,
					     struct mtdx_dev *dst_dev);
	void                 (*end_request)(struct mtdx_request *req);

	int                  (*get_data_buf)(struct mtdx_request *req,
					     char **buf);
	int                  (*get_data_buf_sg)(struct mtdx_request *req,
					        struct scatterlist *sg);

	int                  (*get_param)(struct mtdx_dev *this_dev,
					  enum mtdx_param param,
					  void *val);
	int                  (*set_param)(struct mtdx_dev *this_dev,
					  enum mtdx_param param,
					  unsigned long val);

	struct device         dev;
};

struct mtdx_driver {
	struct mtdx_device_id     *id_table;
	int                       (*probe)(struct mtdx_dev *mdev);
	void                      (*remove)(struct mtdx_dev *mdev);
	int                       (*suspend)(struct mtdx_dev *mdev,
					     pm_message_t state);
	int                       (*resume)(struct mtdx_dev *mdev);

	struct device_driver      driver;
};

int mtdx_register_driver(struct mtdx_driver *drv);
void mtdx_unregister_driver(struct mtdx_driver *drv);

struct mtdx_dev *mtdx_create_dev(struct device *parent,
				 struct mtdx_device_id *id);
struct mtdx_dev *mtdx_create_child(struct mtdx_dev *parent, unsigned int ord,
				   struct mtdx_device_id *id);

#endif
