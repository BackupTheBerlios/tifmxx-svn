/*
 *  memstick.h - Sony MemoryStick support
 *
 *  Copyright (C) 2006 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef _MEMSTICK_H
#define _MEMSTICK_H

#include <linux/workqueue.h>

struct ms_status_register {
	unsigned char reserved1;
	unsigned char interrupt;
	unsigned char status0;
	unsigned char status1;
	unsigned char type;
	unsigned char reserved2;
	unsigned char category;
	unsigned char class;
	unsigned char reserved3[8];
} __attribute__((packed));

struct ms_param_register {
	unsigned char system;
	unsigned char block_address[3];
	unsigned char cp;
	unsigned char page_address;
} __attribute__((packed));

struct ms_extra_data_register {
	unsigned char  overwrite_flag;
	unsigned char  management_flag;
	unsigned short logical_address;
} __attribute__((packed));

struct ms_register {
	struct ms_status_register     status;
	struct ms_param_register      param;
	struct ms_extra_data_register extra_data;
	unsigned char                 reserved[6];
} __attribute__((packed));

struct mspro_param_register {
	unsigned char  system;
	unsigned short data_count;
	unsigned int   data_address;
	unsigned char  cmd_param;
} __attribute__((packed));

struct mspro_register {
	struct ms_status_register    status;
	struct mspro_param_register  param;
	unsigned char                reserved[8];
} __attribute__((packed));

struct ms_register_addr {
	unsigned char r_offset;
	unsigned char r_length;
	unsigned char w_offset;
	unsigned char w_length;
} __attribute__((packed));

enum {
	MS_TPC_READ_LONG_DATA   = 0x02,
	MS_TPC_READ_SHORT_DATA  = 0x03,
	MS_TPC_READ_REG         = 0x04,
	MS_TPC_READ_IO_DATA     = 0x05, // unverified
	MS_TPC_GET_INT          = 0x07,
	MS_TPC_SET_RW_REG_ADRS  = 0x08,
	MS_TPC_EX_SET_CMD       = 0x09,
	MS_TPC_WRITE_IO_DATA    = 0x0a, // unverified
	MS_TPC_WRITE_REG        = 0x0b,
	MS_TPC_WRITE_SHORT_DATA = 0x0c,
	MS_TPC_WRITE_LONG_DATA  = 0x0d,
	MS_TPC_SET_CMD          = 0x0e
};

enum {
	MS_CMD_BLOCK_END     = 0x33,
	MS_CMD_RESET         = 0x3c,
	MS_CMD_BLOCK_WRITE   = 0x55,
	MS_CMD_SLEEP         = 0x5a,
	MS_CMD_BLOCK_ERASE   = 0x99,
	MS_CMD_BLOCK_READ    = 0xaa,
	MS_CMD_CLEAR_BUF     = 0xc3,
	MS_CMD_FLASH_STOP    = 0xcc,
	MSPRO_CMD_FORMAT     = 0x10,
	MSPRO_CMD_SLEEP      = 0x11,
	MSPRO_CMD_READ_DATA  = 0x20,
	MSPRO_CMD_WRITE_DATA = 0x21,
	MSPRO_CMD_READ_ATRB  = 0x24,
	MSPRO_CMD_STOP       = 0x25,
	MSPRO_CMD_ERASE      = 0x26,
	MSPRO_CMD_SET_IBA    = 0x46,
	MSPRO_CMD_SET_IBD    = 0x47
//	MSPRO_CMD_RESET
//	MSPRO_CMD_WAKEUP
//	MSPRO_CMD_IN_IO_DATA
//	MSPRO_CMD_OUT_IO_DATA
//	MSPRO_CMD_READ_IO_ATRB
//	MSPRO_CMD_IN_IO_FIFO
//	MSPRO_CMD_OUT_IO_FIFO
//	MSPRO_CMD_IN_IOM
//	MSPRO_CMD_OUT_IOM
};

#define MEMSTICK_DEF_PAGE_SIZE 0x0200

struct memstick_ios {
	unsigned char power_mode;
#define MEMSTICK_POWER_OFF 0
#define MEMSTICK_POWER_ON  1

	unsigned char interface;
#define MEMSTICK_SERIAL   0
#define MEMSTICK_PARALLEL 1
};

struct memstick_host;
struct memstick_driver;

#define MEMSTICK_ERR_NONE     0
#define MEMSTICK_ERR_TIMEOUT  1
#define MEMSTICK_ERR_BADCRC   2
#define MEMSTICK_ERR_INTERNAL 3

#define MEMSTICK_MATCH_ALL            0x01

#define MEMSTICK_TYPE_LEGACY          0xff
#define MEMSTICK_TYPE_DUO             0x00
#define MEMSTICK_TYPE_PRO             0x01

#define MEMSTICK_CATEGORY_STORAGE     0xff
#define MEMSTICK_CATEGORY_STORAGE_DUO 0x00

#define MEMSTICK_CLASS_GENERIC        0xff
#define MEMSTICK_CLASS_GENERIC_DUO    0x00


struct memstick_device_id {
	unsigned char match_flags;
	unsigned char type;
	unsigned char category;
	unsigned char class;
};

struct memstick_request {
	unsigned char tpc;
	unsigned char data_dir:1,
		      need_card_int:1,
		      block_io:1;
	int           error;
	size_t        length;
	union {
		struct scatterlist *sg;
		char               *data;
	};
	struct list_head   node;
};

struct memstick_dev {
	struct memstick_device_id     id;
	struct memstick_host          *host;

	int                           (*check)(struct memstick_dev *card);

	struct device                 dev;
};

struct memstick_host {
	struct mutex        lock;
	unsigned int        id;
	struct work_struct  media_checker;
	struct class_device cdev;
	struct list_head    preq_list;
	struct list_head    creq_list;
	unsigned int        retries;
	spinlock_t          req_lock;
	wait_queue_head_t   req_wait;

	struct memstick_ios ios;
	struct memstick_dev *card;
	void                (*request)(struct memstick_host *host,
				       struct memstick_request *mrq);
	void                (*set_ios)(struct memstick_host *host,
				       struct memstick_ios *ios);
	unsigned long       private[0] ____cacheline_aligned;
};

struct memstick_driver {
	struct memstick_device_id *id_table;
	int                       (*probe)(struct memstick_dev *card);
	void                      (*remove)(struct memstick_dev *card);
	int                       (*suspend)(struct memstick_dev *card,
					     pm_message_t state);
	int                       (*resume)(struct memstick_dev *card);

	struct device_driver      driver;
};

int memstick_register_driver(struct memstick_driver *drv);
void memstick_unregister_driver(struct memstick_driver *drv);

struct memstick_host *memstick_alloc_host(unsigned int extra,
					  struct device *dev);

int memstick_add_host(struct memstick_host *host);
void memstick_remove_host(struct memstick_host *host);
void memstick_free_host(struct memstick_host *host);
void memstick_detect_change(struct memstick_host *host);

struct memstick_request* memstick_next_req(struct memstick_host *host,
					   struct memstick_request *mrq);
void memstick_queue_req(struct memstick_host *host,
			struct memstick_request *mrq);
struct memstick_request* memstick_get_req(struct memstick_host *host);

void memstick_init_req_sg(struct memstick_request *mrq, unsigned char tpc,
			  struct scatterlist *sg);
void memstick_init_req(struct memstick_request *mrq, unsigned char tpc,
		       char *buf, size_t length);

static inline void *memstick_priv(struct memstick_host *host)
{
        return (void *)host->private;
}

inline void *memstick_get_drvdata(struct memstick_dev *card)
{
	return dev_get_drvdata(&card->dev);
}

inline void memstick_set_drvdata(struct memstick_dev *card, void *data)
{
	dev_set_drvdata(&card->dev, data);
}

#endif
