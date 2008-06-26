/*
 *  MTDx attributes
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef _MTDX_ATTR_H
#define _MTDX_ATTR_H

#include <linux/sysfs.h>
#include "mtdx_common.h"

struct mtdx_attr_parser {
	struct list_head node;
	struct mtdx_attr_parser *children;
};

struct mtdx_attr {
	struct mtdx_dev         *mdev;
	struct attribute_group  grp;
	unsigned int            page_cnt;
	unsigned int            page_size;
	unsigned int            page_fill;
	struct mtdx_attr_parser *parsers;
	char                    *pages[];
};

unsigned int mtdx_attr_get_byte_range(struct mtdx_attr *attr, char *buf,
				      unsigned int offset, unsigned int count);
unsigned int mtdx_attr_set_byte_range(struct mtdx_attr *attr, char *buf,
				      unsigned int offset, unsigned int count);
void mtdx_attr_free(struct mtdx_attr *attr);
struct mtdx_attr *mtdx_attr_alloc(struct mtdx_dev *mdev, unsigned int page_cnt,
				  unsigned int page_size);

#endif
