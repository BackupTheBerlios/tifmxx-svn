#ifndef _LINUX_SCATTERLIST_H
#define _LINUX_SCATTERLIST_H

struct scatterlist {
	void *page;
	unsigned int offset;
	unsigned int length;
};

static inline void *sg_virt(struct scatterlist *sg)
{
	return sg->page + sg->offset;
}

static inline void sg_set_buf(struct scatterlist *sg, void *buf,
                              unsigned int buflen)
{
	sg->page = buf;
	sg->offset = 0;
	sg->length = buflen;
}

static inline void sg_set_page(struct scatterlist *sg, void *page,
                               unsigned int len, unsigned int offset)
{
        sg->page = page;
        sg->offset = offset;
        sg->length = len;
}


#endif

