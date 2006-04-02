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
       CARD_ACTIVE = 0x0008, EJECT_EVENT = 0x0010, FLAG_V2 = 0x0020,
       CARD_RO = 0x0040 };

/* Called from interrupt handler */
static void tifm_sd_signal_irq(struct tifm_dev *sock, unsigned int sock_irq_status)
{
	struct tifm_sd *card = tifm_get_drvdata(sock);
	unsigned int card_status;

	card->flags &= ~(CARD_EVENT | FIFO_EVENT); 
	card->flags |= sock_irq_status;

        if(card->flags & FIFO_EVENT)
        {
                card->fifo_status = readl(sock->addr + SOCK_DMA_FIFO_STATUS);
                writel(card->fifo_status, sock->addr + SOCK_DMA_FIFO_STATUS);

		if(card->fifo_status & 0x4) card->flags |= CARD_BUSY;
		else wake_up_all(&card->event);

		card->flags &= ~CARD_ACTIVE;
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
}

static void tifm_sd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct mmc_command *cmd = mrq->cmd;

	DBG("called; mmc opcode 0x%x, flags 0x%x\n", cmd->opcode, cmd->flags);
	cmd->error = MMC_ERR_TIMEOUT;
	mmc_request_done(mmc, mrq);
}

static void tifm_sd_power(struct tifm_dev *sock, unsigned char mode)
{
	unsigned int rc;

	rc = readl(sock->addr + SOCK_CONTROL);
	switch(mode) {
		case MMC_POWER_OFF:
			DBG("power off\n");
			rc &= 0xffffffbf;
			writel(rc, sock->addr + SOCK_CONTROL);
			break;
		case MMC_POWER_UP:
			DBG("power up\n");
			break;
		case MMC_POWER_ON:
			DBG("power on\n");
			rc |= 0x40;
			writel(rc, sock->addr + SOCK_CONTROL);
			break;
	}
}

static void tifm_sd_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct tifm_dev *sock = mmc_priv(mmc);
	unsigned long f;

	DBG("set_ios (power=%u, clock=%uHz, vdd=%u, mode=%u)\n",
	    ios->power_mode, ios->clock, ios->vdd, ios->bus_mode);

	spin_lock_irqsave(&sock->lock, f);
	//clock, vdd, bus_mode, chip_select
	tifm_sd_power(sock, ios->power_mode);
	//bus_width
	spin_unlock_irqrestore(&sock->lock, f);
}

static int tifm_sd_ro(struct mmc_host *mmc)
{
	int rc;
	unsigned long f;
	struct tifm_sd *card = mmc_priv(mmc);

	DBG("called\n");
	spin_lock_irqsave(&card->dev->lock, f);
	rc = (card->flags & CARD_RO) ? 1 : 0;
	spin_unlock_irqrestore(&card->dev->lock, f);
	return rc;
}

static struct mmc_host_ops tifm_sd_ops = {
	.request = tifm_sd_request,
	.set_ios = tifm_sd_ios,
	.get_ro  = tifm_sd_ro
};

static int tifm_sd_probe(struct tifm_dev *dev)
{
	struct mmc_host *mmc;
	struct tifm_sd *card;

	DBG("called\n");
	mmc = mmc_alloc_host(sizeof(struct tifm_sd), &dev->dev);
	if(!mmc) return -ENOMEM;

	card = mmc_priv(mmc);
	card->dev = dev;
	init_waitqueue_head(&card->event);

	dev->signal_irq = tifm_sd_signal_irq;
	
	mmc->ops = &tifm_sd_ops;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps = MMC_CAP_4_BIT_DATA;
	mmc->f_min = 0;
	mmc->f_max = 0;
	mmc->max_hw_segs = 1;
	mmc->max_phys_segs = 1;
	mmc->max_sectors = 0x3f;
	mmc->max_seg_size = mmc->max_sectors << 9;

	tifm_set_drvdata(dev, mmc);
	mmc_add_host(mmc);	

	return 0;
}

static void tifm_sd_remove(struct tifm_dev *dev)
{
	struct mmc_host *mmc = tifm_get_drvdata(dev);
	struct tifm_sd *card = mmc_priv(mmc);
	unsigned long f;

	DBG("called\n");

	mmc_remove_host(mmc);
	spin_lock_irqsave(&dev->lock, f);
	card->flags |= EJECT_EVENT;
	wake_up_all(&card->event);
	dev->signal_irq = 0;
	tifm_set_drvdata(dev, 0);
	spin_unlock_irqrestore(&dev->lock, f);
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
