#ifndef _SG_BOUNCE_H
#define _SG_BOUNCE_H

unsigned int bounce_to_sg(struct scatterlist *sg, unsigned int *sg_off,
			  char *buf, unsigned int *buf_off, unsigned int count);
unsigned int fill_sg(struct scatterlist *sg, unsigned int *sg_off,
		     unsigned int val, unsigned int count);
unsigned int bounce_from_sg(char *buf, unsigned int *buf_off,
			    struct scatterlist *sg, unsigned int *sg_off,
			    unsigned int count);

#endif
