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


#include "linux/tifm.h"
#include <linux/mmc/protocol.h>
#include <linux/mmc/host.h>
#include <linux/highmem.h>
#include <asm/io.h>

#define DRIVER_NAME "tifm_sd"
#define DRIVER_VERSION "0.8"

static int no_dma = 0;
static int fixed_timeout = 0;
module_param(no_dma, bool, 0644);
module_param(fixed_timeout, bool, 0644);

/* Constants here are mostly from OMAP5912 datasheet */
#define TIFM_MMCSD_RESET      0x0002
#define TIFM_MMCSD_CLKMASK    0x03ff
#define TIFM_MMCSD_POWER      0x0800
#define TIFM_MMCSD_4BBUS      0x8000
#define TIFM_MMCSD_RXDE       0x8000   /* rx dma enable */
#define TIFM_MMCSD_TXDE       0x0080   /* tx dma enable */
#define TIFM_MMCSD_BUFINT     0x0c00   /* set bits: AE, AF */
#define TIFM_MMCSD_DPE        0x0020   /* data timeout counted in kilocycles */
#define TIFM_MMCSD_INAB       0x0080   /* abort / initialize command */
#define TIFM_MMCSD_READ       0x8000

#define TIFM_MMCSD_DATAMASK   0x401d   /* set bits: CERR, EOFB, BRS, CB, EOC */
#define TIFM_MMCSD_ERRMASK    0x01e0   /* set bits: CCRC, CTO, DCRC, DTO     */
#define TIFM_MMCSD_SCMDMASK   0x4015   /* set bits: CERR, EOFB, CB, EOC      */
#define TIFM_MMCSD_EOC        0x0001   /* end of command phase  */
#define TIFM_MMCSD_CD         0x0002   /* card detect           */
#define TIFM_MMCSD_CB         0x0004   /* card enter busy state */
#define TIFM_MMCSD_BRS        0x0008   /* block received/sent   */
#define TIFM_MMCSD_EOFB       0x0010   /* card exit busy state  */
#define TIFM_MMCSD_DTO        0x0020   /* data time-out         */
#define TIFM_MMCSD_DCRC       0x0040   /* data crc error        */
#define TIFM_MMCSD_CTO        0x0080   /* command time-out      */
#define TIFM_MMCSD_CCRC       0x0100   /* command crc error     */
#define TIFM_MMCSD_AF         0x0400   /* fifo almost full      */
#define TIFM_MMCSD_AE         0x0800   /* fifo almost empty     */
#define TIFM_MMCSD_OCRB       0x1000   /* OCR busy              */
#define TIFM_MMCSD_CIRQ       0x2000   /* card irq (cmd40/sdio) */
#define TIFM_MMCSD_CERR       0x4000   /* card status error     */

#define TIFM_MMCSD_FIFO_SIZE  0x0020

#define TIFM_MMCSD_RSP_R0     0x0000
#define TIFM_MMCSD_RSP_R1     0x0100
#define TIFM_MMCSD_RSP_R2     0x0200
#define TIFM_MMCSD_RSP_R3     0x0300
#define TIFM_MMCSD_RSP_R4     0x0400
#define TIFM_MMCSD_RSP_R5     0x0500
#define TIFM_MMCSD_RSP_R6     0x0600

#define TIFM_MMCSD_RSP_BUSY   0x0800

#define TIFM_MMCSD_CMD_BC     0x0000
#define TIFM_MMCSD_CMD_BCR    0x1000
#define TIFM_MMCSD_CMD_AC     0x2000
#define TIFM_MMCSD_CMD_ADTC   0x3000

typedef enum {
	IDLE = 0,
	CMD,    /* main command ended                   */
	BRS,    /* block transfer finished              */
	SCMD,   /* stop command ended                   */
	CARD,   /* card left busy state                 */
	FIFO,   /* FIFO operation completed (uncertain) */
	BUSY,   /* card busy after stop command         */
	READY
} card_state_t;

enum {
	FIFO_RDY   = 0x0001,     /* hardware dependent value */
	EJECT      = 0x0004,
	CARD_BUSY  = 0x0010,
	OPENDRAIN  = 0x0040,     /* hardware dependent value */
	CARD_EVENT = 0x0100,     /* hardware dependent value */
	CARD_RO    = 0x0200,     /* hardware dependent value */
	FIFO_EVENT = 0x10000 };  /* hardware dependent value */

struct tifm_sd {
	struct tifm_dev     *dev;

	unsigned int        flags;
	card_state_t        state;
	unsigned int        clk_freq;
	unsigned int        clk_div;
	unsigned long       timeout_jiffies;

	struct tasklet_struct finish_tasklet;
	struct timer_list     timer;
	struct mmc_request    *req;

	int                   sg_len;
	int                   sg_pos;
	unsigned int          block_pos;
	size_t                written_blocks;
};

/* for some reason, host won't respond correctly to readw/writew */
static void tifm_sd_read_fifo(struct tifm_dev *sock, struct scatterlist *sg,
			      unsigned int off, unsigned int cnt)
{
	unsigned char *buf;
	unsigned int pos = 0, val;

//	dev_dbg(&sock->dev, "read %d from %d\n", w_cnt, w_off);
	if (!cnt)
		return;
	buf = kmap_atomic(sg->page, KM_BIO_SRC_IRQ) + off;
	while (pos < cnt) {
		val = readl(sock->addr + SOCK_MMCSD_DATA);
		buf[pos++] = val & 0xff;
		if (pos == cnt)
			break;
		buf[pos++] = (val >> 8) & 0xff;
	}
	kunmap_atomic(buf - off, KM_BIO_SRC_IRQ);
}

static void tifm_sd_write_fifo(struct tifm_dev *sock, struct scatterlist *sg,
			       unsigned int off, unsigned int cnt)
{
	unsigned char *buf;
	unsigned int pos = 0, val;

	if (!cnt)
		return;
	buf = kmap_atomic(sg->page, KM_BIO_DST_IRQ) + off;
	while (pos < cnt) {
		val = buf[pos++];
		if (pos < cnt)
			val |= buf[pos++] << 8;

		writel(val, sock->addr + SOCK_MMCSD_DATA);
	}
	kunmap_atomic(buf - off, KM_BIO_DST_IRQ);
}

static void tifm_sd_read_data(struct tifm_sd *host, unsigned int host_status)
{
	struct tifm_dev *sock = host->dev;
	struct mmc_command *cmd = host->req->cmd;
	struct scatterlist *sg = cmd->data->sg;
	unsigned int off, cnt, t_size = TIFM_MMCSD_FIFO_SIZE * 2;

	if (host_status & (TIFM_MMCSD_AF | TIFM_MMCSD_BRS)) {
		while (host->sg_pos < host->sg_len) {
			off = sg[host->sg_pos].offset + host->block_pos;
			cnt = sg[host->sg_pos].length - host->block_pos;
			if (cnt < t_size) {
				t_size -= cnt;
				tifm_sd_read_fifo(sock, &sg[host->sg_pos],
						  off, cnt);
				host->sg_pos++; 
				host->block_pos = 0;
				continue;
			} else {
				tifm_sd_read_fifo(sock, &sg[host->sg_pos],
						  off, t_size);
				host->block_pos += t_size;
				break;
			}
		}
	}
}

static void tifm_sd_write_data(struct tifm_sd *host, unsigned int host_status)
{
	struct tifm_dev *sock = host->dev;
	struct mmc_command *cmd = host->req->cmd;
	struct scatterlist *sg = cmd->data->sg;
	unsigned int off, cnt, t_size = TIFM_MMCSD_FIFO_SIZE * 2;

	if (host_status & TIFM_MMCSD_AE) {
		while (host->sg_pos < host->sg_len) {
			off = sg[host->sg_pos].offset + host->block_pos;
			cnt = sg[host->sg_pos].length - host->block_pos;
			if (cnt < t_size) {
				t_size -= cnt;
				tifm_sd_write_fifo(sock, &sg[host->sg_pos],
						   off, cnt);
				host->sg_pos++;
				host->block_pos = 0;
				continue;
			} else {
				tifm_sd_write_fifo(sock, &sg[host->sg_pos],
						   off, t_size);
				host->block_pos += t_size;
				break;
			}
		}
	}
}

static unsigned int tifm_sd_op_flags(struct mmc_command *cmd)
{
	unsigned int rc = 0;

	switch (mmc_resp_type(cmd)) {
	case MMC_RSP_NONE:
		rc |= TIFM_MMCSD_RSP_R0;
		break;
	case MMC_RSP_R1B:
		rc |= TIFM_MMCSD_RSP_BUSY; // deliberate fall-through
	case MMC_RSP_R1:
		rc |= TIFM_MMCSD_RSP_R1;
		break;
	case MMC_RSP_R2:
		rc |= TIFM_MMCSD_RSP_R2;
		break;
	case MMC_RSP_R3:
		rc |= TIFM_MMCSD_RSP_R3;
		break;
	default:
		BUG();
	}
	
	
	switch (mmc_cmd_type(cmd)) {
	case MMC_CMD_BC:
		rc |= TIFM_MMCSD_CMD_BC;
		break;
	case MMC_CMD_BCR:
		rc |= TIFM_MMCSD_CMD_BCR;
		break;
	case MMC_CMD_AC:
		rc |= TIFM_MMCSD_CMD_AC;
		break;
	case MMC_CMD_ADTC:
		rc |= TIFM_MMCSD_CMD_ADTC;
		break;
	default:
		BUG();
	}
	return rc;
}

static void tifm_sd_exec(struct tifm_sd *host, struct mmc_command *cmd)
{
	struct tifm_dev *sock = host->dev;
	unsigned int cmd_mask = tifm_sd_op_flags(cmd) |
				(host->flags & OPENDRAIN);

	if (cmd->data && (cmd->data->flags & MMC_DATA_READ))
		cmd_mask |= TIFM_MMCSD_READ;

	dev_dbg(&sock->dev, "executing opcode 0x%x, arg: 0x%x, mask: 0x%x\n",
		cmd->opcode, cmd->arg, cmd_mask);

	writel((cmd->arg >> 16) & 0xffff, sock->addr + SOCK_MMCSD_ARG_HIGH);
	writel(cmd->arg & 0xffff, sock->addr + SOCK_MMCSD_ARG_LOW);
	writel(cmd->opcode | cmd_mask, sock->addr + SOCK_MMCSD_COMMAND);
}

static void tifm_sd_fetch_resp(struct mmc_command *cmd, struct tifm_dev *sock)
{
	cmd->resp[0] = (readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x1c) << 16)
		       | readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x18);
	cmd->resp[1] = (readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x14) << 16)
		       | readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x10);
	cmd->resp[2] = (readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x0c) << 16)
		       | readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x08);
	cmd->resp[3] = (readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x04) << 16)
		       | readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x00);
}

static void tifm_sd_do_read_cmd(struct tifm_sd *host, unsigned int host_status)
{
	struct tifm_dev *sock = host->dev;
	struct mmc_command *cmd = host->req->cmd;

change_state:
	switch (host->state) {
	case IDLE:
	case CARD:
	case BUSY:
		return;
	case CMD:
		if (host_status & (TIFM_MMCSD_EOC | TIFM_MMCSD_CERR)) {
			tifm_sd_fetch_resp(cmd, sock);
			if (cmd->data) {
				host->state = no_dma ? BRS : FIFO;
			} else {
				host->state = READY;
			}
			goto change_state;
		}
		break;
	case FIFO:
		if (host->flags & FIFO_RDY) {
			host->flags &= ~FIFO_RDY;
			host->state = BRS;
			goto change_state;
		}
		break;
	case BRS:
		if (no_dma)
			tifm_sd_read_data(host, host_status);
		if (host_status & TIFM_MMCSD_BRS) {
			if (host->req->stop) {
				tifm_sd_exec(host, host->req->stop);
				host->state = SCMD;
				break;
			} else {
				host->state = READY;
			}
			goto change_state;
		}
		break;
	case SCMD:
		if (host_status & (TIFM_MMCSD_EOC | TIFM_MMCSD_CERR)) {
			tifm_sd_fetch_resp(host->req->stop, sock);
			host->state = READY;
			goto change_state;
		}
		break;
	case READY:
		tasklet_schedule(&host->finish_tasklet);
		break;
	}
}

static void tifm_sd_do_write_cmd(struct tifm_sd *host, unsigned int host_status)
{
	struct tifm_dev *sock = host->dev;
	struct mmc_command *cmd = host->req->cmd;

change_state:
	switch (host->state) {
	case IDLE:
		return;
	case CMD:
		if (host_status & (TIFM_MMCSD_EOC | TIFM_MMCSD_CERR)) {
			tifm_sd_fetch_resp(cmd, sock);
			if (cmd->data) {
				host->state = no_dma ? BRS : FIFO;
			} else {
				host->state = READY;
			}
			goto change_state;
		}
		break;
	case FIFO:
		if (host->flags & FIFO_RDY) {
			host->flags &= ~FIFO_RDY;
			host->state = BRS;
			goto change_state;
		}
		break;
	case BRS:
		if (no_dma)
			tifm_sd_write_data(host, host_status);
		if (host_status & TIFM_MMCSD_BRS) {
			host->state = CARD;
			goto change_state;
		}
		break;
	case CARD:
		dev_dbg(&sock->dev, "waiting for CARD, have %ld blocks\n",
			host->written_blocks);
		if (!(host->flags & CARD_BUSY)
		    && (host->written_blocks == cmd->data->blocks)) {
			if (host->req->stop) {
				tifm_sd_exec(host, host->req->stop);
				host->state = SCMD;
				break;
			} else {
				host->state = READY;
			}
			goto change_state;
		}
		break;
	case SCMD:
		if (host_status & (TIFM_MMCSD_EOC | TIFM_MMCSD_CERR)) {
			tifm_sd_fetch_resp(host->req->stop, sock);
			host->state = BUSY;
			goto change_state;
		}
		break;
	case BUSY:
		if (!(host->flags & CARD_BUSY)
		    && (host->written_blocks > cmd->data->blocks)) {
			host->state = READY;
			goto change_state;	
		}
		break;
	case READY:
		tasklet_schedule(&host->finish_tasklet);
		break;
	}
}

static int tifm_sd_set_dma(struct tifm_sd *host, struct mmc_data *r_data)
{
	struct tifm_dev *sock = host->dev;
	unsigned int dma_cnt;

	if (host->sg_pos < host->sg_len) {
		dma_cnt = r_data->sg[host->sg_pos].length;
		dev_dbg(&sock->dev, "setting dma for %d blocks\n",
			dma_cnt / r_data->blksz);
		dma_cnt = ((dma_cnt / r_data->blksz) << 8) & TIFM_DMA_SZMASK;
		writel(sg_dma_address(&r_data->sg[host->sg_pos]),
		       sock->addr + SOCK_DMA_ADDRESS);
		if (r_data->flags & MMC_DATA_WRITE)
			writel(dma_cnt | TIFM_DMA_TX | TIFM_DMA_EN,
			       sock->addr + SOCK_DMA_CONTROL);
		else
			writel(dma_cnt | TIFM_DMA_EN,
			       sock->addr + SOCK_DMA_CONTROL);

		return 0;
	} else
		return 1;
}

/* Called from interrupt handler */
static void tifm_sd_signal_irq(struct tifm_dev *sock,
			       unsigned int sock_irq_status)
{
	struct tifm_sd *host;
	unsigned int host_status = 0, fifo_status = 0;
	int error_code = 0;
	struct mmc_command *cmd = 0;

	spin_lock(&sock->lock);
	host = mmc_priv((struct mmc_host*)tifm_get_drvdata(sock));

	if (!host->req)
		goto done;
	else
		cmd = host->req->cmd;

	if (sock_irq_status & FIFO_EVENT) {
		fifo_status = readl(sock->addr + SOCK_DMA_FIFO_STATUS);

		if (cmd->data && !no_dma) {
			host->sg_pos++;
			if (tifm_sd_set_dma(host, cmd->data))
				host->flags |= fifo_status & FIFO_RDY;
		}

		writel(fifo_status, sock->addr + SOCK_DMA_FIFO_STATUS);
	}

	if (sock_irq_status & CARD_EVENT) {
		host_status = readl(sock->addr + SOCK_MMCSD_STATUS);
		writel(host_status, sock->addr + SOCK_MMCSD_STATUS);

		if (host_status & TIFM_MMCSD_ERRMASK) {
			if (host_status & (TIFM_MMCSD_CTO | TIFM_MMCSD_DTO))
				error_code = MMC_ERR_TIMEOUT;
			else if (host_status
				 & (TIFM_MMCSD_CCRC | TIFM_MMCSD_DCRC))
				error_code = MMC_ERR_BADCRC;

			writel(TIFM_FIFO_INT_SETALL,
			       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
			writel(TIFM_DMA_RESET, sock->addr + SOCK_DMA_CONTROL);

			if (host->req->stop) {
				if (host->state == SCMD) {
					host->req->stop->error = error_code;
				} else if (host->state == BRS
					   || host->state == CARD
					   || host->state == FIFO) {
					host->req->cmd->error = error_code;
					tifm_sd_exec(host, host->req->stop);
					host->state = SCMD;
					goto done;
				} else {
					cmd->error = error_code;
				}
			} else {
				cmd->error = error_code;
			}
			host->state = READY;
		}

		if (host_status & TIFM_MMCSD_CB)
			host->flags |= CARD_BUSY;
		if ((host_status & TIFM_MMCSD_EOFB)
		    && (host->flags & CARD_BUSY)) {
			host->written_blocks++;
			host->flags &= ~CARD_BUSY;
		}
        }

	if (cmd->data && (cmd->data->flags & MMC_DATA_WRITE))
		tifm_sd_do_write_cmd(host, host_status);
	else
		tifm_sd_do_read_cmd(host, host_status);

done:
	dev_dbg(&sock->dev, "host_status %x, fifo_status %x\n",
		host_status, fifo_status);
	spin_unlock(&sock->lock);
}

static void tifm_sd_set_data_timeout(struct tifm_sd *host,
				     struct mmc_data *data)
{
	struct tifm_dev *sock = host->dev;
	unsigned int data_timeout = data->timeout_clks;

	if (fixed_timeout)
		return;

	data_timeout += data->timeout_ns /
			((1000000000UL / host->clk_freq) * host->clk_div);

	if (data_timeout < 0xffff) {
		writel(data_timeout, sock->addr + SOCK_MMCSD_DATA_TO);
		writel((~TIFM_MMCSD_DPE)
		       & readl(sock->addr + SOCK_MMCSD_SDIO_MODE_CONFIG),
		       sock->addr + SOCK_MMCSD_SDIO_MODE_CONFIG);
	} else {
		data_timeout = (data_timeout >> 10) + 1;
		if (data_timeout > 0xffff)
			data_timeout = 0;	/* set to unlimited */
		writel(data_timeout, sock->addr + SOCK_MMCSD_DATA_TO);
		writel(TIFM_MMCSD_DPE
		       | readl(sock->addr + SOCK_MMCSD_SDIO_MODE_CONFIG),
		       sock->addr + SOCK_MMCSD_SDIO_MODE_CONFIG);
	}
}

static void tifm_sd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct tifm_sd *host = mmc_priv(mmc);
	struct tifm_dev *sock = host->dev;
	unsigned long flags;
	int sg_count = 0;
	struct mmc_data *r_data = mrq->cmd->data;

	spin_lock_irqsave(&sock->lock, flags);
	if (host->flags & EJECT) {
		spin_unlock_irqrestore(&sock->lock, flags);
		goto err_out;
	}

	if (host->req) {
		printk(KERN_ERR DRIVER_NAME ": unfinished request detected\n");
		spin_unlock_irqrestore(&sock->lock, flags);
		goto err_out;
	}

	if (r_data) {
		tifm_sd_set_data_timeout(host, r_data);

		host->block_pos = 0;
		host->sg_pos = 0;
		host->written_blocks = 0;

		if (no_dma) {
			writel(TIFM_MMCSD_BUFINT
			       | readl(sock->addr + SOCK_MMCSD_INT_ENABLE),
			       sock->addr + SOCK_MMCSD_INT_ENABLE);
			writel(((TIFM_MMCSD_FIFO_SIZE - 1) << 8)
			       | (TIFM_MMCSD_FIFO_SIZE - 1),
			       sock->addr + SOCK_MMCSD_BUFFER_CONFIG);

			host->sg_len = r_data->sg_len;
		} else {
			sg_count = tifm_map_sg(sock, r_data->sg, r_data->sg_len,
					       mrq->cmd->flags & MMC_DATA_WRITE
					       ? PCI_DMA_TODEVICE
					       : PCI_DMA_FROMDEVICE);
			if (sg_count < 1) {
				printk(KERN_ERR DRIVER_NAME
				       ": scatterlist map failed\n");
				spin_unlock_irqrestore(&sock->lock, flags);
				goto err_out;
			}

			writel(TIFM_FIFO_INT_SETALL,
			       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
			writel(ilog2(r_data->blksz) - 2,
			       sock->addr + SOCK_FIFO_PAGE_SIZE);
			writel(TIFM_FIFO_ENABLE,
			       sock->addr + SOCK_FIFO_CONTROL);
			writel(TIFM_FIFO_INTMASK,
			       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_SET);

			if (r_data->flags & MMC_DATA_WRITE)
				writel(TIFM_MMCSD_TXDE,
				       sock->addr + SOCK_MMCSD_BUFFER_CONFIG);
			else
				writel(TIFM_MMCSD_RXDE,
				       sock->addr + SOCK_MMCSD_BUFFER_CONFIG);
			host->sg_len = sg_count;
			tifm_sd_set_dma(host, r_data);
		}

		writel(r_data->blocks - 1,
		       sock->addr + SOCK_MMCSD_NUM_BLOCKS);
		writel(r_data->blksz - 1,
		       sock->addr + SOCK_MMCSD_BLOCK_LEN);
	}

	host->written_blocks = 0;
	host->flags &= ~CARD_BUSY;
	host->req = mrq;
	mod_timer(&host->timer, jiffies + host->timeout_jiffies);
	host->state = CMD;
	writel(TIFM_CTRL_LED | readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);
	tifm_sd_exec(host, mrq->cmd);
	spin_unlock_irqrestore(&sock->lock, flags);
	return;

err_out:
	if (sg_count > 0)
		tifm_unmap_sg(sock, r_data->sg, r_data->sg_len,
			      (r_data->flags & MMC_DATA_WRITE)
			      ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE);

	mrq->cmd->error = MMC_ERR_TIMEOUT;
	mmc_request_done(mmc, mrq);
}

static void tifm_sd_end_cmd(unsigned long data)
{
	struct tifm_sd *host = (struct tifm_sd*)data;
	struct tifm_dev *sock = host->dev;
	struct mmc_host *mmc = tifm_get_drvdata(sock);
	struct mmc_request *mrq;
	struct mmc_data *r_data = NULL;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);

	del_timer(&host->timer);
	mrq = host->req;
	host->req = NULL;
	host->state = IDLE;

	if (!mrq) {
		printk(KERN_ERR DRIVER_NAME ": no request to complete?\n");
		spin_unlock_irqrestore(&sock->lock, flags);
		return;
	}

	r_data = mrq->cmd->data;
	if (r_data) {
		if (no_dma) {
			writel((~TIFM_MMCSD_BUFINT)
			       & readl(sock->addr + SOCK_MMCSD_INT_ENABLE),
			       sock->addr + SOCK_MMCSD_INT_ENABLE);
		} else {
			tifm_unmap_sg(sock, r_data->sg, r_data->sg_len,
				      (r_data->flags & MMC_DATA_WRITE)
				      ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE);
		}

		if (r_data->flags & MMC_DATA_WRITE) {
			if (host->written_blocks > r_data->blocks)
				host->written_blocks--;
			r_data->bytes_xfered = host->written_blocks
					       * r_data->blksz;
		} else {
			r_data->bytes_xfered = r_data->blocks
				- readl(sock->addr + SOCK_MMCSD_NUM_BLOCKS) - 1;
			r_data->bytes_xfered *= r_data->blksz;
			r_data->bytes_xfered += r_data->blksz
				- readl(sock->addr + SOCK_MMCSD_BLOCK_LEN) + 1;
		}
		dev_dbg(&sock->dev, "w_bl %ld, num_bl %u, b_s %d, bsh %u\n",
		host->written_blocks, readl(sock->addr + SOCK_MMCSD_NUM_BLOCKS),
		r_data->blksz, readl(sock->addr + SOCK_MMCSD_BLOCK_LEN));
	}

	writel((~TIFM_CTRL_LED) & readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);

	spin_unlock_irqrestore(&sock->lock, flags);
	mmc_request_done(mmc, mrq);
}

static void tifm_sd_abort(unsigned long data)
{
	struct tifm_sd *host = (struct tifm_sd*)data;

	printk(KERN_ERR DRIVER_NAME
	       ": card failed to respond for a long period of time\n");

	tifm_eject(host->dev);
}

static void tifm_sd_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct tifm_sd *host = mmc_priv(mmc);
	struct tifm_dev *sock = host->dev;
	unsigned int clk_div1, clk_div2;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);

	dev_dbg(&sock->dev, "Setting bus width %d, power %d\n", ios->bus_width,
		ios->power_mode);
	if (ios->bus_width == MMC_BUS_WIDTH_4) {
		writel(TIFM_MMCSD_4BBUS | readl(sock->addr + SOCK_MMCSD_CONFIG),
		       sock->addr + SOCK_MMCSD_CONFIG);
	} else {
		writel((~TIFM_MMCSD_4BBUS)
		       & readl(sock->addr + SOCK_MMCSD_CONFIG),
		       sock->addr + SOCK_MMCSD_CONFIG);
	}

	if (ios->clock) {
		clk_div1 = 20000000 / ios->clock;
		if (!clk_div1)
			clk_div1 = 1;

		clk_div2 = 24000000 / ios->clock;
		if (!clk_div2)
			clk_div2 = 1;

		if ((20000000 / clk_div1) > ios->clock)
			clk_div1++;
		if ((24000000 / clk_div2) > ios->clock)
			clk_div2++;
		if ((20000000 / clk_div1) > (24000000 / clk_div2)) {
			host->clk_freq = 20000000;
			host->clk_div = clk_div1;
			writel((~TIFM_CTRL_FAST_CLK)
			       & readl(sock->addr + SOCK_CONTROL),
			       sock->addr + SOCK_CONTROL);
		} else {
			host->clk_freq = 24000000;
			host->clk_div = clk_div2;
			writel(TIFM_CTRL_FAST_CLK
			       | readl(sock->addr + SOCK_CONTROL),
			       sock->addr + SOCK_CONTROL);
		}
	} else {
		host->clk_div = 0;
	}
	host->clk_div &= TIFM_MMCSD_CLKMASK;
	writel(host->clk_div
	       | ((~TIFM_MMCSD_CLKMASK)
		  & readl(sock->addr + SOCK_MMCSD_CONFIG)),
	       sock->addr + SOCK_MMCSD_CONFIG);

	if (ios->bus_mode == MMC_BUSMODE_OPENDRAIN)
		host->flags |= OPENDRAIN;
	else
		host->flags &= ~OPENDRAIN;

	/* chip_select : maybe later */
	//vdd
	//power is set before probe / after remove

	spin_unlock_irqrestore(&sock->lock, flags);
}

static int tifm_sd_ro(struct mmc_host *mmc)
{
	int rc;
	struct tifm_sd *host = mmc_priv(mmc);
	struct tifm_dev *sock = host->dev;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);

	host->flags |= (CARD_RO & readl(sock->addr + SOCK_PRESENT_STATE));
	rc = (host->flags & CARD_RO) ? 1 : 0;

	spin_unlock_irqrestore(&sock->lock, flags);
	return rc;
}

static const struct mmc_host_ops tifm_sd_ops = {
	.request = tifm_sd_request,
	.set_ios = tifm_sd_ios,
	.get_ro  = tifm_sd_ro
};

static int tifm_sd_initialize_host(struct tifm_sd *host)
{
	int rc;
	unsigned int host_status = 0;
	struct tifm_dev *sock = host->dev;

	writel(0, sock->addr + SOCK_MMCSD_INT_ENABLE);
	mmiowb();
	host->clk_div = 61;
	host->clk_freq = 20000000;
	writel(TIFM_MMCSD_RESET, sock->addr + SOCK_MMCSD_SYSTEM_CONTROL);
	writel(host->clk_div | TIFM_MMCSD_POWER,
	       sock->addr + SOCK_MMCSD_CONFIG);

	/* wait up to 0.51 sec for reset */
	for (rc = 32; rc <= 256; rc <<= 1) {
		if (1 & readl(sock->addr + SOCK_MMCSD_SYSTEM_STATUS)) {
			rc = 0;
			break;
		}
		msleep(rc);
	}

	if (rc) {
		printk(KERN_ERR DRIVER_NAME
		       ": controller failed to reset\n");
		return -ENODEV;
	}

	writel(0, sock->addr + SOCK_MMCSD_NUM_BLOCKS);
	writel(host->clk_div | TIFM_MMCSD_POWER,
	       sock->addr + SOCK_MMCSD_CONFIG);
	writel(TIFM_MMCSD_RXDE, sock->addr + SOCK_MMCSD_BUFFER_CONFIG);

	// command timeout fixed to 64 clocks for now
	writel(64, sock->addr + SOCK_MMCSD_COMMAND_TO);
	writel(TIFM_MMCSD_INAB, sock->addr + SOCK_MMCSD_COMMAND);

	for (rc = 16; rc <= 64; rc <<= 1) {
		host_status = readl(sock->addr + SOCK_MMCSD_STATUS);
		writel(host_status, sock->addr + SOCK_MMCSD_STATUS);
		if (!(host_status & TIFM_MMCSD_ERRMASK)
		    && (host_status & TIFM_MMCSD_EOC)) {
			rc = 0;
			break;
		}
		msleep(rc);
	}

	if (rc) {
		printk(KERN_ERR DRIVER_NAME
		       ": card not ready - probe failed on initialization\n");
		return -ENODEV;
	}

	writel(TIFM_MMCSD_DATAMASK | TIFM_MMCSD_ERRMASK,
	       sock->addr + SOCK_MMCSD_INT_ENABLE);
	mmiowb();

	return 0;
}

static int tifm_sd_probe(struct tifm_dev *sock)
{
	struct mmc_host *mmc;
	struct tifm_sd *host;
	int rc = -EIO;

	if (!(TIFM_SOCK_STATE_OCCUPIED
	      & readl(sock->addr + SOCK_PRESENT_STATE))) {
		printk(KERN_WARNING DRIVER_NAME ": card gone, unexpectedly\n");
		return rc;
	}

	mmc = mmc_alloc_host(sizeof(struct tifm_sd), &sock->dev);
	if (!mmc)
		return -ENOMEM;

	host = mmc_priv(mmc);
	tifm_set_drvdata(sock, mmc);
	host->dev = sock;
	host->timeout_jiffies = msecs_to_jiffies(1000);

	tasklet_init(&host->finish_tasklet, tifm_sd_end_cmd,
		     (unsigned long)host);
	setup_timer(&host->timer, tifm_sd_abort, (unsigned long)host);

	mmc->ops = &tifm_sd_ops;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps = MMC_CAP_4_BIT_DATA | MMC_CAP_MULTIWRITE;
	mmc->f_min = 20000000 / 60;
	mmc->f_max = 24000000;

//	mmc->max_blk_count = 2048;
//	mmc->max_hw_segs = mmc->max_blk_count / 127;
	mmc->max_hw_segs = 16;
//	mmc->max_blk_size = 2048;
//	mmc->max_seg_size = 127 * mmc->max_blk_size;
	mmc->max_seg_size = 127 * 2048;

	mmc->max_phys_segs = mmc->max_hw_segs;
	mmc->max_sectors = 127 * mmc->max_phys_segs;

//	mmc->max_req_size = mmc->max_seg_size * mmc->max_hw_segs;

	sock->signal_irq = tifm_sd_signal_irq;
	rc = tifm_sd_initialize_host(host);

	if (!rc)
		rc = mmc_add_host(mmc);
	if (rc)
		goto out_free_mmc;

	return 0;
out_free_mmc:
	mmc_free_host(mmc);
	return rc;
}

static void tifm_sd_remove(struct tifm_dev *sock)
{
	struct mmc_host *mmc = tifm_get_drvdata(sock);
	struct tifm_sd *host = mmc_priv(mmc);
	unsigned long flags;

	tasklet_kill(&host->finish_tasklet);
	spin_lock_irqsave(&sock->lock, flags);
	host->flags |= EJECT;
	writel(0, sock->addr + SOCK_MMCSD_INT_ENABLE);
	mmiowb();
	
	if (host->req) {
		writel(TIFM_FIFO_INT_SETALL,
		       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
		writel(0, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_SET);
		host->req->cmd->error = MMC_ERR_TIMEOUT;
		if (host->req->stop)
			host->req->stop->error = MMC_ERR_TIMEOUT;
		tasklet_schedule(&host->finish_tasklet);
	}
	spin_unlock_irqrestore(&sock->lock, flags);
	// temporary hack
	msleep(1000);
	mmc_remove_host(mmc);
	dev_dbg(&sock->dev, "after remove\n");

	/* The meaning of the bit majority in this constant is unknown. */
	writel(0xfff8 & readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);

	mmc_free_host(mmc);
}

#ifdef CONFIG_PM

static int tifm_sd_suspend(struct tifm_dev *sock, pm_message_t state)
{
	struct mmc_host *mmc = tifm_get_drvdata(sock);
	int rc;

	rc = mmc_suspend_host(mmc, state);
	/* The meaning of the bit majority in this constant is unknown. */
	writel(0xfff8 & readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);
	return rc;
}

static int tifm_sd_resume(struct tifm_dev *sock)
{
	struct mmc_host *mmc = tifm_get_drvdata(sock);
	struct tifm_sd *host = mmc_priv(mmc);

	if (sock->type != TIFM_TYPE_SD
	    || tifm_sd_initialize_host(host)) {
		tifm_eject(sock);
		return 0;
	} else {
		return mmc_resume_host(mmc);
	}
}

#else

#define tifm_sd_suspend NULL
#define tifm_sd_resume NULL

#endif /* CONFIG_PM */

static struct tifm_device_id tifm_sd_id_tbl[] = {
	{ TIFM_TYPE_SD }, { 0 }
};

static struct tifm_driver tifm_sd_driver = {
	.driver = {
		.name  = DRIVER_NAME,
		.owner = THIS_MODULE
	},
	.id_table = tifm_sd_id_tbl,
	.probe    = tifm_sd_probe,
	.remove   = tifm_sd_remove,
	.suspend  = tifm_sd_suspend,
	.resume   = tifm_sd_resume
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
