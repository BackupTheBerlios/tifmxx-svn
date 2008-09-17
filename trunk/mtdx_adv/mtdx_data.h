#ifndef _MTDX_DATA_H
#define _MTDX_DATA_H

#include <linux/bio.h>
#include <linux/scatterlist.h>

#define mtdx_data_is_buf(m_data) (((m_data)->dptr & 3UL) == 0)
#define mtdx_data_is_bio(m_data) (((m_data)->dptr & 3UL) == 1)

struct mtdx_data {
	void *dptr;
	unsigned int b_off;
	unsigned int b_len;
	unsigned int c_off;
	unsigned int bv_idx;
	struct bio *b_seg;
};

static inline char *mtdx_data_get_buf(struct mtdx_data *m_data)
{
	return mtdx_data_is_buf(m_data) ? m_data->dptr & ~3UL : NULL;
}

static inline struct bio *mtdx_data_get_bio(struct mtdx_data *m_data)
{
	return mtdx_data_is_bio(m_data) ? m_data->dptr & ~3UL : NULL;
}

void mtdx_data_set_buf(struct mtdx_data *m_data, char *buf,
		       unsigned int offset, unsigned int length);
void mtdx_data_set_bio(struct mtdx_data *m_data, struct *bio,
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

/*
 *
 */
#define MTDX_OOB_MODE_ALL 0
#define MTDX_OOB_MODE_ONE 1
#define MTDX_OOB_MODE_TWO 2

struct mtdx_oob {
	char         *dptr;
	unsigned int pos;
	unsigned int inc;
};

static inline void mtdx_oob_rewind(struct mtdx_oob *m_oob)
{
	m_oob->pos = 0;
}

void mtdx_oob_init(struct mtdx_oob *m_oob, char *o_buf, unsigned int mode,
		   unsigned int inc);
char *mtdx_oob_get_entry(struct mtdx_oob *m_oob, unsigned int idx);
char *mtdx_oob_get_next(struct mtdx_oob *m_oob);

#endif
