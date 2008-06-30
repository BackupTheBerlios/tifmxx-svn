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
	unsigned int c_page = offset / attr->page_size;
	unsigned int p_off = offset % attr->page_size;
	unsigned int c_count;

	while (count) {
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
		p_off = 0;
		c_page++;
	}

	return i_count - count;
}
EXPORT_SYMBOL(mtdx_attr_get_byte_range);

unsigned int mtdx_attr_set_byte_range(struct mtdx_attr *attr, void *buf,
				      unsigned int offset, unsigned int count)
{
	unsigned int i_count = count, c_count;
	unsigned int c_page = offset / attr->page_size;
	unsigned int p_off = offset % attr->page_size;


	while (count) {
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
		p_off = 0;
		c_page++;
	}

	return i_count - count;
}
EXPORT_SYMBOL(mtdx_attr_set_byte_range);

static int mtdx_attr_verify_print(struct mtdx_attr *attr,
				  struct mtdx_attr_entry *entry,
				  unsigned int attr_off, char *out_buf,
				  unsigned int out_off, unsigned int *out_size)
{
	struct mtdx_attr_value *c_val;
	unsigned int val_cnt = 0, p_size = 0, l_size;
	int rc = 0, c_size, p_size = 0;
	char *c_buf;

	while (1) {
		c_val = entry->values[val_cnt++];
		if (!c_val->verify)
			break;

		c_size = c_val->verify(attr, attr_off, c_val->param);
		if (c_size < 0) {
			rc = c_size;
			break;
		}

		l_size = 0;

		if (c_val->name)
			l_size = strlen(c_val->name) + 1;

		if (c_val->print) {
			c_buf = c_val->print(attr, attr_off, c_size,
					     c_val->param);
			if (!c_buf) {
				rc = -ENOMEM;
				break;
			}

			if (l_size)
				l_size++;

			l_size += strlen(c_buf) + 1;
		}

		if (l_size && out_buf) {
			if (out_off > p_size) {
				off_p = 0;
				
			}
			int off_s = out_off - p_size;
			int off_p = 
			if (off_s <= 0)
				off_s = 0;

			
			int off_e = (out_off + out_size) - (p_size + l_size);
			if (off_s < 0 && off_e)
		}
	}
}

void mtdx_attr_free(struct mtdx_attr *attr)
{
	unsigned int cnt;

	for (cnt = 0; cnt < attr->page_cnt; ++cnt)
		kfree(attr->pages[cnt]);

	kfree(attr);
}
EXPORT_SYMBOL(mtdx_attr_free);

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
EXPORT_SYMBOL(mtdx_attr_alloc);

int mtdx_attr_value_range_verify(struct mtdx_attr *attr, unsigned int offset,
				 long param)
{
	if ((offset + param) <= (attr->page_cnt * attr->page_size))
		return param;
	else
		return -E2BIG;
}
EXPORT_SYMBOL(mtdx_attr_value_range_verify);

int mtdx_attr_value_string_verify(struct mtdx_attr *attr, unsigned int offset,
				  long param)
{
	unsigned int c_page = offset / attr->page_size;
	unsigned int p_off = offset % attr->page_size;
	int count = 0;
	char *c_ptr;

	while (c_page < attr->page_cnt) {
		if (!attr->pages[c_page]) {
			if (param == attr->page_fill)
				break;
			else
				count += attr->page_size - p_off;
		} else {
			c_ptr = memchr(attr->pages[c_page] + p_off, param,
				       attr->page_size - p_off);
			if (c_ptr) {
				count += c_ptr - (attr->pages[c_page] + p_off);
				break;
			}
		}
		p_off = 0;
		c_page++;
	}
	return count;
}
EXPORT_SYMBOL(mtdx_attr_value_string_verify);

char *mtdx_attr_value_string_print(struct mtdx_attr *attr, unsigned int offset,
				   unsigned int size, long param)
{
	char *rv = kmalloc(size, GFP_KERNEL);

	if (!rv)
		return rv;

	if (size != mtdx_attr_get_byte_range(attr, rv, offset, size)) {
		kfree(rv);
		return NULL;
	}

	return rv;
}
EXPORT_SYMBOL(mtdx_attr_value_string_print);

char *mtdx_attr_value_be_num_print(struct mtdx_attr *attr, unsigned int offset,
				   unsigned int size, long param)
{
	unsigned long long val = 0;
	char format[8];

	if (size > 8)
		return NULL;

	if (size != mtdx_attr_get_byte_range(attr, &val, offset, size))
		return NULL;

	val = be64_to_cpu(val);

	if (size <= sizeof(int)) {
		sprintf(format, "%%0%dx", size * 2);
		return kasprintf(GFP_KERNEL, format, (unsigned int)val);
	} else if (size <= sizeof(long)) {
		sprintf(format, "%%0%dlx", size * 2);
		return kasprintf(GFP_KERNEL, format, (unsigned long)val);
	} else if (size <= sizeof(long long)) {
		sprintf(format, "%%0%dllx", size * 2);
		return kasprintf(GFP_KERNEL, format, (unsigned long long)val);
	} else
		return NULL;
}
EXPORT_SYMBOL(mtdx_attr_value_be_num_print);

char *mtdx_attr_value_le_num_print(struct mtdx_attr *attr, unsigned int offset,
				   unsigned int size, long param)
{
	unsigned long long val = 0;
	char format[8];

	if (size > 8)
		return NULL;

	if (size != mtdx_attr_get_byte_range(attr, &val, offset, size))
		return NULL;

	val = le64_to_cpu(val);

	if (size <= sizeof(int)) {
		sprintf(format, "%%0%dx", size * 2);
		return kasprintf(GFP_KERNEL, format, (unsigned int)val);
	} else if (size <= sizeof(long)) {
		sprintf(format, "%%0%dlx", size * 2);
		return kasprintf(GFP_KERNEL, format, (unsigned long)val);
	} else if (size <= sizeof(long long)) {
		sprintf(format, "%%0%dllx", size * 2);
		return kasprintf(GFP_KERNEL, format, (unsigned long long)val);
	} else
		return NULL;
}
EXPORT_SYMBOL(mtdx_attr_value_le_num_print);
