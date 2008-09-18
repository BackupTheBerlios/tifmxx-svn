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

#include "mtdx_data.h"

void bio_set_fwd_iter(struct bio *b_head, struct bio_byte_iter *iter,
		      unsigned int pos)
{
	iter->iter_pos = pos;
	iter->seg_pos = 0;
	iter->vec_pos = 0;

	for (iter->seg = b_head; iter->seg; iter->seg = iter->seg->bi_next) {
		for (iter->idx = 0; iter->idx < iter->seg->bi_vcnt;
		     ++iter->idx) {
			if ((pos - iter->vec_pos)
			    < iter->seg->bi_io_vec[iter->idx].bv_len)
				return;

			iter->vec_pos += iter->seg->bi_io_vec[iter->idx].bv_len;
		}
		iter->seg_pos = iter->vec_pos;
	}

	iter->iter_pos = iter->vec_pos;
}

void bio_inc_fwd_iter(struct bio_fwd_iter *iter, unsigned int off)
{
	unsigned int inc;

	for (; iter->seg; iter->seg = iter->seg->bi_next) {
		for (; iter->idx < iter->seg->bi_vcnt;
		     ++iter->idx) {
			inc = iter->seg->bi_io_vec[iter->idx].bv_len;
			if (off >= inc) {
				off -= inc;
				iter->iter_pos += inc;
			} else {
				iter->iter_pos += off;
				return;
			}

			iter->vec_pos += iter->seg->bi_io_vec[iter->idx].bv_len;
		}
		iter->seg_pos = iter->vec_pos;
	}

	iter->iter_pos = iter->vec_pos;
}

void mtdx_data_set_buf(struct mtdx_data *m_data, char *data,
		       unsigned int offset, unsigned int length)
{
	m_data->data = data;
	m_data->b_seg = NULL;
	m_data->bv_idx = NULL;
	m_data->b_off = offset;
	m_data->b_len = length;
	m_data->c_off = offset;
}

static unsigned int bio_full_size(struct bio *d_bio)
{
	unsigned int cnt;

	for (cnt = 0; cnt < bio->
}

void mtdx_data_set_bio(struct mtdx_data *m_data, struct *bio,
		       unsigned int offset, unsigned int length)
{
	struct bio *b_cur;

	m_data->d_bio = d_bio;
	m_data->b_off = offset;
	m_data->b_len = length;
	m_data->c_off = 0;

	m_data->b_seg = d_bio;
	b_cur = d_bio->next;

	do {
		for (vi_cnt = 0; vi_cnt < m_data->b_seg->vi_cnt; ++vi_cnt) {
			bv_len = m_data->b_seg->bi_io_vec[vi_cnt].bv_len;

			if ((m_data->c_off + bv_len) <= m_data->b_off)
				m_data->c_off += bv_len;
			else
				break;
		}

		

	}
	while (b_cur) {
		
	}
	for (m_data->b_seg = m_data->d_bio; m_data->b_seg;
	     m_data->b_seg = m_data->b_seg->bi_next) {
		for (m_data->bv_idx = 0;
		     m_data->bv_idx < m_data->b_seg->bi_vcnt;
		     ++m_data->bv_idx) {
			
		}
	}

}

void mtdx_data_subset(struct mtdx_data *m_data, struct mtdx_data *sub_data,
		      unsigned int offset, unsigned int length);

void mtdx_data_fill(struct mtdx_data *m_data, int c, unsigned int offset,
		    unsigned int length);

