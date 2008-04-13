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
/*
#undef dev_dbg

#define dev_dbg(dev, format, arg...)   \
	dev_printk(KERN_EMERG , dev , format , ## arg)
*/
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

static int h_xd_card_read(struct xd_card_media *card,
			  struct xd_card_request **req);
static int h_xd_card_read_tmp(struct xd_card_media *card,
			      struct xd_card_request **req);
static int h_xd_card_read_copy(struct xd_card_media *card,
			       struct xd_card_request **req);
static int h_xd_card_write(struct xd_card_media *card,
			   struct xd_card_request **req);
static int h_xd_card_write_tmp(struct xd_card_media *card,
			       struct xd_card_request **req);
static int h_xd_card_erase(struct xd_card_media *card,
			   struct xd_card_request **req);
static int h_xd_card_req_init(struct xd_card_media *card,
			      struct xd_card_request **req);
static void xd_card_free_media(struct xd_card_media *card);

#define DRIVER_NAME "xd_card"

static int major;
module_param(major, int, 0644);

static unsigned int cmd_retries = 3;
module_param(cmd_retries, uint, 0644);

static struct workqueue_struct *workqueue;
static DEFINE_IDR(xd_card_disk_idr);
static DEFINE_MUTEX(xd_card_disk_lock);

static const unsigned char xd_card_cis_header[] = {
	0x01, 0x03, 0xD9, 0x01, 0xFF, 0x18, 0x02, 0xDF, 0x01, 0x20
};

/*** Helper functions ***/

static void xd_card_addr_to_extra(struct xd_card_extra *extra,
				  unsigned int addr)
{
	addr &= 0x3ff;
	extra->addr1 |= 0x10 | (addr >> 7) | ((addr << 9) & 0xfe00);
	extra->addr1 |= (hweight16(extra->addr1) & 1) << 8;
	extra->addr2 = extra->addr1;
}

static unsigned int xd_card_extra_to_addr(struct xd_card_extra *extra)
{
	unsigned int addr1, addr2;

	addr1 = ((extra->addr1 & 7) << 7) | ((extra->addr1 & 0xfe00) >> 9);
	addr2 = ((extra->addr2 & 7) << 7) | ((extra->addr2 & 0xfe00) >> 9);

	if (extra->addr1 == 0xffff)
		addr1 = FLASH_BD_INVALID;
	else if ((((~hweight16(addr1)) ^ (extra->addr1 >> 8)) & 1)
		 || !(extra->addr1 & 0x10))
		addr1 = FLASH_BD_INVALID;


	if (extra->addr2 == 0xffff)
		addr2 = FLASH_BD_INVALID;
	else if ((((~hweight16(addr2)) ^ (extra->addr2 >> 8)) & 1)
		 || !(extra->addr2 & 0x10))
		addr2 = FLASH_BD_INVALID;

	if (addr1 == FLASH_BD_INVALID)
		addr1 = addr2;

	return addr1;
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
			xd_card_free_media(card);
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

/*** Information ***/

static ssize_t xd_card_id_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct xd_card_host *host = dev_get_drvdata(dev);
	ssize_t rc = 0;

	mutex_lock(&host->lock);
	if (!host->card)
		goto out;

	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "ID 1:\n");
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\tmaker code: %02x\n",
			host->card->id1.maker_code);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\tdevice code: %02x\n",
			host->card->id1.device_code);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\toption code 1: %02x\n",
			host->card->id1.option_code1);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\toption code 2: %02x\n",
			host->card->id1.option_code2);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\nID 2:\n");
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\tcharacteristics code: "
			"%02x\n", host->card->id2.characteristics_code);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\tvendor code 1: %02x\n",
			host->card->id2.vendor_code1);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\tsize code: %02x\n",
			host->card->id2.size_code);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\tvendor code 2: %02x\n",
			host->card->id2.vendor_code2);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\tvendor code 3: %02x\n",
			host->card->id2.vendor_code3);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\nID 3:\n");
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\tvendor code 1: %02x\n",
			host->card->id3.vendor_code1);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\tvendor code 2: %02x\n",
			host->card->id3.vendor_code2);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\tid code: %02x\n",
			host->card->id3.id_code);
	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "\tvendor code 3: %02x\n",
			host->card->id3.vendor_code3);
out:
	mutex_unlock(&host->lock);
	return rc;
}


static ssize_t xd_card_cis_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xd_card_host *host = dev_get_drvdata(dev);
	struct xd_card_media *card;
	ssize_t rc = 0;
	size_t cnt;

	mutex_lock(&host->lock);
	if (!host->card)
		goto out;
	card = host->card;

	for (cnt = 0; cnt < sizeof(card->cis); ++cnt) {
		if (cnt && !(cnt & 0xf)) {
			if (PAGE_SIZE - rc)
				buf[rc++] = '\n';
		}
		rc += scnprintf(buf + rc, PAGE_SIZE - rc, "%02x ",
				card->cis[cnt]);
	}

out:
	mutex_unlock(&host->lock);
	return rc;
}

static ssize_t xd_card_idi_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xd_card_host *host = dev_get_drvdata(dev);
	struct xd_card_media *card;
	ssize_t rc = 0;
	size_t cnt;

	mutex_lock(&host->lock);
	if (!host->card)
		goto out;
	card = host->card;

	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "vendor code 1: ");
	for (cnt = 0; cnt < sizeof(card->idi.vendor_code1); ++cnt) {
		rc += scnprintf(buf + rc, PAGE_SIZE - rc, "%02x",
				card->idi.vendor_code1[cnt]);
	}
	if (PAGE_SIZE - rc)
		buf[rc++] = '\n';

	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "serial number: ");
	for (cnt = 0; cnt < sizeof(card->idi.serial_number); ++cnt) {
		rc += scnprintf(buf + rc, PAGE_SIZE - rc, "%02x",
				card->idi.serial_number[cnt]);
	}
	if (PAGE_SIZE - rc)
		buf[rc++] = '\n';

	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "model number: ");
	for (cnt = 0; cnt < sizeof(card->idi.model_number); ++cnt) {
		rc += scnprintf(buf + rc, PAGE_SIZE - rc, "%02x",
				card->idi.model_number[cnt]);
	}
	if (PAGE_SIZE - rc)
		buf[rc++] = '\n';

	rc += scnprintf(buf + rc, PAGE_SIZE - rc, "vendor code 2: ");
	for (cnt = 0; cnt < sizeof(card->idi.vendor_code2); ++cnt) {
		rc += scnprintf(buf + rc, PAGE_SIZE - rc, "%02x",
				card->idi.vendor_code1[cnt]);
	}
	if (PAGE_SIZE - rc)
		buf[rc++] = '\n';

out:
	mutex_unlock(&host->lock);
	return rc;
}

static ssize_t xd_card_format_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct xd_card_host *host = dev_get_drvdata(dev);
	ssize_t rc = 0;

	mutex_lock(&host->lock);
	if (!host->card)
		goto out;

	if (host->card->format)
		rc = sprintf(buf, "Block %x of %x, progress %d%%\n",
			     host->card->trans_cnt, host->card->trans_len,
			     (host->card->trans_cnt * 100)
			     / host->card->trans_len);
	else
		rc = sprintf(buf, "Not running\n");

out:
	mutex_unlock(&host->lock);
	return rc;
}

static int xd_card_format_thread(void *data);

static ssize_t xd_card_format_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct xd_card_host *host = dev_get_drvdata(dev);
	struct task_struct *f_thread, *q_thread;
	unsigned long flags;

	mutex_lock(&host->lock);
	if (!host->card)
		goto out;

	if (host->card->format)
		goto out;

	if (count < 6 || strncmp(buf, "format", 6))
		goto out;

	host->card->format = 1;

	f_thread = kthread_create(xd_card_format_thread, host->card,
				  DRIVER_NAME"_fmtd");
	if (IS_ERR(f_thread))
		goto out;

	spin_lock_irqsave(&host->card->q_lock, flags);
	blk_stop_queue(host->card->queue);
	q_thread = host->card->q_thread;
	host->card->q_thread = f_thread;
	spin_unlock_irqrestore(&host->card->q_lock, flags);

	if (q_thread) {
		mutex_unlock(&host->lock);
		kthread_stop(q_thread);
		mutex_lock(&host->lock);
	}

	wake_up_process(f_thread);
out:
	mutex_unlock(&host->lock);
	return count;
}

static DEVICE_ATTR(xd_card_id, S_IRUGO, xd_card_id_show, NULL);
static DEVICE_ATTR(xd_card_cis, S_IRUGO, xd_card_cis_show, NULL);
static DEVICE_ATTR(xd_card_idi, S_IRUGO, xd_card_idi_show, NULL);
static DEVICE_ATTR(xd_card_format, S_IRUGO | S_IWUSR, xd_card_format_show,
		   xd_card_format_store);

static ssize_t xd_card_block_map_read(struct kobject *kobj,
				      struct bin_attribute *attr,
				      char *buf, loff_t offset,
				      size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct xd_card_host *host = dev_get_drvdata(dev);
	ssize_t rc = 0;

	mutex_lock(&host->lock);
	if (host->card)
		rc = flash_bd_read_map(host->card->fbd, buf, offset, count);
	mutex_unlock(&host->lock);
	return rc;
}

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
	if (host->card) {
		host->card->next_request[1] = host->card->next_request[0];
		host->card->next_request[0] = h_xd_card_req_init;
		host->retries = cmd_retries;
		host->request(host);
	}
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


static unsigned long long xd_card_req_address(struct xd_card_media *card,
					      unsigned int byte_off)
{
	unsigned long long rv = card->flash_req.zone;

	rv <<= card->block_addr_bits;
	rv |= card->flash_req.phy_block;
	rv <<= card->page_addr_bits;
	rv |= card->flash_req.page_off * (card->page_size / card->hw_page_size);
	rv <<= 8;

	if (byte_off)
		rv += (byte_off / card->hw_page_size) << 8;

	return rv;
}

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

static int h_xd_card_read_extra(struct xd_card_media *card,
				struct xd_card_request **req)
{
	if ((*req)->error) {
		complete(&card->req_complete);
		return (*req)->error;
	}

	if (!card->host->extra_pos) {
		complete(&card->req_complete);
		return -EAGAIN;
	} else {
		unsigned int extra_pos = sizeof(struct xd_card_extra)
					 / (card->page_size
					    / card->hw_page_size);
		extra_pos = card->host->extra_pos / extra_pos;
		extra_pos *= card->hw_page_size;
		(*req)->count = 0;
		(*req)->error = 0;
		(*req)->addr = xd_card_req_address(card, extra_pos);
		return 0;
	}
}

static void xd_card_advance(struct xd_card_media *card, int count)
{
	unsigned int s_len;

	if (count >= 0) {
		while (count) {
			if (card->seg_pos == card->seg_count)
				break;
			s_len = card->req_sg[card->seg_pos].length
				- card->seg_off;
			s_len = min(count, (int)s_len);
			card->seg_off += s_len;
			count -= s_len;
			if (card->seg_off
			    == card->req_sg[card->seg_pos].length) {
				card->seg_off = 0;
				card->seg_pos++;
			}
		}
	} else {
		count = -count;

		while (count >= card->seg_off) {
			count -= card->seg_off;
			if (card->seg_pos)
				card->seg_pos--;
			else {
				card->seg_off = 0;
				break;
			}
			card->seg_off = card->req_sg[card->seg_pos].length;
		}

		card->seg_off -= count;
		if (card->seg_off == card->req_sg[card->seg_pos].length) {
			card->seg_off = 0;
			card->seg_pos++;
		}
	}
}

enum tmp_action {
	FILL_TMP = 0,
	FLUSH_TMP,
	ONE_FILL
};

static void xd_card_copy_tmp(struct xd_card_media *card, unsigned int off,
			     unsigned int count, enum tmp_action act)
{
	struct page *pg;
	unsigned char *buf;
	unsigned int p_off, p_cnt;
	unsigned long flags;

	while (count) {
		if (card->seg_off == card->req_sg[card->seg_pos].length) {
			card->seg_off = 0;
			card->seg_pos++;
			if (card->seg_pos == card->seg_count)
				break;
		}

		p_off = card->req_sg[card->seg_pos].offset + card->seg_off;

		pg = nth_page(sg_page(&card->req_sg[card->seg_pos]),
			      p_off >> PAGE_SHIFT);
		p_off = offset_in_page(p_off);
		p_cnt = PAGE_SIZE - p_off;
		p_cnt = min(p_cnt, count);

		local_irq_save(flags);
		buf = kmap_atomic(pg, KM_BIO_SRC_IRQ) + p_off;

		switch (act) {
		case FILL_TMP:
			memcpy(card->t_buf + off, buf, p_cnt);
			break;
		case FLUSH_TMP:
			memcpy(buf, card->t_buf + off, p_cnt);
			break;
		case ONE_FILL:
			memset(buf, 0xff, p_cnt);
			break;
		};	

		kunmap_atomic(buf - p_off, KM_BIO_SRC_IRQ);
		local_irq_restore(flags);

		count -= p_cnt;
		off += p_cnt;
		card->seg_off += p_cnt;
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
	c_sg = &card->req_sg[card->seg_pos];
	off = card->seg_off;
	s_len = c_sg->length - off;

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

		off += e_pos;

		do_other++;
		ref_ecc = card->host->extra.ecc_hi[0]
			  | (card->host->extra.ecc_hi[1] << 8)
			  | (card->host->extra.ecc_hi[2] << 16);
	}
	return 0;
}

static void xd_card_update_extra(struct xd_card_media *card)
{
	unsigned int act_ecc;
	unsigned int e_pos = 0, off = 0, e_state, p_off, p_cnt, s_len;
	struct scatterlist *c_sg;
	struct page *pg;
	unsigned char *buf;
	int do_other = 0;
	unsigned long flags;

	c_sg = &card->req_sg[card->seg_pos];
	off = card->seg_off;
	s_len = c_sg->length - off;

	if (s_len < card->page_size)
		return;

	xd_card_addr_to_extra(&card->host->extra, card->flash_req.log_block);

	if (card->auto_ecc || (card->req.flags & XD_CARD_REQ_NO_ECC))
		return;

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

		if (!do_other) {
			card->host->extra.ecc_lo[0] = act_ecc & 0xff;
			card->host->extra.ecc_lo[1] = (act_ecc >> 8) & 0xff;
			card->host->extra.ecc_lo[2] = (act_ecc >> 16) & 0xff;
		} else {
			card->host->extra.ecc_hi[0] = act_ecc & 0xff;
			card->host->extra.ecc_hi[1] = (act_ecc >> 8) & 0xff;
			card->host->extra.ecc_hi[2] = (act_ecc >> 16) & 0xff;
		}
		
		off += e_pos;

		do_other++;
	}
}

static void xd_card_update_extra_tmp(struct xd_card_media *card)
{
	unsigned int act_ecc;
	unsigned int e_pos = 0, e_state = 0, len = card->page_size;
	unsigned char *buf = card->t_buf + card->trans_cnt;

	if ((card->trans_len - card->trans_cnt) < card->page_size)
		return;

	xd_card_addr_to_extra(&card->host->extra, card->flash_req.log_block);

	if (card->auto_ecc || (card->req.flags & XD_CARD_REQ_NO_ECC))
		return;

	xd_card_ecc_step(&e_state, &e_pos, buf, len);
	act_ecc = xd_card_ecc_value(e_state);
	card->host->extra.ecc_lo[0] = act_ecc & 0xff;
	card->host->extra.ecc_lo[1] = (act_ecc >> 8) & 0xff;
	card->host->extra.ecc_lo[2] = (act_ecc >> 16) & 0xff;

	e_state = 0;
	buf += e_pos;
	len -= e_pos;
	e_pos = 0;
	xd_card_ecc_step(&e_state, &e_pos, buf, len);
	act_ecc = xd_card_ecc_value(e_state);
	card->host->extra.ecc_hi[0] = act_ecc & 0xff;
	card->host->extra.ecc_hi[1] = (act_ecc >> 8) & 0xff;
	card->host->extra.ecc_hi[2] = (act_ecc >> 16) & 0xff;
}

static int xd_card_check_ecc_tmp(struct xd_card_media *card)
{
	unsigned int ref_ecc, act_ecc;
	unsigned int e_pos = 0, e_state = 0, c_pos, len = card->page_size;
	unsigned char c_mask;
	unsigned char *buf = card->t_buf + card->trans_cnt - card->page_size;
	int rc;

	if (card->trans_cnt < card->page_size)
		return -EILSEQ;

	ref_ecc = card->host->extra.ecc_lo[0]
		  | (card->host->extra.ecc_lo[1] << 8)
		  | (card->host->extra.ecc_lo[2] << 16);

	xd_card_ecc_step(&e_state, &e_pos, buf, len);

	act_ecc = xd_card_ecc_value(e_state);
	rc = xd_card_fix_ecc(&c_pos, &c_mask, act_ecc, ref_ecc);

	if (rc == -1)
		return -EILSEQ;
	else if (rc == 1)
		buf[c_pos] ^= c_mask;

	ref_ecc = card->host->extra.ecc_hi[0]
		  | (card->host->extra.ecc_hi[1] << 8)
		  | (card->host->extra.ecc_hi[2] << 16);

	e_state = 0;
	buf += e_pos;
	len -= e_pos;
	e_pos = 0;
	xd_card_ecc_step(&e_state, &e_pos, buf, len);

	act_ecc = xd_card_ecc_value(e_state);
	rc = xd_card_fix_ecc(&c_pos, &c_mask, act_ecc, ref_ecc);

	if (rc == -1)
		return -EILSEQ;
	else if (rc == 1)
		buf[c_pos] ^= c_mask;

	return 0;
}

static int xd_card_trans_req(struct xd_card_media *card,
			     struct xd_card_request *req)
{
	unsigned int count = 0;
	int rc = 0;

process_next:
	dev_dbg(card->host->dev,
		"fbd req %s: (%x) %x -> %x, dst: %x + %x, src: %x:%x\n",
		flash_bd_cmd_name(card->flash_req.cmd), card->flash_req.zone,
		card->flash_req.log_block, card->flash_req.phy_block,
		card->flash_req.page_off, card->flash_req.page_cnt,
		card->flash_req.src.phy_block, card->flash_req.src.page_off);

	switch (card->flash_req.cmd) {
	case FBD_NONE:
		return -EAGAIN;
	case FBD_READ:
		req->cmd = XD_CARD_CMD_READ1;
		req->flags = XD_CARD_REQ_DATA;
		req->addr = xd_card_req_address(card, 0);
		req->error = 0;
		req->count = 0;
		sg_set_page(&req->sg, sg_page(&card->req_sg[card->seg_pos]),
			    card->req_sg[card->seg_pos].length
			    - card->seg_off,
			    card->req_sg[card->seg_pos].offset
			    + card->seg_off);

		dev_dbg(card->host->dev, "trans r %d, %d, %p, %x, %x\n",
			card->seg_pos, card->seg_off, req->sg.page,
			req->sg.offset, req->sg.length);

		card->trans_cnt = 0;
		card->trans_len = card->flash_req.page_cnt * card->page_size;

		if (card->auto_ecc) {
			req->sg.length = min(card->trans_len, req->sg.length);
		} else {
			req->flags |= XD_CARD_REQ_EXTRA;
			req->sg.length = card->hw_page_size;
			card->host->extra_pos = 0;
		}

		card->next_request[0] = h_xd_card_read; 
		return 0;
	case FBD_READ_TMP:
		req->cmd = XD_CARD_CMD_READ1;
		req->flags = XD_CARD_REQ_DATA;
		req->addr = xd_card_req_address(card, 0);
		req->error = 0;
		req->count = 0;
		card->trans_cnt = 0;
		card->trans_len = card->flash_req.page_cnt * card->page_size;

		if (card->auto_ecc)
			sg_set_buf(&req->sg, card->t_buf, card->trans_len);
		else {
			req->flags |= XD_CARD_REQ_EXTRA;
			sg_set_buf(&req->sg, card->t_buf, card->hw_page_size);
			card->host->extra_pos = 0;
		}

		card->next_request[0] = h_xd_card_read_tmp;
		return 0;
	case FBD_FLUSH_TMP:
		xd_card_copy_tmp(card, card->flash_req.byte_off,
				 card->flash_req.byte_cnt, FLUSH_TMP);
		card->trans_cnt -= card->flash_req.byte_cnt;
		count = card->flash_req.byte_cnt;
		break;
	case FBD_SKIP:
		xd_card_copy_tmp(card, 0, card->flash_req.byte_cnt, ONE_FILL);
		card->trans_cnt -= card->flash_req.byte_cnt;
		count = card->flash_req.byte_cnt;
		break;
	case FBD_ERASE_TMP:
		memset(card->t_buf + card->flash_req.byte_off, 0xff,
		       card->flash_req.byte_cnt);
		count = card->flash_req.byte_cnt;
		break;
	case FBD_ERASE:
		req->cmd = XD_CARD_CMD_ERASE_SET;
		req->flags = XD_CARD_REQ_DIR | XD_CARD_REQ_STATUS;
		req->addr = card->flash_req.zone;
		req->addr <<= card->block_addr_bits;
		req->addr |= card->flash_req.phy_block;
		req->addr <<= card->page_addr_bits;
		req->error = 0;
		req->count = 0;
		card->trans_cnt = 0;
		card->trans_len = 0;

		card->host->set_param(card->host, XD_CARD_ADDR_SIZE,
				      card->addr_bytes - 1);
		card->next_request[0] = h_xd_card_erase;
		return 0;
	case FBD_COPY:
		req->cmd = XD_CARD_CMD_READ1;
		req->flags = XD_CARD_REQ_DATA;
		req->addr = card->flash_req.zone;
		req->addr <<= card->block_addr_bits;
		req->addr |= card->flash_req.src.phy_block;
		req->addr <<= card->page_addr_bits;
		req->addr |= card->flash_req.src.page_off
			     * (card->page_size / card->hw_page_size);
		req->addr <<= 8;

		req->error = 0;
		req->count = 0;
		card->trans_cnt = 0;
		card->trans_len = card->flash_req.page_cnt * card->page_size;

		if (card->auto_ecc)
			sg_set_buf(&req->sg, card->t_buf, card->trans_len);
		else {
			req->flags |= XD_CARD_REQ_EXTRA;
			sg_set_buf(&req->sg, card->t_buf, card->hw_page_size);
			card->host->extra_pos = 0;
		}

		card->next_request[0] = h_xd_card_read_copy;
		return 0;
	case FBD_WRITE:
		req->cmd = XD_CARD_CMD_INPUT;
		req->flags = XD_CARD_REQ_DATA | XD_CARD_REQ_DIR
			     | XD_CARD_REQ_EXTRA | XD_CARD_REQ_STATUS;
		req->addr = xd_card_req_address(card, 0);
		req->error = 0;
		req->count = 0;
		sg_set_page(&req->sg, sg_page(&card->req_sg[card->seg_pos]),
			    card->hw_page_size,
			    card->req_sg[card->seg_pos].offset
			    + card->seg_off);

		dev_dbg(card->host->dev, "trans w %d, %d, %p, %x, %x\n", card->seg_pos,
			card->seg_off, req->sg.page, req->sg.offset, req->sg.length);

		card->trans_len = card->flash_req.page_cnt * card->page_size;
		card->trans_cnt = 0;

		memset(&card->host->extra, 0xff, sizeof(struct xd_card_extra));
		xd_card_update_extra(card);

		card->next_request[0] = h_xd_card_write;
		return 0;
	case FBD_WRITE_TMP:
		req->cmd = XD_CARD_CMD_INPUT;
		req->flags = XD_CARD_REQ_DATA | XD_CARD_REQ_DIR
			     | XD_CARD_REQ_EXTRA | XD_CARD_REQ_STATUS;
		req->addr = xd_card_req_address(card, 0);
		req->error = 0;
		req->count = 0;
		card->trans_cnt = 0;
		card->trans_len = card->flash_req.page_cnt * card->page_size;

		sg_set_buf(&req->sg, card->t_buf, card->hw_page_size);

		memset(&card->host->extra, 0xff, sizeof(struct xd_card_extra));
		xd_card_update_extra_tmp(card);

		card->next_request[0] = h_xd_card_write_tmp; 
		return 0;
	case FBD_FILL_TMP:
		xd_card_copy_tmp(card, card->flash_req.byte_off,
				 card->flash_req.byte_cnt, FILL_TMP);
		card->trans_cnt -= card->flash_req.byte_cnt;
		count = card->flash_req.byte_cnt;
		break;
	case FBD_MARK:
		memset(card->t_buf, 0xff,
		       card->page_size * card->flash_req.page_cnt);
		req->cmd = XD_CARD_CMD_INPUT;
		req->flags = XD_CARD_REQ_DATA | XD_CARD_REQ_DIR
			     | XD_CARD_REQ_EXTRA | XD_CARD_REQ_NO_ECC
			     | XD_CARD_REQ_STATUS;
		req->addr = xd_card_req_address(card, 0);
		req->error = 0;
		req->count = 0;
		card->trans_cnt = 0;
		card->trans_len = card->flash_req.page_cnt * card->page_size;

		sg_set_buf(&req->sg, card->t_buf, card->hw_page_size);

		memset(&card->host->extra, 0xff, sizeof(struct xd_card_extra));
		xd_card_update_extra_tmp(card);

		card->next_request[0] = h_xd_card_write_tmp; 
		return 0;
	case FBD_MARK_BAD:
		memset(card->t_buf, 0xff,
		       card->page_size * card->flash_req.page_cnt);
		req->cmd = XD_CARD_CMD_INPUT;
		req->flags = XD_CARD_REQ_DATA | XD_CARD_REQ_DIR
			     | XD_CARD_REQ_EXTRA | XD_CARD_REQ_NO_ECC
			     | XD_CARD_REQ_STATUS;
		req->addr = xd_card_req_address(card, 0);
		req->error = 0;
		req->count = 0;
		card->trans_cnt = 0;
		card->trans_len = card->flash_req.page_cnt * card->page_size;

		sg_set_buf(&req->sg, card->t_buf, card->hw_page_size);

		memset(&card->host->extra, 0xff, sizeof(struct xd_card_extra));
		xd_card_update_extra_tmp(card);
		card->host->extra.block_status = 0xf0;

		card->next_request[0] = h_xd_card_write_tmp; 
		return 0;
	default:
		BUG();
	};

	rc = flash_bd_next_req(card->fbd, &card->flash_req, count, rc);
	if (rc)
		return rc;

	goto process_next;
}

static int xd_card_try_next_req(struct xd_card_media *card,
				struct xd_card_request **req)
{
	int rc = (*req)->error;

	rc = flash_bd_next_req(card->fbd, &card->flash_req,
			       card->trans_cnt, rc);

	if (rc) {
		complete(&card->req_complete);
		return rc;
	}

	rc = xd_card_trans_req(card, *req);

	if (rc) {
		complete(&card->req_complete);
		return rc;
	}

	return 0;
}

static int h_xd_card_read(struct xd_card_media *card,
			  struct xd_card_request **req)
{
	unsigned int count;
	int rc = 0;

	dev_dbg(card->host->dev, "read %d (%d) of %d at %d:%d\n",
		(*req)->count, (*req)->error, card->trans_cnt, card->seg_pos,
		card->seg_off);

	if ((*req)->count) {
		if (card->auto_ecc) {
			xd_card_advance(card, (*req)->count);
			card->trans_cnt += (*req)->count;
			count = min(card->trans_len - card->trans_cnt,
				    (*req)->sg.length - card->seg_off);

			if (!count)
				goto out;

			(*req)->cmd = XD_CARD_CMD_READ1;
			(*req)->flags = XD_CARD_REQ_DATA;
			(*req)->addr = xd_card_req_address(card,
							   card->trans_cnt);
			(*req)->error = 0;
			(*req)->count = 0;
			sg_set_page(&(*req)->sg,
				    sg_page(&card->req_sg[card->seg_pos]),
				    count,
				    card->req_sg[card->seg_pos].offset
				    + card->seg_off);
			return 0;
		} else {
			rc = card->hw_page_size;

			if ((*req)->count != rc) {
				rc *= card->host->extra_pos
				      / (sizeof(struct xd_card_extra)
					 / (card->page_size
					    / card->hw_page_size));
				xd_card_advance(card, -rc);
				card->trans_cnt -= rc;
				rc = -EILSEQ;
			} else {
				xd_card_advance(card, rc);
				card->trans_cnt += rc;
				rc = 0;
			}

			if (!rc && !card->host->extra_pos) {
				xd_card_advance(card, -card->page_size);
				rc = xd_card_check_ecc(card);
				if (!rc)
					xd_card_advance(card, card->page_size);
				else
					card->trans_cnt -= card->page_size;
			}

			if (rc || (card->trans_cnt >= card->trans_len))
				goto out;

			(*req)->count = 0;
			(*req)->error = 0;
			(*req)->addr = xd_card_req_address(card,
							   card->trans_cnt);
			sg_set_page(&(*req)->sg,
				    sg_page(&card->req_sg[card->seg_pos]),
				    card->hw_page_size,
				    card->req_sg[card->seg_pos].offset
				    + card->seg_off);
			return 0;
		}
	}

out:
	if (!(*req)->error)
		(*req)->error = rc;

	return xd_card_try_next_req(card, req);
}

static int h_xd_card_read_tmp(struct xd_card_media *card,
			      struct xd_card_request **req)
{
	int rc = 0;

	dev_dbg(card->host->dev, "read tmp %d of %d\n",
		(*req)->count, card->trans_cnt);

	if ((*req)->count) {
		if (card->auto_ecc)
			card->trans_cnt = (*req)->count;
		else {
			rc = card->hw_page_size;

			if ((*req)->count != rc) {
				rc *= card->host->extra_pos
				      / (sizeof(struct xd_card_extra)
					 / (card->page_size
					    / card->hw_page_size));
				card->trans_cnt -= rc;
				rc = -EILSEQ;
			} else {
				card->trans_cnt += rc;
				rc = 0;
			}

			if (!rc && !card->host->extra_pos) {
				rc = xd_card_check_ecc_tmp(card);
				if (rc)
					card->trans_cnt -= card->page_size;
			}

			if (!rc && (card->trans_cnt < card->trans_len)) {
				(*req)->count = 0;
				(*req)->error = 0;
				(*req)->addr
					= xd_card_req_address(card,
							      card->trans_cnt);
				sg_set_buf(&(*req)->sg,
					   card->t_buf + card->trans_cnt,
					   card->hw_page_size);
				return 0;
			}
		}
	}

	if (!(*req)->error)
		(*req)->error = rc;

	return xd_card_try_next_req(card, req);
}

static int h_xd_card_check_stat(struct xd_card_media *card,
				struct xd_card_request **req)
{
	if (!(*req)->error) {
		if ((*req)->status & XD_CARD_STTS_FAIL)
			(*req)->error = -EFAULT;
	}
	return xd_card_try_next_req(card, req);
}

static int h_xd_card_write_tmp_adv(struct xd_card_media *card,
				   struct xd_card_request **req)
{
	if (!(*req)->error && ((*req)->status & XD_CARD_STTS_FAIL))
		(*req)->error = -EFAULT;

	if (!(*req)->error) {
		card->trans_cnt += card->hw_page_size;

		if (card->trans_cnt < card->trans_len) {
			if (!card->host->extra_pos)
				xd_card_update_extra_tmp(card);

			(*req)->cmd = XD_CARD_CMD_INPUT;
			(*req)->flags = XD_CARD_REQ_DATA
					| XD_CARD_REQ_DIR
					| XD_CARD_REQ_EXTRA
					| XD_CARD_REQ_STATUS;
			(*req)->addr = xd_card_req_address(card,
							   card->trans_cnt);
			(*req)->error = 0;
			(*req)->count = card->hw_page_size;
			sg_set_buf(&(*req)->sg, card->t_buf + card->trans_cnt,
				   (*req)->count);
			card->next_request[0] = h_xd_card_write_tmp;
			return 0;
		}
	}

	return xd_card_try_next_req(card, req);
}

static int h_xd_card_write_adv(struct xd_card_media *card,
			       struct xd_card_request **req)
{
	if (!(*req)->error && ((*req)->status & XD_CARD_STTS_FAIL))
		(*req)->error = -EFAULT;

	if (!(*req)->error) {
		xd_card_advance(card, card->hw_page_size);
		card->trans_cnt += card->hw_page_size;

		if (card->trans_cnt < card->trans_len) {
			if (!card->host->extra_pos)
				xd_card_update_extra(card);

			(*req)->cmd = XD_CARD_CMD_INPUT;
			(*req)->flags = XD_CARD_REQ_DATA
					| XD_CARD_REQ_DIR
					| XD_CARD_REQ_EXTRA
					| XD_CARD_REQ_STATUS;
			(*req)->addr = xd_card_req_address(card,
							   card->trans_cnt);
			(*req)->error = 0;
			(*req)->count = card->hw_page_size;
			sg_set_page(&(*req)->sg,
				    sg_page(&card->req_sg[card->seg_pos]),
				    (*req)->count,
				    card->req_sg[card->seg_pos].offset
				    + card->seg_off);
			card->next_request[0] = h_xd_card_write;
			return 0;
		}
	}

	return xd_card_try_next_req(card, req);
}

static int h_xd_card_write_stat(struct xd_card_media *card,
				struct xd_card_request **req)
{
	if (!(*req)->error) {
		(*req)->cmd = XD_CARD_CMD_STATUS1;
		(*req)->flags = XD_CARD_REQ_STATUS;
		(*req)->error = 0;
		(*req)->count = 0;
		card->trans_cnt = 0;
		card->trans_len = 0;
		card->next_request[0] = card->next_request[1];
		return 0;
	} else
		return xd_card_try_next_req(card, req);
}

static int h_xd_card_write(struct xd_card_media *card,
			   struct xd_card_request **req)
{
	if (!(*req)->error) {
		if ((*req)->count != card->hw_page_size)
			(*req)->error = -EIO;
	}

	if ((*req)->error)
		return xd_card_try_next_req(card, req);

	if (card->host->caps & XD_CARD_CAP_CMD_SHORTCUT) {
		return h_xd_card_write_adv(card, req);
	} else {
		(*req)->cmd = XD_CARD_CMD_PAGE_PROG;
		(*req)->flags = XD_CARD_REQ_DIR;
		(*req)->error = 0;
		(*req)->count = 0;

		card->next_request[0] = h_xd_card_write_stat;
		card->next_request[1] = h_xd_card_write_adv;
		return 0;
	}
}

static int h_xd_card_write_tmp(struct xd_card_media *card,
			       struct xd_card_request **req)
{
	if (!(*req)->error) {
		if ((*req)->count != card->hw_page_size)
			(*req)->error = -EIO;
	}

	if ((*req)->error)
		return xd_card_try_next_req(card, req);

	if (card->host->caps & XD_CARD_CAP_CMD_SHORTCUT) {
		return h_xd_card_write_tmp_adv(card, req);
	} else {
		(*req)->cmd = XD_CARD_CMD_PAGE_PROG;
		(*req)->flags = XD_CARD_REQ_DIR;
		(*req)->error = 0;
		(*req)->count = 0;

		card->next_request[0] = h_xd_card_write_stat;
		card->next_request[1] = h_xd_card_write_tmp_adv;
		return 0;
	}
}

static int h_xd_card_read_copy(struct xd_card_media *card,
			       struct xd_card_request **req)
{
	int rc = 0;

	dev_dbg(card->host->dev, "read copy %d of %d\n",
		(*req)->count, card->trans_cnt);

	if ((*req)->count) {
		if (card->auto_ecc)
			card->trans_cnt = (*req)->count;
		else {
			rc = card->hw_page_size;

			if ((*req)->count != rc) {
				rc *= card->host->extra_pos
				      / (sizeof(struct xd_card_extra)
					 / (card->page_size
					    / card->hw_page_size));
				card->trans_cnt -= rc;
				rc = -EILSEQ;
			} else {
				card->trans_cnt += rc;
				rc = 0;
			}

			if (!rc && !card->host->extra_pos) {
				rc = xd_card_check_ecc_tmp(card);
				if (rc)
					card->trans_cnt -= card->page_size;
			}

			if (!rc && (card->trans_cnt < card->trans_len)) {
				(*req)->count = 0;
				(*req)->error = 0;
				(*req)->addr
					= xd_card_req_address(card,
							      card->trans_cnt);
				sg_set_buf(&(*req)->sg,
					   card->t_buf + card->trans_cnt,
					   card->hw_page_size);
				return 0;
			}
		}
	}

	if (!(*req)->error) {
		if (rc)
			(*req)->error = rc;
		else if (card->trans_cnt != card->trans_len)
			(*req)->error = -EIO;
	}

	if (!(*req)->error) {
		(*req)->cmd = XD_CARD_CMD_INPUT;
		(*req)->flags = XD_CARD_REQ_DATA | XD_CARD_REQ_DIR
				| XD_CARD_REQ_EXTRA | XD_CARD_REQ_STATUS;
		(*req)->addr = xd_card_req_address(card, 0);
		(*req)->error = 0;
		(*req)->count = card->hw_page_size;
		card->trans_cnt = 0;
		card->trans_len = card->flash_req.page_cnt * card->page_size;

		sg_set_buf(&(*req)->sg, card->t_buf, card->hw_page_size);

		memset(&card->host->extra, 0xff, sizeof(struct xd_card_extra));
		xd_card_update_extra_tmp(card);
		card->next_request[0] = h_xd_card_write_tmp;
		return 0;
	} else
		return xd_card_try_next_req(card, req);
}

static int h_xd_card_erase_stat_fmt(struct xd_card_media *card,
				    struct xd_card_request **req)
{
	if (!(*req)->error) {
		(*req)->cmd = XD_CARD_CMD_STATUS1;
		(*req)->flags = XD_CARD_REQ_STATUS;
		(*req)->error = 0;
		(*req)->count = 0;
		card->next_request[0] = h_xd_card_default;
		return 0;
	} else {
		complete(&card->req_complete);
		return (*req)->error;
	}
}

static int h_xd_card_erase_fmt(struct xd_card_media *card,
			       struct xd_card_request **req)
{
	card->host->set_param(card->host, XD_CARD_ADDR_SIZE, card->addr_bytes);
	if ((*req)->error || (card->host->caps & XD_CARD_CAP_CMD_SHORTCUT)) {
		complete(&card->req_complete);
		return (*req)->error;
	} else {
		(*req)->cmd = XD_CARD_CMD_ERASE_START;
		(*req)->flags = XD_CARD_REQ_DIR;
		(*req)->error = 0;
		(*req)->count = 0;
		card->next_request[0] = h_xd_card_erase_stat_fmt;
		return 0;
	}
}

static int h_xd_card_erase_stat(struct xd_card_media *card,
				struct xd_card_request **req)
{
	if (!(*req)->error) {
		(*req)->cmd = XD_CARD_CMD_STATUS1;
		(*req)->flags = XD_CARD_REQ_STATUS;
		(*req)->error = 0;
		(*req)->count = 0;
		card->trans_cnt = 0;
		card->trans_len = 0;
		card->next_request[0] = h_xd_card_check_stat;
		return 0;
	} else
		return xd_card_try_next_req(card, req);
}

static int h_xd_card_erase(struct xd_card_media *card,
			   struct xd_card_request **req)
{
	card->host->set_param(card->host, XD_CARD_ADDR_SIZE, card->addr_bytes);
	if (!(*req)->error) {
		if (card->host->caps & XD_CARD_CAP_CMD_SHORTCUT) {
			if ((*req)->status & XD_CARD_STTS_FAIL)
				(*req)->error = -EFAULT;

			return xd_card_try_next_req(card, req);
		}

		(*req)->cmd = XD_CARD_CMD_ERASE_START;
		(*req)->flags = XD_CARD_REQ_DIR;
		(*req)->error = 0;
		(*req)->count = 0;
		card->trans_cnt = 0;
		card->trans_len = 0;
		card->next_request[0] = h_xd_card_erase_stat;
		return 0;
	} else
		return xd_card_try_next_req(card, req);
}

static int xd_card_get_status(struct xd_card_host *host,
			      unsigned char cmd, unsigned char *status)
{
	struct xd_card_media *card = host->card;

	card->req.cmd = cmd;
	card->req.flags = XD_CARD_REQ_STATUS;
	card->req.error = 0;

	card->next_request[0] = h_xd_card_default;
	xd_card_new_req(host);
	wait_for_completion(&card->req_complete);
	*status = card->req.status;
	card->req.flags = 0;
	return card->req.error;
}

static int xd_card_get_id(struct xd_card_host *host, unsigned char cmd,
			  void *data, unsigned int count)
{
	struct xd_card_media *card = host->card;

	card->req.cmd = cmd;
	card->req.flags = XD_CARD_REQ_ID;
	card->req.addr = 0;
	card->req.count = count;
	card->req.id = data;
	card->req.error = 0;
	card->next_request[0] = h_xd_card_default;
	xd_card_new_req(host);
	wait_for_completion(&card->req_complete);
	card->req.flags = 0;
	return card->req.error;
}


static int xd_card_bad_data(struct xd_card_host *host)
{
	if (host->extra.data_status != 0xff) {
		if (hweight8(host->extra.block_status) < 5)
			return -1;
	}

	return 0;
}

static int xd_card_bad_block(struct xd_card_host *host)
{
	if (host->extra.block_status != 0xff) {
		if (hweight8(host->extra.block_status) < 7)
			return -1;
	}

	return 0;
}

static int xd_card_read_extra(struct xd_card_host *host, unsigned int zone,
			      unsigned int phy_block, unsigned int page)
{
	struct xd_card_media *card = host->card;

	card->flash_req.zone = zone;
	card->flash_req.phy_block = phy_block;
	card->flash_req.page_off = page;
	card->req.cmd = XD_CARD_CMD_READ3;
	card->req.flags = XD_CARD_REQ_EXTRA;
	card->req.addr = xd_card_req_address(card, 0);
	card->req.error = 0;
	card->req.count = 0;

	host->extra_pos = 0;

	card->next_request[0] = h_xd_card_read_extra;
	xd_card_new_req(host);
	wait_for_completion(&card->req_complete);
	return card->req.error;
}

static int xd_card_find_cis(struct xd_card_host *host)
{
	struct xd_card_media *card = host->card;
	unsigned int r_size = sizeof(card->cis) + sizeof(card->idi);
	unsigned int p_cnt, b_cnt;
	unsigned char *buf;
	unsigned int last_block = card->phy_block_cnt - card->log_block_cnt - 1;
	int rc = 0;

	card->cis_block = FLASH_BD_INVALID;
	buf = kmalloc(2 * r_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	sg_set_buf(&card->req_sg[0], buf, 2 * r_size);
	card->seg_count = 1;

	dev_dbg(host->dev, "buf %p, %p, %x, %x\n", buf, card->req_sg[0].page,
		card->req_sg[0].offset, card->req_sg[0].length);
	dev_dbg(host->dev, "looking for cis in %d blocks\n", last_block);

	/* Map the desired blocks for one to one log/phy mapping */
	for (b_cnt = 0; b_cnt < last_block; ++b_cnt)
		flash_bd_set_full(card->fbd, 0, b_cnt, b_cnt);

	for (b_cnt = 0; b_cnt < last_block; ++b_cnt) {
		for (p_cnt = 0; p_cnt < card->page_cnt; ++p_cnt) {
			dev_dbg(host->dev, "checking page %d:%d\n", b_cnt,
				p_cnt);

			card->seg_pos = 0;
			card->seg_off = 0;
			rc = flash_bd_start_reading(card->fbd,
						    (b_cnt * card->page_cnt
						     + p_cnt) * card->page_size,
						    2 * r_size);
			if (rc)
				goto out;

			rc = flash_bd_next_req(card->fbd, &card->flash_req, 0,
					       0);

			if (rc) {
				flash_bd_end(card->fbd);
				goto out;
			}

			xd_card_trans_req(card, &card->req);

			card->req.flags |= XD_CARD_REQ_NO_ECC
					   | XD_CARD_REQ_EXTRA;

			xd_card_new_req(host);
			wait_for_completion(&card->req_complete);
			rc = card->req.error;

			flash_bd_end(card->fbd);

			dev_dbg(host->dev, "extra: %08x, %02x, %02x, %04x "
				"(%02x, %02x, %02x), %04x, (%02x, %02x, %02x)\n",
				host->extra.reserved, host->extra.data_status,
				host->extra.block_status, host->extra.addr1,
				host->extra.ecc_hi[0], host->extra.ecc_hi[1],
				host->extra.ecc_hi[2], host->extra.addr2,
				host->extra.ecc_lo[0], host->extra.ecc_lo[1],
				host->extra.ecc_lo[2]);

			if (!rc) {
				if (xd_card_bad_block(host))
					break;
				else if (!xd_card_bad_data(host)) {
					memcpy(&card->cis, buf,
					       sizeof(card->cis));
					rc = sizeof(card->cis);
					memcpy(&card->idi, buf + rc,
					       sizeof(card->idi));
					rc += sizeof(card->idi);
					if (memcmp(&card->cis, buf + rc,
						   sizeof(card->cis))) {
						rc = -EMEDIUMTYPE;
						goto out;
					}
					rc += sizeof(card->cis);
					if (memcmp(&card->idi, buf + rc,
						   sizeof(card->idi))) {
						rc = -EMEDIUMTYPE;
						goto out;
					}

					card->cis_block = b_cnt;
					rc = 0;
					goto out;
				}
			} else
				goto out;
		}
	}

out:
	for (b_cnt = 0; b_cnt < last_block; ++b_cnt)
		flash_bd_set_empty(card->fbd, 0, b_cnt, 0);

	if (card->cis_block != FLASH_BD_INVALID) {
		for (b_cnt = 0; b_cnt <= card->cis_block; ++b_cnt)
			flash_bd_set_full(card->fbd, 0, b_cnt,
					  FLASH_BD_INVALID);
	}
	kfree(buf);
	return rc;
}

static int xd_card_resolve_conflict(struct xd_card_host *host,
				    unsigned int zone,
				    unsigned int phy_block,
				    unsigned int log_block)
{
	struct xd_card_media *card = host->card;
	unsigned int o_phy_block = flash_bd_get_physical(card->fbd, zone,
							 log_block);
	unsigned int l_addr, l_addr_o;
	int rc;

	rc = xd_card_read_extra(host, zone, phy_block, card->page_cnt - 1);
	if (rc)
		return rc;

	l_addr = xd_card_extra_to_addr(&host->extra);

	rc = xd_card_read_extra(host, zone, o_phy_block, card->page_cnt - 1);
	if (rc)
		return rc;

	l_addr_o = xd_card_extra_to_addr(&host->extra);


	dev_dbg(host->dev, "block map conflict (%x) %x -> %x, %x (%x, %x)\n",
		zone, log_block, phy_block, o_phy_block, l_addr, l_addr_o);

	if (l_addr != log_block)
		l_addr = FLASH_BD_INVALID;

	if (l_addr_o !=log_block)
		l_addr_o = FLASH_BD_INVALID;

	if (l_addr == l_addr_o) {
		if (phy_block < o_phy_block) {
			flash_bd_set_empty(card->fbd, zone, o_phy_block, 0);
			flash_bd_set_full(card->fbd, zone, phy_block,
					  log_block);
		} else
			flash_bd_set_empty(card->fbd, zone, phy_block, 0);
	} else if (l_addr != FLASH_BD_INVALID) {
		flash_bd_set_empty(card->fbd, zone, o_phy_block, 0);
		flash_bd_set_full(card->fbd, zone, phy_block, log_block);
	} else if (l_addr_o != FLASH_BD_INVALID) {
		flash_bd_set_empty(card->fbd, zone, phy_block, 0);
	}

	return 0;
}

static int xd_card_fill_lut(struct xd_card_host *host)
{
	struct xd_card_media *card = host->card;
	unsigned int z_cnt = 0, b_cnt, log_block, s_block = card->cis_block + 1;
	int rc;

	for (z_cnt = 0; z_cnt < card->zone_cnt; ++z_cnt) {
		for (b_cnt = s_block; b_cnt < card->phy_block_cnt; ++b_cnt) {
			rc = xd_card_read_extra(host, z_cnt, b_cnt, 0);
			if (rc)
				return rc;

			if (xd_card_bad_block(host)) {
				flash_bd_set_full(card->fbd, z_cnt, b_cnt,
						  FLASH_BD_INVALID);
				continue;
			}

			log_block = xd_card_extra_to_addr(&host->extra);
			if (log_block == FLASH_BD_INVALID) {
				flash_bd_set_empty(card->fbd, z_cnt, b_cnt, 0);
				continue;
			}

			rc = flash_bd_set_full(card->fbd, z_cnt, b_cnt,
					       log_block);
			if (rc == -EEXIST)
				rc = xd_card_resolve_conflict(host, z_cnt,
							      b_cnt, log_block);
			dev_dbg(host->dev, "fill lut (%d) %d -> %d, %d\n",
				z_cnt, log_block, b_cnt, rc);

			if (rc)
				return rc;
		}
		s_block = 0;
	}
	return 0;
}

/* Mask ROM devices are required to have the same format as Flash ones */
static int xd_card_fill_lut_rom(struct xd_card_host *host)
{
	struct xd_card_media *card = host->card;
	unsigned int z_cnt = 0, b_cnt;
	unsigned int s_block = card->cis_block + 1;

	for (z_cnt = 0; z_cnt < card->zone_cnt; ++z_cnt) {
		for (b_cnt = 0; b_cnt < card->log_block_cnt; ++b_cnt) {
			flash_bd_set_full(card->fbd, z_cnt, s_block, b_cnt);
			s_block++;
		}
		s_block = 0;
	}
	return 0;
}

/*** Data transfer ***/

static void xd_card_process_request(struct xd_card_media *card,
				    struct request *req)
{
	int rc, chunk;
	unsigned int t_len = 0;
	unsigned long flags;

	do {
		card->seg_pos = 0;
		card->seg_off = 0;
		card->seg_count = blk_rq_map_sg(req->q, req, card->req_sg);

		if (!card->seg_count) {
			rc = -ENOMEM;
			goto req_failed;
		}

		if (rq_data_dir(req) == READ) {
			dev_dbg(card->host->dev, "Read segs: %d, offset: %lx, "
				"size: %lx\n", card->seg_count,
				req->sector << 9,
				req->nr_sectors << 9);
			rc = flash_bd_start_reading(card->fbd, req->sector << 9,
						    req->nr_sectors << 9);
		} else {
			dev_dbg(card->host->dev, "Write segs: %d, offset: %lx, "
				"size: %lx\n", card->seg_count,
				req->sector << 9,
				req->nr_sectors << 9);		
			rc = flash_bd_start_writing(card->fbd, req->sector << 9,
						    req->nr_sectors << 9);
		}

		if (rc)
			goto req_failed;

		rc = flash_bd_next_req(card->fbd, &card->flash_req, 0, 0);

		if (rc) {
			flash_bd_end(card->fbd);
			goto req_failed;
		}

		rc = xd_card_trans_req(card, &card->req);
		if (!rc) {
			xd_card_new_req(card->host);
			wait_for_completion(&card->req_complete);
			rc = card->req.error;
		}

		t_len = flash_bd_end(card->fbd);
		dev_dbg(card->host->dev, "transferred %x\n", t_len);
req_failed:
		spin_lock_irqsave(&card->q_lock, flags);
		if (t_len > 0)
			chunk = end_that_request_chunk(req, 1, t_len);
		else
			chunk = end_that_request_first(req, rc,
						       req->nr_sectors);

		if (!chunk) {
			add_disk_randomness(req->rq_disk);
			blkdev_dequeue_request(req);
			end_that_request_last(req, rc < 0 ? rc : 1);
		}
		spin_unlock_irqrestore(&card->q_lock, flags);
	} while (chunk);
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

static int xd_card_format_thread(void *data)
{
	struct xd_card_media *card = data;
	struct xd_card_host *host = card->host;
	struct flash_bd *o_fbd;
	int rc = 0;
	unsigned long flags;

	mutex_lock(&host->lock);
	card->trans_len = card->zone_cnt << card->block_addr_bits;
	o_fbd = card->fbd;
	card->fbd = flash_bd_init(card->zone_cnt, card->phy_block_cnt,
				  card->log_block_cnt, card->page_cnt,
				  card->page_size);
	if (!card->fbd) {
		card->fbd = o_fbd;
		rc = -ENOMEM;
		goto out;
	}

	flash_bd_destroy(o_fbd);

	for(card->trans_cnt = card->cis_block + 1;
	    card->trans_cnt < card->trans_len;
	    ++card->trans_cnt) {
		host->set_param(host, XD_CARD_ADDR_SIZE, card->addr_bytes - 1);
		card->req.cmd = XD_CARD_CMD_ERASE_SET;
		card->req.flags = XD_CARD_REQ_DIR | XD_CARD_REQ_STATUS;
		card->req.addr = card->trans_cnt << card->page_addr_bits;
		card->req.error = 0;
		card->req.count = 0;
		card->next_request[0] = h_xd_card_erase_fmt;

		xd_card_new_req(host);
		wait_for_completion(&card->req_complete);
		rc = card->req.error;

		if (!rc) {
			if (!(card->req.status & XD_CARD_STTS_FAIL))
				flash_bd_set_empty(card->fbd,
						   card->trans_cnt
						   >> card->block_addr_bits,
						   card->trans_cnt
						   & ((1
						       << card->block_addr_bits)
						      - 1), 1);
			else
				flash_bd_set_full(card->fbd,
						   card->trans_cnt
						   >> card->block_addr_bits,
						   card->trans_cnt
						   & ((1
						       << card->block_addr_bits)
						      - 1), FLASH_BD_INVALID);
		}

		mutex_unlock(&host->lock);
		yield();
		mutex_lock(&host->lock);
		if (kthread_should_stop()) {
			dev_info(host->dev, "format aborted at block "
				 "%x of %x\n", card->trans_cnt,
				 card->trans_len);
			card->format = 0;
			mutex_unlock(&host->lock);
			return 0;
		}

		if (rc)
			break;
	}
out:
	if (!rc)
		dev_info(host->dev, "format complete\n");
	else
		dev_err(host->dev, "format failed (%d)\n", rc);

	host->card->format = 0;
	if (!host->card->q_thread) {
		mutex_unlock(&host->lock);
		return 0;
	}

	host->card->q_thread = kthread_run(xd_card_queue_thread,
					   host->card,
					   DRIVER_NAME"d");
	if (IS_ERR(host->card->q_thread)) {
		host->card->q_thread = NULL;
		xd_card_detect_change(host);
	} else {
		spin_lock_irqsave(&host->card->q_lock, flags);
		blk_start_queue(host->card->queue);
		spin_unlock_irqrestore(&host->card->q_lock, flags);
	}

	mutex_unlock(&host->lock);
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

static int xd_card_set_disk_size(struct xd_card_media *card)
{
	card->page_size = 512;
	card->hw_page_size = 512;
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
		card->hw_page_size = 256;
		card->capacity = 2000;
		card->cylinders = 125;
		card->heads = 4;
		card->sectors_per_head = 4;
		card->phy_block_cnt = 256;
		card->log_block_cnt = 250;
		card->page_cnt = 8;
		break;
	case 0x5d:
	case 0xea:
	case 0x64:
		if (card->id1.device_code != 0x5d) {
			card->hw_page_size = 256;
			card->phy_block_cnt = 512;
			card->log_block_cnt = 500;
			card->page_cnt = 8;
		} else {
			card->mask_rom = 1;
			card->phy_block_cnt = 256;
			card->log_block_cnt = 250;
			card->page_cnt = 16;
		}
		card->capacity = 4000;
		card->cylinders = 125;
		card->heads = 4;
		card->sectors_per_head = 8;
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

	max_sectors = card->zone_cnt * card->log_block_cnt * card->page_size
		      * card->page_cnt;
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

	blk_queue_hardsect_size(card->queue, card->page_size);

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

static void xd_card_set_media_param(struct xd_card_host *host)
{
	struct xd_card_media *card = host->card;

	host->set_param(host, XD_CARD_PAGE_SIZE, card->hw_page_size);
	host->set_param(host, XD_CARD_EXTRA_SIZE,
			sizeof(struct xd_card_extra)
			/ (card->page_size / card->hw_page_size));
	host->set_param(host, XD_CARD_ADDR_SIZE, card->addr_bytes);

	if (!card->sm_media)
		host->set_param(host, XD_CARD_CLOCK, XD_CARD_NORMAL);
	else
		host->set_param(host, XD_CARD_CLOCK, XD_CARD_SLOW);
}

static int xd_card_sysfs_register(struct xd_card_host *host)
{
	int rc;

	rc = device_create_file(host->dev, &dev_attr_xd_card_id);
	if (rc)
		return rc;

	rc = device_create_file(host->dev, &dev_attr_xd_card_cis);
	if (rc)
		goto out_remove_id;

	rc = device_create_file(host->dev, &dev_attr_xd_card_idi);
	if (rc)
		goto out_remove_cis;

	host->card->dev_attr_block_map.attr.name = "xd_card_block_map";
	host->card->dev_attr_block_map.attr.mode = S_IRUGO;
	host->card->dev_attr_block_map.attr.owner = THIS_MODULE;

	host->card->dev_attr_block_map.size
		= flash_bd_map_size(host->card->fbd);
	host->card->dev_attr_block_map.read = xd_card_block_map_read;

	rc = device_create_bin_file(host->dev, &host->card->dev_attr_block_map);
	if (rc)
		goto out_remove_idi;

	rc = device_create_file(host->dev, &dev_attr_xd_card_format);
	if (!rc)
		return 0;

	device_remove_bin_file(host->dev, &host->card->dev_attr_block_map);
out_remove_idi:
	device_remove_file(host->dev, &dev_attr_xd_card_idi);
out_remove_cis:
	device_remove_file(host->dev, &dev_attr_xd_card_cis);
out_remove_id:
	device_remove_file(host->dev, &dev_attr_xd_card_id);
	return rc;
}

static void xd_card_sysfs_unregister(struct xd_card_host *host)
{
	device_remove_file(host->dev, &dev_attr_xd_card_format);
	device_remove_bin_file(host->dev, &host->card->dev_attr_block_map);
	device_remove_file(host->dev, &dev_attr_xd_card_idi);
	device_remove_file(host->dev, &dev_attr_xd_card_cis);
	device_remove_file(host->dev, &dev_attr_xd_card_id);
}

static int xd_card_init_media(struct xd_card_host *host)
{
	struct task_struct *q_thread;
	int rc;

	xd_card_set_media_param(host);

	if (!host->card->mask_rom)
		rc = xd_card_fill_lut(host);
	else
		rc = xd_card_fill_lut_rom(host);

	if (rc)
		return rc;

	/* We just read a single page to reset flash column pointer. */
	host->card->flash_req.zone = 0;
	host->card->flash_req.phy_block = host->card->cis_block;
	host->card->flash_req.page_off = 0;
	host->card->flash_req.page_cnt = 1;
	host->card->req.cmd = XD_CARD_CMD_READ1;
	host->card->req.flags = XD_CARD_REQ_DATA;
	host->card->req.addr = xd_card_req_address(host->card, 0);
	host->card->req.error = 0;
	host->card->req.count = 0;
	host->card->trans_cnt = 0;
	host->card->trans_len = host->card->hw_page_size;

	sg_set_buf(&host->card->req.sg, host->card->t_buf,
		   host->card->trans_len);
	host->card->next_request[0] = h_xd_card_default;
	xd_card_new_req(host);
	wait_for_completion(&host->card->req_complete);

	host->card->q_thread = kthread_run(xd_card_queue_thread,
					   host->card, DRIVER_NAME"d");

	if (IS_ERR(host->card->q_thread)) {
		rc = PTR_ERR(host->card->q_thread);
		host->card->q_thread = NULL;
		return rc;
	}

	rc = xd_card_sysfs_register(host);
	if (rc)
		goto out_stop_thread;

	rc = xd_card_init_disk(host->card);
	if (rc)
		goto out_sysfs_unregister;

	wake_up_process(host->card->q_thread);
	return 0;

out_sysfs_unregister:
	xd_card_sysfs_unregister(host);
out_stop_thread:
	q_thread = host->card->q_thread;
	host->card->q_thread = NULL;
	mutex_unlock(&host->lock);
	kthread_stop(q_thread);
	mutex_lock(&host->lock);
	return rc;
}

static void xd_card_free_media(struct xd_card_media *card)
{
	flash_bd_destroy(card->fbd);
	kfree(card->t_buf);
	kfree(card);
}

struct xd_card_media *xd_card_alloc_media(struct xd_card_host *host)
{
	struct xd_card_media *card = kzalloc(sizeof(struct xd_card_media),
					     GFP_KERNEL);
	struct xd_card_media *old_card = host->card;
	int rc = -ENOMEM;
	unsigned char status;

	if (!card)
		goto out;

	card->host = host;
	host->card = card;
	card->usage_count = 1;
	spin_lock_init(&card->q_lock);
	init_waitqueue_head(&card->q_wait);
	init_completion(&card->req_complete);

	rc = xd_card_get_status(host, XD_CARD_CMD_RESET, &status);
	if (rc)
		goto out;

	rc = xd_card_get_status(host, XD_CARD_CMD_STATUS1, &status);
	if (rc)
		goto out;
/*
	if (!(status & XD_CARD_STTS_RW))
		card->read_only = 1;
*/
	rc = xd_card_get_id(host, XD_CARD_CMD_ID1, &card->id1,
			    sizeof(card->id1));
	if (rc)
		goto out;

	rc = xd_card_set_disk_size(card);
	if (rc)
		goto out;

	if (card->mask_rom)
		card->read_only = 1;

	rc = card->page_cnt * (card->page_size / card->hw_page_size);
	card->page_addr_bits = fls(rc - 1);
	if ((1 << card->page_addr_bits) < rc)
		card->page_addr_bits++;

	card->block_addr_bits = fls(card->phy_block_cnt - 1);
	if ((1 << card->block_addr_bits) < card->phy_block_cnt)
		card->block_addr_bits++;

	if (card->capacity < 62236)
		card->addr_bytes = 3;
	else if (card->capacity < 8388608)
		card->addr_bytes = 4;
	else {
		rc = -EINVAL;
		goto out;
	}

	dev_dbg(host->dev, "address bits: page %d, block %d, addr %d\n",
		card->page_addr_bits, card->block_addr_bits, card->addr_bytes);

	if (host->caps & XD_CARD_CAP_AUTO_ECC)
		card->auto_ecc = 1;

	if (host->caps & XD_CARD_CAP_FIXED_EXTRA) {
		if (card->page_size != card->hw_page_size)
			card->auto_ecc = 0;
	}

	if (!card->sm_media) {
		rc = xd_card_get_id(host, XD_CARD_CMD_ID2, &card->id2,
				    sizeof(card->id2));
		if (rc)
			goto out;
	}

	if (!card->sm_media) {
		/* This appears to be totally optional */
		xd_card_get_id(host, XD_CARD_CMD_ID3, &card->id3,
			       sizeof(card->id3));
	}

	xd_card_set_media_param(host);

	dev_dbg(host->dev, "alloc %d bytes for tmp\n",
		card->page_size * card->page_cnt);
	card->t_buf = kmalloc(card->page_size * card->page_cnt, GFP_KERNEL);
	if (!card->t_buf) {
		rc = -ENOMEM;
		goto out;
	}

	dev_dbg(host->dev, "init flash_bd\n");
	card->fbd = flash_bd_init(card->zone_cnt, card->phy_block_cnt,
				  card->log_block_cnt, card->page_cnt,
				  card->page_size);
	if (!card->fbd) {
		rc = -ENOMEM;
		goto out;
	}

	rc = xd_card_find_cis(host);
	if (rc)
		goto out;

	/* It is possible to parse CIS header, but recent version of the spec
	 * suggests that it is frozen and information it contains (mostly
	 * ancient Compact Flash stuff) irrelevant.
	 */
	if (memcmp(xd_card_cis_header, card->cis, sizeof(xd_card_cis_header)))
		rc = -EMEDIUMTYPE;

out:
	host->card = old_card;
	if (host->card)
		xd_card_set_media_param(host);
	if (rc) {
		xd_card_free_media(card);
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
	xd_card_sysfs_unregister(host);

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
	dev_dbg(host->dev, "card allocated %p\n", card);

	if (IS_ERR(card)) {
		dev_dbg(host->dev, "error %ld allocating card\n",
			PTR_ERR(card));
		if (host->card) {
			dev_dbg(host->dev, "x1 %p\n", host->card);
			xd_card_remove_media(host->card);
			dev_dbg(host->dev, "x2\n");
			host->card = NULL;
		}
	} else {
		dev_dbg(host->dev, "new card %02x, %02x, %02x, %02x\n",
			card->id1.maker_code, card->id1.device_code,
			card->id1.option_code1, card->id1.option_code2);

		if (host->card) {
			xd_card_remove_media(host->card);
			host->card = NULL;
		}

		host->card = card;
		if (xd_card_init_media(host)) {
			xd_card_free_media(host->card);
			host->card = NULL;
		}
	}
	dev_dbg(host->dev, "x3\n");
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
int xd_card_suspend_host(struct xd_card_host *host)
{
	struct task_struct *q_thread = NULL;
	unsigned long flags;

	mutex_lock(&host->lock);
	if (host->card) {
		spin_lock_irqsave(&host->card->q_lock, flags);
		q_thread = host->card->q_thread;
		host->card->q_thread = NULL;
		blk_stop_queue(host->card->queue);
		spin_unlock_irqrestore(&host->card->q_lock, flags);

		if (q_thread) {
			mutex_unlock(&host->lock);
			kthread_stop(q_thread);
			mutex_lock(&host->lock);
		}
	}
	host->set_param(host, XD_CARD_POWER, XD_CARD_POWER_OFF);
	mutex_unlock(&host->lock);
	return 0;
}
EXPORT_SYMBOL(xd_card_suspend_host);

/**
 * xd_card_resume_host - notify bus driver of host resumption
 * @host - host to use
 */
int xd_card_resume_host(struct xd_card_host *host)
{
	struct xd_card_media *card;
	unsigned int flags;

	mutex_lock(&host->lock);
	if (host->card) {
		host->set_param(host, XD_CARD_POWER, XD_CARD_POWER_ON);
#if defined(CONFIG_XD_CARD_UNSAFE_RESUME)
		card = xd_card_alloc_media(host);
		if (!IS_ERR(card)
		    && !memcmp(&card->cis, &host->card->cis,
			       sizeof(card->cis))
		    && !memcmp(&card->idi, &host->card->idi,
			       sizeof(card->idi))) {
			xd_card_free_media(card);
			host->card->q_thread = kthread_run(xd_card_queue_thread,
							   host->card,
							   DRIVER_NAME"d");
			if (IS_ERR(host->card->q_thread)) {
				host->card->q_thread = NULL;
				xd_card_detect_change(host);
			}
		} else
			xd_card_detect_change(host);
#else
		xd_card_detect_change(host);
#endif /* CONFIG_XD_CARD_UNSAFE_RESUME */
		spin_lock_irqsave(&host->card->q_lock, flags);
		blk_start_queue(host->card->queue);
		spin_unlock_irqrestore(&host->card->q_lock, flags);
	} else
		xd_card_detect_change(host);
	mutex_unlock(&host->lock);
	return 0;
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
