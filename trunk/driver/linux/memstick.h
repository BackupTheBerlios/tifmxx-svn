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
	unsigned char overwrite_flag;
	unsigned char management_flag;
	unsigned char logical_address[2];
} __attribute__((packed));

struct ms_register {
	struct ms_status_register     status;
	struct ms_param_register      param;
	struct ms_extra_data_register extra_data;
	unsigned char                 reserved[6];
} __attribute__((packed));

struct mspro_param_register {
	unsigned char system;
	unsigned char data_count[2];
	unsigned char data_address[4];
	unsigned char cmd_param;
} __attribute__((packed));

struct mspro_register {
	struct ms_status_register    status;
	struct mspro_param_register  param;
	unsigned char                reserved[8];
} __attribute__((packed));

typedef enum {
	MS_TPC_READ_LONG_DATA   = 0x02,
	MS_TPC_READ_SHORT_DATA  = 0x03,
	MS_TPC_READ_REG         = 0x04,
	MS_TPC_GET_INT          = 0x07,
	MS_TPC_SET_RW_REG_ADRS  = 0x08,
	MS_TPC_EX_SET_CMD       = 0x09,
	MS_TPC_WRITE_REG        = 0x0b,
	MS_TPC_WRITE_SHORT_DATA = 0x0c,
	MS_TPC_WRITE_LONG_DATA  = 0x0d,
	MS_TPC_SET_CMD          = 0x0e
} memstick_tpc_t;

typedef enum {
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
} memstick_cmd_t;

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

typedef enum { MEMSTICK_ERR_NONE = 0,
	       MEMSTICK_ERR_TIMEOUT = 1,
	       MEMSTICK_ERR_BADCRC = 2,
	       MEMSTICK_ERR_BADMEDIA = 3
} memstick_error_t;

#define MEMSTICK_ANY_ID (~0)

struct memstick_device_id {
	unsigned char type;
	unsigned char category;
	unsigned char class;
};

struct memstick_request {
	memstick_tpc_t   tpc;
	unsigned int     tpc_arg_len;
	unsigned int     tpc_val_len;
	unsigned char    tpc_data[32];
	unsigned int     retries;
	memstick_error_t error;

	unsigned int     data_blocks;
	unsigned int     data_dir:1;

	unsigned int     bytes_xfered;
	void             *done_data;
	void             (*done)(struct memstick_request *req);
};

struct memstick_dev {
	unsigned char        type;
	unsigned char        category;
	unsigned char        class;

	struct memstick_host *host;
	unsigned int         readonly:1; /* card is read-only */

	memstick_error_t     (*check)(struct memstick_dev *card);

	struct device        dev;
};

struct memstick_host {
	struct mutex           lock;
	unsigned int           id;
	struct delayed_work    media_checker;
	struct class_device    cdev;

	struct memstick_ios    ios;
	struct memstick_dev   *card;
	void                   (*request)(struct memstick_host *host,
					  struct memstick_request *req);
	void                   (*set_ios)(struct memstick_host *host,
					  struct memstick_ios *ios);
	unsigned long          private[0] ____cacheline_aligned;
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

struct memstick_host *memstick_alloc_host(unsigned int extra,
					  struct device *dev);

int memstick_add_host(struct memstick_host *host);
void memstick_remove_host(struct memstick_host *host);
void memstick_free_host(struct memstick_host *host);
void memstick_detect_change(struct memstick_host *host, unsigned long delay);

void memstick_wait_for_req(struct memstick_host *host,
			   struct memstick_request *mrq);

memstick_error_t memstick_get_int(struct memstick_host *host,
				  unsigned char *int_reg);

memstick_error_t memstick_set_rw_reg_adrs(struct memstick_host *host,
					  unsigned char read_off,
					  unsigned char read_len,
					  unsigned char write_off,
					  unsigned char write_len);

memstick_error_t memstick_read_reg(struct memstick_host *host,
				   struct ms_register *ms_reg);

static inline void *memstick_priv(struct memstick_host *host)
{
        return (void *)host->private;
}

#endif
