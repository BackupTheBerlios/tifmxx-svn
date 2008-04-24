/*
 *  xD Picture card support
 *
 *  Copyright (C) 2008 JMicron Technology Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef _XD_CARD_H
#define _XD_CARD_H

#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/kthread.h>
#include <linux/scatterlist.h>
#include "flash_bd.h"

struct xd_card_id1 {
	unsigned char maker_code;
	unsigned char device_code;
	unsigned char option_code1;
	unsigned char option_code2;
} __attribute__((packed));

struct xd_card_id2 {
	unsigned char characteristics_code;
#define XD_CARD_CELL_TYPE_MASK   0xc0
#define XD_CARD_MULTI_BLOCK_MASK 0x30

	unsigned char vendor_code1;
	unsigned char size_code;
	unsigned char vendor_code2;
	unsigned char vendor_code3;
} __attribute__((packed));

struct xd_card_id3 {
	unsigned char vendor_code1;
	unsigned char vendor_code2;
	unsigned char id_code;
	unsigned char vendor_code3;
} __attribute__((packed));

struct xd_card_extra {
	unsigned int   reserved;
	unsigned char  data_status;
	unsigned char  block_status;
	unsigned short addr1;
	unsigned char  ecc_hi[3];
	unsigned short addr2; /* this should be identical to addr1 */
	unsigned char  ecc_lo[3];
} __attribute__((packed));

struct xd_card_idi {
	unsigned char vendor_code1[6];
	unsigned char serial_number[20];
	unsigned char model_number[40];
	unsigned char vendor_code2[62];
} __attribute__((packed));

enum {
	XD_CARD_CMD_INPUT       = 0x80,
	XD_CARD_CMD_READ1       = 0x00,
	XD_CARD_CMD_READ2       = 0x01,
	XD_CARD_CMD_READ3       = 0x50,
	XD_CARD_CMD_RESET       = 0xff,
	XD_CARD_CMD_PAGE_PROG   = 0x10,
	XD_CARD_CMD_DUMMY_PROG  = 0x11,
	XD_CARD_CMD_MULTI_PROG  = 0x15,
	XD_CARD_CMD_ERASE_SET   = 0x60,
	XD_CARD_CMD_ERASE_START = 0xd0,
	XD_CARD_CMD_STATUS1     = 0x70,
	XD_CARD_CMD_STATUS2     = 0x71,
	XD_CARD_CMD_ID1         = 0x90,
	XD_CARD_CMD_ID2         = 0x91,
	XD_CARD_CMD_ID3         = 0x9a
};

struct xd_card_request {
	unsigned char      cmd;
	unsigned char      flags;
#define XD_CARD_REQ_NO_ECC 0x20
#define XD_CARD_REQ_DATA   0x10
#define XD_CARD_REQ_ID     0x08
#define XD_CARD_REQ_STATUS 0x04
#define XD_CARD_REQ_EXTRA  0x02
#define XD_CARD_REQ_DIR    0x01

	unsigned char      status;
#define XD_CARD_STTS_RW      0x80
#define XD_CARD_STTS_READY   0x40
#define XD_CARD_STTS_D3_FAIL 0x10
#define XD_CARD_STTS_D2_FAIL 0x08
#define XD_CARD_STTS_D1_FAIL 0x04
#define XD_CARD_STTS_D0_FAIL 0x02
#define XD_CARD_STTS_FAIL    0x01

	void               *id;
	unsigned long long addr;
	int                error;
	unsigned int       count;
	struct scatterlist sg;
};


enum {
	XD_CARD_CIS_TPL_DEVICE    = 0x01,
	XD_CARD_CIS_TPL_NO_LINK   = 0x14,
	XD_CARD_CIS_TPL_VERS_1    = 0x15,
	XD_CARD_CIS_TPL_JEDEC_C   = 0x18,
	XD_CARD_CIS_TPL_CONFIG    = 0x1a,
	XD_CARD_CIS_TPL_CFTABLE_E = 0x1b,
	XD_CARD_CIS_TPL_MANF_ID   = 0x20,
	XD_CARD_CIS_TPL_FUNC_ID   = 0x21,
	XD_CARD_CIS_TPL_FUNC_E    = 0x22
};

struct xd_card_host;

struct xd_card_media {
	struct xd_card_host     *host;
	unsigned int            usage_count;
	struct gendisk          *disk;
	struct request_queue    *queue;
	spinlock_t              q_lock;
	struct task_struct      *f_thread;
	struct request          *block_req;
	struct flash_bd_request flash_req;
	struct flash_bd         *fbd;
	struct bin_attribute    dev_attr_block_map;
	struct xd_card_request  req;
	struct completion       req_complete;
	int                   (*next_request[2])(struct xd_card_media *card,
						 struct xd_card_request **req);

	unsigned char           mask_rom:1,
				sm_media:1,
				read_only:1,
				auto_ecc:1;

	/* These bits must be protected by q_lock */
	unsigned char           has_request:1,
				format:1,
				eject:1;

	unsigned char           page_addr_bits;
	unsigned char           block_addr_bits;
	unsigned char           addr_bytes;

	unsigned int            capacity;
	unsigned int            cylinders;
	unsigned int            heads;
	unsigned int            sectors_per_head;

	unsigned int            page_size;
	unsigned int            hw_page_size;

	unsigned int            zone_cnt;
	unsigned int            phy_block_cnt;
	unsigned int            log_block_cnt;
	unsigned int            page_cnt;
	unsigned int            cis_block;

	struct xd_card_id1      id1;
	struct xd_card_id2      id2;
	struct xd_card_id3      id3;
	unsigned char           cis[128];
	struct xd_card_idi      idi;

#define XD_CARD_MAX_SEGS 32
	struct scatterlist      req_sg[XD_CARD_MAX_SEGS];
	unsigned int            seg_count;
	unsigned int            seg_pos;
	unsigned int            seg_off;

	unsigned int            trans_cnt;
	unsigned int            trans_len;
	unsigned char           *t_buf;
};

enum xd_card_param {
	XD_CARD_POWER = 1,
	XD_CARD_CLOCK,
	XD_CARD_PAGE_SIZE,
	XD_CARD_EXTRA_SIZE,
	XD_CARD_ADDR_SIZE
};

#define XD_CARD_POWER_OFF 0
#define XD_CARD_POWER_ON  1

#define XD_CARD_NORMAL   0
#define XD_CARD_SLOW     1

struct xd_card_host {
	struct device           *dev;
	struct mutex            lock;
	struct work_struct      media_checker;
	struct xd_card_media    *card;
	struct xd_card_extra    extra;
	unsigned int            extra_pos;

	unsigned int            retries;
	unsigned int            caps;
#define XD_CARD_CAP_AUTO_ECC     1
#define XD_CARD_CAP_FIXED_EXTRA  2
#define XD_CARD_CAP_CMD_SHORTCUT 4

	/* Notify the host that some flash memory requests are pending. */
	void (*request)(struct xd_card_host *host);
	/* Set host IO parameters (power, clock, etc).     */
	void (*set_param)(struct xd_card_host *host, enum xd_card_param param,
			  int value);
	unsigned long       private[0] ____cacheline_aligned;
};

#define XD_CARD_PART_SHIFT 3

void xd_card_detect_change(struct xd_card_host *host);
int xd_card_suspend_host(struct xd_card_host *host);
int xd_card_resume_host(struct xd_card_host *host);
struct xd_card_host *xd_card_alloc_host(unsigned int extra, struct device *dev);
void xd_card_free_host(struct xd_card_host *host);
int xd_card_next_req(struct xd_card_host *host, struct xd_card_request **req);
void xd_card_set_extra(struct xd_card_host *host, unsigned char *e_data,
		       unsigned int e_size);
void xd_card_get_extra(struct xd_card_host *host, unsigned char *e_data,
		       unsigned int e_size);

unsigned long long xd_card_trans_addr(struct xd_card_host *host,
				      unsigned int zone, unsigned int block,
				      unsigned int page);

int xd_card_ecc_step(unsigned int *state, unsigned int *pos,
		     unsigned char *data, unsigned int count);
unsigned int xd_card_ecc_value(unsigned int state);
int xd_card_fix_ecc(unsigned int *pos, unsigned char *mask,
		    unsigned int act_ecc, unsigned int ref_ecc);

static inline void *xd_card_priv(struct xd_card_host *host)
{
	return (void *)host->private;
}

#endif

