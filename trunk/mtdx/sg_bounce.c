/*
 *  MTDX copy to/from sg entry
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/scatterlist.h>

unsigned int bounce_to_sg(struct scatterlist *sg, unsigned int *sg_off,
			  char *buf, unsigned int *buf_off, unsigned int count)
{
	char *dst;
#ifdef CONFIG_HIGHMEM
	unsigned long flags;
	unsigned int s_off, p_off, p_cnt, rc;
	struct page *pg;
#endif

	if (*sg_off >= sg->length)
		return 0;

	count = min(sg->length - *sg_off, count);

#ifndef CONFIG_HIGHMEM
	dst = sg_virt(sg);
	memcpy(dst + *sg_off, buf + *buf_off, count);
	*sg_off += count;
	*buf_off += count;
#else
	rc = count;
	while (rc) {
		s_off = sg->offset + *sg_off;
		pg = nth_page(sg_page(sg), s_off >> PAGE_SHIFT);
		p_off = offset_in_page(s_off);
		p_cnt = min(PAGE_SIZE - p_off, rc);

		local_irq_save(flags);
		dst = kmap_atomic(pg, KM_BOUNCE_READ) + p_off;
		memcpy(dst, buf + *buf_off, p_cnt);
		kunmap_atomic(dst - p_off, KM_BOUNCE_READ);
		local_irq_restore(flags);

		rc -= p_cnt;
		*sg_off += p_cnt;
		*buf_off += p_cnt;
	}
#endif
	return count;
}

unsigned int bounce_from_sg(char *buf, unsigned int *buf_off,
			    struct scatterlist *sg, unsigned int *sg_off,
			    unsigned int count)
{
	char *src;
#ifdef CONFIG_HIGHMEM
	unsigned long flags;
	unsigned int s_off, p_off, p_cnt, rc;
	struct page *pg;
#endif

	if (*sg_off >= sg->length)
		return 0;

	count = min(sg->length - *sg_off, count);

#ifndef CONFIG_HIGHMEM
	src = sg_virt(sg);
	memcpy(buf + *buf_off, src + *sg_off, count);
	*sg_off += count;
	*buf_off += count;
#else
	rc = count;
	while (rc) {
		s_off = sg->offset + *sg_off;
		pg = nth_page(sg_page(sg), s_off >> PAGE_SHIFT);
		p_off = offset_in_page(s_off);
		p_cnt = min(PAGE_SIZE - p_off, rc);

		local_irq_save(flags);
		src = kmap_atomic(pg, KM_BOUNCE_READ) + p_off;
		memcpy(buf + *buf_off, src, p_cnt);
		kunmap_atomic(src - p_off, KM_BOUNCE_READ);
		local_irq_restore(flags);

		rc -= p_cnt;
		*sg_off += p_cnt;
		*buf_off += p_cnt;
	}
#endif
	return count;
}
