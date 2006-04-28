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

#define DRIVER_NAME "tifm_sd"
#define DRIVER_VERSION "0.2"

enum { CARD_EVENT = 0x0001, FIFO_EVENT = 0x0002, CARD_BUSY = 0x0004,
       CARD_READY = 0x0008, EJECT_EVENT = 0x0010, FLAG_V2 = 0x0020,
       CARD_RO = 0x0040, HOST_REG = 0x0080 };

typedef enum { SD_INV = 0, SD_MMC = 1, SD_SD = 2, SD_IO = 3 } sd_type_id;

struct tifm_sd {
	struct tifm_dev     *dev;

	wait_queue_head_t   event;
	unsigned int        flags;
	unsigned int        status;
	unsigned int        fifo_status;
	unsigned int        clk_freq;
	unsigned int        clk_div;
	sd_type_id          media_type;

	/* local copy of current ios */
	unsigned int        clock;
	unsigned short      vdd;
	unsigned char       bus_mode;
	unsigned char       chip_select;
	unsigned char       bus_width;
	unsigned char       power_mode;

	struct mmc_request  *req;
	struct work_struct  cmd_handler;
};

/* Called from interrupt handler */
static unsigned int tifm_sd_signal_irq(struct tifm_dev *sock, unsigned int sock_irq_status)
{
	struct mmc_host *mmc = tifm_get_drvdata(sock);
	struct tifm_sd *card = mmc_priv(mmc);
	unsigned int card_status;

	card->flags &= ~(CARD_EVENT | FIFO_EVENT); 
	card->flags |= (sock_irq_status & 0x00000100) ? CARD_EVENT : 0;
	card->flags |= (sock_irq_status & 0x00010000) ? FIFO_EVENT : 0;

        if(card->flags & FIFO_EVENT)
        {
                card->fifo_status = readl(sock->addr + SOCK_DMA_FIFO_STATUS);
                writel(card->fifo_status, sock->addr + SOCK_DMA_FIFO_STATUS);

		if(card->fifo_status & 0x4) card->flags |= CARD_BUSY;
		else wake_up_all(&card->event);

		card->flags &= ~CARD_READY; // was card_active
        }

        if(card->flags & CARD_EVENT)
        {
                card_status = readl(sock->addr + SOCK_MMCSD_STATUS);
                writel(card_status, sock->addr + SOCK_MMCSD_STATUS);
		card->status |= card_status;

		if(card_status & 0x8) {
			writel(0x0014 | readl(sock->addr + SOCK_MMCSD_INT_ENABLE),
			       sock->addr + SOCK_MMCSD_INT_ENABLE);
			card->flags |= FLAG_V2;
		}

		if(card_status & 0x0010) card->flags &= ~FLAG_V2;
		if(card_status & 0x0004) card->flags |= FLAG_V2;
		wake_up_all(&card->event);
        }
	DBG("after irq: card_status = 0x%08x, fifo_status = 0x%08x, flags = 0x%08x\n",
	    card->status, card->fifo_status, card->flags);
	return sock_irq_status;
}

inline static unsigned int tifm_sd_test_flag(struct tifm_sd *card, unsigned int flags)
{
	unsigned int rc = card->flags & flags;
	return rc;
}

static int tifm_sd_wait_for_eoc(struct tifm_sd *card)
{
	int err_code = 0;

	while(!(card->status & 1)) {
		if(card->flags & EJECT_EVENT) err_code = MMC_ERR_TIMEOUT;
		if(card->status & 0x4000) err_code = MMC_ERR_FAILED;
		if(card->status & 0x0080) err_code = MMC_ERR_TIMEOUT;
		if(card->status & 0x0100) err_code = MMC_ERR_BADCRC;
		if(err_code) break;

		if(!wait_event_timeout(card->event,
				       tifm_sd_test_flag(card, CARD_EVENT | EJECT_EVENT),
				       msecs_to_jiffies(500))) // arbitrary time-out value; Should I use data timeout instead?
			err_code = MMC_ERR_TIMEOUT;

		card->flags &= ~CARD_EVENT;
	}
	return err_code;
}

// 0x00    BC   -      0x0000       go idle state
// 0x01    BCR  R3     0x1300  2,6     send op cond
// 0x02    BCR  R2     0x1200  1     all send cid
// 0x03    AC   R1     0x1600  5     set relative address
// 0x03    BCR  R6     0x1600  5     app send relative address
// 0x04    BC   -                   set dsr
// 0x05                0x1300  2,6
// 0x06    AC   R1     0x2100  0,3     app set bus width
// 0x07    AC   R1b    0x2900  4     select card
// 0x09    AC   R2     0x2200  1     send csd
// 0x0a    AC   R2     0x2200  1     send cid
// 0x0b    ADTC R1                  read dat until stop
// 0x0c    AC   R1b    0x2900  4     stop transmission
// 0x0d    AC   R1     0x2100  0,3     send status
// 0x0f    AC   -                   go inactive state
// 0x10    AC   R1     0x2100  0,3     set block len
// 0x11    ADTC R1     0xb100  0,3     read single block
// 0x12    ADTC R1     0xb100  0,3     read multiple block
// 0x14    ADTC R1                  write dat until stop
// 0x17    AC   R1     0x2100  0,3     set block count
// 0x18    ADTC R1     0x3100  0,3     write block
// 0x19    ADTC R1     0x3100  0,3     write multiple block
// 0x1a    ADTC R1                  program cid
// 0x1b    ADTC R1                  program csd
// 0x1c    AC   R1b                 set write prot
// 0x1d    AC   R1b                 clr write prot
// 0x1e    ADTC R1b                 send write prot
// 0x23    AC   R1                  erase group start
// 0x24    AC   R1                  erase group end
// 0x26    AC   R1b                 erase
// 0x27    AC   R4                  fast io
// 0x28    BCR  R5                  go irq state
// 0x29    BCR  R3     0x1300  2,6    app op cond
// 0x2a    ADTC R1     0x2100  0,3    lock unlock
// 0x2a    AC   R1     0x2100  0,3    app lock unlock
// 0x33    ADTC R1                  app send scr
// 0x37    AC   R1     0x2100  0,3    app cmd
// 0x38    ADTC R1                  gen cmd

static inline unsigned int tifm_sd_op_flags(unsigned int cmd_flags, unsigned int data_flags)
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
		/* most probably and when implemented:
		case MMC_RSP_R4:
			rc |= 0x0100;
			break;
		case MMC_RSP_R6R:
			rc |= 0x0300;
			break;
		*/
	}

	switch(cmd_flags & MMC_CMD_MASK)
	{
		case MMC_CMD_AC:
			rc |= 0x2000;
			break;
		case MMC_CMD_ADTC:
			rc |= 0x3000;
			if(data_flags & MMC_DATA_READ) rc |= 0x8000;
			break;
		case MMC_CMD_BCR:
			rc |= 0x1000;
			break;
	}
	return rc;
}


static void tifm_sd_execute(void *data)
{
	struct tifm_sd *card = (struct tifm_sd*)data;
	struct tifm_dev *sock = card->dev;
	struct mmc_host *host = tifm_get_drvdata(sock);
	struct mmc_command *cmd = card->req->cmd;
	struct mmc_request *mrq = card->req;
	int err_code = 0;

	DBG("started processing opcode 0x%x, arg: 0x%x\n", cmd->opcode, cmd->arg);
	if(!get_device(&sock->dev)) return;	
	down(&sock->lock);

	writel((cmd->arg >> 16) & 0xffff, sock->addr + SOCK_MMCSD_ARG_HIGH);
	writel(cmd->arg & 0xffff, sock->addr + SOCK_MMCSD_ARG_LOW);
	card->status = 0;
	card->flags &= ~CARD_EVENT;
	writel(cmd->opcode | tifm_sd_op_flags(cmd->flags, cmd->data ? cmd->data->flags : 0),
		sock->addr + SOCK_MMCSD_COMMAND);

	err_code = tifm_sd_wait_for_eoc(card);

	cmd->error = err_code;
	if(!err_code) {
		cmd->resp[0] = readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x1c); cmd->resp[0] <<= 16;
		cmd->resp[0] |= readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x18);
		cmd->resp[1] = readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x14); cmd->resp[1] <<= 16;
		cmd->resp[1] |= readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x10);
		cmd->resp[2] = readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x0c); cmd->resp[2] <<= 16;
		cmd->resp[2] |= readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x08);
		cmd->resp[3] = readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x04); cmd->resp[3] <<= 16;
		cmd->resp[3] |= readl(sock->addr + SOCK_MMCSD_RESPONSE + 0x00);
		DBG("resp: %08x %08x %08x %08x\n", cmd->resp[0], cmd->resp[1], cmd->resp[2], cmd->resp[3]);
	}
	DBG("finished processing opcode 0x%x, error code %d\n", cmd->opcode, cmd->error);
	mrq = card->req;
	card->req = 0;
	up(&sock->lock);
	put_device(&sock->dev);
	mmc_request_done(host, mrq);
}

static void tifm_sd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct mmc_command *cmd = mrq->cmd;
	struct tifm_sd *card = mmc_priv(mmc);
	struct tifm_dev *sock = card->dev;

	DBG("called; mmc opcode 0x%x, flags 0x%x\n", cmd->opcode, cmd->flags);
	down(&sock->lock);
	if(card->req) DBG("Error! Have queued command.\n");
	card->req = mrq;
	tifm_schedule_work(card->dev, &card->cmd_handler);
	up(&sock->lock);
}

static void tifm_sd_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct tifm_sd *card = mmc_priv(mmc);
	struct tifm_dev *sock = card->dev;

	if(!get_device(&sock->dev)) return;
	DBG("set_ios (power=%u, clock=%uHz, vdd=%u, width=%u, mode=%u)\n",
	    ios->power_mode, ios->clock, ios->vdd, ios->bus_width, ios->bus_mode);

	down(&sock->lock);
	if(card->bus_width != ios->bus_width) {
		DBG("setting bus width %d\n", ios->bus_width);
		if(ios->bus_width == MMC_BUS_WIDTH_4) {
			card->clk_freq = 24000000;
			mmc->f_min = card->clk_freq / 60;
			writel(0x0100 | readl(sock->addr + SOCK_CONTROL), sock->addr + SOCK_CONTROL);
		} else {
			card->clk_freq = 20000000;
			mmc->f_min = card->clk_freq / 60;
			writel(0xfffffeff & readl(sock->addr + SOCK_CONTROL), sock->addr + SOCK_CONTROL);
		}
		card->bus_width = ios->bus_width;
	}

	if(ios->clock && ios->clock != card->clock) {
		card->clk_div = card->clk_freq / ios->clock;
		if(card->clk_div * ios->clock < card->clk_freq) card->clk_div++;
		writel(card->clk_div | (0xffc0 & readl(sock->addr + SOCK_MMCSD_CONFIG)),
		       sock->addr + SOCK_MMCSD_CONFIG);
		DBG("setting clock divider %d, base freq %d\n", card->clk_div, card->clk_freq);
		card->clock = card->clk_freq / card->clk_div;
	}
	
	//vdd, bus_mode, chip_select
	if(ios->power_mode != card->power_mode) {
		if(ios->power_mode == MMC_POWER_ON)
			tifm_sock_power(sock, 1);
		else if(ios->power_mode == MMC_POWER_OFF)
			tifm_sock_power(sock, 0);
		card->power_mode = ios->power_mode;
	}
	up(&sock->lock);
	put_device(&sock->dev);
}

static int tifm_sd_ro(struct mmc_host *mmc)
{
	int rc;
	struct tifm_sd *card = mmc_priv(mmc);
	struct tifm_dev *sock = card->dev;

	if(!get_device(&sock->dev)) return 1;
	down(&sock->lock);
	card->flags |= (0x0200 & readl(sock->addr + SOCK_PRESENT_STATE)) ? CARD_RO : 0;
	rc = (card->flags & CARD_RO) ? 1 : 0;
	DBG("value: %d\n", rc);
	up(&sock->lock);
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

	long rc;

	if(!get_device(&sock->dev)) return;
	down(&sock->lock);
	DBG("called\n");
	
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
	rc = wait_event_timeout(card->event, tifm_sd_test_flag(card, CARD_EVENT | EJECT_EVENT),
			       msecs_to_jiffies(100));
	card->flags &= ~CARD_EVENT;
	if(card->flags & EJECT_EVENT) {
		DBG("card removed\n");
		up(&sock->lock);
	} else if(!rc) {
		DBG("timed out waiting for interrupt\n");
		up(&sock->lock);
		tifm_eject(sock);
	} else {
		card->flags |= CARD_READY | HOST_REG;
		PREPARE_WORK(&card->cmd_handler, tifm_sd_execute, (void*)card);
		up(&sock->lock);
		mmc_add_host(host);
	}
	
	put_device(&sock->dev);
	return;
}

static int tifm_sd_probe(struct tifm_dev *dev)
{
	struct mmc_host *mmc;
	struct tifm_sd *card;
	u64 t;
	int rc = -EIO;

	DBG("called\n");
	if(!(0x8 & readl(dev->addr + SOCK_PRESENT_STATE))) {
		DBG("card gone, unexpectedly\n");
		return rc;
	}

	mmc = mmc_alloc_host(sizeof(struct tifm_sd), &dev->dev);
	if(!mmc) return -ENOMEM;

	card = mmc_priv(mmc);
	card->dev = dev;
	card->clk_div = 60;
	init_waitqueue_head(&card->event);
	INIT_WORK(&card->cmd_handler, tifm_sd_card_init, (void*)card);

	tifm_set_drvdata(dev, mmc);
	dev->signal_irq = tifm_sd_signal_irq;


	writel(0x0000, dev->addr + SOCK_MMCSD_INT_ENABLE);
	writel(0x0002, dev->addr + SOCK_MMCSD_SYSTEM_CONTROL);
	writel(card->clk_div | 0x0800, dev->addr + SOCK_MMCSD_CONFIG);

	t=get_jiffies_64();
	while(get_jiffies_64() - t < msecs_to_jiffies(100))
	{
		if(0x1 & readl(dev->addr + SOCK_MMCSD_SYSTEM_STATUS))
		{
			rc = 0;
			break;
		}
		msleep(10);
	}
	if(rc) {
		DBG("card not ready?\n");
		goto err_out_free_mmc;
	}
		
	// There are two base frequencies, set through SOCK_CONTROL register: 20MHz and 24 MHz.
	card->clk_freq = 20000000;

	mmc->ops = &tifm_sd_ops;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps = MMC_CAP_4_BIT_DATA;
	mmc->f_min = 334000; // TI never set clk_div above 60
	mmc->f_max = 24000000;
	mmc->max_hw_segs = 1;
	mmc->max_phys_segs = 1;
	mmc->max_sectors = 0x3f;
	mmc->max_seg_size = mmc->max_sectors << 9;

	tifm_schedule_work(card->dev, &card->cmd_handler);

	return 0;

err_out_free_mmc:
	writel(0xfff8 & readl(dev->addr + SOCK_CONTROL), dev->addr + SOCK_CONTROL);
	writel(0, dev->addr + SOCK_MMCSD_INT_ENABLE);
	dev->signal_irq = 0;
	tifm_set_drvdata(dev, 0);
	mmc_free_host(mmc);
	return rc;
}

static void tifm_sd_remove(struct tifm_dev *dev)
{
	struct mmc_host *mmc = tifm_get_drvdata(dev);
	struct tifm_sd *card = mmc_priv(mmc);

	DBG("called\n");

	if(card->flags & HOST_REG) mmc_remove_host(mmc);

	writel(0xfff8 & readl(dev->addr + SOCK_CONTROL), dev->addr + SOCK_CONTROL);
	writel(0, dev->addr + SOCK_MMCSD_INT_ENABLE);

	card->flags |= EJECT_EVENT;
	wake_up_all(&card->event);
	dev->signal_irq = 0;

	tifm_cancel_work(card->dev, &card->cmd_handler);

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
