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

struct mtdx_attr;

struct mtdx_attr_value {
	char             *name;
	long             param;
	int              (*verify)(struct mtdx_attr *attr, unsigned int offset,
				   long param);
	char             *(*print)(struct mtdx_attr *attr, unsigned int offset,
				   unsigned int size, long param);
};

struct mtdx_attr_entry {
	struct list_head       node;
	struct mtdx_attr_value *values;
};

struct mtdx_attr {
	struct mtdx_dev        *mdev;
	struct attribute_group grp;
	unsigned int           page_cnt;
	unsigned int           page_size;
	unsigned int           page_fill;
	struct mtdx_attr_entry *entries;
	char                   *pages[];
};

unsigned int mtdx_attr_get_byte_range(struct mtdx_attr *attr, void *buf,
				      unsigned int offset, unsigned int count);
unsigned int mtdx_attr_set_byte_range(struct mtdx_attr *attr, void *buf,
				      unsigned int offset, unsigned int count);
void mtdx_attr_free(struct mtdx_attr *attr);
struct mtdx_attr *mtdx_attr_alloc(struct mtdx_dev *mdev, unsigned int page_cnt,
				  unsigned int page_size);
int mtdx_attr_add_entry(struct mtdx_attr *attr, struct mtdx_attr_value *values);


/* Verification function is applied to attribute blob at certain offset and
 * returns number of bytes that will be consumed by current value (if positive)
 * or error code (if negative).
 */

int mtdx_attr_value_range_verify(struct mtdx_attr *attr, unsigned int offset,
				 long param);
int mtdx_attr_value_string_verify(struct mtdx_attr *attr, unsigned int offset,
				  long param);

/* Print function should set the representation of the value into newly
 * allocated buffer, which will be later freed by kfree.
 */
char *mtdx_attr_value_string_print(struct mtdx_attr *attr, unsigned int offset,
				   unsigned int size, long param);
char *mtdx_attr_value_be_num_print(struct mtdx_attr *attr, unsigned int offset,
				   unsigned int size, long param);
char *mtdx_attr_value_le_num_print(struct mtdx_attr *attr, unsigned int offset,
				   unsigned int size, long param);
#endif
