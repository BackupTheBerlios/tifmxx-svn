/*
 *  tifm_ms.c - TI FlashMedia driver
 *
 *  Copyright (C) 2006 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Special thanks to Carlos Corbacho for providing various MemoryStick cards
 * that made this driver possible.
 *
 */

#include "linux/tifm.h"
#include "linux/memstick.h"
#include <linux/highmem.h>
#include <asm/io.h>

#define DRIVER_NAME "tifm_ms"
#define DRIVER_VERSION "0.1"

static int no_dma = 0;
module_param(no_dma, bool, 0644);

#define TIFM_MS_TIMEOUT      0x00100
#define TIFM_MS_BADCRC       0x00200
#define TIFM_MS_EOTPC        0x01000
#define TIFM_MS_INT          0x02000

/* The meaning of the bit majority in this constant is unknown. */
#define TIFM_MS_SERIAL       0x04010

#define TIFM_MS_SYS_LATCH    0x00100
#define TIFM_MS_SYS_NOT_RDY  0x00800
#define TIFM_MS_SYS_DATA     0x10000

#define TIFM_MS_FIFO_THR     0x00008 /* use fifo trasnfers above this size */
/* Hardware flags */
enum {
	CMD_READY  = 0x0001,
	FIFO_READY = 0x0002,
	CARD_READY = 0x0004,
	DMA_TRNS   = 0x0008
};

struct tifm_ms {
	struct tifm_dev         *dev;
	unsigned short          eject:1,
				no_dma:1;
	unsigned short          cmd_flags;
	unsigned int            mode_mask;
	unsigned long           timeout_jiffies;

	struct tasklet_struct   finish_tasklet;
	struct timer_list       timer;

	struct memstick_request *req;
	unsigned int            cmd_pos;
};

static void tifm_ms_read_fifo(struct tifm_ms *host, struct scatterlist *sg,
			       unsigned int off)
{
	struct tifm_dev *sock = host->dev;
	unsigned int cnt;
	unsigned char *buf;

	buf = kmap_atomic(sg->page, KM_BIO_DST_IRQ) + off;
	for (cnt = 0; cnt < 128; cnt++)
		((unsigned int*)buf)[cnt]
			= __raw_readl(sock->addr + SOCK_FIFO_ACCESS + cnt / 4);
	kunmap_atomic(buf - off, KM_BIO_DST_IRQ);
}

static void tifm_ms_write_fifo(struct tifm_ms *host, struct scatterlist *sg,
			       unsigned int off)
{
	struct tifm_dev *sock = host->dev;
	unsigned int cnt;
	unsigned char *buf;

	buf = kmap_atomic(sg->page, KM_BIO_SRC_IRQ) + off;

	for (cnt = 0; cnt < 128; cnt++)
		__raw_writel(((unsigned int*)buf)[cnt],
			     sock->addr + SOCK_FIFO_ACCESS + cnt / 4);
	kunmap_atomic(buf - off, KM_BIO_SRC_IRQ);
}

static int tifm_ms_transfer_data(struct tifm_ms *host)
{
	struct memstick_request *req = host->req;
	unsigned int cnt = req->sg[host->sg_pos].length >> 9;
	unsigned int off = host->block_pos << 9;

	if ((cnt - host->block_pos) == 1)
		host->block_pos = 0;
	else
		host->block_pos++;

	if (host->sg_pos < host->sg_len) {
		if (req->data_dir == WRITE)
			tifm_ms_write_fifo(host, &req->sg[host->sg_pos],
					   off);
		else
			tifm_ms_read_fifo(host, &req->sg[host->sg_pos],
					  off);

		return 0;
	} else
		return 1;
}

static int tifm_ms_set_dma(struct tifm_ms *host)
{
	struct tifm_dev *sock = host->dev;
	struct memstick_request *req = host->req;

	unsigned int dma_cnt = sg_dma_len(&req->sg[host->sg_pos]) >> 9;
	unsigned int dma_off = host->block_pos << 9;

	if ((dma_cnt - host->block_pos) <= TIFM_DMA_TSIZE) {
		dma_cnt -= host->block_pos;
		host->block_pos = 0;
	} else {
		dma_cnt = TIFM_DMA_TSIZE;
		host->block_pos += TIFM_DMA_TSIZE;
	}

	if (host->sg_pos < host->sg_len) {
		dev_dbg(&sock->dev, "setting dma for %d blocks\n", dma_cnt);
		writel(sg_dma_address(&req->sg[host->sg_pos]) + dma_off,
		       sock->addr + SOCK_DMA_ADDRESS);
		if (req->data_dir == WRITE)
			writel((dma_cnt << 8) | TIFM_DMA_TX | TIFM_DMA_EN,
			       sock->addr + SOCK_DMA_CONTROL);
		else
			writel((dma_cnt << 8) | TIFM_DMA_EN,
			       sock->addr + SOCK_DMA_CONTROL);

		return 0;
	} else
		return 1;
}

static void tifm_ms_write_data(struct tifm_ms *host)
{
	struct scatterlist *sg = &host->req->sg[host->cmd_pos];
	unsigned int data_len = sg->length;
	unsigned int cnt = 0;

	while (cnt < sg->length) {
		off = 
		
	}
}

static int tifm_ms_issue_command(struct tifm_ms *host)
{
	struct tifm_dev *sock = host->dev;
	int use_fifo = 0;
	unsigned int cnt = 0, data_len, data_dir, val;

	if (host->cmd_pos == host->req->count)
		return 1;

	data_len = host->req->sg[host->cmd_pos].length;
	data_dir = host->req->cmd[host->cmd_pos].data_dir;

	if ((data_len < TIFM_MS_FIFO_THR)
	    || (data_len != roundup_pow_of_two(data_len))) {
		/* The meaning of the bit majority in this constant is unknown. */
		writel(host->mode_mask | 0x2607
		       | (data_dir == WRITE ? TIFM_MS_SYS_LATCH : 0),
		       sock->addr + SOCK_MS_SYSTEM);

		if (data_dir == WRITE) {
			while (cnt < data_len) {
				writel(TIFM_MS_SYS_LATCH
				       | readl(sock->addr + SOCK_MS_SYSTEM),
				       sock->addr + SOCK_MS_SYSTEM);
				if (data_len - cnt >= 4) {
					val =
						*(unsigned int*)(req->short_data + cnt);
					__raw_writel(val,
						     sock->addr + SOCK_MS_DATA);
					cnt += 4;
			} else {
				val = req->short_data[cnt++];
				if (cnt < req->short_data_len)
					val = (val << 8)
					      | req->short_data[cnt++];
				if (cnt < req->short_data_len)
					val = (val << 8)
					      | req->short_data[cnt++];
				writel(val, sock->addr + SOCK_MS_DATA);
			}
		}

		writel(TIFM_MS_SYS_LATCH
		       | readl(sock->addr + SOCK_MS_SYSTEM),
		       sock->addr + SOCK_MS_SYSTEM);
		writel(0, sock->addr + SOCK_MS_DATA);
	}

	cmd |= req->short_data_len;
	cmd_mask = readl(sock->addr + SOCK_MS_SYSTEM);
	if (req->blocks)
		cmd_mask |= TIFM_MS_SYS_DATA;
	else
		cmd_mask &= ~TIFM_MS_SYS_DATA;

	cmd_mask |= TIFM_MS_SYS_NOT_RDY;

	dev_dbg(&sock->dev, "executing TPC %x, %x\n", cmd, cmd_mask);
	writel(cmd_mask, sock->addr + SOCK_MS_SYSTEM);
	writel(cmd, sock->addr + SOCK_MS_COMMAND);


		if (host->req->cmd[host->cmd_pos].data_dir == WRITE)) {

		}
	} else {
		use_dma = data_len == roundup_pow_of_two(data_len)
			  ? use_dma : 0;
		if (use_dma) {
		} else {
		}
	}
	return 0;
}

static void tifm_ms_check_status(struct tifm_ms *host)
{
	if (host->req->error == MEMSTICK_ERR_NONE) {
		if (!(host->cmd_flags & CMD_READY))
			return;
		if (host->req->blocks && !(host->cmd_flags & FIFO_READY))
			return;
		if (host->req->need_card_int
		    && !(host->cmd_flags & CARD_READY))
			return;
	}
	tasklet_schedule(&host->finish_tasklet);
}

/* Called from interrupt handler */
static void tifm_ms_data_event(struct tifm_dev *sock)
{
	struct tifm_ms *host;
	unsigned int fifo_status = 0;

	spin_lock(&sock->lock);
	host = memstick_priv((struct memstick_host*)tifm_get_drvdata(sock));
	fifo_status = readl(sock->addr + SOCK_DMA_FIFO_STATUS);
	dev_dbg(&sock->dev, "data event: fifo_status %x, flags %x\n",
		fifo_status, host->cmd_flags);

	if (host->req && host->req->blocks
	    && (fifo_status & TIFM_FIFO_READY)) {
		if (!host->block_pos)
			host->sg_pos++;

		if (host->no_dma ? tifm_ms_transfer_data(host)
		    : tifm_ms_set_dma(host)) {
			host->cmd_flags |= FIFO_READY;
			tifm_ms_check_status(host);
		}
	}
	writel(fifo_status, sock->addr + SOCK_DMA_FIFO_STATUS);
	spin_unlock(&sock->lock);
}

/* Called from interrupt handler */
static void tifm_ms_event(struct tifm_dev *sock)
{
	struct tifm_ms *host;
	unsigned int host_status = 0;

	spin_lock(&sock->lock);
	host = memstick_priv((struct memstick_host*)tifm_get_drvdata(sock));

	if (host->req) {
		host_status = readl(sock->addr + SOCK_MS_STATUS);

		if (host_status & TIFM_MS_TIMEOUT)
			host->req->error = MEMSTICK_ERR_TIMEOUT;
		else if (host_status & TIFM_MS_BADCRC)
			host->req->error = MEMSTICK_ERR_BADCRC;

		if (host->req->error) {
			writel(TIFM_FIFO_INT_SETALL,
			       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
			writel(TIFM_DMA_RESET, sock->addr + SOCK_DMA_CONTROL);
		}

		if (host_status & TIFM_MS_EOTPC)
			host->cmd_flags |= CMD_READY;
		if (host_status & TIFM_MS_INT)
			host->cmd_flags |= CARD_READY;

		tifm_ms_check_status(host);
		writel(TIFM_MS_SYS_NOT_RDY | readl(sock->addr + SOCK_MS_SYSTEM),
		       sock->addr + SOCK_MS_SYSTEM);
	}
	spin_unlock(&sock->lock);
	return;
}

static void tifm_ms_request(struct memstick_host *msh,
			    struct memstick_request *req)
{
	struct tifm_ms *host = memstick_priv(msh);
	struct tifm_dev *sock = host->dev;
	memstick_error_t err = MEMSTICK_ERR_TIMEOUT;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);

	if (host->eject) {
		req->cmd[0].error = MEMSTICK_ERR_TIMEOUT;
		goto err_out;
	}

	if (host->req) {
		printk(KERN_ERR "%s : unfinished request detected\n",
		       sock->dev.bus_id);
		req->cmd[0].error = MEMSTICK_ERR_TIMEOUT;
		goto err_out;
	}

	host->cmd_pos = 0;
	host->req = req;
	mod_timer(&host->timer, jiffies + host->timeout_jiffies);

	writel(TIFM_CTRL_LED | readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);

	if (!tifm_ms_issue_command(host))
		return;

	writel((~TIFM_CTRL_LED) & readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);

	host->req = NULL;

err_out:
	spin_unlock_irqrestore(&sock->lock, flags);
	memstick_request_done(msh, req, 0);
	return;
}


	if ((req->sg->length > TIFM_MS_FIFO_THR)
	    && (req->sg->length == roundup_pow_of_two(req->sg->length))) {
		if (host->no_dma) {
			if (req->data_dir == WRITE)
				tifm_ms_write_fifo(host, &sg);
		} else {
			if (req->data_dir == READ) {
			}
		}
	} else {
	}


		

		if (host->no_dma) {
			host->sg_len = req->sg_len;
		} else {
			sg_count = tifm_map_sg(sock, req->sg, req->sg_len,
					       req->data_dir == WRITE
					       ? PCI_DMA_TODEVICE
					       : PCI_DMA_FROMDEVICE);
			if (sg_count < 1) {
				printk(KERN_ERR "%s : scatterlist map failed\n",
				       sock->dev.bus_id);
				goto err_out;
			}

			writel(TIFM_FIFO_INT_SETALL,
			       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
			writel(TIFM_FIFO_ENABLE,
			       sock->addr + SOCK_FIFO_CONTROL);
			writel(TIFM_FIFO_INTMASK,
			       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_SET);

			host->sg_len = sg_count;
			tifm_ms_set_dma(host);
		}
	}

	host->cmd_flags = 0;
	host->req = req;



	/* The meaning of the bit majority in this constant is unknown. */
	writel(host->mode_mask | 0x2607
	       | (req->short_data_dir == WRITE ? TIFM_MS_SYS_LATCH : 0),
	       sock->addr + SOCK_MS_SYSTEM);

	if (req->short_data_dir == WRITE) {
		while (cnt < req->short_data_len) {
			writel(TIFM_MS_SYS_LATCH
			       | readl(sock->addr + SOCK_MS_SYSTEM),
			       sock->addr + SOCK_MS_SYSTEM);
			if (req->short_data_len - cnt >= 4) {
				val = *(unsigned int*)(req->short_data + cnt);
				__raw_writel(val, sock->addr + SOCK_MS_DATA);
				cnt += 4;
			} else {
				val = req->short_data[cnt++];
				if (cnt < req->short_data_len)
					val = (val << 8)
					      | req->short_data[cnt++];
				if (cnt < req->short_data_len)
					val = (val << 8)
					      | req->short_data[cnt++];
				writel(val, sock->addr + SOCK_MS_DATA);
			}
		}

		writel(TIFM_MS_SYS_LATCH
		       | readl(sock->addr + SOCK_MS_SYSTEM),
		       sock->addr + SOCK_MS_SYSTEM);
		writel(0, sock->addr + SOCK_MS_DATA);
	}

	cmd |= req->short_data_len;
	cmd_mask = readl(sock->addr + SOCK_MS_SYSTEM);
	if (req->blocks)
		cmd_mask |= TIFM_MS_SYS_DATA;
	else
		cmd_mask &= ~TIFM_MS_SYS_DATA;

	cmd_mask |= TIFM_MS_SYS_NOT_RDY;

	dev_dbg(&sock->dev, "executing TPC %x, %x\n", cmd, cmd_mask);
	writel(cmd_mask, sock->addr + SOCK_MS_SYSTEM);
	writel(cmd, sock->addr + SOCK_MS_COMMAND);

	spin_unlock_irqrestore(&sock->lock, flags);
	return;

err_out:
	spin_unlock_irqrestore(&sock->lock, flags);
	req->cmd[0].error = MEMSTICK_ERR_TIMEOUT;
	memstick_request_done(msh, req, 0);
}

static void tifm_ms_end_request(unsigned long data)
{
	struct tifm_ms *host = (struct tifm_ms*)data;
	struct tifm_dev *sock = host->dev;
	struct memstick_host *msh = tifm_get_drvdata(sock);
	struct memstick_request *req;
	unsigned int cnt = 0, val = 0;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);

	del_timer(&host->timer);
	req = host->req;
	host->req = NULL;

	if (!req) {
		printk(KERN_ERR " %s : no request to complete?\n",
		       sock->dev.bus_id);
		spin_unlock_irqrestore(&sock->lock, flags);
		return;
	}

	writel((~TIFM_MS_SYS_DATA) & readl(sock->addr + SOCK_MS_SYSTEM),
	       sock->addr + SOCK_MS_SYSTEM);

	if ((req->short_data_dir == READ) && !req->error) {
		while (cnt < req->short_data_len) {
			val = readl(sock->addr + SOCK_MS_DATA);
			if (req->short_data_len - cnt >= 4) {
				*(unsigned int*)(req->short_data + cnt)
					= cpu_to_le32(val);
				cnt += 4;
			} else {
				req->short_data[cnt++] = val & 0xff;
				if (cnt == req->short_data_len)
					break;
				req->short_data[cnt++] = (val >> 8) & 0xff;
				if (cnt == req->short_data_len)
					break;
				req->short_data[cnt++] = (val >> 16) & 0xff;
					break;
			}
		}
	}

	if (req->blocks) {
		if (!host->no_dma)
			tifm_unmap_sg(sock, req->sg, req->sg_len,
				      req->data_dir == WRITE
				      ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE);
	}
	writel((~TIFM_CTRL_LED) & readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);

	spin_unlock_irqrestore(&sock->lock, flags);

	memstick_request_done(msh, req, s_count);
}

static void tifm_ms_ios(struct memstick_host *msh, struct memstick_ios *ios)
{
	struct tifm_ms *host = memstick_priv(msh);
	struct tifm_dev *sock = host->dev;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);
	if (ios->interface == MEMSTICK_SERIAL) {
		host->mode_mask = TIFM_MS_SERIAL;
		writel((~TIFM_CTRL_FAST_CLK)
		       & readl(sock->addr + SOCK_CONTROL),
		       sock->addr + SOCK_CONTROL);
	} else if (ios->interface == MEMSTICK_PARALLEL) {
		host->mode_mask = 0;
		writel(TIFM_CTRL_FAST_CLK
		       | readl(sock->addr + SOCK_CONTROL),
		       sock->addr + SOCK_CONTROL);
	}
	spin_unlock_irqrestore(&sock->lock, flags);
}

static void tifm_ms_abort(unsigned long data)
{
	struct tifm_ms *host = (struct tifm_ms*)data;

	printk(KERN_ERR
	       "%s : card failed to respond for a long period of time "
	       "(%x, %x)\n",
	       host->dev->dev.bus_id, host->req->tpc, host->cmd_flags);

	tifm_eject(host->dev);
}

static int tifm_ms_initialize_host(struct tifm_ms *host)
{
	struct tifm_dev *sock = host->dev;

	host->mode_mask = 0x4010;
	writel(0x8000, sock->addr + SOCK_MS_SYSTEM);
	writel(0x0a00, sock->addr + SOCK_MS_SYSTEM);
	writel(0xffffffff, sock->addr + SOCK_MS_STATUS);
	/* block size is always 512B */
	writel(7, sock->addr + SOCK_FIFO_PAGE_SIZE);
	return 0;
}

static int tifm_ms_probe(struct tifm_dev *sock)
{
	struct memstick_host *msh;
	struct tifm_ms *host;
	int rc = -EIO;

	if (!(TIFM_SOCK_STATE_OCCUPIED
	      & readl(sock->addr + SOCK_PRESENT_STATE))) {
		printk(KERN_WARNING "%s : card gone, unexpectedly\n",
		       sock->dev.bus_id);
		return rc;
	}

	msh = memstick_alloc_host(sizeof(struct tifm_ms), &sock->dev);
	if (!msh)
		return -ENOMEM;

	host = memstick_priv(msh);
	tifm_set_drvdata(sock, msh);
	host->dev = sock;
	host->timeout_jiffies = msecs_to_jiffies(1000);
	host->no_dma = no_dma;
	tasklet_init(&host->finish_tasklet, tifm_ms_end_request,
		     (unsigned long)host);

	setup_timer(&host->timer, tifm_ms_abort, (unsigned long)host);

	msh->request = tifm_ms_request;
	msh->set_ios = tifm_ms_ios;
	sock->event = tifm_ms_event;
	sock->data_event = tifm_ms_data_event;
	rc = tifm_ms_initialize_host(host);

	if (!rc)
		rc = memstick_add_host(msh);
	if (!rc)
		return 0;

	memstick_free_host(msh);
	return rc;
}

static void tifm_ms_remove(struct tifm_dev *sock)
{
	struct memstick_host *msh = tifm_get_drvdata(sock);
	struct tifm_ms *host = memstick_priv(msh);
	unsigned long flags;

	tasklet_kill(&host->finish_tasklet);
	spin_lock_irqsave(&sock->lock, flags);
	host->eject = 1;
	if (host->req) {
		writel(TIFM_FIFO_INT_SETALL,
		       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
		writel(0, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_SET);
		host->req->error = MEMSTICK_ERR_TIMEOUT;
		tasklet_schedule(&host->finish_tasklet);
	}
	spin_unlock_irqrestore(&sock->lock, flags);

	memstick_remove_host(msh);

	writel(0x0a00, sock->addr + SOCK_MS_SYSTEM);
	writel(0xffffffff, sock->addr + SOCK_MS_STATUS);

	/* The meaning of the bit majority in this constant is unknown. */
	writel(0xfff8 & readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);

	memstick_free_host(msh);
}

#define tifm_ms_suspend NULL
#define tifm_ms_resume NULL

static struct tifm_device_id tifm_ms_id_tbl[] = {
	{ TIFM_TYPE_MS }, { 0 }
};

static struct tifm_driver tifm_ms_driver = {
	.driver = {
		.name  = DRIVER_NAME,
		.owner = THIS_MODULE
	},
	.id_table = tifm_ms_id_tbl,
	.probe    = tifm_ms_probe,
	.remove   = tifm_ms_remove,
	.suspend  = tifm_ms_suspend,
	.resume   = tifm_ms_resume
};

static int __init tifm_ms_init(void)
{
	return tifm_register_driver(&tifm_ms_driver);
}

static void __exit tifm_ms_exit(void)
{
	tifm_unregister_driver(&tifm_ms_driver);
}

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("TI FlashMedia MemoryStick driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(tifm, tifm_ms_id_tbl);
MODULE_VERSION(DRIVER_VERSION);

module_init(tifm_ms_init);
module_exit(tifm_ms_exit);
