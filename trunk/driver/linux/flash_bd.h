/*
 *  Simple flash to block device translation layer
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

/*
 * Unlike MTD components (if I understand it correctly) this translation layer
 * avoids keeping any metadata on the flash, to prevent interference with
 * removable media metadata and keep things compatible.
 *
 */

#ifndef _FLASH_BD_H
#define _FLASH_BD_H

#include <linux/scatterlist.h>

#define FLASH_BD_INVALID 0xffffffffU

struct flash_bd;

enum flash_bd_cmd {
	FBD_NONE = 0,
	FBD_READ,           /* read from media                               */
	FBD_READ_TMP,       /* read from media into temporary storage        */
	FBD_FLUSH_TMP,      /* copy data from temporary to permanent storage */
	FBD_SKIP,           /* pretend like reading from media               */
	FBD_ERASE,          /* erase media block                             */
	FBD_COPY,           /* media side page copy                          */
	FBD_WRITE,          /* write to media                                */
	FBD_WRITE_TMP,      /* write to media from temporary storage         */
	FBD_FILL_TMP,       /* copy data from permanent to temporary storage */
	FBD_BLOCK_MARK_1,   /* set media block log->phy entry (pre-write)  */
	FBD_BLOCK_MARK_2    /* set media block log->phy entry (post-write) */
};

struct flash_bd_request {
	enum flash_bd_cmd  cmd;
	unsigned int       log_block;
	unsigned int       phy_block;
	union {
		unsigned int page_off;
		unsigned int byte_off; /* for FBD_FILL/FLUSH_TMP */
	};
	union {
		unsigned int page_cnt;
		unsigned int byte_cnt; /* for FBD_FILL/FLUSH_TMP */
	};
	/* FBD_READ, FBD_WRITE - must be supplied elsewhere
	 * FBD_READ_BUF, FBD_WRITE_BUF, FBD_FLUSH_BUF, FBD_FILL_BUF, FBD_COPY -
	 * will be supplied by the flash_bd
	 * 
	 */
	struct scatterlist *sg;
	unsigned int       extra_offset;
	unsigned int       sg_count;
	struct { /* used by FBD_COPY */
		unsigned int phy_block;
		unsigned int page_off;
	} dst;
};

struct flash_bd* flash_bd_init(unsigned int phy_block_cnt,
			       unsigned int log_block_cnt,
			       unsigned int page_cnt,
			       unsigned int page_size);
void flash_bd_destroy(struct flash_bd *fbd);

#endif
