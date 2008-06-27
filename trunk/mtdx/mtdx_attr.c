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

#include "mtdx_attr.h"
#include <linux/module.h>
#include <linux/err.h>

unsigned int mtdx_attr_get_byte_range(struct mtdx_attr *attr, void *buf,
				      unsigned int offset, unsigned int count)
{
	unsigned int i_count = count;
	unsigned int c_page, p_off, c_count;

	while (count) {
		c_page = offset / attr->page_size;
		p_off = offset % attr->page_size;
		c_count = min(count, attr->page_size - p_off);

		if (c_page >= attr->page_cnt)
			break;

		if (!attr->pages[c_page])
			memset(buf + offset, attr->page_fill, c_count);
		else
			memcpy(buf + offset, attr->pages[c_page] + p_off,
			       c_count);
		offset += c_count;
		count -= c_count;
	}

	return i_count - count;
}

unsigned int mtdx_attr_set_byte_range(struct mtdx_attr *attr, void *buf,
				      unsigned int offset, unsigned int count)
{
	unsigned int i_count = count;
	unsigned int c_page, p_off, c_count;

	while (count) {
		c_page = offset / attr->page_size;
		p_off = offset % attr->page_size;
		c_count = min(count, attr->page_size - p_off);

		if (c_page >= attr->page_cnt)
			break;

		if (!attr->pages[c_page]) {
			attr->pages[c_page] = kmalloc(attr->page_size,
						      GFP_KERNEL);

			if (!attr->pages[c_page])
				break;

			memset(attr->pages[c_page], attr->page_fill,
			       attr->page_size);
		}

		memcpy(attr->pages[c_page] + p_off, buf + offset,
		       c_count);
		offset += c_count;
		count -= c_count;
	}

	return i_count - count;
}

void mtdx_attr_free(struct mtdx_attr *attr)
{
	unsigned int cnt;

	for (cnt = 0; cnt < attr->page_cnt; ++cnt)
		kfree(attr->pages[cnt]);

	kfree(attr);
}

struct mtdx_attr *mtdx_attr_alloc(struct mtdx_dev *mdev, unsigned int page_cnt,
				  unsigned int page_size)
{
	struct mtdx_attr *attr = kzalloc(sizeof(struct mtdx_attr)
					 + sizeof(char*) * page_cnt,
					 GFP_KERNEL);

	if (!attr)
		return ERR_PTR(-ENOMEM);

	attr->mdev = mdev;
	attr->page_cnt = page_cnt;
	attr->page_size = page_size;

	return attr;
}

int mtdx_attr_value_range_verify(struct mtdx_attr *attr, int offset, long param)
{
	if ((offset + param) <= (attr->page_cnt * attr->page_size))
		return param;
	else
		return -E2BIG;
}

int mtdx_attr_value_string_verify(struct mtdx_attr *attr, int offset,
				  long param)
{
	unsigned int c_page = offset / attr->page_size;
	unsigned int p_off = offset % / attr->page_size;
	char delim = param;
	int count;


}

int mtdx_attr_value_be_num_print(struct mtdx_attr *attr, int offset,
				 char *out_buf, unsigned int out_count,
				 long param)
{
	unsigned long long val;
	char format[8];

	if (param > 8)
		return -EINVAL;

	if (param != mtdx_attr_get_byte_range(attr, &val, offset, count))
		return -EINVAL;

	val = be64_to_cpu(val);

	if (param <= sizeof(int)) {
		sprintf(format, "%%0%dx", param * 2);
		return snprintf(out_buf, out_count, format, (unsigned int)val);
	} else if (param <= sizeof(long) {
		sprintf(format, "%%0%dlx", param * 2);
		return snprintf(out_buf, out_count, format, (unsigned long)val);
	} else if (param <= sizeof(long long) {
		sprintf(format, "%%0%dllx", param * 2);
		return snprintf(out_buf, out_count, format,
				(unsigned long long)val);
	} else
		return -EINVAL;
}

int mtdx_attr_value_le_num_print(struct mtdx_attr *attr, int offset,
				 char *out_buf, unsigned int out_count,
				 long param)
{
	unsigned long long val;
	char format[8];

	if (param > 8)
		return -EINVAL;

	if (param != mtdx_attr_get_byte_range(attr, &val, offset, count))
		return -EINVAL;

	val = le64_to_cpu(val);

	if (param <= sizeof(int)) {
		sprintf(format, "%%0%dx", param * 2);
		return snprintf(out_buf, out_count, format, (unsigned int)val);
	} else if (param <= sizeof(long) {
		sprintf(format, "%%0%dlx", param * 2);
		return snprintf(out_buf, out_count, format, (unsigned long)val);
	} else if (param <= sizeof(long long) {
		sprintf(format, "%%0%dllx", param * 2);
		return snprintf(out_buf, out_count, format, (unsigned long long)val);
	} else
		return -EINVAL;
}
