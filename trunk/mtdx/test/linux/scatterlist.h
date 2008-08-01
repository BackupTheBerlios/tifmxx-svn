#ifndef _LINUX_SCATTERLIST_H
#define _LINUX_SCATTERLIST_H

struct scatterlist {
	const void *page;
	unsigned int offset;
	unsigned int length;
};

static inline void *sg_virt(struct scatterlist *sg)
{
	return sg->page + sg->offset;
}

static inline void sg_set_buf(struct scatterlist *sg, const void *buf,
                              unsigned int buflen)
{
	sg->page = buf;
	sg->offset = 0;
	sg->length = buflen;
}

#endif

