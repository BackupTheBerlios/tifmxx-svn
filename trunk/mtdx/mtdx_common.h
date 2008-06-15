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
	unsigned int zone_cnt_log;  /* Number of bits set aside for zoning */
	unsigned int log_block_cnt; /* Number of logical eraseblocks       */
	unsigned int phy_block_cnt; /* Number of physical eraseblocks      */
	unsigned int page_cnt;      /* Number of pages per eraseblock      */
	unsigned int page_size;     /* Size of page in bytes               */
	unsigned int oob_size;      /* Size of oob in bytes                */
};

struct mtdx_dev;

enum mtdx_command {
	MTDX_CMD_NONE = 0,
	MTDX_CMD_READ,       /* read both page data and oob  */
	MTDX_CMD_READ_DATA,  /* read only page data          */
	MTDX_CMD_READ_OOB,   /* read only page oob           */
	MTDX_CMD_ERASE,      /* erase block                  */
	MTDX_CMD_WRITE,      /* write both page data and oob */
	MTDX_CMD_WRITE_DATA, /* write only page data         */
	MTDX_CMD_WRITE_OOB,  /* write only page oob          */
	MTDX_CMD_INVALIDATE, /* mark pages as bad            */
	MTDX_CMD_COPY        /* copy pages                   */
};

enum mtdx_page_status {
	MTDX_PAGE_UNKNOWN = 0,
	MTDX_PAGE_ERASED,      /* Page is clean                            */
	MTDX_PAGE_UNMAPPED,    /* Page is good for writing but needs erase */
	MTDX_PAGE_MAPPED,      /* Page contains live data                  */
	MTDX_PAGE_SMAPPED,     /* Prefer this page in case of conflict     */
	MTDX_PAGE_INVALID,     /* Page is defective                        */
	MTDX_PAGE_FAILURE,     /* Page data is not guaranteed              */
	MTDX_PAGE_RESERVED     /* Page is good,  but shouldn't be used     */
};

/* Generic oob structure. Produced by the MTD device method from the opaque
 * oob blob.
 */
struct mtdx_page_info {
	enum mtdx_page_status status;
	unsigned int          log_block;
	unsigned int          phy_block;
	unsigned int          page_offset;
};

enum mtdx_param {
	MTDX_PARAM_NONE = 0,
	MTDX_PARAM_GEO,        /* Get device geometry | struct mtdx_geo      */
	MTDX_PARAM_HD_GEO      /* Get device geometry | struct hd_geometry   */
};

struct mtdx_request {
	struct mtdx_dev      *src_dev;
	enum mtdx_command    cmd;
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

	void                 (*new_request)(struct mtdx_dev *this_dev);
	struct mtdx_request  *(*get_request)(struct mtdx_dev *this_dev);
	void                 (*end_request)(struct mtdx_request *req,
					    int error, unsigned int count);

	char                 *(*get_data_buf)(struct mtdx_request *req);
	int                  (*get_data_buf_sg)(struct mtdx_request *req,
					        struct scatterlist *sg);
	char                 *(*get_oob_buf)(struct mtdx_request *req);

	int                  (*oob_to_info)(struct mtdx_dev *this_dev,
					    struct mtdx_page_info *p_info,
					    void *oob);
	int                  (*info_to_oob)(struct mtdx_dev *this_dev,
					    void *oob,
					    struct mtdx_page_info *p_info);

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
static inline void mtdx_complete_request(struct mtdx_request *req, int error,
					 unsigned int count)
{
	req->src_dev->end_request(req, error, count);
}

#endif
