#ifndef __LINUX_BIO_H
#define __LINUX_BIO_H

struct bio_vec {
	void		*bv_page;
	unsigned int	bv_len;
	unsigned int	bv_offset;
};

struct bio;

#define bvec_to_phys(bv)        ((bv)->bv_page + (bv)->bv_offset)

#define BIOVEC_PHYS_MERGEABLE(vec1, vec2)       \
        ((bvec_to_phys((vec1)) + (vec1)->bv_len) == bvec_to_phys((vec2)))

struct bio {
	struct bio		*bi_next;	/* request queue link */

	unsigned short		bi_vcnt;	/* how many bio_vec's */
	unsigned short		bi_idx;		/* current index into bvl_vec */

	unsigned int		bi_max_vecs;	/* max bvl_vecs we can hold */

	struct bio_vec		*bi_io_vec;	/* the actual vec list */
};

static inline char *bvec_kmap_irq(struct bio_vec *bvec, unsigned long *flags)
{
	return bvec->bv_page + bvec->bv_offset;
}

static inline void bvec_kunmap_irq(char *buffer, unsigned long *flags)
{
}

#endif /* __LINUX_BIO_H */
