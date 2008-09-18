#ifndef _MTDX_DATA_H
#define _MTDX_DATA_H

#include <linux/bio.h>
#include <linux/scatterlist.h>

struct bio_fwd_iter {
	unsigned int iter_pos; /* absolute position in the referenced bio */
	unsigned int seg_pos;  /* position of current segment             */
	unsigned int vec_pos;  /* position of current bio_vec             */
	unsigned int idx;      /* current bio_vec index                   */
	struct bio   *seg;     /* current segment                         */
};

void bio_set_fwd_iter(struct bio *b_head, struct bio_fwd_iter *iter,
		      unsigned int pos);
void bio_inc_fwd_iter(struct bio_fwd_iter *iter, unsigned int off);

static inline void bio_copy_fwd_iter(struct bio_fwd_iter *dst,
				     const struct bio_fwd_iter *src)
{
	memcpy(dst, src, sizeof(struct bio_fwd_iter));
}

struct mtdx_data {
	union {
		char       *data;
		struct bio *d_bio;
	};
	struct bio   *b_seg; /* if this is NULL, raw data is assumed */
	unsigned int bv_idx;
	unsigned int b_off;
	unsigned int b_len;
	unsigned int c_off;
};

void mtdx_data_set_buf(struct mtdx_data *m_data, char *data,
		       unsigned int offset, unsigned int length);
void mtdx_data_set_bio(struct mtdx_data *m_data, struct bio *d_bio,
		       unsigned int offset, unsigned int length);

void mtdx_data_subset(struct mtdx_data *m_data, struct mtdx_data *sub_data,
		      unsigned int offset, unsigned int length);

void mtdx_data_fill(struct mtdx_data *m_data, int c, unsigned int offset,
		    unsigned int length);

/*
 * Get a single scatterlist entry spanning at most 'length' bytes and starting
 * at the specified 'offset'.
 */
void mtdx_data_get_sg(struct mtdx_data *m_data, struct scatterlist *sg,
		      unsigned int offset, unsigned int length);

/*
 * Same as above, but fills a bio_vec entry. If highmem page is encountered,
 * the entry will not cross page boundaries.
 */
void mtdx_data_get_bvec(struct mtdx_data *m_data, struct bio_vec *bvec,
			unsigned int offset, unsigned int length);

struct mtdx_oob {
	char         *data;
	unsigned int count;
	unsigned int inc;
	unsigned int pos;
};

static inline void mtdx_oob_init(struct mtdx_oob *m_oob, char *data,
				 unsigned int num_entries, unsigned int inc)
{
	m_oob->data = data;
	m_oob->count = num_entries * inc;
	m_oob->inc = inc;
	m_oob->pos = 0;
}

static inline char *mtdx_oob_get_entry(struct mtdx_oob *m_oob, unsigned int idx)
{
	if ((m_oob->inc * idx) < m_oob->count)
		return &m_oob->data[m_oob->inc * idx];
	else
		return &m_oob->data[m_oob->count - m_oob->inc];
}

static inline char *mtdx_oob_get_next(struct mtdx_oob *m_oob)
{
	char *rv = &m_oob->data[m_oob->pos];

	if ((m_oob->pos + m_oob->inc) < m_oob->count)
		m_oob->pos += m_oob->inc;

	return rv;
}

#endif
