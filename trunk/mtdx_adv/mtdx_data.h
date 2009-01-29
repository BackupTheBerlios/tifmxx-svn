#ifndef _MTDX_DATA_H
#define _MTDX_DATA_H

#include <linux/bio.h>
#include <linux/scatterlist.h>

struct mtdx_data_iter;

struct mtdx_data_iter_ops {
	void (*set_iter)(struct mtdx_data_iter *iter, unsigned int pos);
	void (*inc_iter)(struct mtdx_data_iter *iter, unsigned int off);
	void (*dec_iter)(struct mtdx_data_iter *iter, unsigned int off);
	void (*fill_iter)(struct mtdx_data_iter *iter, int c,
			  unsigned int count);
	void (*get_sg)(struct mtdx_data_iter *iter,
		       struct scatterlist *sg, unsigned int length);
	void (*get_bvec)(struct mtdx_data_iter *iter,
			 struct bio_vec *bvec, unsigned int length);
};

struct mtdx_buf_iter {
	char         *data;
	unsigned int length;
};

struct mtdx_bio_iter {
	struct bio   *head;    /* start of bio                            */
	struct bio   *seg;     /* current segment                         */
	unsigned int seg_pos;  /* position of current segment             */
	unsigned int vec_pos;  /* position of current bio_vec             */
	unsigned int idx;      /* current bio_vec index                   */
};

struct mtdx_data_iter {
	const struct mtdx_data_iter_ops *ops;
	unsigned int iter_pos; /* absolute position in the referenced bio */
	union {
		struct mtdx_buf_iter r_buf;
		struct mtdx_bio_iter r_bio;
	};
};

void mtdx_data_iter_init_buf(struct mtdx_data_iter *iter, void *data,
			     unsigned int length);
void mtdx_data_iter_init_bio(struct mtdx_data_iter *iter, struct bio *bio);

static inline void mtdx_data_iter_set(struct mtdx_data_iter *iter,
				      unsigned int pos)
{
	iter->ops->set_iter(iter, pos);
}

static inline void mtdx_data_iter_inc(struct mtdx_data_iter *iter,
				      unsigned int off)
{
	iter->ops->inc_iter(iter, off);
}

static inline void mtdx_data_iter_dec(struct mtdx_data_iter *iter,
				      unsigned int off)
{
	iter->ops->dec_iter(iter, off);
}

static inline void mtdx_data_iter_fill(struct mtdx_data_iter *iter,
				       int c, unsigned int count)
{
	iter->ops->fill_iter(iter, c, count);
}

/*
 * Get a single scatterlist entry spanning at most 'length' bytes and starting
 * at the specified 'offset'.
 */
static inline void mtdx_data_iter_get_sg(struct mtdx_data_iter *iter,
					 struct scatterlist *sg,
					 unsigned int length)
{
	iter->ops->get_sg(iter, sg, length);
}

/*
 * Same as above, but fills a bio_vec entry. If highmem page is encountered,
 * the entry will not cross page boundaries.
 */
static inline void mtdx_data_iter_get_bvec(struct mtdx_data_iter *iter,
					   struct bio_vec *bvec,
					   unsigned int length)
{
	iter->ops->get_bvec(iter, bvec, length);
}

struct mtdx_oob_iter {
	char         *data;
	unsigned int count;
	unsigned int inc;
	unsigned int pos;
};

static inline void mtdx_oob_iter_init(struct mtdx_oob_iter *m_oob, void *data,
				      unsigned int num_entries,
				      unsigned int inc)
{
	m_oob->data = data;
	m_oob->count = num_entries * inc;
	m_oob->inc = inc;
	m_oob->pos = 0;
}

static inline void mtdx_oob_iter_inc(struct mtdx_oob_iter *m_oob,
				     unsigned int off)
{
	m_oob->pos = min(m_oob->pos + off * m_oob->inc,
			 m_oob->count - m_oob->inc);
}

static inline void mtdx_oob_iter_dec(struct mtdx_oob_iter *m_oob,
				     unsigned int off)
{
	if (off * m_oob->inc <= m_oob->pos)
		m_oob->pos -= off * m_oob->inc;
	else
		m_oob->pos = 0;
}

static inline void *mtdx_oob_iter_get(struct mtdx_oob_iter *m_oob)
{
	return &m_oob->data[m_oob->pos];
}

#endif
