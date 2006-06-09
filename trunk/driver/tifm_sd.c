/*
 *
 *  TI FlashMedia driver
 *
 *  Copyright (C) 2006 Alex Dubov <oakad@yahoo.com>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "tifm.h"
#include <linux/mmc/protocol.h>
#include <linux/mmc/host.h>
#include <linux/highmem.h>

#define DRIVER_NAME "tifm_sd"
#define DRIVER_VERSION "0.3"

static int no_dma = 0;
module_param(no_dma, bool, 0644);

enum { CARD_EVENT = 0x0001, FIFO_EVENT = 0x0002, EJECT_EVENT = 0x0004,
       CARD_BUSY = 0x0008, FLAG_W2 = 0x0010, CARD_RO = 0x0020, HOST_REG = 0x0040 };

struct tifm_sd {
	struct tifm_dev     *dev;

	wait_queue_head_t   event;
	unsigned int        flags;
	unsigned int        status;
	unsigned int        fifo_status;
	unsigned int        clk_freq;
	unsigned int        clk_div;
	unsigned int        timeout_clks;
	unsigned long       timeout_jiffies;

	struct mmc_request  *req;
	struct work_struct  cmd_handler;

};

static void tifm_sd_do_cmd_nodma(void *data);

/* Called from interrupt handler */
static unsigned int tifm_sd_signal_irq(struct tifm_dev *sock, unsigned int sock_irq_status)
{
	struct tifm_sd *card = mmc_priv((struct mmc_host*)tifm_get_drvdata(sock));
	unsigned int card_status;

	spin_lock(&sock->lock);
	card->flags &= ~(CARD_EVENT | FIFO_EVENT); 
	card->flags |= (sock_irq_status & 0x00000100) ? CARD_EVENT : 0;
	card->flags |= (sock_irq_status & 0x00010000) ? FIFO_EVENT : 0;

        if(card->flags & FIFO_EVENT)
        {
                card->fifo_status = readl(sock->addr + SOCK_DMA_FIFO_STATUS);
                writel(card->fifo_status, sock->addr + SOCK_DMA_FIFO_STATUS);

		wake_up_all(&card->event);
        }

        if(card->flags & CARD_EVENT)
        {
                card_status = readl(sock->addr + SOCK_MMCSD_STATUS);
                writel(card_status, sock->addr + SOCK_MMCSD_STATUS);
		card->status |= card_status;

		if(card_status & 0x8) {
			writel(0x0014 | readl(sock->addr + SOCK_MMCSD_INT_ENABLE),
			       sock->addr + SOCK_MMCSD_INT_ENABLE);
			card->flags |= FLAG_W2;
		}

		if(card_status & 0x0010) card->flags &= ~FLAG_W2;
		if(card_status & 0x0004) card->flags |= FLAG_W2;
		wake_up_all(&card->event);
        }
	spin_unlock(&sock->lock);
//	DBG("after irq: card_status = 0x%08x, fifo_status = 0x%08x, flags = 0x%08x\n",
//	    card->status, card->fifo_status, card->flags);
	return sock_irq_status;
}

static inline unsigned int tifm_sd_test_flag(struct tifm_sd *card, unsigned int flags)
{
	unsigned int rc;

	spin_lock_irqsave(&card->dev->lock, card->dev->irq_flags);
	rc = card->flags & flags;
	spin_unlock_irqrestore(&card->dev->lock, card->dev->irq_flags);
	return rc;
}

static inline int tifm_sd_wait_for_fifo(struct tifm_sd *card)
{
	int error_code = 0;

	while(!(card->fifo_status & 1)) {
		spin_unlock_irqrestore(&card->dev->lock, card->dev->irq_flags);
		if(!wait_event_timeout(card->event, tifm_sd_test_flag(card, FIFO_EVENT | EJECT_EVENT),
				       card->timeout_jiffies)) error_code = MMC_ERR_TIMEOUT;
		spin_lock_irqsave(&card->dev->lock, card->dev->irq_flags);
		card->flags &= ~FIFO_EVENT;
		if(error_code || card->flags & EJECT_EVENT) {
			error_code = MMC_ERR_TIMEOUT;
			break;
		}
	}
	card->fifo_status = 0;
	if(error_code) DBG("timeout\n");
	return error_code;
}

static inline int tifm_sd_wait_for_eoc(struct tifm_sd *card)
{
	int err_code = 0;

	while(!(card->status & 1)) {
		if(card->flags & EJECT_EVENT) err_code = MMC_ERR_TIMEOUT;
		if(card->status & 0x4000) err_code = MMC_ERR_FAILED;
		if(card->status & 0x0080) err_code = MMC_ERR_TIMEOUT;
		if(card->status & 0x0100) err_code = MMC_ERR_BADCRC;
		if(err_code) break;

		spin_unlock_irqrestore(&card->dev->lock, card->dev->irq_flags);
		if(!wait_event_timeout(card->event,
				       tifm_sd_test_flag(card, CARD_EVENT | EJECT_EVENT),
				       msecs_to_jiffies(1000))) // arbitrary time-out value; TI numbers are even larger
			err_code = MMC_ERR_TIMEOUT;
		spin_lock_irqsave(&card->dev->lock, card->dev->irq_flags);

		card->flags &= ~CARD_EVENT;
	}
	return err_code;
}

static inline int tifm_sd_wait_for_brs(struct tifm_sd *card)
{
	int err_code = 0;

	while(!(card->status & 8)) {
		if(card->flags & EJECT_EVENT) err_code = MMC_ERR_TIMEOUT;
		if(card->status & 0x4000) err_code = MMC_ERR_FAILED;
		if(card->status & 0x0080) err_code = MMC_ERR_TIMEOUT;
		if(card->status & 0x0100) err_code = MMC_ERR_BADCRC;
		if(err_code) break;

		spin_unlock_irqrestore(&card->dev->lock, card->dev->irq_flags);
		if(!wait_event_timeout(card->event,
				       tifm_sd_test_flag(card, CARD_EVENT | EJECT_EVENT),
				       card->timeout_jiffies)) // arbitrary time-out value; TI numbers are even larger
			err_code = MMC_ERR_TIMEOUT;
		spin_lock_irqsave(&card->dev->lock, card->dev->irq_flags);

		card->flags &= ~CARD_EVENT;
	}

	if(err_code == MMC_ERR_TIMEOUT) {
		if(0x8 & readl(card->dev->addr + SOCK_MMCSD_STATUS)) err_code = 0;
	}

	if(err_code) DBG("timeout\n");
	return err_code;
}

static inline int tifm_sd_wait_for_card(struct tifm_sd *card)
{
	int err_code = 0;

	while(card->flags & FLAG_W2) {
		if(card->flags & EJECT_EVENT) {
			err_code = MMC_ERR_TIMEOUT;
			break;
		}
		spin_unlock_irqrestore(&card->dev->lock, card->dev->irq_flags);

		if(!wait_event_timeout(card->event, tifm_sd_test_flag(card, CARD_EVENT | EJECT_EVENT),
				       card->timeout_jiffies)) err_code = MMC_ERR_TIMEOUT;

		spin_lock_irqsave(&card->dev->lock, card->dev->irq_flags);
		card->flags &= ~CARD_EVENT;
		if(err_code) {
			err_code = 0;
			break;
		}
	}

	if(!err_code)
		writel(readl(card->dev->addr + SOCK_MMCSD_INT_ENABLE) & 0xffffffeb,
		       card->dev->addr + SOCK_MMCSD_INT_ENABLE);

	return err_code;
}

static inline void tifm_sd_set_timeout(struct tifm_sd *card, unsigned int timeout_clks)
{
	struct timespec t;
	
	if(timeout_clks) {
		if(card->timeout_clks != timeout_clks) {
			t.tv_sec = 0;
			t.tv_nsec = card->clk_div * timeout_clks * (1000000000l / card->clk_freq);
			card->timeout_jiffies = timespec_to_jiffies(&t);
			card->timeout_clks = timeout_clks;
		}	

	} else {
		// conservative timeout value
		card->timeout_jiffies = msecs_to_jiffies(1000);
		card->timeout_clks = card->clk_freq / card->clk_div;
	}
	//DBG("setting timeout %d ms\n", jiffies_to_msecs(card->timeout_jiffies));
}

static inline unsigned int tifm_sd_op_flags(unsigned int cmd_flags)
{
	unsigned int rc = 0;

	switch(cmd_flags & 0x1f)
	{
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

	switch(cmd_flags & MMC_CMD_MASK)
	{
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


static int tifm_sd_execute(struct tifm_sd *card, struct mmc_command *cmd)
{
	struct tifm_dev *sock = card->dev;
	unsigned int cmd_mask = tifm_sd_op_flags(cmd->flags);

	if(cmd->data) {
		if(((cmd->flags & MMC_CMD_MASK) == MMC_CMD_ADTC)
		   && (cmd->data->flags & MMC_DATA_READ)) cmd_mask |= 0x8000;
	}
	//DBG("executing opcode 0x%x, arg: 0x%x, mask: 0x%x\n", cmd->opcode, cmd->arg, cmd_mask);

	writel((cmd->arg >> 16) & 0xffff, sock->addr + SOCK_MMCSD_ARG_HIGH);
	writel(cmd->arg & 0xffff, sock->addr + SOCK_MMCSD_ARG_LOW);
	card->status = 0;
	card->flags &= ~CARD_EVENT;
	writel(cmd->opcode | cmd_mask, sock->addr + SOCK_MMCSD_COMMAND);

	cmd->error = tifm_sd_wait_for_eoc(card);
	if(cmd->error) DBG("cmd error %d\n", cmd->error);

	if(!cmd->error) {
		cmd->resp[0] = readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x1c); cmd->resp[0] <<= 16;
		cmd->resp[0] |= readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x18);
		cmd->resp[1] = readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x14); cmd->resp[1] <<= 16;
		cmd->resp[1] |= readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x10);
		cmd->resp[2] = readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x0c); cmd->resp[2] <<= 16;
		cmd->resp[2] |= readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x08);
		cmd->resp[3] = readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x04); cmd->resp[3] <<= 16;
		cmd->resp[3] |= readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x00);
	}

//	DBG("finished processing opcode 0x%x, error code %d\n", cmd->opcode, cmd->error);
	
	return cmd->error;
}

static inline void tifmxx_sd_prepare_data(struct tifm_sd *card, struct mmc_command *cmd)
{
	struct tifm_dev *sock = card->dev;
	unsigned int dest_cnt;
	
	/* DMA style IO */

	writel(0xffff, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
	writel(0x0001, sock->addr + SOCK_FIFO_CONTROL);
	writel(0x0005, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_SET);
	
	dest_cnt = cmd->data->blocks << 8;
	
	writel(sg_dma_address(cmd->data->sg), sock->addr + SOCK_DMA_ADDRESS);
	
	if(cmd->data->flags & MMC_DATA_WRITE) {
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

static inline int tifm_sd_finish_data(struct tifm_sd *card, struct mmc_command *cmd)
{
	struct tifm_dev *sock = card->dev;

	if(cmd->data->flags & MMC_DATA_WRITE)
		cmd->error = tifm_sd_wait_for_card(card);

	if(cmd->error) return cmd->error;

	cmd->error = tifm_sd_wait_for_fifo(card);
	
	cmd->data->bytes_xfered = cmd->data->blocks - readl(sock->addr + SOCK_MMCSD_NUM_BLOCKS);
	cmd->data->bytes_xfered <<= cmd->data->blksz_bits;
	cmd->data->bytes_xfered += (1 << cmd->data->blksz_bits) - readl(sock->addr + SOCK_MMCSD_BLOCK_LEN) + 1;
	//DBG("dma transferred %d\n", cmd->data->bytes_xfered);
	return cmd->error;
}

static void tifm_sd_do_cmd(void *data)
{
	struct tifm_sd *card = (struct tifm_sd*)data;
	struct tifm_dev *sock = card->dev;
	struct mmc_host *host = tifm_get_drvdata(sock);
	struct mmc_request *mrq = card->req;
	struct mmc_data *r_data = mrq->cmd->data;
	int sg_count = 0;

	if(r_data && !mrq->stop) return tifm_sd_do_cmd_nodma(data);

	if(!get_device(&sock->dev)) return;
	spin_lock_irqsave(&sock->lock, sock->irq_flags);
	
	if(r_data) {
		if(card->flags & CARD_BUSY) {
			mrq->cmd->error = MMC_ERR_FIFO;
			DBG("transfer still in progress\n");
			goto out;
		}

		card->flags |= CARD_BUSY;
		tifm_sd_set_timeout(card, r_data->timeout_clks);
		BUG_ON(r_data->sg_len != 1);
		sg_count = tifm_map_sg(sock, r_data->sg, r_data->sg_len,
				       mrq->cmd->flags & MMC_DATA_WRITE
				       ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE);

		if(sg_count < 1) {
			mrq->cmd->error =  MMC_ERR_FAILED;
			DBG("can't map\n");
			goto out;
		}				       
		tifmxx_sd_prepare_data(card, mrq->cmd);
	}

	tifm_sd_execute(card, mrq->cmd);
	
	if(r_data) {
		if(mrq->cmd->error || (mrq->cmd->error = tifm_sd_wait_for_brs(card))) {
			writel(0xffff, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
			writel(0x0002, sock->addr + SOCK_DMA_CONTROL);
		}
		
		if(mrq->stop) {
			if(tifm_sd_execute(card, mrq->stop))
			{
				writel(0xffff, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
				writel(0x0002, sock->addr + SOCK_DMA_CONTROL);
				goto out;
			}
		}
		
		if(!mrq->cmd->error && (tifm_sd_finish_data(card, mrq->cmd))) {
			DBG("tifm_sd_finish_data: error %d.\n", mrq->cmd->error);
	    		writel(0xffff, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
			writel(0x0002, sock->addr + SOCK_DMA_CONTROL);
		}
		
	}

out:
	if(card->flags & CARD_BUSY) {
		if(sg_count)
			tifm_unmap_sg(sock, r_data->sg, r_data->sg_len,
			    	      (r_data->flags & MMC_DATA_WRITE)
				      ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE);
		
		card->flags &= ~CARD_BUSY;		
	}
	card->req = 0;
	if(!(card->flags & EJECT_EVENT)) mmc_request_done(host, mrq);
	spin_unlock_irqrestore(&sock->lock, sock->irq_flags);
	put_device(&sock->dev);
}

unsigned long tifm_sd_read(struct tifm_sd *card, char *buf, unsigned long count)
{
	struct tifm_dev *sock = card->dev;
	unsigned long rc = 0;
	int r_time = 1, t_val;

	while(count - rc) {
		while(!(card->status & 0x0400)) {
			if((card->flags & EJECT_EVENT) || (card->status & 0x80) || !r_time)
				card->req->cmd->error = MMC_ERR_TIMEOUT;
			if(card->status & 0x100)
				card->req->cmd->error = MMC_ERR_BADCRC;
			if(card->status & 0x4000)
				card->req->cmd->error = MMC_ERR_FAILED;
				
			if(card->req->cmd->error) return rc;
			
			spin_unlock_irqrestore(&sock->lock, sock->irq_flags);
			r_time = wait_event_timeout(card->event, tifm_sd_test_flag(card, CARD_EVENT | EJECT_EVENT),
						    card->timeout_jiffies);
			spin_lock_irqsave(&sock->lock, sock->irq_flags);
			card->flags &= ~CARD_EVENT;
		}
		card->status &= 0xfffffbff;
		
		t_val = readl(sock->addr + SOCK_MMCSD_DATA);
		buf[rc++] = t_val & 0xff;
		if(count - rc) buf[rc++] = (t_val >> 8) & 0xff;
	}
	return rc;
}

unsigned long tifm_sd_write(struct tifm_sd *card, char *buf, unsigned long count)
{
	struct tifm_dev *sock = card->dev;
	unsigned long rc = 0;
	int r_time = 1, t_val;

	writel(0x0014 | readl(sock->addr + SOCK_MMCSD_INT_ENABLE), sock->addr + SOCK_MMCSD_INT_ENABLE);
	card->flags |= FLAG_W2;
	
	while(count - rc) {
		t_val = buf[rc++] & 0x00ff;
		if(count - rc) t_val |= (buf[rc++] << 8) & 0xff00;
		while(!(card->status & 0x0800)) {
			if((card->flags & EJECT_EVENT) || (card->status & 0x80) || !r_time)
				card->req->cmd->error = MMC_ERR_TIMEOUT;
			if(card->status & 0x100)
				card->req->cmd->error = MMC_ERR_BADCRC;
			if(card->status & 0x4000)
				card->req->cmd->error = MMC_ERR_FAILED;
				
			if(card->req->cmd->error) return rc;
			
			spin_unlock_irqrestore(&sock->lock, sock->irq_flags);
			r_time = wait_event_timeout(card->event, tifm_sd_test_flag(card, CARD_EVENT | EJECT_EVENT),
						    card->timeout_jiffies);
			spin_lock_irqsave(&sock->lock, sock->irq_flags);
			card->flags &= ~CARD_EVENT;
		}
		card->status &= 0xfffff7ff;
		writel(t_val, sock->addr + SOCK_MMCSD_DATA);
	}
	card->req->cmd->error = tifm_sd_wait_for_card(card);
	return rc;
}

static void tifm_sd_do_cmd_nodma(void *data)
{
	struct tifm_sd *card = (struct tifm_sd*)data;
	struct tifm_dev *sock = card->dev;
	struct mmc_host *host = tifm_get_drvdata(sock);
	struct mmc_request *mrq = card->req;
	struct mmc_data *r_data = mrq->cmd->data;
	char *r_buf = 0;
	size_t m_length = 0;

	if(!get_device(&sock->dev)) return;
	spin_lock_irqsave(&sock->lock, sock->irq_flags);
	
	if(r_data) {
		if(card->flags & CARD_BUSY) {
			mrq->cmd->error = MMC_ERR_FIFO;
			DBG("transfer still in progress\n");
			goto out;
		}

		card->flags |= CARD_BUSY;
		tifm_sd_set_timeout(card, r_data->timeout_clks);
		BUG_ON(r_data->sg_len != 1);
		r_buf = kmap(r_data->sg[0].page) + r_data->sg[0].offset;
		writel(0x0000, sock->addr + SOCK_MMCSD_BUFFER_CONFIG);
		writel(0x0c00 | readl(sock->addr + SOCK_MMCSD_INT_ENABLE), sock->addr + SOCK_MMCSD_INT_ENABLE);
		m_length = mrq->cmd->data->blocks << mrq->cmd->data->blksz_bits;
		writel(mrq->cmd->data->blocks - 1, sock->addr + SOCK_MMCSD_NUM_BLOCKS);
		writel((1 << mrq->cmd->data->blksz_bits) - 1, sock->addr + SOCK_MMCSD_BLOCK_LEN);		
		card->status &= 0xfffff3ff;
	}

	if(!tifm_sd_execute(card, mrq->cmd)) {
		if(r_data) {
			r_data->bytes_xfered = r_data->flags & MMC_DATA_READ
				? tifm_sd_read(card, r_buf, m_length)
				: tifm_sd_write(card, r_buf, m_length);
			
			writel(0xfffff3ff & readl(sock->addr + SOCK_MMCSD_INT_ENABLE), sock->addr + SOCK_MMCSD_INT_ENABLE);
			kunmap(r_data->sg[0].page);
		}
	}
	if(mrq->stop) tifm_sd_execute(card, mrq->stop);

out:
	card->req = 0;
	card->flags &= ~CARD_BUSY;
	if(!(card->flags & EJECT_EVENT)) mmc_request_done(host, mrq);
	spin_unlock_irqrestore(&sock->lock, sock->irq_flags);
	put_device(&sock->dev);
}

static void tifm_sd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct tifm_sd *card = mmc_priv(mmc);
	
	if(card->req) DBG("Error! Have queued command.\n");
	card->req = mrq;
	queue_work(card->dev->wq, &card->cmd_handler);
}

static void tifm_sd_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct tifm_sd *card = mmc_priv(mmc);
	struct tifm_dev *sock = card->dev;
	unsigned int clk_div1, clk_div2;

	if(!get_device(&sock->dev)) return;
	spin_lock_irqsave(&sock->lock, sock->irq_flags);
	
	DBG("setting bus width %d\n", ios->bus_width);
	if(ios->bus_width == MMC_BUS_WIDTH_4) {
		writel(0x8800 | card->clk_div, sock->addr + SOCK_MMCSD_CONFIG);
	} else {
		writel(0x0800 | card->clk_div, sock->addr + SOCK_MMCSD_CONFIG);
	}

	if(ios->clock) {
		clk_div1 = 20000000 / ios->clock; if(!clk_div1) clk_div1 = 1;
		clk_div2 = 24000000 / ios->clock; if(!clk_div2) clk_div2 = 1;
		if((20000000 / clk_div1) > ios->clock) clk_div1++;
		if((24000000 / clk_div2) > ios->clock) clk_div2++;
		if((20000000 / clk_div1) > (24000000 / clk_div2)) {
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
	DBG("want clock %d, setting %d\n", ios->clock, card->clk_div ? card->clk_freq / card->clk_div : 0);

	//vdd, bus_mode, chip_select
	DBG("power mode %d\n", ios->power_mode);
	if(ios->power_mode == MMC_POWER_ON)
		writel(readl(sock->addr + SOCK_CONTROL) | 0x00000040, sock->addr + SOCK_CONTROL);
	else if(ios->power_mode == MMC_POWER_OFF)
		writel(readl(sock->addr + SOCK_CONTROL) & 0xffffffbf, sock->addr + SOCK_CONTROL);

	spin_unlock_irqrestore(&sock->lock, sock->irq_flags);
	put_device(&sock->dev);
}

static int tifm_sd_ro(struct mmc_host *mmc)
{
	int rc;
	struct tifm_sd *card = mmc_priv(mmc);
	struct tifm_dev *sock = card->dev;

	if(!get_device(&sock->dev)) return 1;
	spin_lock_irqsave(&sock->lock, sock->irq_flags);
	
	card->flags |= (0x0200 & readl(sock->addr + SOCK_PRESENT_STATE)) ? CARD_RO : 0;
	rc = (card->flags & CARD_RO) ? 1 : 0;
	
	spin_unlock_irqrestore(&sock->lock, sock->irq_flags);
	put_device(&sock->dev);
	return rc;
}

static struct mmc_host_ops tifm_sd_ops = {
	.request = tifm_sd_request,
	.set_ios = tifm_sd_ios,
	.get_ro  = tifm_sd_ro
};

static void tifm_sd_card_init(void *data)
{
	struct tifm_sd *card = (struct tifm_sd*)data;
	struct tifm_dev *sock = card->dev;
	struct mmc_host *host = tifm_get_drvdata(sock);
	long rc = 1;

	if(!get_device(&sock->dev)) return;
	spin_lock_irqsave(&sock->lock, sock->irq_flags);
	
	writel(0x0000, sock->addr + SOCK_MMCSD_INT_ENABLE);
	writel(0x0002, sock->addr + SOCK_MMCSD_SYSTEM_CONTROL);
	writel(0x000b, sock->addr + SOCK_MMCSD_CONFIG);


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
	spin_unlock_irqrestore(&sock->lock, sock->irq_flags);

	rc = wait_event_timeout(card->event, tifm_sd_test_flag(card, CARD_EVENT | EJECT_EVENT),
			       msecs_to_jiffies(100));
	spin_lock_irqsave(&sock->lock, sock->irq_flags);

	card->flags &= ~CARD_EVENT;
	if(card->flags & EJECT_EVENT) {
		DBG("card removed\n");
		spin_unlock_irqrestore(&sock->lock, sock->irq_flags);
	} else if(!rc) {
		DBG("timed out waiting for interrupt\n");
		spin_unlock_irqrestore(&sock->lock, sock->irq_flags);
		tifm_eject(sock);
	} else {
		card->flags |= HOST_REG;
		PREPARE_WORK(&card->cmd_handler,
			     no_dma ? tifm_sd_do_cmd_nodma : tifm_sd_do_cmd,
			     data);
		spin_unlock_irqrestore(&sock->lock, sock->irq_flags);
		mmc_add_host(host);
	}
	
	put_device(&sock->dev);
	return;
}

static int tifm_sd_probe(struct tifm_dev *dev)
{
	struct mmc_host *mmc;
	struct tifm_sd *card;
	int rc = -EIO;

	if(!(0x8 & readl(dev->addr + SOCK_PRESENT_STATE))) {
		DBG("card gone, unexpectedly\n");
		return rc;
	}

	mmc = mmc_alloc_host(sizeof(struct tifm_sd), &dev->dev);
	if(!mmc) return -ENOMEM;

	card = mmc_priv(mmc);
	card->dev = dev;
	card->clk_div = 61;
	init_waitqueue_head(&card->event);
	INIT_WORK(&card->cmd_handler, tifm_sd_card_init, (void*)card);

	tifm_set_drvdata(dev, mmc);
	dev->signal_irq = tifm_sd_signal_irq;

	card->clk_freq = 20000000;

	mmc->ops = &tifm_sd_ops;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps = MMC_CAP_4_BIT_DATA;
	mmc->f_min = 20000000 / 60;
	mmc->f_max = 24000000;
	mmc->max_hw_segs = 1;
	mmc->max_phys_segs = 1;
	mmc->max_sectors = 64; //6b hw block counter
	mmc->max_seg_size = mmc->max_sectors << 11; //2k maximum hw block length

	queue_work(dev->wq, &card->cmd_handler);

	return 0;
}

static void tifm_sd_remove(struct tifm_dev *dev)
{
	struct mmc_host *mmc = tifm_get_drvdata(dev);
	struct tifm_sd *card = mmc_priv(mmc);

	DBG("card removed\n");
	card->flags |= EJECT_EVENT;
	wake_up_all(&card->event);
	dev->signal_irq = 0;

	if(card->flags & HOST_REG) mmc_remove_host(mmc);

	writel(0xfff8 & readl(dev->addr + SOCK_CONTROL), dev->addr + SOCK_CONTROL);
	writel(0, dev->addr + SOCK_MMCSD_INT_ENABLE);

	tifm_set_drvdata(dev, 0);
	mmc_free_host(mmc);
}

static tifm_device_id tifm_sd_id_tbl[] = {
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
