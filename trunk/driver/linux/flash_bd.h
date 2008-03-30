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

#include <linux/types.h>

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
	FBD_MARK,           /* write only extradata to some pages            */
	FBD_MARK_BAD        /* as above, mark pages as bad                   */
};

struct flash_bd_request {
	enum flash_bd_cmd cmd;
	unsigned int      zone;
	unsigned int      log_block;
	unsigned int      phy_block;
	union {
		unsigned int page_off;
		unsigned int byte_off; /* for FBD_FILL/FLUSH_TMP */
	};
	union {
		unsigned int page_cnt;
		unsigned int byte_cnt; /* for FBD_FILL/FLUSH_TMP, FBD_SKIP */
	};
	struct { /* used by FBD_COPY */
		unsigned int phy_block;
		unsigned int page_off;
	} src;
};

struct flash_bd* flash_bd_init(unsigned int zone_cnt,
			       unsigned int phy_block_cnt,
			       unsigned int log_block_cnt,
			       unsigned int page_cnt,
			       unsigned int page_size);
void flash_bd_destroy(struct flash_bd *fbd);
int flash_bd_set_empty(struct flash_bd *fbd, unsigned int zone,
		       unsigned int phy_block, int erased);
int flash_bd_set_full(struct flash_bd *fbd, unsigned int zone,
		      unsigned int phy_block, unsigned int log_block);
unsigned int flash_bd_get_physical(struct flash_bd *fbd, unsigned int zone,
				   unsigned int log_block);
int flash_bd_next_req(struct flash_bd *fbd, struct flash_bd_request *req,
		      unsigned int count, int error);
unsigned int flash_bd_end(struct flash_bd *fbd);
int flash_bd_start_reading(struct flash_bd *fbd, unsigned long long offset,
			   unsigned int count);
size_t flash_bd_map_size(struct flash_bd *fbd);
ssize_t flash_bd_read_map(struct flash_bd *fbd, char *buf, loff_t offset,
			  size_t count);

#endif
