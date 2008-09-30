/*
 *  MTDX core data functionality
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include "mtdx_data.h"

static void mtdx_data_iter_buf_set(struct mtdx_data_iter *iter,
				   unsigned int pos)
{
	iter->iter_pos = min(pos, iter->r_buf.length);
}

static void mtdx_data_iter_buf_inc(struct mtdx_data_iter *iter,
				   unsigned int off)
{
	iter->iter_pos += off;
	mtdx_data_iter_buf_set(iter, iter->iter_pos);
}

static void mtdx_data_iter_buf_dec(struct mtdx_data_iter *iter,
				   unsigned int off)
{
	iter->iter_pos -= min(iter->iter_pos, off);
	mtdx_data_iter_buf_set(iter, iter->iter_pos);
}

static void mtdx_data_iter_buf_fill(struct mtdx_data_iter *iter, int c,
				    unsigned int count)
{
	unsigned int c_sz = min(count, iter->r_buf.length - iter->iter_pos);

	if (c_sz) {
		memset(iter->r_buf.data + iter->iter_pos, c, c_sz);
		mtdx_data_iter_buf_inc(iter, c_sz);
	}
}

static void mtdx_data_iter_buf_get_sg(struct mtdx_data_iter *iter,
				      struct scatterlist *sg,
				      unsigned int length)
{
	unsigned int c_sz = min(length, iter->r_buf.length - iter->iter_pos);

	sg->length = c_sz;

	if (c_sz) {
		sg_set_buf(sg, iter->r_buf.data + iter->iter_pos, c_sz);
		mtdx_data_iter_buf_inc(iter, c_sz);
	}
}

static void mtdx_data_iter_buf_get_bvec(struct mtdx_data_iter *iter,
					struct bio_vec *bvec,
					unsigned int length)
{
	unsigned int c_sz = min(length, iter->r_buf.length - iter->iter_pos);

	bvec->bv_len = c_sz;

	if (c_sz) {
		bvec->bv_page = virt_to_page(iter->r_buf.data
					     + iter->iter_pos);
		bvec->bv_offset = offset_in_page(iter->r_buf.data
						 + iter->iter_pos);
		mtdx_data_iter_buf_inc(iter, c_sz);
	}
}

static struct mtdx_data_iter_ops mtdx_data_iter_buf_ops = {
	.set_iter  = mtdx_data_iter_buf_set,
	.inc_iter  = mtdx_data_iter_buf_inc,
	.dec_iter  = mtdx_data_iter_buf_dec,
	.fill_iter = mtdx_data_iter_buf_fill,
	.get_sg    = mtdx_data_iter_buf_get_sg,
	.get_bvec  = mtdx_data_iter_buf_get_bvec
};

void mtdx_data_iter_init_buf(struct mtdx_data_iter *iter, void *data,
			     unsigned int length)
{
	memset(iter, 0, sizeof(struct mtdx_data_iter));
	iter->ops = &mtdx_data_iter_buf_ops;
	iter->r_buf.data = data;
	iter->r_buf.length = length;
}
EXPORT_SYMBOL(mtdx_data_iter_init_buf);

static void mtdx_data_iter_bio_set(struct mtdx_data_iter *iter,
				   unsigned int pos)
{
	struct mtdx_bio_iter *b_iter = &iter->r_bio;

	iter->iter_pos = pos;
	b_iter->seg = b_iter->head;
	b_iter->seg_pos = 0;
	b_iter->vec_pos = 0;
	b_iter->idx = 0;

	for (; b_iter->seg; b_iter->seg = b_iter->seg->bi_next) {
		for (b_iter->idx = 0; b_iter->idx < b_iter->seg->bi_vcnt;
		     ++b_iter->idx) {
			if ((pos - b_iter->vec_pos)
			    < b_iter->seg->bi_io_vec[b_iter->idx].bv_len)
				return;

			b_iter->vec_pos += b_iter->seg->bi_io_vec[b_iter->idx]
						       .bv_len;
		}
		b_iter->seg_pos = b_iter->vec_pos;
	}
	iter->iter_pos = b_iter->vec_pos;
}

static void mtdx_data_iter_bio_inc(struct mtdx_data_iter *iter,
				   unsigned int off)
{
	struct mtdx_bio_iter *b_iter = &iter->r_bio;
	unsigned int inc;

	for (; b_iter->seg; b_iter->seg = b_iter->seg->bi_next) {
		for (; b_iter->idx < b_iter->seg->bi_vcnt;
		     ++b_iter->idx) {
			inc = b_iter->seg->bi_io_vec[b_iter->idx].bv_len;
			if (off >= inc) {
				off -= inc;
				iter->iter_pos += inc;
			} else {
				iter->iter_pos += off;
				return;
			}

			b_iter->vec_pos += b_iter->seg->bi_io_vec[b_iter->idx]
							.bv_len;
		}
		b_iter->seg_pos = b_iter->vec_pos;
	}

	iter->iter_pos = b_iter->vec_pos;
}

static void mtdx_data_iter_bio_dec(struct mtdx_data_iter *iter,
				   unsigned int off)
{
	struct mtdx_bio_iter *b_iter = &iter->r_bio;
	unsigned int pos = iter->iter_pos - min(iter->iter_pos, off);

	if (b_iter->seg_pos <= pos) {
		while (b_iter->vec_pos > pos) {
			iter->iter_pos -= iter->iter_pos - b_iter->vec_pos;
			b_iter->idx--;
			b_iter->vec_pos -= b_iter->seg->bi_io_vec[b_iter->idx]
						       .bv_len;
		}
		iter->iter_pos = pos; 
	} else
		mtdx_data_iter_bio_set(iter, pos);
}

static void mtdx_data_iter_bio_fill(struct mtdx_data_iter *iter, int c,
				    unsigned int count)
{
	struct mtdx_bio_iter *b_iter = &iter->r_bio;
	struct bio_vec *b_vec;
	unsigned int c_off, c_sz;
	char *c_buf;
	unsigned long flags;

	while (b_iter->seg && count) {
		b_vec = &b_iter->seg->bi_io_vec[b_iter->idx];
		c_off = iter->iter_pos - b_iter->vec_pos;
		c_sz = min(count, b_vec->bv_len - c_off);

		c_buf = bvec_kmap_irq(b_vec, &flags);
		memset(c_buf + c_off, c, c_sz);
		bvec_kunmap_irq(c_buf, &flags);

		count -= c_sz;
		mtdx_data_iter_bio_inc(iter, c_sz);
	}
}

static void mtdx_data_iter_bio_get_sg(struct mtdx_data_iter *iter,
				      struct scatterlist *sg,
				      unsigned int length)
{
	struct mtdx_bio_iter *b_iter = &iter->r_bio;
	struct bio_vec *bvec, *bvprv;
	unsigned int c_off, c_sz;

	sg->length = 0;

	if (!b_iter->seg)
		return;

	bvprv = &b_iter->seg->bi_io_vec[b_iter->idx];
	c_off = iter->iter_pos - b_iter->vec_pos;
	c_sz = min(bvprv->bv_len - c_off, length);

	sg_set_page(sg, bvprv->bv_page, c_sz, bvprv->bv_offset + c_off);

	length -= c_sz;
	
	mtdx_data_iter_bio_inc(iter, c_sz);

	while (b_iter->seg && length) {
		bvec = &b_iter->seg->bi_io_vec[b_iter->idx];

		if (!BIOVEC_PHYS_MERGEABLE(bvprv, bvec))
			return;

		c_sz = min(length, bvec->bv_len);
		sg->length += c_sz;
		length -= c_sz;
		bvprv = bvec;
		mtdx_data_iter_bio_inc(iter, c_sz);
	}
}

static void mtdx_data_iter_bio_get_bvec(struct mtdx_data_iter *iter,
					struct bio_vec *bvec,
					unsigned int length)
{
	struct mtdx_bio_iter *b_iter = &iter->r_bio;
	unsigned int c_off;

	bvec->bv_len = 0;

	if (!b_iter->seg)
		return;

	memcpy(bvec, &b_iter->seg->bi_io_vec[b_iter->idx],
	       sizeof(struct bio_vec));

	c_off = iter->iter_pos - b_iter->vec_pos;
	bvec->bv_offset += c_off;
	bvec->bv_len -= c_off;

	bvec->bv_len = min(bvec->bv_len, length);
	mtdx_data_iter_bio_inc(iter, bvec->bv_len);
}

static struct mtdx_data_iter_ops mtdx_data_iter_bio_ops = {
	.set_iter  = mtdx_data_iter_bio_set,
	.inc_iter  = mtdx_data_iter_bio_inc,
	.dec_iter  = mtdx_data_iter_bio_dec,
	.fill_iter = mtdx_data_iter_bio_fill,
	.get_sg    = mtdx_data_iter_bio_get_sg,
	.get_bvec  = mtdx_data_iter_bio_get_bvec
};

void mtdx_data_iter_init_bio(struct mtdx_data_iter *iter, struct bio *bio)
{
	memset(iter, 0, sizeof(struct mtdx_data_iter));
	iter->ops = &mtdx_data_iter_bio_ops;
	iter->r_bio.head = bio;
	iter->r_bio.seg = bio;
}
EXPORT_SYMBOL(mtdx_data_iter_init_bio);
