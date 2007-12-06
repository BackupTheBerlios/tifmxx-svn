/*
 *  flash_bd.h - Simple flash to block device translation layer
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

#define FLASH_BD_INVALID 0xffffffffU

struct flash_bd_info {
	unsigned int zone_cnt;
	unsigned int zone_ssize;
	unsigned int block_cnt;
	unsigned int page_cnt;
	unsigned int page_size;
};

struct flash_bd;

enum flash_bd_cmd {
	NOTHING = 0,
	READ,
	READ_BUF,
	ERASE,
	BLOCK_BEGIN,
	WRITE,
	BLOCK_END
};

struct flash_bd_request {
	enum flash_bd_cmd cmd;
	unsigned int      zone;
	unsigned int      block;
	unsigned int      page;
	unsigned int      count;
	unsigned int      logical;
};

#endif
