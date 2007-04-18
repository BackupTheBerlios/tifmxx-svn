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
#include <linux/log2.h>
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

/* Hardware flags */
enum {
	CMD_READY  = 0x0001,
	FIFO_READY = 0x0002,
	CARD_READY = 0x0004,
	DATA_CARRY = 0x0008
};

struct tifm_ms {
	struct tifm_dev         *dev;
	unsigned short          eject:1,
				no_dma:1;
	unsigned short          cmd_flags;
	unsigned int            mode_mask;
	unsigned long           timeout_jiffies;

	struct timer_list       timer;
	struct memstick_request *req;
	unsigned int            io_word;
};

void tifm_ms_read_fifo(struct tifm_ms *host, unsigned int fifo_offset,
		       struct page *pg, unsigned int page_off,
		       unsigned int length)
{
	struct tifm_dev *sock = host->dev;
	unsigned int cnt = 0, off = 0;
	unsigned char *buf = kmap_atomic(pg, KM_BIO_DST_IRQ) + page_off;

	if (host->cmd_flags & DATA_CARRY) {
		while ((fifo_offset & 3) && length) {
			buf[off++] = host->io_word & 0xff;
			host->io_word >>= 8;
			length--;
			fifo_offset++;
		}
		if (!(fifo_offset & 3))
			host->cmd_flags &= ~DATA_CARRY;
		if (!length)
			return;
	}

	do {
		host->io_word = readl(sock + SOCK_FIFO_ACCESS + fifo_offset);
		cnt = 4;
		while (length && cnt) {
			buf[off++] = (host->io_word >> 8) & 0xff;
			cnt--;
			length--;
		}
		fifo_offset += 4 - cnt;
	} while (length);

	if (cnt)
		host->cmd_flags |= DATA_CARRY;

	kunmap_atomic(buf - page_off, KM_BIO_DST_IRQ);
}

void tifm_ms_write_fifo(struct tifm_ms *host, unsigned int fifo_offset,
			struct page *pg, unsigned int page_off,
			unsigned int length)
{
	struct tifm_dev *sock = host->dev;
	unsigned int cnt = 0, off = 0;
	unsigned char *buf = kmap_atomic(pg, KM_BIO_SRC_IRQ) + page_off;

	if (host->cmd_flags & DATA_CARRY) {
		while (fifo_offset & 3) {
			host->io_word |= buf[off++] << (8 * (fifo_offset & 3));
			length--;
			fifo_offset++;
		}
		if (!(fifo_offset & 3)) {
			writel(host->io_word, sock + SOCK_FIFO_ACCESS
			       + fifo_offset - 4);

			host->cmd_flags &= ~DATA_CARRY;
		}
		if (!length)
			return;
	}

	do {
		cnt = 4;
		host->io_word = 0;
		while (length && cnt) {
			host->io_word |= buf[off++] << (4 - cnt);
			cnt--;
			length--;
		}
		fifo_offset += 4 - cnt;
		if (!cnt)
			writel(host->io_word, sock + SOCK_FIFO_ACCESS
			       + fifo_offset - 4);

	} while (length);

	if (cnt)
		host->cmd_flags |= DATA_CARRY;

	kunmap_atomic(buf - page_off, KM_BIO_SRC_IRQ);
}

static void tifm_ms_move_block(struct tifm_ms *host)
{
	unsigned int length = host->req->sg.length, t_size;
	unsigned int off = host->req->sg.offset;
	unsigned int p_off, p_cnt;
	struct page *pg;
	unsigned long flags;

	dev_dbg(&host->dev->dev, "moving block\n");
	local_irq_save(flags);
	t_size = length;
	while (t_size) {
		pg = nth_page(host->req->sg.page, off >> PAGE_SHIFT);
		p_off = offset_in_page(off);
		p_cnt = PAGE_SIZE - p_off;
		p_cnt = min(p_cnt, t_size);

		if (host->req->data_dir == WRITE)
			tifm_ms_write_fifo(host, length - t_size,
					   pg, p_off, p_cnt);
		else
			tifm_ms_read_fifo(host, length - t_size,
					  pg, p_off, p_cnt);

		t_size -= p_cnt;
	}
	local_irq_restore(flags);
}

int tifm_ms_issue_cmd(struct tifm_ms *host)
{
	struct tifm_dev *sock = host->dev;
	struct memstick_request *req = host->req;
	unsigned int length = 0;
	unsigned int cmd = 0, cmd_mask, cnt, tval = 0;

	host->cmd_flags = 0;

	if (!req)
		return 0;

	if (req->block_io) {
		if (!host->no_dma) {
			if (1 != tifm_map_sg(sock, &req->sg, 1,
					     req->data_dir == READ
					     ? PCI_DMA_TODEVICE
					     : PCI_DMA_FROMDEVICE)) {
				req->error = MEMSTICK_ERR_INTERNAL;
				return req->error;
			}
			length = sg_dma_len(&req->sg);
		} else
			length = req->sg.length;

		writel(TIFM_FIFO_INT_SETALL,
		       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
		writel(ilog2(length) - 2,
		       sock->addr + SOCK_FIFO_PAGE_SIZE);
		writel(TIFM_FIFO_ENABLE,
		       sock->addr + SOCK_FIFO_CONTROL);
		writel(TIFM_FIFO_INTMASK,
		       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_SET);

		if (!host->no_dma) {
			writel(sg_dma_address(&req->sg),
			       sock->addr + SOCK_DMA_ADDRESS);
			if (host->req->data_dir == WRITE)
				writel((1 << 8) | TIFM_DMA_TX | TIFM_DMA_EN,
				       sock->addr + SOCK_DMA_CONTROL);
			else
				writel((1 << 8) | TIFM_DMA_EN,
				       sock->addr + SOCK_DMA_CONTROL);
		} else {
			if (req->data_dir == WRITE)
				tifm_ms_move_block(host);
		}

		cmd_mask = readl(sock + SOCK_MS_SYSTEM);
		cmd_mask |= TIFM_MS_SYS_DATA | TIFM_MS_SYS_NOT_RDY;
		writel(cmd_mask, sock + SOCK_MS_SYSTEM);
	} else {
		length = req->length;
		cmd_mask = host->mode_mask | 0x2607; /* unknown constant */

		if (req->data_dir == WRITE) {
			cmd_mask |= TIFM_MS_SYS_LATCH;
			writel(cmd_mask, sock + SOCK_MS_SYSTEM);
			for (cnt = 0; (length - cnt) >= 4; cnt += 4) {
				writel(TIFM_MS_SYS_LATCH
				       | readl(sock + SOCK_MS_SYSTEM),
				       sock + SOCK_MS_SYSTEM);
				writel(*(int*)(req->data + cnt),
				       sock + SOCK_MS_DATA);
				dev_dbg(&sock->dev, "writing %x\n", *(int*)(req->data + cnt));
			}
			switch (length - cnt) {
			case 3:
				tval |= (unsigned char)req->data[cnt + 2] << 16;
			case 2:
				tval |= (unsigned char)req->data[cnt + 1] << 8;
			case 1:
				tval = (unsigned char)req->data[cnt];
				writel(TIFM_MS_SYS_LATCH
				       | readl(sock + SOCK_MS_SYSTEM),
				       sock + SOCK_MS_SYSTEM);
				writel(tval, sock + SOCK_MS_DATA);
				dev_dbg(&sock->dev, "writing %x\n", tval);
			}
			writel(TIFM_MS_SYS_LATCH
			       | readl(sock + SOCK_MS_SYSTEM),
			       sock + SOCK_MS_SYSTEM);
			writel(0, sock + SOCK_MS_DATA);
			dev_dbg(&sock->dev, "writing %x\n", 0);
		} else 
			writel(cmd_mask, sock + SOCK_MS_SYSTEM);

		cmd_mask = readl(sock + SOCK_MS_SYSTEM);
		cmd_mask &= ~TIFM_MS_SYS_DATA;
		cmd_mask |= TIFM_MS_SYS_NOT_RDY;
		writel(cmd_mask, sock + SOCK_MS_SYSTEM);
	}


	mod_timer(&host->timer, jiffies + host->timeout_jiffies);
	writel(TIFM_CTRL_LED | readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);
	cmd = (host->req->tpc & 0xf) << 12;
	cmd |= length;
	writel(cmd, sock->addr + SOCK_MS_COMMAND);
	dev_dbg(&sock->dev, "executing TPC %x, %x\n", cmd, cmd_mask);
	return 0;
}

void tifm_ms_complete_cmd(struct tifm_ms *host)
{
	struct tifm_dev *sock = host->dev;
	struct memstick_host *msh = tifm_get_drvdata(sock);
	struct memstick_request *req = host->req;
	unsigned int cnt, tval = 0;

	del_timer(&host->timer);
	if (req->block_io) {
		if (!host->no_dma)
			tifm_unmap_sg(sock, &req->sg, 1,
				      req->data_dir == READ
				      ? PCI_DMA_TODEVICE
				      : PCI_DMA_FROMDEVICE);

		else if (req->data_dir == READ)
			tifm_ms_move_block(host);
	} else {
		writel(~TIFM_MS_SYS_DATA & readl(sock->addr + SOCK_MS_SYSTEM),
		       sock->addr + SOCK_MS_SYSTEM);

		if (req->data_dir == READ) {
			for (cnt = 0; (req->length - cnt) >= 4; cnt += 4)
				*(int*)(req->data + cnt)
					= readl(sock->addr + SOCK_MS_DATA);

			if (req->length - cnt)
				tval = readl(sock->addr + SOCK_MS_DATA);
			switch (req->length - cnt) {
			case 3:
				req->data[cnt + 2] = (tval >> 16) & 0xff;
			case 2:
				req->data[cnt + 1] = (tval >> 8) & 0xff;
			case 1:
				req->data[cnt] = tval & 0xff;
			}
		}
	}

	writel((~TIFM_CTRL_LED) & readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);
	host->req = memstick_next_req(msh, host->req);
	while (tifm_ms_issue_cmd(host))
		host->req = memstick_next_req(msh, host->req);
}

static int tifm_ms_check_status(struct tifm_ms *host)
{
	if (host->req->error == MEMSTICK_ERR_NONE) {
		if (!(host->cmd_flags & CMD_READY))
			return 1;
		if (host->req->block_io && !(host->cmd_flags & FIFO_READY))
			return 1;
		if (host->req->need_card_int
		    && !(host->cmd_flags & CARD_READY))
			return 1;
	}
	return 0;
}

/* Called from interrupt handler */
static void tifm_ms_data_event(struct tifm_dev *sock)
{
	struct tifm_ms *host;
	unsigned int fifo_status = 0;
	int rc = 1;

	spin_lock(&sock->lock);
	host = memstick_priv((struct memstick_host*)tifm_get_drvdata(sock));
	fifo_status = readl(sock->addr + SOCK_DMA_FIFO_STATUS);
	dev_dbg(&sock->dev, "data event: fifo_status %x, flags %x\n",
		fifo_status, host->cmd_flags);

	if (host->req) {
		if (fifo_status & TIFM_FIFO_READY) {
			host->cmd_flags |= FIFO_READY;
			rc = tifm_ms_check_status(host);
		}
	}

	writel(fifo_status, sock->addr + SOCK_DMA_FIFO_STATUS);
	if (!rc)
		tifm_ms_complete_cmd(host);

	spin_unlock(&sock->lock);
}


/* Called from interrupt handler */
static void tifm_ms_card_event(struct tifm_dev *sock)
{
	struct tifm_ms *host;
	unsigned int host_status = 0;
	int rc = 1;

	spin_lock(&sock->lock);
	host = memstick_priv((struct memstick_host*)tifm_get_drvdata(sock));
	host_status = readl(sock->addr + SOCK_MS_STATUS);
	dev_dbg(&sock->dev, "host event: host_status %x, flags %x\n",
		host_status, host->cmd_flags);

	if (host->req) {
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

		rc = tifm_ms_check_status(host);

	}
	
	if (rc)
		writel(TIFM_MS_SYS_NOT_RDY | readl(sock->addr + SOCK_MS_SYSTEM),
		       sock->addr + SOCK_MS_SYSTEM);
	else
		tifm_ms_complete_cmd(host);

	spin_unlock(&sock->lock);
	return;
}

void tifm_ms_request(struct memstick_host *msh, struct memstick_request *mrq)
{
	struct tifm_ms *host = memstick_priv(msh);
	struct tifm_dev *sock = host->dev;
	unsigned long flags;

	dev_dbg(&sock->dev, "request %d\n", mrq->tpc);
	spin_lock_irqsave(&sock->lock, flags);
	if (host->eject) {
		spin_unlock_irqrestore(&sock->lock, flags);
		while (mrq) {
			mrq->error = MEMSTICK_ERR_TIMEOUT;
			mrq = memstick_next_req(msh, mrq);
		}
		return;
	}

	if (host->req) {
		printk(KERN_ERR "%s : unfinished request detected\n",
		       sock->dev.bus_id);
		spin_unlock_irqrestore(&sock->lock, flags);
		tifm_eject(host->dev);
		return;
	}

	host->req = mrq;
	while (tifm_ms_issue_cmd(host))
		host->req = memstick_next_req(msh, host->req);
	spin_unlock_irqrestore(&sock->lock, flags);
	return;
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
	} else if (ios->interface == MEMSTICK_PARALLEL
		   && tifm_has_ms_pif(sock)) {
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

	host->mode_mask = TIFM_MS_SERIAL;
	writel(0x8000, sock->addr + SOCK_MS_SYSTEM);
	writel(0x0200 | TIFM_MS_SYS_NOT_RDY, sock->addr + SOCK_MS_SYSTEM);
	writel(0xffffffff, sock->addr + SOCK_MS_STATUS);
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

	setup_timer(&host->timer, tifm_ms_abort, (unsigned long)host);

	msh->request = tifm_ms_request;
	msh->set_ios = tifm_ms_ios;
	sock->card_event = tifm_ms_card_event;
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
	struct memstick_request *mrq;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);
	host->eject = 1;
	mrq = host->req;
	host->req = NULL;
	if (mrq) {
		del_timer(&host->timer);
		writel(TIFM_FIFO_INT_SETALL,
		       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
		writel(TIFM_DMA_RESET, sock->addr + SOCK_DMA_CONTROL);
		if (mrq->block_io && !host->no_dma)
			tifm_unmap_sg(sock, &mrq->sg, 1,
				      mrq->data_dir == READ
				      ? PCI_DMA_TODEVICE
				      : PCI_DMA_FROMDEVICE);
	}
	spin_unlock_irqrestore(&sock->lock, flags);

	while (mrq) {
		mrq->error = MEMSTICK_ERR_TIMEOUT;
		mrq = memstick_next_req(msh, mrq);
	};

	memstick_remove_host(msh);

	writel(0x0200 | TIFM_MS_SYS_NOT_RDY, sock->addr + SOCK_MS_SYSTEM);
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
