/*
 *  tifm_sd.c - TI FlashMedia driver
 *
 *  Copyright (C) 2006 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */


#include "tifm.h"
#include <linux/mmc/protocol.h>
#include <linux/mmc/host.h>
#include <linux/highmem.h>

#define DRIVER_NAME "tifm_sd"
#define DRIVER_VERSION "0.5"

static int no_dma = 0;
module_param(no_dma, bool, 0644);

enum { CARD_EVENT = 0x0001, FIFO_EVENT = 0x0002, EJECT_EVENT = 0x0004,
       FLAG_W2 = 0x0008, CARD_RO = 0x0010, HOST_REG = 0x0020};

typedef enum { IDLE = 0, W_RESP, W_BRS, W_STOP_RESP, W_FIFO, W_CARD, W_WRITE,
	       W_READ, READY} card_state_t;

struct tifm_sd {
	struct tifm_dev     *dev;

	wait_queue_head_t   event;
	unsigned int        flags;
	unsigned int        status;
	unsigned int        fifo_status;
	card_state_t        state;
	unsigned int        clk_freq;
	unsigned int        clk_div;
	unsigned long       timeout_jiffies;

	struct mmc_request    *req;
	struct work_struct    cmd_handler;
	struct work_struct    abort_handler;

	char                  *buffer;
	unsigned long         buffer_size;
	unsigned long         buffer_pos;

};

static inline unsigned int tifm_sd_op_flags(unsigned int cmd_flags)
{
	unsigned int rc = 0;

	switch (cmd_flags & 0x1f) {
		case MMC_RSP_R1:
			rc |= 0x0100;
			break;
		case MMC_RSP_R1B:
			rc |= 0x0900;
			break;
		case MMC_RSP_R2:
			rc |= 0x0200;
			break;
		case MMC_RSP_R3:
			rc |= 0x0300;
			break;
		case MMC_RSP_R6:
			rc |= 0x0600;
			break;
	}

	switch (cmd_flags & MMC_CMD_MASK) {
		case MMC_CMD_AC:
			rc |= 0x2000;
			break;
		case MMC_CMD_ADTC:
			rc |= 0x3000;
			break;
		case MMC_CMD_BCR:
			rc |= 0x1000;
			break;
	}
	return rc;
}

static void tifm_sd_exec(struct tifm_sd *card, struct mmc_command *cmd)
{
	struct tifm_dev *sock = card->dev;
	unsigned int cmd_mask = tifm_sd_op_flags(cmd->flags);

	if (cmd->data) {
		if (((cmd->flags & MMC_CMD_MASK) == MMC_CMD_ADTC)
		    && (cmd->data->flags & MMC_DATA_READ))
			cmd_mask |= 0x8000;
	}
	DBG("executing opcode 0x%x, arg: 0x%x, mask: 0x%x\n", cmd->opcode, cmd->arg, cmd_mask);

	writel((cmd->arg >> 16) & 0xffff, sock->addr + SOCK_MMCSD_ARG_HIGH);
	writel(cmd->arg & 0xffff, sock->addr + SOCK_MMCSD_ARG_LOW);
	card->flags &= ~CARD_EVENT;
	writel(cmd->opcode | cmd_mask, sock->addr + SOCK_MMCSD_COMMAND);
}

static inline void tifm_sd_fetch_resp(struct mmc_command *cmd, struct tifm_dev *sock)
{
	cmd->resp[0] = readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x1c); cmd->resp[0] <<= 16;
	cmd->resp[0] |= readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x18);
	cmd->resp[1] = readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x14); cmd->resp[1] <<= 16;
	cmd->resp[1] |= readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x10);
	cmd->resp[2] = readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x0c); cmd->resp[2] <<= 16;
	cmd->resp[2] |= readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x08);
	cmd->resp[3] = readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x04); cmd->resp[3] <<= 16;
	cmd->resp[3] |= readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x00);
}

static inline void tifm_sd_process_cmd(struct tifm_sd *card)
{
	int err_code = 0;
	struct mmc_command *cmd;
	struct tifm_dev *sock = card->dev;
	unsigned int t_val;

	if (card->state == IDLE) {
		DBG("idle interrupt %x, %x, %x\n", card->flags, card->status, card->fifo_status);
		return;
	}
	
	cancel_delayed_work(&card->abort_handler);
	
	cmd = card->req->cmd;
	
change_state:
	if (card->status & 0x4000)
		err_code = MMC_ERR_FAILED;
	if (card->status & 0x0080)
		err_code = MMC_ERR_TIMEOUT;
	if (card->status & 0x0100)
		err_code = MMC_ERR_BADCRC;

	if (err_code || cmd->error) {
		if (card->state == W_STOP_RESP) {
			card->req->stop->error = err_code;
			if (card->req->data && !card->buffer) { // dma transfer
				writel(0xffff, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
				writel(0x0002, sock->addr + SOCK_DMA_CONTROL);
			}
			card->state = READY;
		} else if (card->state == W_BRS) {
			writel(0xffff, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
			writel(0x0002, sock->addr + SOCK_DMA_CONTROL);
			cmd->error = err_code;
			if (card->req->stop) {
				card->state = W_STOP_RESP;
				tifm_sd_exec(card, card->req->stop);
				queue_delayed_work(sock->wq, &card->abort_handler, card->timeout_jiffies);
				return;
			}
			else
				card->state = READY;
		} else {
			cmd->error = err_code;
			if (card->req->data && !card->buffer) { // dma transfer
				writel(0xffff, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
				writel(0x0002, sock->addr + SOCK_DMA_CONTROL);
			}
			card->state = READY;
		}
	}

	DBG("state: %d, error: %d\n", card->state, err_code);
	switch (card->state) {
		case IDLE:
			break;
		case W_RESP:
			if (card->status & 1) {
				tifm_sd_fetch_resp(cmd, sock);
				if (!cmd->data) {
					card->state = READY;
				} else if (!card->buffer) {
					card->state = W_BRS;
				} else if (cmd->data->flags & MMC_DATA_READ) {
					card->state = W_READ;
				} else {
					writel(0x0014 | readl(sock->addr + SOCK_MMCSD_INT_ENABLE),
					       sock->addr + SOCK_MMCSD_INT_ENABLE);
					card->flags |= FLAG_W2;
					card->state = W_WRITE;
				}
			}
			if (card->state != W_RESP)
				goto change_state;
			break;
		case W_BRS:
			if (card->status & 8) {
				if (card->req->stop) {
					card->state = W_STOP_RESP;
					tifm_sd_exec(card, card->req->stop);
					break;
				} else if (cmd->error) {
					card->state = READY;
				} else {
					card->state = (cmd->data->flags & MMC_DATA_WRITE)
						      ? W_CARD : W_FIFO;
				}
				goto change_state;
			}
			break;
		case W_STOP_RESP:
			if (card->status & 1) {
				tifm_sd_fetch_resp(card->req->stop, sock);
				card->state = card->buffer ? READY
					      : ((cmd->data->flags & MMC_DATA_WRITE)
						 ? W_CARD : W_FIFO);
				goto change_state;
			}
			break;
		case W_CARD:
			if (card->flags & FLAG_W2) {
				writel(readl(card->dev->addr + SOCK_MMCSD_INT_ENABLE) & 0xffffffeb,
				       card->dev->addr + SOCK_MMCSD_INT_ENABLE);
				if (!card->buffer) {
					card->state = W_FIFO;
				} else if (card->req->stop) {
					card->state = W_STOP_RESP;
					tifm_sd_exec(card, card->req->stop);
					break;
				} else {
					card->state = READY;
				}
				goto change_state;
			}
			break;
		case W_FIFO:
			if (card->fifo_status & 1) {
				card->state = READY;
				card->fifo_status = 0;
				goto change_state;
			}
			break;
		case W_WRITE:
			if (card->status & 0x0800) {
				card->status &= 0xfffff7ff;
				if (card->buffer_size > card->buffer_pos) {
					t_val = card->buffer[card->buffer_pos++] & 0x00ff;
					if (card->buffer_size > card->buffer_pos)
						t_val |= ((card->buffer[card->buffer_pos++]) << 8) & 0xff00;
					writel(t_val, sock->addr + SOCK_MMCSD_DATA);
				}
				if (card->buffer_size == card->buffer_pos) {
					card->state = W_CARD;
					goto change_state;
				}
			}
			break;
		case W_READ:
			if (card->status & 0x0400) {
				card->status &= 0xfffffbff;
				t_val = readl(sock->addr + SOCK_MMCSD_DATA);
				if (card->buffer_size > card->buffer_pos)
					card->buffer[card->buffer_pos++] = t_val & 0xff;
				if (card->buffer_size > card->buffer_pos)
					card->buffer[card->buffer_pos++] = (t_val >> 8) & 0xff;

				if (card->buffer_size == card->buffer_pos) {
					if (card->req->stop) {
						card->state = W_STOP_RESP;
						tifm_sd_exec(card, card->req->stop);
						break;
					}
					else card->state = READY;
					goto change_state;
				}
			}
			break;
		case READY:
			queue_work(sock->wq, &card->cmd_handler);
			card->state = IDLE;
			return;
	}

	queue_delayed_work(sock->wq, &card->abort_handler, card->timeout_jiffies);
}

/* Called from interrupt handler */
static unsigned int tifm_sd_signal_irq(struct tifm_dev *sock, unsigned int sock_irq_status)
{
	struct tifm_sd *card = mmc_priv((struct mmc_host*)tifm_get_drvdata(sock));
	unsigned int card_status;

	spin_lock(&sock->lock);
	card->flags &= ~(CARD_EVENT | FIFO_EVENT); 
	card->flags |= (sock_irq_status & 0x00000100) ? CARD_EVENT : 0;
	card->flags |= (sock_irq_status & 0x00010000) ? FIFO_EVENT : 0;

	if (card->flags & FIFO_EVENT) {
		card->fifo_status = readl(sock->addr + SOCK_DMA_FIFO_STATUS);
		writel(card->fifo_status, sock->addr + SOCK_DMA_FIFO_STATUS);
	}

	if (card->flags & CARD_EVENT) {
		card_status = readl(sock->addr + SOCK_MMCSD_STATUS);
		writel(card_status, sock->addr + SOCK_MMCSD_STATUS);
		card->status |= card_status;

		if (card_status & 0x8) {
			writel(0x0014 | readl(sock->addr + SOCK_MMCSD_INT_ENABLE),
			       sock->addr + SOCK_MMCSD_INT_ENABLE);
			card->flags |= FLAG_W2;
		}

		if (card_status & 0x0010)
			card->flags &= ~FLAG_W2;
		if (card_status & 0x0004)
			card->flags |= FLAG_W2;
        }

	if (!(card->flags & EJECT_EVENT) && (card->flags & (CARD_EVENT | FIFO_EVENT))) {
		if (!(card->flags & HOST_REG)) {
			cancel_delayed_work(&card->abort_handler);
			queue_work(sock->wq, &card->cmd_handler);
		}
		else
			tifm_sd_process_cmd(card);
		card->flags &= ~CARD_EVENT;
	}
	spin_unlock(&sock->lock);
	DBG("after irq: card_status = 0x%08x, fifo_status = 0x%08x, flags = 0x%08x\n",
	    card->status, card->fifo_status, card->flags);
	return sock_irq_status;
}


static inline void tifm_sd_prepare_data(struct tifm_sd *card, struct mmc_command *cmd)
{
	struct tifm_dev *sock = card->dev;
	unsigned int dest_cnt;
	
	/* DMA style IO */

	writel(0xffff, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
	writel(0x0001, sock->addr + SOCK_FIFO_CONTROL);
	writel(0x0005, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_SET);
	
	dest_cnt = cmd->data->blocks << 8;
	
	writel(sg_dma_address(cmd->data->sg), sock->addr + SOCK_DMA_ADDRESS);
	
	if (cmd->data->flags & MMC_DATA_WRITE) {
		writel((readl(sock->addr + SOCK_DMA_CONTROL) & 0x80)
		       | dest_cnt | 0x8001, sock->addr + SOCK_DMA_CONTROL);
		writel(0x0080, sock->addr + SOCK_MMCSD_BUFFER_CONFIG);
	}
	else {
		writel((readl(sock->addr + SOCK_DMA_CONTROL) & 0x80)
		       | dest_cnt | 0x0001, sock->addr + SOCK_DMA_CONTROL);
		writel(0x8000, sock->addr + SOCK_MMCSD_BUFFER_CONFIG);
	}

	writel(cmd->data->blocks - 1, sock->addr + SOCK_MMCSD_NUM_BLOCKS);
	writel((1 << cmd->data->blksz_bits) - 1, sock->addr + SOCK_MMCSD_BLOCK_LEN);

	card->status &= 0xfffffff7;
	card->fifo_status = 0;
}

static void tifm_sd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct tifm_sd *card = mmc_priv(mmc);
	struct tifm_dev *sock = card->dev;
	struct mmc_host *host = tifm_get_drvdata(sock);
	unsigned long flags;
	int sg_count = 0;
	struct mmc_data *r_data = mrq->cmd->data;
	char *t_buffer = 0;

	if (r_data && (!mrq->stop || no_dma)) {
		t_buffer = kmap(r_data->sg->page);
		if (!t_buffer) {
			printk(KERN_ERR "tifm_sd: kmap failed\n");
			goto err_out;
		}
	}
	
	spin_lock_irqsave(&sock->lock, flags);
	if (card->flags & EJECT_EVENT) {
		spin_unlock_irqrestore(&sock->lock, flags);
		goto err_out;
	}

	if (card->req) {
		printk(KERN_ERR "tifm_sd: unfinished request detected\n");
		spin_unlock_irqrestore(&sock->lock, flags);
		goto err_out;
	}

	if (r_data) {
		if (!mrq->stop || no_dma) {
			card->buffer = t_buffer + r_data->sg->offset;
			writel(0x0000, sock->addr + SOCK_MMCSD_BUFFER_CONFIG);
			writel(0x0c00 | readl(sock->addr + SOCK_MMCSD_INT_ENABLE), sock->addr + SOCK_MMCSD_INT_ENABLE);
			card->buffer_size = mrq->cmd->data->blocks << mrq->cmd->data->blksz_bits;
			card->buffer_pos = 0;
			writel(r_data->blocks - 1, sock->addr + SOCK_MMCSD_NUM_BLOCKS);
			writel((1 << r_data->blksz_bits) - 1, sock->addr + SOCK_MMCSD_BLOCK_LEN);		
			card->status &= 0xfffff3ff;
		} else  {
			sg_count = tifm_map_sg(sock, r_data->sg, r_data->sg_len,
				       mrq->cmd->flags & MMC_DATA_WRITE
				       ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE);
			if (sg_count != 1) {
				printk(KERN_ERR "tifm_sd: scatterlist map failed\n");
				spin_unlock_irqrestore(&sock->lock, flags);
				goto err_out;
			}

			tifm_sd_prepare_data(card, mrq->cmd);
		}
	}
	card->status = 0;
	card->fifo_status = 0;
	card->state = W_RESP;
	card->req = mrq;
	queue_delayed_work(sock->wq, &card->abort_handler, card->timeout_jiffies);
	writel(readl(sock->addr + SOCK_CONTROL) | 0x00000040, sock->addr + SOCK_CONTROL);
	tifm_sd_exec(card, mrq->cmd);
	spin_unlock_irqrestore(&sock->lock, flags);
	return;

err_out:
	if (t_buffer)
		kunmap(r_data->sg->page);
	if (sg_count > 0)
		tifm_unmap_sg(sock, r_data->sg, r_data->sg_len,
			      (r_data->flags & MMC_DATA_WRITE)
			      ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE);

	mrq->cmd->error = MMC_ERR_TIMEOUT;
	mmc_request_done(host, mrq);
}

static void tifm_sd_end_cmd(void *data)
{
	struct tifm_sd *card = (struct tifm_sd*)data;
	struct tifm_dev *sock = card->dev;
	struct mmc_host *host = tifm_get_drvdata(sock);
	struct mmc_request *mrq;
	int unmap_type = 0;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);
	mrq = card->req;
	card->req = 0;
	if (!mrq) {
		spin_unlock_irqrestore(&sock->lock, flags);
		printk(KERN_ERR "tifm_sd: no request to complete?\n");
		return;
	}
	if (mrq->cmd->data) {
		 if (card->buffer) {
			writel(0xfffff3ff & readl(sock->addr + SOCK_MMCSD_INT_ENABLE), sock->addr + SOCK_MMCSD_INT_ENABLE);
			unmap_type = 1;
			mrq->cmd->data->bytes_xfered = card->buffer_pos;
			card->buffer = 0;
			card->buffer_pos = 0;
			card->buffer_size = 0;
		} else {
			unmap_type = 2;
			mrq->cmd->data->bytes_xfered = mrq->cmd->data->blocks - readl(sock->addr + SOCK_MMCSD_NUM_BLOCKS) - 1;
			mrq->cmd->data->bytes_xfered <<= mrq->cmd->data->blksz_bits;
			mrq->cmd->data->bytes_xfered += (1 << mrq->cmd->data->blksz_bits) - readl(sock->addr + SOCK_MMCSD_BLOCK_LEN) + 1;
		}
	}
	writel(readl(sock->addr + SOCK_CONTROL) & 0xffffffbf, sock->addr + SOCK_CONTROL);

	spin_unlock_irqrestore(&sock->lock, flags);

	if (unmap_type == 1)
		kunmap(mrq->cmd->data->sg->page);
	else if (unmap_type == 2)
		tifm_unmap_sg(sock, mrq->cmd->data->sg, mrq->cmd->data->sg_len,
			      (mrq->cmd->data->flags & MMC_DATA_WRITE)
			      ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE);
	mmc_request_done(host, mrq);
}

static void tifm_sd_abort(void *data)
{
	printk(KERN_ERR "tifm_sd: card failed to respond for a long period of time");
	tifm_eject(((struct tifm_sd*)data)->dev);
}

static void tifm_sd_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct tifm_sd *card = mmc_priv(mmc);
	struct tifm_dev *sock = card->dev;
	unsigned int clk_div1, clk_div2;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);
	
	DBG("Setting bus width %d\n", ios->bus_width);
	if (ios->bus_width == MMC_BUS_WIDTH_4) {
		writel(0x8800 | card->clk_div, sock->addr + SOCK_MMCSD_CONFIG);
	} else {
		writel(0x0800 | card->clk_div, sock->addr + SOCK_MMCSD_CONFIG);
	}

	if (ios->clock) {
		if (!(clk_div1 = 20000000 / ios->clock))
			clk_div1 = 1;
		if (!(clk_div2 = 24000000 / ios->clock))
			clk_div2 = 1;
		if ((20000000 / clk_div1) > ios->clock)
			clk_div1++;
		if ((24000000 / clk_div2) > ios->clock)
			clk_div2++;
		if ((20000000 / clk_div1) > (24000000 / clk_div2)) {
			card->clk_freq = 20000000;
			card->clk_div = clk_div1;
			writel(0xfffffeff & readl(sock->addr + SOCK_CONTROL), sock->addr + SOCK_CONTROL);
		} else {
			card->clk_freq = 24000000;
			card->clk_div = clk_div2;
			writel(0x00000100 | readl(sock->addr + SOCK_CONTROL), sock->addr + SOCK_CONTROL);
		}
	} else {
		card->clk_div = 0;
	}
	writel(card->clk_div | (0xffc0 & readl(sock->addr + SOCK_MMCSD_CONFIG)),
	       sock->addr + SOCK_MMCSD_CONFIG);
	//DBG("Want clock %d, setting %d\n", ios->clock, card->clk_div ? card->clk_freq / card->clk_div : 0);

	//vdd, bus_mode, chip_select
	//power is set before probe / after remove
	
	spin_unlock_irqrestore(&sock->lock, flags);
}

static int tifm_sd_ro(struct mmc_host *mmc)
{
	int rc;
	struct tifm_sd *card = mmc_priv(mmc);
	struct tifm_dev *sock = card->dev;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);
	
	card->flags |= (0x0200 & readl(sock->addr + SOCK_PRESENT_STATE)) ? CARD_RO : 0;
	rc = (card->flags & CARD_RO) ? 1 : 0;
	
	spin_unlock_irqrestore(&sock->lock, flags);
	return rc;
}

static struct mmc_host_ops tifm_sd_ops = {
	.request = tifm_sd_request,
	.set_ios = tifm_sd_ios,
	.get_ro  = tifm_sd_ro
};

static void tifm_sd_register_host(void *data)
{
	struct tifm_sd *card = (struct tifm_sd*)data;
	struct tifm_dev *sock = card->dev;
	struct mmc_host *host = tifm_get_drvdata(sock);
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);
	card->flags |= HOST_REG;
	PREPARE_WORK(&card->cmd_handler, tifm_sd_end_cmd, data);
	spin_unlock_irqrestore(&sock->lock, flags);
	mmc_add_host(host);
}

static int tifm_sd_probe(struct tifm_dev *sock)
{
	struct mmc_host *mmc;
	struct tifm_sd *card;
	int rc = -EIO;

	if (!(0x8 & readl(sock->addr + SOCK_PRESENT_STATE))) {
		printk(KERN_WARNING "tifm_sd: card gone, unexpectedly\n");
		return rc;
	}

	mmc = mmc_alloc_host(sizeof(struct tifm_sd), &sock->dev);
	if (!mmc)
		return -ENOMEM;

	card = mmc_priv(mmc);
	card->dev = sock;
	card->clk_div = 61;
	init_waitqueue_head(&card->event);
	INIT_WORK(&card->cmd_handler, tifm_sd_register_host, (void*)card);
	INIT_WORK(&card->abort_handler, tifm_sd_abort, (void*)card);

	tifm_set_drvdata(sock, mmc);
	sock->signal_irq = tifm_sd_signal_irq;

	card->clk_freq = 20000000;
	card->timeout_jiffies = msecs_to_jiffies(2000);

	mmc->ops = &tifm_sd_ops;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps = MMC_CAP_4_BIT_DATA;
	mmc->f_min = 20000000 / 60;
	mmc->f_max = 24000000;
	mmc->max_hw_segs = 1;
	mmc->max_phys_segs = 1;
	mmc->max_sectors = 64; //6b hw block counter
	mmc->max_seg_size = mmc->max_sectors << 11; //2k maximum hw block length

	writel(0x0000, sock->addr + SOCK_MMCSD_INT_ENABLE);
	writel(0x0002, sock->addr + SOCK_MMCSD_SYSTEM_CONTROL);
	writel(0x000b, sock->addr + SOCK_MMCSD_CONFIG);
/*
	for(rc = 0; rc < 50; rc++) 
		if (1 & readl(sock->addr + SOCK_MMCSD_SYSTEM_STATUS)) {
			rc = 0;
			break;
		}
		msleep(10);
        }

	if (rc) {
		printk(KERN_ERR "tifm_sd: card not ready - probe failed\n");
		mmc_free_host(mmc);
		return -ENODEV;
	}
*/
	writel(0x0000, sock->addr + SOCK_MMCSD_NUM_BLOCKS);
	writel(card->clk_div | 0x0800, sock->addr + SOCK_MMCSD_CONFIG);
	writel(0x8000, sock->addr + SOCK_MMCSD_BUFFER_CONFIG);
	writel(0x41e9, sock->addr + SOCK_MMCSD_INT_ENABLE);
	writel(0x0020 | readl(sock->addr + SOCK_MMCSD_SDIO_MODE_CONFIG),
	       sock->addr + SOCK_MMCSD_SDIO_MODE_CONFIG);
	writel(0x0040, sock->addr + SOCK_MMCSD_COMMAND_TO);
	writel(0x7fff, sock->addr + SOCK_MMCSD_DATA_TO);
	writel(0x0080, sock->addr + SOCK_MMCSD_COMMAND);
	writel(card->clk_div | 0x0800, sock->addr + SOCK_MMCSD_CONFIG);
	
	queue_delayed_work(sock->wq, &card->abort_handler, card->timeout_jiffies);

	return 0;
}

static void tifm_sd_remove(struct tifm_dev *sock)
{
	struct mmc_host *mmc = tifm_get_drvdata(sock);
	struct tifm_sd *card = mmc_priv(mmc);
	struct mmc_data *r_data;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);
	card->flags |= EJECT_EVENT;
	cancel_delayed_work(&card->abort_handler);
	card->state = IDLE;
	spin_unlock_irqrestore(&sock->lock, flags);
	flush_workqueue(sock->wq);
	
	if (card->req) {
		if (card->req->cmd->data) {
    			r_data = card->req->cmd->data;
			if (card->buffer)
				kunmap(r_data->sg->page);
			else
				tifm_unmap_sg(sock, r_data->sg, r_data->sg_len,
					      (r_data->flags & MMC_DATA_WRITE)
					      ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE);
		}
		card->req->cmd->error = MMC_ERR_TIMEOUT;
		mmc_request_done(mmc, card->req);
		msleep(10); //wait for mmc_block to relax (remove when fixed)
	}

	if (card->flags & HOST_REG)
		mmc_remove_host(mmc);

	writel(0xfff8 & readl(sock->addr + SOCK_CONTROL), sock->addr + SOCK_CONTROL);
	writel(0, sock->addr + SOCK_MMCSD_INT_ENABLE);

	tifm_set_drvdata(sock, 0);
	mmc_free_host(mmc);
}

static tifm_media_id tifm_sd_id_tbl[] = {
	FM_SD, 0
};

static struct tifm_driver tifm_sd_driver = {
	.driver = {
		.name  = DRIVER_NAME,
		.owner = THIS_MODULE
	},
	.id_table = tifm_sd_id_tbl,
	.probe    = tifm_sd_probe,
	.remove   = tifm_sd_remove
};

static int __init tifm_sd_init(void)
{
	return tifm_register_driver(&tifm_sd_driver);
}

static void __exit tifm_sd_exit(void)
{
	tifm_unregister_driver(&tifm_sd_driver);
}

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("TI FlashMedia SD driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(tifm, tifm_sd_id_tbl);
MODULE_VERSION(DRIVER_VERSION);

module_init(tifm_sd_init);
module_exit(tifm_sd_exit);
