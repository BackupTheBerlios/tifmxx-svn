#ifndef __LINUX_BIO_H
#define __LINUX_BIO_H

struct bio_vec {
	struct page	*bv_page;
	unsigned int	bv_len;
	unsigned int	bv_offset;
};

struct bio;

struct bio {
	struct bio		*bi_next;	/* request queue link */

	unsigned short		bi_vcnt;	/* how many bio_vec's */
	unsigned short		bi_idx;		/* current index into bvl_vec */

	unsigned int		bi_max_vecs;	/* max bvl_vecs we can hold */

	struct bio_vec		*bi_io_vec;	/* the actual vec list */
};

#endif /* __LINUX_BIO_H */
