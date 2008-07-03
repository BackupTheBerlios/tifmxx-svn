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

unsigned int __mtdx_attr_get_byte_range(struct mtdx_attr *attr, void *buf,
					unsigned int offset,
					unsigned int count)
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
EXPORT_SYMBOL(__mtdx_attr_get_byte_range);

unsigned int __mtdx_attr_set_byte_range(struct mtdx_attr *attr, void *buf,
					unsigned int offset,
					unsigned int count)
{
	unsigned int i_count = count, c_count, cnt;
	unsigned int c_page = offset / attr->page_size;
	unsigned int p_off = offset % attr->page_size;


	while (count) {
		c_count = min(count, attr->page_size - p_off);

		if (c_page >= attr->page_cnt)
			break;

		for (cnt = 0; cnt < c_count; ++cnt)
			if (((char *)buf)[offset + cnt] != attr->page_fill)
				break;

		if (cnt == c_count) {
			if (attr->pages[c_page]) {
				memset(attr->pages[c_page] + p_off,
				       attr->page_fill, c_count);

				for (cnt = 0; cnt < attr->page_size; ++cnt)
					if (attr->pages[c_page][cnt]
					    != attr->page_fill)
						break;
				
				if (cnt == attr->page_size) {
					kfree(attr->pages[c_page]);
					attr->pages[c_page] = NULL;
				}
			}
		} else {
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
		}
		offset += c_count;
		count -= c_count;
		p_off = 0;
		c_page++;
	}

	return i_count - count;
}
EXPORT_SYMBOL(__mtdx_attr_set_byte_range);

static int append_string(char *buf, unsigned int buf_off,
			 int buf_size, char *str,
			 unsigned int str_off, int str_size)
{
	if (!buf || (buf_size <= 0) || (str_size <= 0))
		return str_size;

	if (str_off < buf_off) {
		if ((str_off + str_size) > buf_off) {
			str_off = buf_off - str_off;
			buf_off = 0;
			str_size = min(buf_size, str_size - (int)str_off);
		} else
			return 0;
	} else {
		if ((buf_off + buf_size) > str_off) {
			buf_off = str_off - buf_off;
			str_off = 0;
			str_size = min(str_size, buf_size - (int)buf_off);
		} else
			return 0;
	}

	memcpy(buf + buf_off, str + str_off, str_size);
	return str_size;
}

static int mtdx_attr_verify_entry(struct mtdx_attr *attr,
				  struct mtdx_attr_entry *entry,
				  unsigned int attr_off)
{
	struct mtdx_attr_value *c_val;
	unsigned int val_cnt = 0;
	int rc = 0, c_size;

	if (!entry->values) {
		if ((attr_off + entry->skip)
		    <= (attr->page_cnt * attr->page_size))
			return entry->skip;
		else
			return -E2BIG;
	}

	while (1) {
		c_val = &entry->values[val_cnt++];
		if (!c_val->verify)
			break;

		c_size = c_val->verify(attr, attr_off, c_val->param);
		if (c_size < 0) {
			rc = c_size;
			break;
		}

		rc += c_size;
	}

	if (rc >= 0) {
		rc += entry->skip;
		if ((attr_off + rc) >= (attr->page_cnt * attr->page_size))
			rc = -E2BIG;
	}

	return rc;
}

static int mtdx_attr_print_entry(struct mtdx_attr *attr,
				 struct mtdx_attr_entry *entry,
				 char *out_buf, unsigned int *out_off,
				 int out_size)
{
	struct mtdx_attr_value *c_val;
	unsigned int val_cnt = 0, l_size;
	unsigned int c_str_off = 0, c_str_size;
	unsigned int attr_off = entry->offset;
	int rc = 0, c_size;
	char *c_buf;

	if (!entry->values) {
		if ((attr_off + entry->skip)
		    <= (attr->page_cnt * attr->page_size))
			return entry->skip;
		else
			return -E2BIG;
	}

	while (1) {
		if (out_buf && (out_size <= 0))
			break;

		c_val = &entry->values[val_cnt++];
		if (!c_val->verify)
			break;

		c_size = c_val->verify(attr, attr_off, c_val->param);
		if (c_size < 0) {
			rc = c_size;
			break;
		}

		rc += c_size;
		l_size = 0;

		if (c_val->name) {
			c_str_size = strlen(c_val->name);

			l_size = append_string(out_buf, *out_off,
					       out_size, c_val->name,
					       c_str_off, c_str_size);
			*out_off += l_size;
			out_size -= l_size;
			c_str_off += c_str_size;
		}

		if (c_val->print) {
			c_buf = c_val->print(attr, attr_off, c_size,
					     c_val->param);
			if (!c_buf) {
				rc = -ENOMEM;
				break;
			}

			c_str_size = strlen(c_buf);

			if (c_val->name) {
				l_size = append_string(out_buf, *out_off,
						       out_size, ": ",
						       c_str_off, 2);
				*out_off += l_size;
				out_size -= l_size;
				c_str_off += 2;
			}

			l_size = append_string(out_buf, *out_off,
					       out_size, c_buf,
					       c_str_off, c_str_size);
			*out_off += l_size;
			out_size -= l_size;

			l_size = append_string(out_buf, *out_off,
					       out_size, "\n",
					       c_str_off, 1);
			*out_off += l_size;
			out_size -= l_size;
			c_str_off += c_str_size + 1;
			kfree(c_buf);
		} else if (c_val->name) {
			l_size = append_string(out_buf, *out_off, out_size,
					       "\n", c_str_off, 1);
			*out_off += l_size;
			out_size -= l_size;
			c_str_off++;
		}
		attr_off += c_size;
	}

	if (rc >= 0) {
		rc += entry->skip;
		if ((attr_off + entry->skip)
		    >= (attr->page_cnt * attr->page_size))
			rc = -E2BIG;
	}

	return rc;
}

static int mtdx_attr_verify_all(struct mtdx_attr *attr)
{
	struct list_head *p;
	struct mtdx_attr_entry *entry;
	unsigned int next_offset = 0;
	int rc = 0;

	__list_for_each(p, &attr->entries) {
		entry = list_entry(p, struct mtdx_attr_entry, node);

		entry->offset = next_offset;
		rc = mtdx_attr_verify_entry(attr, entry, next_offset);
		if (rc >= 0)
			next_offset += rc;
		else {
			struct list_head *q;

			while (p != &attr->entries) {
				q = p->next;
				list_move_tail(p, &attr->bad_entries);
				entry = list_entry(p, struct mtdx_attr_entry,
						   node);
				sysfs_remove_file_from_group(
					&attr->mdev->dev.kobj,
					&entry->sysfs_attr.attr,
					attr->sysfs_grp.name);
				p = q;
			}
			return rc;
		}
	}

	while (!list_empty(&attr->bad_entries)) {
		p = attr->bad_entries.next;
		entry = list_entry(p, struct mtdx_attr_entry, node);
		rc = mtdx_attr_verify_entry(attr, entry, next_offset);

		if (rc < 0)
			break;

		entry->offset = next_offset;
		next_offset += rc;
		rc = sysfs_add_file_to_group(&attr->mdev->dev.kobj,
					     &entry->sysfs_attr.attr,
					     attr->sysfs_grp.name);
		if (rc)
			return rc;

		list_move_tail(p, &attr->entries);
	}

	return 0;
}

static ssize_t mtdx_attr_blob_read(struct kobject *kobj,
				   struct bin_attribute *sysfs_attr,
				   char *buf, loff_t offset, size_t count)
{
	struct mtdx_attr *attr = sysfs_attr->private;
	ssize_t rc = 0;

	mutex_lock(attr->lock);
	rc = __mtdx_attr_get_byte_range(attr, buf, offset, count);

	if (rc >= 0)
		rc = count;
	mutex_unlock(attr->lock);
	return rc;
}

static ssize_t mtdx_attr_blob_write(struct kobject *kobj,
				    struct bin_attribute *sysfs_attr,
				    char *buf, loff_t offset, size_t count)
{
	struct mtdx_attr *attr = sysfs_attr->private;
	ssize_t rc = 0;

	mutex_lock(attr->lock);
	if(0 < __mtdx_attr_set_byte_range(attr, buf, offset, count)) {
		attr->modified = 1;
		rc = mtdx_attr_verify_all(attr);
	}

	if (rc >= 0)
		rc = count;
	mutex_unlock(attr->lock);
	return rc;
}

static ssize_t mtdx_attr_entry_read(struct kobject *kobj,
				    struct bin_attribute *sysfs_attr,
				    char *buf, loff_t offset, size_t count)
{
	struct mtdx_attr *attr = sysfs_attr->private;
	struct mtdx_attr_entry *entry = container_of(sysfs_attr,
						     struct mtdx_attr_entry,
						     sysfs_attr);
	unsigned int p_off = offset;
	ssize_t rc = 0;

	mutex_lock(attr->lock);
	rc = mtdx_attr_print_entry(attr, entry, buf, &p_off, count);

	if (rc >= 0)
		rc = count;
	mutex_unlock(attr->lock);
	return rc;
}

int mtdx_attr_add_entry(struct mtdx_attr *attr, struct mtdx_attr_value *values,
			const char *name, unsigned int skip)
{
	struct mtdx_attr_entry *entry = NULL;
	unsigned int p_off = 0;
	int rc = 0;

	mutex_lock(attr->lock);
	if (!list_empty(&attr->bad_entries)) {
		rc = -EACCES;
		goto out;
	}

	if ((values && !name) || (!values && !skip)) {
		rc = -EINVAL;
		goto out;
	}
		
	entry = kzalloc(sizeof(struct mtdx_attr_entry), GFP_KERNEL);
	if (!entry) {
		rc = -ENOMEM;
		goto out;
	}

	if (name) {
		entry->sysfs_attr.attr.name = kstrdup(name, GFP_KERNEL);
		if (!entry->sysfs_attr.attr.name) {
			rc = -ENOMEM;
			goto out;
		}
	}

	entry->values = values;
	entry->skip = skip;
	entry->offset = 0;

	if (!list_empty(&attr->entries)) {
		struct mtdx_attr_entry *last_entry
			= list_entry(attr->entries.prev, struct mtdx_attr_entry,
				     node);

		entry->offset = last_entry->offset + last_entry->c_size
				+ last_entry->skip;
	}

	rc = mtdx_attr_print_entry(attr, entry, NULL, &p_off, 0);

	if (rc < 0)
		goto out;

	entry->c_size = rc;
	list_add_tail(&entry->node, &attr->entries);
	entry->sysfs_attr.attr.mode = 0444;
	entry->sysfs_attr.size = p_off;
	entry->sysfs_attr.private = attr;
	entry->sysfs_attr.read = mtdx_attr_entry_read;

	rc = sysfs_add_file_to_group(&attr->mdev->dev.kobj,
				     &entry->sysfs_attr.attr,
				     attr->sysfs_grp.name);
out:
	if (rc) {
		if (entry) {
			kfree(entry->sysfs_attr.attr.name);
			kfree(entry);
		}
	}

	mutex_unlock(attr->lock);
	return rc;
}
EXPORT_SYMBOL(mtdx_attr_add_entry);

void mtdx_attr_free(struct mtdx_attr *attr)
{
	unsigned int cnt;
	struct list_head *p;
	struct mtdx_attr_entry *entry;

	if (attr->sysfs_grp.name)
		sysfs_remove_group(&attr->mdev->dev.kobj, &attr->sysfs_grp);

	kfree(attr->sysfs_blob.attr.name);
	kfree(attr->sysfs_grp.name);

	while (!list_empty(&attr->entries)) {
		p = attr->entries.next;
		entry = list_entry(p, struct mtdx_attr_entry, node);
		list_del(p);
		kfree(entry->sysfs_attr.attr.name);
		kfree(entry);
	}

	while (!list_empty(&attr->bad_entries)) {
		p = attr->bad_entries.next;
		entry = list_entry(p, struct mtdx_attr_entry, node);
		list_del(p);
		kfree(entry->sysfs_attr.attr.name);
		kfree(entry);
	}

	for (cnt = 0; cnt < attr->page_cnt; ++cnt)
		kfree(attr->pages[cnt]);

	kfree(attr);
}
EXPORT_SYMBOL(mtdx_attr_free);

struct mtdx_attr *mtdx_attr_alloc(struct mtdx_dev *mdev, const char *name,
				  unsigned int page_cnt,
				  unsigned int page_size)
{
	struct mtdx_attr *attr = kzalloc(sizeof(struct mtdx_attr)
					 + sizeof(char*) * page_cnt,
					 GFP_KERNEL);
	int rc = 0;

	if (!attr)
		return ERR_PTR(-ENOMEM);

	attr->mdev = mdev;
	mutex_init(attr->lock);
	attr->page_cnt = page_cnt;
	attr->page_size = page_size;

	attr->sysfs_grp.name = kstrdup(name, GFP_KERNEL);
	if (!attr->sysfs_grp.name) {
		rc = -ENOMEM;
		goto err_out;
	}

	rc = sysfs_create_group(&mdev->dev.kobj, &attr->sysfs_grp);

	if (rc) {
		attr->sysfs_grp.name = NULL;
		goto err_out;
	}

	attr->sysfs_blob.attr.name = kasprintf(GFP_KERNEL, "%s.bin", name);
	if (!attr->sysfs_blob.attr.name) {
		rc = -ENOMEM;
		goto err_out;
	}
	attr->sysfs_blob.attr.mode = 0644;

	attr->sysfs_blob.size = page_cnt * page_size;
	attr->sysfs_blob.private = attr;
	attr->sysfs_blob.read = mtdx_attr_blob_read;
	attr->sysfs_blob.write = mtdx_attr_blob_write;

	INIT_LIST_HEAD(&attr->entries);
	INIT_LIST_HEAD(&attr->bad_entries);

	rc = sysfs_add_file_to_group(&mdev->dev.kobj, &attr->sysfs_blob.attr,
				     name);
	if (rc)
		goto err_out;

	return attr;
err_out:
	mtdx_attr_free(attr);
	return ERR_PTR(rc);
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

	if (size != __mtdx_attr_get_byte_range(attr, rv, offset, size)) {
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

	if (size != __mtdx_attr_get_byte_range(attr, &val, offset, size))
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

	if (size != __mtdx_attr_get_byte_range(attr, &val, offset, size))
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
