/*
 *  xD picture card block device support
 *
 *  Copyright (C) 2008 JMicron Technology Corporation <www.jmicron.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "linux/xd_card.h"
#include <linux/idr.h>

static inline void sg_set_page(struct scatterlist *sg, struct page *page,
                               unsigned int len, unsigned int offset)
{
	sg->page = page;
	sg->offset = offset;
	sg->length = len;
}

static inline struct page *sg_page(struct scatterlist *sg)
{
	return sg->page;
}

static unsigned int rq_byte_size(struct request *rq)
{
	if (blk_fs_request(rq))
		return rq->hard_nr_sectors << 9;

	return rq->data_len;
}

static inline void __end_request(struct request *rq, int uptodate,
                                 unsigned int nr_bytes, int dequeue)
{
	if (!end_that_request_chunk(rq, uptodate, nr_bytes)) {
		if (dequeue)
			blkdev_dequeue_request(rq);
		add_disk_randomness(rq->rq_disk);
		end_that_request_last(rq, uptodate);
        }
}

void end_queued_request(struct request *rq, int uptodate)
{
	__end_request(rq, uptodate, rq_byte_size(rq), 1);
}

#define DRIVER_NAME "xd_card"

static int major;
module_param(major, int, 0644);

static unsigned int cmd_retries = 3;
module_param(cmd_retries, uint, 0644);

static struct workqueue_struct *workqueue;
static DEFINE_IDR(xd_card_disk_idr);
static DEFINE_MUTEX(xd_card_disk_lock);

#define XD_CARD_MAX_SEGS  32

static const unsigned char xd_card_cis_header[] = {
	0x01, 0x03, 0xD9, 0x01, 0xFF, 0x18, 0x02, 0xDF, 0x01, 0x20
};

/*** Helper functions ***/

static void xd_card_addr_to_extra(unsigned short *addr, unsigned int b_addr)
{
	b_addr &= 0x3ff;
	*addr = hweight16(b_addr) & 1;
	*addr <<= 8;
	*addr |= 0x10 | (b_addr >> 7) | ((b_addr << 9) & 0xfe00);
	
}

static int xd_card_extra_to_addr(unsigned int *b_addr, unsigned short addr)
{
	*b_addr = ((addr & 7) << 7) | ((addr & 0xfe00) >> 9);
	return (((addr >> 8) ^ hweight16(*b_addr)) & 1)
	       || !(addr & 0x10);
	
}

/*** Block device ***/

static int xd_card_bd_open(struct inode *inode, struct file *filp)
{
	struct gendisk *disk = inode->i_bdev->bd_disk;
	struct xd_card_media *card;
	int rc = -ENXIO;

	mutex_lock(&xd_card_disk_lock);
 	card = disk->private_data;

	if (card && card->host) {
		card->usage_count++;
		if ((filp->f_mode & FMODE_WRITE) && card->read_only)
			rc = -EROFS;
		else
			rc = 0;
	}

	mutex_unlock(&xd_card_disk_lock);

	return rc;
}


static int xd_card_disk_release(struct gendisk *disk)
{
	struct xd_card_media *card;
	int disk_id = disk->first_minor >> XD_CARD_PART_SHIFT;

	mutex_lock(&xd_card_disk_lock);
 	card = disk->private_data;

	if (card && card->usage_count) {
		card->usage_count--;
		if (!card->usage_count) {
			kfree(card);
			disk->private_data = NULL;
			idr_remove(&xd_card_disk_idr, disk_id);
			put_disk(disk);
		}
	}

	mutex_unlock(&xd_card_disk_lock);

	return 0;
}

static int xd_card_bd_release(struct inode *inode, struct file *filp)
{
	struct gendisk *disk = inode->i_bdev->bd_disk;
	return xd_card_disk_release(disk);
}

static int xd_card_bd_getgeo(struct block_device *bdev,
			     struct hd_geometry *geo)
{
	struct xd_card_media *card;

	mutex_lock(&xd_card_disk_lock);
	card = bdev->bd_disk->private_data;
	if (card) {
		geo->heads = card->heads;
		geo->sectors = card->sectors_per_head;
		geo->cylinders = card->cylinders;
		mutex_unlock(&xd_card_disk_lock);
		return 0;
	}
	mutex_unlock(&xd_card_disk_lock);
	return -ENODEV;
}

static struct block_device_operations xd_card_bdops = {
	.open    = xd_card_bd_open,
	.release = xd_card_bd_release,
	.getgeo  = xd_card_bd_getgeo,
	.owner   = THIS_MODULE
};

/*** Protocol handlers ***/


/**
 * xd_card_next_req - called by host driver to obtain next request to process
 * @host: host to use
 * @req: pointer to stick the request to
 *
 * Host calls this function from idle state (*req == NULL) or after finishing
 * previous request (*req should point to it). If previous request was
 * unsuccessful, it is retried for predetermined number of times. Return value
 * of 0 means that new request was assigned to the host.
 */
int xd_card_next_req(struct xd_card_host *host, struct xd_card_request **req)
{
	int rc = -ENXIO;

	if ((*req) && (*req)->error && host->retries) {
		(*req)->error = rc;
		host->retries--;
		return 0;
	}

	if (host->card && host->card->next_request[0])
		rc = host->card->next_request[0](host->card, req);

	if (!rc)
		host->retries = cmd_retries > 1 ? cmd_retries - 1 : 1;
	else
		*req = NULL;

	return rc;
}
EXPORT_SYMBOL(xd_card_next_req);

/**
 * xd_card_new_req - notify the host that some requests are pending
 * @host: host to use
 */
void xd_card_new_req(struct xd_card_host *host)
{
	host->retries = cmd_retries;
	host->request(host);
}
EXPORT_SYMBOL(xd_card_new_req);

/**
 * xd_card_set_extra - deliver extra data value to host
 * host: host in question
 * e_data: pointer to extra data
 * e_size: number of extra data bytes
 */
void xd_card_set_extra(struct xd_card_host *host, unsigned char *e_data,
		       unsigned int e_size)
{
	if (e_size <= (sizeof(host->extra) - host->extra_pos)) {
		memcpy(((unsigned char *)&host->extra) + host->extra_pos,
		       e_data, e_size);
		host->extra_pos += e_size;
		host->extra_pos %= sizeof(host->extra);
	}
}
EXPORT_SYMBOL(xd_card_set_extra);


/**
 * xd_card_get_extra - get extra data value from host
 * host: host in question
 * e_data: pointer to extra data
 * e_size: number of extra data bytes
 */
void xd_card_get_extra(struct xd_card_host *host, unsigned char *e_data,
		       unsigned int e_size)
{
	if (e_size <= (sizeof(host->extra) - host->extra_pos)) {
		memcpy(e_data,
		       ((unsigned char *)&host->extra) + host->extra_pos,
		       e_size);
		host->extra_pos += e_size;
		host->extra_pos %= sizeof(host->extra);
	}
}
EXPORT_SYMBOL(xd_card_get_extra);

/*
 * Functions prefixed with "h_" are protocol callbacks. They can be called from
 * interrupt context. Return value of 0 means that request processing is still
 * ongoing, while special error value of -EAGAIN means that current request is
 * finished (and request processor should come back some time later).
 */


static int h_xd_card_req_init(struct xd_card_media *card,
			      struct xd_card_request **req)
{
	*req = &card->req;
	card->next_request[0] = card->next_request[1];
	return 0;
}

static int h_xd_card_default(struct xd_card_media *card,
			     struct xd_card_request **req)
{
	complete(&card->req_complete);

	if (!(*req)->error)
		return -EAGAIN;
	else
		return (*req)->error;
}

static void xd_card_rewind(struct xd_card_media *card, unsigned int page_cnt)
{
	unsigned int p_cnt = page_cnt;

	while (p_cnt > card->current_page) {
		p_cnt -= card->current_page + 1;
		if (card->current_seg) {
			card->current_seg--;
			card->current_page = card->req_sg[card->current_seg]
						  .length / card->page_size;
			card->current_page--;
		} else {
			card->current_page = 0;
			goto have_remainder;
		}
	}

	card->current_page -= p_cnt;
	p_cnt = 0;

have_remainder:
	p_cnt = page_cnt - p_cnt;

	while (p_cnt > card->page_pos) {
		p_cnt -= card->page_pos;
		if (card->block_pos) {
			card->block_pos--;
			card->page_pos = card->page_cnt - 1;
		} else if (card->zone_pos) {
			card->block_pos = card->phy_block_cnt - 1;
			card->page_pos = card->page_cnt - 1;
		} else
			return;
	}
	card->page_pos -= p_cnt;
}

static void xd_card_advance(struct xd_card_media *card, unsigned int page_cnt)
{
	card->current_page += page_cnt;
	while (card->current_page >= (card->req_sg[card->current_seg]
					   .length / card->page_size)) {
		card->current_page -= card->req_sg[card->current_seg]
					   .length / card->page_size;
		card->current_seg++;
		if (card->current_seg == card->seg_count)
			break;
	}

	card->page_pos += page_cnt;
	while (card->page_pos >= card->page_cnt) {
		card->page_pos -= card->page_cnt;
		card->block_pos++;

		if (card->block_pos == card->phy_block_cnt) {
			card->block_pos = 0;
			card->zone_pos++;
		}

		if (card->zone_pos == card->zone_cnt)
			break;
	}
}

static int xd_card_check_ecc(struct xd_card_media *card)
{
	unsigned int ref_ecc, act_ecc;
	unsigned int e_pos = 0, c_pos, off = 0, e_state, p_off, p_cnt, s_len;
	struct scatterlist *c_sg;
	struct page *pg;
	unsigned char *buf;
	unsigned char c_mask;
	int do_other = 0;
	unsigned long flags;

	ref_ecc = card->host->extra.ecc_lo[0]
		  | (card->host->extra.ecc_lo[1] << 8)
		  | (card->host->extra.ecc_lo[2] << 16);
	c_sg = &card->req_sg[card->current_seg];
	off = card->current_page * card->page_size;
	s_len = c_sg->length - off;

	/* We are checking at most two pages of data, which may be spread
	 * over 2 segments.
	 */
	while (do_other < 2) {
		e_pos = 0;
		e_state = 0;

		while (s_len) {
			pg = nth_page(sg_page(c_sg),
				      (c_sg->offset + off + e_pos)
				       >> PAGE_SHIFT);
			p_off = offset_in_page(c_sg->offset + off + e_pos);
			p_cnt = PAGE_SIZE - p_off;
			p_cnt = min(p_cnt, s_len);

			local_irq_save(flags);
			buf = kmap_atomic(pg, KM_BIO_SRC_IRQ) + p_off;

			if (xd_card_ecc_step(&e_state, &e_pos, buf, p_cnt)) {
				kunmap_atomic(buf - p_off, KM_BIO_SRC_IRQ);
				local_irq_restore(flags);
				s_len = c_sg->length - off - e_pos;
				break;
			}
			kunmap_atomic(buf - p_off, KM_BIO_SRC_IRQ);
			local_irq_restore(flags);
			s_len = c_sg->length - off - e_pos;
		}

		act_ecc = xd_card_ecc_value(e_state);
		switch (xd_card_fix_ecc(&c_pos, &c_mask, act_ecc, ref_ecc)) {
		case -1:
			return -EILSEQ;
		case 0:
			break;
		case 1:
			pg = nth_page(sg_page(c_sg),
				      (c_sg->offset + off + c_pos)
				       >> PAGE_SHIFT);
			p_off = offset_in_page(c_sg->offset + off + c_pos);
			local_irq_save(flags);
			buf = kmap_atomic(pg, KM_BIO_SRC_IRQ) + p_off;
			*buf ^= c_mask;
			kunmap_atomic(buf - p_off, KM_BIO_SRC_IRQ);
			local_irq_restore(flags);
			break;
		}

		if (s_len >= card->page_size)
			off += e_pos;
		else {
			off = 0;
			c_sg = &card->req_sg[card->current_seg + 1];
			s_len = c_sg->length;
		}

		do_other++;
		ref_ecc = card->host->extra.ecc_hi[0]
			  | (card->host->extra.ecc_hi[1] << 8)
			  | (card->host->extra.ecc_hi[2] << 16);
	}
	return 0;
}

static int h_xd_card_read(struct xd_card_media *card,
			  struct xd_card_request **req)
{
	unsigned int p_cnt = (*req)->count / card->page_size;

	if (p_cnt) {
		if (card->auto_ecc)
			xd_card_advance(card, p_cnt);
		else {
			/* this assumes p_cnt to be equal 1 */
			if (!card->host->extra_pos) {
				xd_card_rewind(card, card->page_inc - 1);
				(*req)->error = xd_card_check_ecc(card);
				if (!(*req)->error)
					xd_card_advance(card, card->page_inc);
			} else
				xd_card_advance(card, 1);
		}
	}

	if ((*req)->error) {
		complete(&card->req_complete);
		return (*req)->error;
	}

	if (card->current_seg == card->seg_count) {
		complete(&card->req_complete);
		return -EAGAIN;
	}

	if (card->zone_pos == card->zone_cnt) {
		complete(&card->req_complete);
		return -EINVAL;
	}

	(*req)->addr = card->zone_pos;
	(*req)->addr <<= card->block_addr_bits;
	(*req)->addr |= card->block_pos;
	(*req)->addr <<= card->page_addr_bits;
	(*req)->addr |= card->page_pos;
	(*req)->addr <<= 8;
	(*req)->count = 0;
	(*req)->error = 0;

	(*req)->sg = card->req_sg[card->current_seg];
	(*req)->sg.offset += card->current_page * card->page_size;
	(*req)->sg.length -= card->current_page * card->page_size;

	if (card->auto_ecc) {
		p_cnt = (*req)->sg.length / card->page_size;
		if (p_cnt > (card->page_cnt - card->page_pos))
			p_cnt -= card->page_cnt - card->page_pos;
	} else
		p_cnt = 1;

	(*req)->sg.length = p_cnt * card->page_size;

	return 0;
}

static int xd_card_get_status(struct xd_card_media *card,
			      unsigned char cmd, unsigned char *status)
{
	card->req.cmd = cmd;
	card->req.flags = XD_CARD_REQ_STATUS;
	card->req.error = 0;

	card->next_request[0] = h_xd_card_req_init;
	card->next_request[1] = h_xd_card_default;
	xd_card_new_req(card->host);
	wait_for_completion(&card->req_complete);
	*status = card->req.status;
	return card->req.error;
}

static int xd_card_get_id(struct xd_card_media *card, unsigned char cmd,
			  void *data, unsigned int count)
{
	card->req.cmd = cmd;
	card->req.flags = XD_CARD_REQ_ID;
	card->req.count = count;
	card->req.id = data;
	card->req.error = 0;
	card->next_request[0] = h_xd_card_req_init;
	card->next_request[1] = h_xd_card_default;
	xd_card_new_req(card->host);
	wait_for_completion(&card->req_complete);
	return card->req.error;
}


static int xd_card_bad_data(struct xd_card_media *card)
{
	if (card->host->extra.data_status != 0xff) {
		if (hweight8(card->host->extra.block_status) < 5)
			return -1;
	}

	return 0;
}

static int xd_card_bad_block(struct xd_card_media *card)
{
	if (card->host->extra.block_status != 0xff) {
		if (hweight8(card->host->extra.block_status) < 7)
			return -1;
	}

	return 0;
}

static int xd_card_find_cis(struct xd_card_host *host,
			    struct xd_card_media *card)
{
	unsigned int r_size = sizeof(card->cis) + sizeof(card->idi);
	unsigned int p_cnt, b_cnt;
	unsigned char *buf;
	unsigned int last_block = card->phy_block_cnt - card->log_block_cnt - 1;
	int rc = 0, good_page = 0;;

	p_cnt = (2 * r_size) / card->page_size;
	if ((p_cnt * card->page_size) < r_size)
		p_cnt++;
	if (p_cnt % card->page_inc)
		p_cnt++;

	buf = kmalloc(p_cnt * card->page_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	sg_set_buf(&card->req_sg[0], buf, p_cnt * card->page_size);
	card->seg_count = 1;
	card->zone_pos = 0;

	for (b_cnt = 0; b_cnt < last_block; ++b_cnt) {
		card->req.cmd = XD_CARD_CMD_READ1;
		card->req.flags = XD_CARD_REQ_DATA | XD_CARD_REQ_EXTRA
				  | XD_CARD_REQ_NO_ECC;
		sg_set_buf(&card->req.sg, buf,
			   card->page_inc * card->page_size);
		card->page_pos = 0;

		while (!good_page) {
			card->req.error = 0;
			card->req.count = 0;
			card->next_request[0] = h_xd_card_req_init;
			card->next_request[1] = h_xd_card_read;
			card->current_seg = 0;
			card->current_page = 0;
			card->block_pos = b_cnt;
			host->extra_pos = 0;

			xd_card_new_req(host);
			wait_for_completion(&card->req_complete);
			rc = card->req.error;

			if (!rc) {
				if (xd_card_bad_block(card))
					goto next_block;
				else if (xd_card_bad_data(card)) {
					if (!card->page_pos)
						goto next_block;
				} else
					good_page = 1;
			} else
				goto out;
		}

		card->req.sg.offset += card->req.count;
		card->req.sg.length -= card->req.count;

		if (card->req.sg.length / card->page_size) {
			card->req.error = 0;
			card->req.count = 0;
			card->next_request[0] = h_xd_card_req_init;
			card->next_request[1] = h_xd_card_read;

			xd_card_new_req(host);
			wait_for_completion(&card->req_complete);
			rc = card->req.error;
		}

		if (rc)
			break;

		if (memcmp(buf, buf + r_size, r_size)) {
			rc = -EMEDIUMTYPE;
			break;
		}

		memcpy(card->cis, buf, sizeof(card->cis));
		memcpy(&card->idi, buf + sizeof(card->cis), sizeof(card->idi));
		break;

next_block:
		continue;
	}

out:
	kfree(buf);
	return rc;
}

/*** Data transfer ***/

static void xd_card_process_request(struct xd_card_media *card,
				    struct request *req)
{
}

static int xd_card_has_request(struct xd_card_media *card)
{
	int rc = 0;
	unsigned long flags;

	spin_lock_irqsave(&card->q_lock, flags);
	if (kthread_should_stop() || card->has_request)
		rc = 1;
	spin_unlock_irqrestore(&card->q_lock, flags);
	return rc;
}

static int xd_card_queue_thread(void *data)
{
	struct xd_card_media *card = data;
	struct xd_card_host *host = card->host;
	struct request *req;
	unsigned long flags;

	while (1) {
		wait_event(card->q_wait, xd_card_has_request(card));
		dev_dbg(host->dev, "thread iter\n");

		spin_lock_irqsave(&card->q_lock, flags);
		req = elv_next_request(card->queue);
		dev_dbg(host->dev, "next req %p\n", req);
		if (!req) {
			card->has_request = 0;
			if (kthread_should_stop()) {
				spin_unlock_irqrestore(&card->q_lock, flags);
				break;
			}
		} else
			card->has_request = 1;
		spin_unlock_irqrestore(&card->q_lock, flags);

		if (req) {
			mutex_lock(&host->lock);
			xd_card_process_request(card, req);
			mutex_unlock(&host->lock);
		}
	}
	dev_dbg(host->dev, "thread finished\n");
	return 0;
}

static void xd_card_request(struct request_queue *q)
{
	struct xd_card_media *card = q->queuedata;
	struct request *req = NULL;

	if (card->q_thread) {
		card->has_request = 1;
		wake_up_all(&card->q_wait);
	} else {
		while ((req = elv_next_request(q)) != NULL)
			end_queued_request(req, -ENODEV);
	}
}


/*** Initialization ***/

static int xd_card_compare_media(struct xd_card_media *card1,
				 struct xd_card_media *card2)
{
	return memcmp(&card1->idi, &card2->idi, sizeof(card1->idi));
}

static int xd_card_set_disk_size(struct xd_card_media *card)
{
	card->page_size = 512;
	card->extra_size = 16;
	card->zone_cnt = 1;
	card->phy_block_cnt = 1024;
	card->log_block_cnt = 1000;
	card->page_cnt = 32;

	if (card->id1.option_code2 != 0xc0)
		card->sm_media = 1;

	switch (card->id1.device_code) {
	case 0x6e:
	case 0xe8:
	case 0xec:
		card->extra_size = 8;
		card->page_size = 256;
		card->capacity = 2000;
		card->cylinders = 125;
		card->heads = 4;
		card->sectors_per_head = 4;
		card->phy_block_cnt = 256;
		card->log_block_cnt = 250;
		card->page_cnt = 16;
		break;
	case 0x5d:
	case 0xea:
	case 0x64:
		if (card->id1.device_code != 0x5d) {
			card->extra_size = 8;
			card->page_size = 256;
			card->phy_block_cnt = 512;
			card->log_block_cnt = 500;
		} else {
			card->mask_rom = 1;
			card->phy_block_cnt = 256;
			card->log_block_cnt = 250;
		}
		card->capacity = 4000;
		card->cylinders = 125;
		card->heads = 4;
		card->sectors_per_head = 8;
		card->page_cnt = 16;
		break;
	case 0xd5:
		if (!card->sm_media)
			card->mask_rom = 1; /* deliberate fall-through */
		else {
			card->capacity = 4095630;
			card->cylinders = 985;
			card->heads = 66;
			card->sectors_per_head = 63;
			card->zone_cnt = 128;
			break;
		}
	case 0xe3:
	case 0xe5:
	case 0x6b:
		card->capacity = 8000;
		card->cylinders = 250;
		card->heads = 4;
		card->sectors_per_head = 8;
		card->phy_block_cnt = 512;
		card->log_block_cnt = 500;
		card->page_cnt = 16;
		break;
	case 0xd6:
		if (card->sm_media)
			card->mask_rom = 1; /* deliberate fall-through */
		else
			return -EMEDIUMTYPE;
	case 0xe6:
		card->capacity = 16000;
		card->cylinders = 250;
		card->heads = 4;
		card->sectors_per_head = 16;
		card->page_cnt = 16;
		break;
	case 0x57:
		card->mask_rom = 1;
	case 0x73:
		card->capacity = 32000;
		card->cylinders = 500;
		card->heads = 4;
		card->sectors_per_head = 16;
		break;
	case 0x58:
		card->mask_rom = 1;
	case 0x75:
		card->capacity = 64000;
		card->cylinders = 500;
		card->heads = 8;
		card->sectors_per_head = 16;
		card->zone_cnt = 2;
		break;
	case 0xd9:
		if (card->sm_media)
			card->mask_rom = 1; /* deliberate fall-through */
		else
			return -EMEDIUMTYPE;
	case 0x76:
		card->capacity = 128000;
		card->cylinders = 500;
		card->heads = 8;
		card->sectors_per_head = 32;
		card->zone_cnt = 4;
		break;
	case 0xda:
		card->mask_rom = 1;
	case 0x79:
		card->capacity = 256000;
		card->cylinders = 500;
		card->heads = 16;
		card->sectors_per_head = 32;
		card->zone_cnt = 8;
		break;
	case 0x5b:
		card->mask_rom = 1;
	case 0x71:
		card->capacity = 512000;
		card->cylinders = 1000;
		card->heads = 16;
		card->sectors_per_head = 32;
		card->zone_cnt = 16;
		break;
	case 0xdc:
		card->capacity = 1023120;
		card->cylinders = 1015;
		card->heads = 16;
		card->sectors_per_head = 63;
		card->zone_cnt = 32;
		break;
	case 0xd3:
		card->capacity = 2047815;
		card->cylinders = 985;
		card->heads = 33;
		card->sectors_per_head = 63;
		card->zone_cnt = 64;
		break;
	default:
		return -EMEDIUMTYPE;
	};
	return 0;
}

static int xd_card_init_disk(struct xd_card_media *card)
{
	struct xd_card_host *host = card->host;
	int rc, disk_id;
	u64 limit = BLK_BOUNCE_HIGH;
	unsigned int max_sectors;

	if (host->dev->dma_mask && *(host->dev->dma_mask))
		limit = *(host->dev->dma_mask);

	rc = xd_card_set_disk_size(card);
	if (rc)
		return rc;

	if (card->mask_rom)
		card->read_only = 1;

	if (!idr_pre_get(&xd_card_disk_idr, GFP_KERNEL))
		return -ENOMEM;

	mutex_lock(&xd_card_disk_lock);
	rc = idr_get_new(&xd_card_disk_idr, card, &disk_id);
	mutex_unlock(&xd_card_disk_lock);

	if (rc)
		return rc;

	if ((disk_id << XD_CARD_PART_SHIFT) > 255) {
		rc = -ENOSPC;
		goto out_release_id;
	}

	card->disk = alloc_disk(1 << XD_CARD_PART_SHIFT);
	if (!card->disk) {
		rc = -ENOMEM;
		goto out_release_id;
	}

	card->queue = blk_init_queue(xd_card_request, &card->q_lock);
	if (!card->queue) {
		rc = -ENOMEM;
		goto out_put_disk;
	}

	card->queue->queuedata = card;

	max_sectors = card->log_block_cnt * card->page_size * card->page_cnt;
	max_sectors >>= 9;

	blk_queue_bounce_limit(card->queue, limit);
	blk_queue_max_sectors(card->queue, max_sectors);
	blk_queue_max_phys_segments(card->queue, XD_CARD_MAX_SEGS);
	blk_queue_max_hw_segments(card->queue, XD_CARD_MAX_SEGS);
	blk_queue_max_segment_size(card->queue, max_sectors << 9);

	card->disk->major = major;
	card->disk->first_minor = disk_id << XD_CARD_PART_SHIFT;
	card->disk->fops = &xd_card_bdops;
	card->usage_count = 1;
	card->disk->private_data = card;
	card->disk->queue = card->queue;
	card->disk->driverfs_dev = host->dev;

	sprintf(card->disk->disk_name, "xd_card%d", disk_id);

	blk_queue_hardsect_size(card->queue, 512);

	set_capacity(card->disk, card->capacity);
	dev_dbg(host->dev, "capacity set %d\n", card->capacity);

	mutex_unlock(&host->lock);
	add_disk(card->disk);
	mutex_lock(&host->lock);
	return 0;

out_put_disk:
	put_disk(card->disk);
out_release_id:
	mutex_lock(&xd_card_disk_lock);
	idr_remove(&xd_card_disk_idr, disk_id);
	mutex_unlock(&xd_card_disk_lock);
	return rc;
}

static void xd_card_set_media_param(struct xd_card_host *host,
				    struct xd_card_media *card)
{
	host->set_param(host, XD_CARD_PAGE_SIZE, card->page_size);
	host->set_param(host, XD_CARD_EXTRA_SIZE, card->extra_size);
	if (card->capacity < 32768)
		host->set_param(host, XD_CARD_ADDR_SIZE, 3);
	else if (card->capacity < 8388608)
		host->set_param(host, XD_CARD_ADDR_SIZE, 4);
}

static int xd_card_init_media(struct xd_card_media *card)
{
	xd_card_set_media_param(card->host, card);

	return -ENODEV;
}

struct xd_card_media *xd_card_alloc_media(struct xd_card_host *host)
{
	struct xd_card_media *card = kzalloc(sizeof(struct xd_card_media),
					     GFP_KERNEL);
	int rc = -ENOMEM;
	unsigned char status;

	if (!card)
		goto out;

	card->host = host;
	card->usage_count = 1;
	spin_lock_init(&card->q_lock);
	init_completion(&card->req_complete);

	rc = xd_card_get_status(card, XD_CARD_CMD_RESET, &status);
	if (rc)
		goto out;

	rc = xd_card_get_status(card, XD_CARD_CMD_STATUS1, &status);
	if (rc)
		goto out;

	if (!(status & XD_CARD_STTS_RW))
		card->read_only = 1;

	rc = xd_card_get_id(card, XD_CARD_CMD_ID1, &card->id1,
			    sizeof(card->id1));
	if (rc)
		goto out;

	rc = xd_card_set_disk_size(card);
	if (rc)
		goto out;

	card->page_addr_bits = fls(card->page_cnt);
	card->block_addr_bits = fls(card->phy_block_cnt);

	if (host->caps & XD_CARD_CAP_AUTO_ECC)
		card->auto_ecc = 1;

	if (host->caps & XD_CARD_FIXED_EXTRA) {
		if (card->extra_size != sizeof(struct xd_card_extra))
			card->auto_ecc = 0;
	}

	if (!card->sm_media) {
		rc = xd_card_get_id(card, XD_CARD_CMD_ID2, &card->id2,
				    sizeof(card->id2));
		if (rc)
			goto out;
	}

	if (!card->sm_media) {
		/* This appears to be totally optional */
		xd_card_get_id(card, XD_CARD_CMD_ID3, &card->id3,
			       sizeof(card->id3));
	}

	xd_card_set_media_param(host, card);
	card->page_inc = sizeof(struct xd_card_extra) / card->extra_size;
	if (!card->page_inc)
		card->page_inc = 1;
	rc = xd_card_find_cis(host, card);
	if (rc)
		goto out;

	if (memcmp(xd_card_cis_header, card->cis, sizeof(xd_card_cis_header)))
		rc = -EMEDIUMTYPE;

out:
	if (host->card)
		xd_card_set_media_param(host, host->card);
	if (rc) {
		kfree(card);
		return ERR_PTR(rc);
	} else
		return card;
}

static void xd_card_remove_media(struct xd_card_media *card)
{
	struct xd_card_host *host = card->host;
	struct task_struct *q_thread = NULL;
	struct gendisk *disk = card->disk;
	unsigned long flags;

	del_gendisk(card->disk);
	dev_dbg(host->dev, "xd card remove\n");
	spin_lock_irqsave(&card->q_lock, flags);
	q_thread = card->q_thread;
	card->q_thread = NULL;
	spin_unlock_irqrestore(&card->q_lock, flags);

	if (q_thread) {
		mutex_unlock(&host->lock);
		kthread_stop(q_thread);
		mutex_lock(&host->lock);
	}

	dev_dbg(host->dev, "queue thread stopped\n");

	blk_cleanup_queue(card->queue);
	card->queue = NULL;

	xd_card_disk_release(disk);
}

static void xd_card_check(struct work_struct *work)
{
	struct xd_card_host *host = container_of(work, struct xd_card_host,
						 media_checker);
	struct xd_card_media *card;

	dev_dbg(host->dev, "xd_card_check started\n");
	mutex_lock(&host->lock);

	if (!host->card)
		host->set_param(host, XD_CARD_POWER, XD_CARD_POWER_ON);

	card = xd_card_alloc_media(host);

	if (IS_ERR(card)) {
		dev_dbg(host->dev, "error %ld allocating card\n",
			PTR_ERR(card));
		if (host->card) {
			xd_card_remove_media(host->card);
			host->card = NULL;
		}
	} else {
		dev_dbg(host->dev, "new card %02x, %02x, %02x\n",
			card->id1.maker_code, card->id1.device_code,
			card->id3.id_code);
		if (host->card) {
			if (host->card->bad_media
			    || xd_card_compare_media(card, host->card)) {
				xd_card_remove_media(host->card);
				host->card = NULL;
			}
		}

		if (!host->card) {
			host->card = card;
			if (xd_card_init_media(card)) {
				kfree(host->card);
				host->card = NULL;
			}
		} else
			kfree(card);
	}

	if (!host->card)
		host->set_param(host, XD_CARD_POWER, XD_CARD_POWER_OFF);

	mutex_unlock(&host->lock);
	dev_dbg(host->dev, "xd_card_check finished\n");
}

/**
 * xd_card_detect_change - schedule media detection on memstick host
 * @host: host to use
 */
void xd_card_detect_change(struct xd_card_host *host)
{
	queue_work(workqueue, &host->media_checker);
}
EXPORT_SYMBOL(xd_card_detect_change);

/**
 * xd_card_suspend_host - notify bus driver of host suspension
 * @host - host to use
 */
void xd_card_suspend_host(struct xd_card_host *host)
{
	mutex_lock(&host->lock);
	host->set_param(host, XD_CARD_POWER, XD_CARD_POWER_OFF);
	mutex_unlock(&host->lock);
}
EXPORT_SYMBOL(xd_card_suspend_host);

/**
 * xd_card_resume_host - notify bus driver of host resumption
 * @host - host to use
 */
void xd_card_resume_host(struct xd_card_host *host)
{
	mutex_lock(&host->lock);
	if (host->card)
		host->set_param(host, XD_CARD_POWER, XD_CARD_POWER_ON);
	mutex_unlock(&host->lock);
	xd_card_detect_change(host);
}
EXPORT_SYMBOL(xd_card_resume_host);


/**
 * xd_card_alloc_host - allocate an xd_card_host structure
 * @extra: size of the user private data to allocate
 * @dev: parent device of the host
 */
struct xd_card_host *xd_card_alloc_host(unsigned int extra, struct device *dev)
{
	struct xd_card_host *host = kzalloc(sizeof(struct xd_card_host) + extra,
					    GFP_KERNEL);
	if (!host)
		return NULL;

	host->dev = dev;
	mutex_init(&host->lock);
	INIT_WORK(&host->media_checker, xd_card_check);
	return host;
}
EXPORT_SYMBOL(xd_card_alloc_host);

/**
 * xd_card_free_host - stop request processing and deallocate xd card host
 * @host: host to use
 */
void xd_card_free_host(struct xd_card_host *host)
{
	flush_workqueue(workqueue);
	mutex_lock(&host->lock);
	if (host->card)
		xd_card_remove_media(host->card);
	host->card = NULL;
	mutex_unlock(&host->lock);

	mutex_destroy(&host->lock);
	kfree(host);
}
EXPORT_SYMBOL(xd_card_free_host);

static int __init xd_card_init(void)
{
	int rc = -ENOMEM;

	workqueue = create_freezeable_workqueue("kxd_card");
	if (!workqueue)
		return -ENOMEM;

	rc = register_blkdev(major, DRIVER_NAME);
	if (rc < 0) {
		printk(KERN_ERR DRIVER_NAME ": failed to register "
		       "major %d, error %d\n", major, rc);
		destroy_workqueue(workqueue);
		return rc;
	}

	if (!major)
		major = rc;

	return 0;
}

static void __exit xd_card_exit(void)
{
	unregister_blkdev(major, DRIVER_NAME);
	destroy_workqueue(workqueue);
	idr_destroy(&xd_card_disk_idr);
}

module_init(xd_card_init);
module_exit(xd_card_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("xD picture card block device driver");
