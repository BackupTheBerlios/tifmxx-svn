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

struct mtdx_dev_geo {
	unsigned int zone_size_log; /* Number of bits set aside for zoning */
	unsigned int log_block_cnt; /* Number of logical eraseblocks       */
	unsigned int phy_block_cnt; /* Number of physical eraseblocks      */
	unsigned int page_cnt;      /* Number of pages per eraseblock      */
	unsigned int page_size;     /* Size of page in bytes               */
	unsigned int oob_size;      /* Size of oob in bytes                */
	unsigned int fill_value;    /* default value in memory cells       */
};

struct mtdx_dev;

enum mtdx_command {
	MTDX_CMD_NONE = 0,
	MTDX_CMD_READ,       /* read both page data and oob    */
	MTDX_CMD_READ_DATA,  /* read only page data            */
	MTDX_CMD_READ_OOB,   /* read only page oob             */
	MTDX_CMD_ERASE,      /* erase block                    */
	MTDX_CMD_WRITE,      /* write both page data and oob   */
	MTDX_CMD_WRITE_DATA, /* write only page data           */
	MTDX_CMD_WRITE_OOB,  /* write only page oob            */
	MTDX_CMD_SELECT,     /* mark pages as being phased out */
	MTDX_CMD_INVALIDATE, /* mark pages as bad              */
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

struct mtdx_request {
	struct mtdx_dev      *src_dev;
	enum mtdx_command    cmd;
	unsigned int         log_block;
	unsigned int         phy_block;
	unsigned int         offset;
	unsigned int         length;
};

struct mtdx_dev {
	struct mtdx_device_id id;
	unsigned int          ord;
	struct list_head      queue_node;

	/* notify device of pending requests                          */
	int                  (*new_request)(struct mtdx_dev *this_dev,
					    struct mtdx_dev *req_dev);

	/* get any available requests from device                     */
	struct mtdx_request  *(*get_request)(struct mtdx_dev *this_dev);

	/* complete an active request                                 */
	void                 (*end_request)(struct mtdx_request *req,
					    int error, unsigned int count);

	/* Get data buffers associated with request. Each buffer should contain
	 * integral number of pages. Can be called several times, to obtain all
	 * buffer chunks.
	 */
	char                 *(*get_data_buf)(struct mtdx_request *req,
					      unsigned int *count);
	int                  (*get_data_buf_sg)(struct mtdx_request *req,
					        struct scatterlist *sg);

	/* Get and put source address associated with copy command.
	 * Put_copy_source can be optionally called to inform the child that
	 * there's problem with source block (destination block problems are
	 * reported through end_request.
	 */
	int                  (*get_copy_source)(struct mtdx_request *req,
						unsigned int *phy_block,
						unsigned int *offset);
	void                 (*put_copy_source)(struct mtdx_request *req,
						int src_error);

	/* Get oob buffer (size is agreed upon in advance). Should be called
	 * once for each page accessed.
	 */
	char                 *(*get_oob_buf)(struct mtdx_request *req);

	/* Translate opaque oob blob to/from generic block decription. */
	int                  (*oob_to_info)(struct mtdx_dev *this_dev,
					    struct mtdx_page_info *p_info,
					    void *oob);
	int                  (*info_to_oob)(struct mtdx_dev *this_dev,
					    void *oob,
					    struct mtdx_page_info *p_info);

	/* Get/set device metadata.                                   */
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

struct mtdx_dev *mtdx_alloc_dev(struct device *parent,
				const struct mtdx_device_id *id);
void __mtdx_free_dev(struct mtdx_dev *mdev);
void mtdx_drop_children(struct mtdx_dev *mdev);
int mtdx_page_list_append(struct list_head *head, struct mtdx_page_info *info);
void mtdx_page_list_free(struct list_head *head);

static inline void mtdx_complete_request(struct mtdx_request *req, int error,
					 unsigned int count)
{
	req->src_dev->end_request(req, error, count);
}

static inline int mtdx_get_data_buf_sg(struct mtdx_request *req,
				       struct scatterlist *sg)
{
	return req->src_dev->get_data_buf_sg(req, sg);
}

static inline int mtdx_get_copy_source(struct mtdx_request *req,
				       unsigned int *phy_block,
				       unsigned int *offset)
{
	return req->src_dev->get_copy_source(req, phy_block, offset);
}

static inline void mtdx_put_copy_source(struct mtdx_request *req,
					int src_error)
{
	req->src_dev->put_copy_source(req, src_error);
}

static inline char *mtdx_get_oob_buf(struct mtdx_request *req)
{
	return req->src_dev->get_oob_buf(req);
}

static inline void *mtdx_get_drvdata(struct mtdx_dev *mdev)
{
	return dev_get_drvdata(&mdev->dev);
}

static inline void mtdx_set_drvdata(struct mtdx_dev *mdev, void *data)
{
	dev_set_drvdata(&mdev->dev, data);
}

static inline struct mtdx_dev *mtdx_queue_entry(struct list_head *head)
{
	return container_of(head, struct mtdx_dev, queue_node);
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
