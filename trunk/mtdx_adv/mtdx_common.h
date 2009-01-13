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
#include "mtdx_data.h"

#define MTDX_INVALID_BLOCK 0xffffffff

struct mtdx_device_id {
	unsigned char inp_wmode;
	unsigned char out_wmode;
#define MTDX_WMODE_NONE         0x00
/* Write only full eraseblocks */
#define MTDX_WMODE_PEB          0x01
/* Write only pages higher in offset than anything already written */
#define MTDX_WMODE_PAGE_PEB_INC 0x02
/* Write arbitrary pages in the eraseblock */
#define MTDX_WMODE_PAGE_PEB     0x03
/* Write integral number of pages elsewhere */
#define MTDX_WMODE_PAGE         0x04
/* No limitations on request contents. */
#define MTDX_WMODE_RAM          0x05

	unsigned char inp_rmode;
	unsigned char out_rmode;
#define MTDX_RMODE_NONE     0x00
/* Read pages from single eraseblock */
#define MTDX_RMODE_PAGE_PEB 0x01
/* Read integral number of pages from elsewhere */
#define MTDX_RMODE_PAGE     0x02
/* No limitations on request contents. */
#define MTDX_RMODE_RAM      0x03

	unsigned short type;
#define MTDX_TYPE_NONE       0x00
#define MTDX_TYPE_MEDIA      0x01
#define MTDX_TYPE_FTL        0x02
#define MTDX_TYPE_ADAPTER    0x03

	unsigned short id;
#define MTDX_ID_INVALID           0x0000
#define MTDX_ID_MEDIA_SMARTMEDIA  0x0001
#define MTDX_ID_MEDIA_MEMORYSTICK 0x0002
#define MTDX_ID_FTL_SIMPLE        0x0003
#define MTDX_ID_ADAPTER_BLKDEV    0x0004
};

struct mtdx_geo {
	unsigned int zone_cnt;      /* Number of allocation zones */
	unsigned int log_block_cnt; /* Number of logical eraseblocks       */
	unsigned int phy_block_cnt; /* Number of physical eraseblocks      */
	unsigned int page_cnt;      /* Number of pages per eraseblock      */
	unsigned int page_size;     /* Size of page in bytes               */
	unsigned int oob_size;      /* Size of oob in bytes                */
	unsigned int fill_value;    /* default value in memory cells       */
	unsigned int (*log_to_zone)(const struct mtdx_geo *geo,
				    unsigned int log_addr,
				    unsigned int *log_off);
	unsigned int (*zone_to_log)(const struct mtdx_geo *geo,
				    unsigned int zone,
				    unsigned int log_off);
	unsigned int (*phy_to_zone)(const struct mtdx_geo *geo,
				    unsigned int phy_addr,
				    unsigned int *phy_off);
	unsigned int (*zone_to_phy)(const struct mtdx_geo *geo,
				    unsigned int zone,
				    unsigned int phy_off);
};

static inline unsigned int mtdx_geo_log_to_zone(const struct mtdx_geo *geo,
						unsigned int log_addr,
						unsigned int *log_off)
{
	unsigned int l_off;

	if (!log_off)
		log_off = &l_off;

	if (geo->log_to_zone)
		return geo->log_to_zone(geo, log_addr, log_off);
	else {
		unsigned int z_sz = geo->log_block_cnt / geo->zone_cnt;
		*log_off = log_addr % z_sz;
		return log_addr / z_sz;
	}
}

static inline unsigned int mtdx_geo_zone_to_log(const struct mtdx_geo *geo,
						unsigned int zone,
						unsigned int log_off)
{
	if (geo->zone_to_log)
		return geo->zone_to_log(geo, zone, log_off);
	else {
		unsigned int z_sz = geo->log_block_cnt / geo->zone_cnt;

		if (log_off < z_sz)
			return zone * z_sz + log_off;
		else
			return MTDX_INVALID_BLOCK;
	}
}

static inline unsigned int mtdx_geo_phy_to_zone(const struct mtdx_geo *geo,
						unsigned int phy_addr,
						unsigned int *phy_off)
{
	unsigned int p_off;

	if (!phy_off)
		phy_off = &p_off;

	if (!geo->phy_to_zone) {
		unsigned int z_sz = geo->phy_block_cnt / geo->zone_cnt;
		*phy_off = phy_addr % z_sz;
		return phy_addr / z_sz;
	} else
		return geo->phy_to_zone(geo, phy_addr, phy_off);
}

static inline unsigned int mtdx_geo_zone_to_phy(const struct mtdx_geo *geo,
						unsigned int zone,
						unsigned int phy_off)
{
	if (!geo->zone_to_phy) {
		unsigned int z_sz = geo->phy_block_cnt / geo->zone_cnt;
		if (phy_off < z_sz)
			return (zone * z_sz) + phy_off;
		else
			return MTDX_INVALID_BLOCK;
	} else
		return geo->zone_to_phy(geo, zone, phy_off);
}

struct mtdx_dev;

enum mtdx_command {
	MTDX_CMD_NONE = 0,
	MTDX_CMD_READ,       /* read both page data and oob    */
	MTDX_CMD_ERASE,      /* erase block                    */
	MTDX_CMD_WRITE,      /* write both page data and oob   */
	MTDX_CMD_OVERWRITE,  /* special cases write            */
	MTDX_CMD_COPY        /* copy pages                     */
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
	struct list_head      node;
	enum mtdx_page_status status;
	unsigned int          log_block;
	unsigned int          phy_block;
	unsigned int          page_offset;
};

enum mtdx_param {
	MTDX_PARAM_NONE = 0,
	MTDX_PARAM_GEO,            /* struct mtdx_geo                */
	MTDX_PARAM_HD_GEO,         /* struct hd_geometry             */
	MTDX_PARAM_SPECIAL_BLOCKS, /* list of struct mtdx_page_info  */
	MTDX_PARAM_READ_ONLY,      /* boolean int                    */
	MTDX_PARAM_DEV_SUFFIX,     /* char[DEVICE_ID_SIZE]           */
	MTDX_PARAM_DMA_MASK        /* u64*                           */
};

enum mtdx_message {
	MTDX_MSG_NONE = 0,
	MTDX_MSG_INV_BLOCK_MAP     /* invalidate flash translation maps */
};

struct mtdx_pos {
	unsigned int b_addr;
	unsigned int offset;
};

struct mtdx_request {
	enum mtdx_command     cmd;       /* command to execute              */
	unsigned int          logical;   /* logical block address           */
	struct mtdx_pos       phy;       /* request target physical address */
	struct mtdx_pos       copy;      /* request copy source address     */
	unsigned int          length;    /* request data length             */
	struct mtdx_data_iter *req_data; /* optional - request data         */
	struct mtdx_oob_iter  *req_oob;  /* optional - request extra data   */
};

struct mtdx_dev {
	struct mtdx_device_id id;
	unsigned int          ord;
	struct list_head      q_node;

	/* notify device of pending requests                          */
	void                 (*new_request)(struct mtdx_dev *this_dev,
					    struct mtdx_dev *req_dev);

	/* get any available requests from device                     */
	struct mtdx_request  *(*get_request)(struct mtdx_dev *this_dev);

	/* complete an active request                                 */
	void                 (*end_request)(struct mtdx_dev *this_dev,
					    struct mtdx_request *req,
					    unsigned int count,
					    int dst_error, int src_error);

	/* Translate opaque oob blob to/from generic block decription. */
	int                  (*oob_to_info)(struct mtdx_dev *this_dev,
					    struct mtdx_page_info *p_info,
					    void *oob);
	int                  (*info_to_oob)(struct mtdx_dev *this_dev,
					    void *oob,
					    struct mtdx_page_info *p_info);

	/* Get device metadata (child calls parent)                    */
	int                  (*get_param)(struct mtdx_dev *this_dev,
					  enum mtdx_param param,
					  void *val);
	/* notify children about some event                            */
	void                 (*notify)(struct mtdx_dev *this_dev,
				       enum mtdx_message msg);

	struct device         dev;
};

struct mtdx_dev_queue {
	spinlock_t       lock;
	struct list_head head;
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

struct mtdx_dev *mtdx_alloc_dev(struct device *parent,
				const struct mtdx_device_id *id);
void __mtdx_free_dev(struct mtdx_dev *mdev);
void mtdx_drop_children(struct mtdx_dev *mdev);
void mtdx_notify_children(struct mtdx_dev *mdev, enum mtdx_message msg);
int mtdx_page_list_append(struct list_head *head, struct mtdx_page_info *info);
void mtdx_page_list_free(struct list_head *head);

static inline void mtdx_dev_queue_init(struct mtdx_dev_queue *devq)
{
	spin_lock_init(&devq->lock);
	INIT_LIST_HEAD(&devq->head);
}

void mtdx_dev_queue_push_back(struct mtdx_dev_queue *devq,
			      struct mtdx_dev *mdev);
struct mtdx_dev *mtdx_dev_queue_pop_front(struct mtdx_dev_queue *devq);
int mtdx_dev_queue_empty(struct mtdx_dev_queue *devq);

static inline void *mtdx_get_drvdata(struct mtdx_dev *mdev)
{
	return dev_get_drvdata(&mdev->dev);
}

static inline void mtdx_set_drvdata(struct mtdx_dev *mdev, void *data)
{
	dev_set_drvdata(&mdev->dev, data);
}

int mtdx_append_dev_list(struct list_head *head, struct mtdx_dev *r_dev);

/* Some bitmap helpers */
int bitmap_region_empty(unsigned long *bitmap, unsigned int offset,
			unsigned int length);
void bitmap_clear_region(unsigned long *bitmap, unsigned int offset,
			 unsigned int length);
void bitmap_set_region(unsigned long *bitmap, unsigned int offset,
		       unsigned int length);

#endif
