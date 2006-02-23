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

#include "tifmxx.h"
#include <linux/mmc/protocol.h>

static void tifmxx_mmcsd_request(struct mmc_host* host, struct mmc_request* req)
{
}

void tifmxx_mmcsd_set_ios(struct mmc_host *host, struct mmc_ios *ios)
{
}

int tifmxx_mmcsd_get_ro(struct mmc_host *host)
{
	return 1;
}

static struct mmc_host_ops tifmxx_mmcsd_ops = {
	.request = tifmxx_mmcsd_request,
	.set_ios = tifmxx_mmcsd_set_ios,
	.get_ro  = tifmxx_mmcsd_get_ro
};

void tifmxx_mmcsd_eject(struct tifmxx_socket* sock)
{
	sock->eject = 0;
	sock->signal_int = 0;
	sock->process_int = 0;

	sock->flags |=	CARD_EJECTED;
	wake_up_all(&sock->hw_wq);
	mmc_remove_host(sock->c_dev.platform_data);
	mmc_free_host(sock->c_dev.platform_data);
	device_unregister(&sock->c_dev);
	memset(&sock->c_dev, 0, sizeof(struct device));
}

void tifmxx_mmcsd_signal_int(struct tifmxx_socket* sock, unsigned int sock_int_status)
{
	struct tifmxx_mmcsd_private* sock_data = mmc_priv(sock->c_dev.platform_data);

	sock->flags &= ~(INT_B0 | INT_B1); sock->flags |= sock_int_status;
	
	if(!(sock->flags & INT_B1)) {
		sock->dma_fifo_status = readl(sock->sock_addr + SOCK_DMA_FIFO_STATUS);
		writel(sock->dma_fifo_status, sock->sock_addr + SOCK_DMA_FIFO_STATUS);
	}

	if(sock->flags & INT_B0) {
		sock_data->mmcsd_status = readl(sock->sock_addr + SOCK_MMCSD_STATUS);
		sock_data->next_cmd_status = sock_data->mmcsd_status | sock_data->cmd_status;
		writel(sock_data->mmcsd_status, sock->sock_addr + SOCK_MMCSD_STATUS);
	}
}

void tifmxx_mmcsd_process_int(struct tifmxx_socket* sock)
{
	struct tifmxx_mmcsd_private* sock_data = mmc_priv(sock->c_dev.platform_data);

	if(!(sock->flags & INT_B1)) {
		if(sock->dma_fifo_status & 0x4) sock->flags |= CARD_BUSY; 
           	else {
			sock->flags |= SOCK_EVENT;
			wake_up_all(&sock->hw_wq); // signal event_2
           	}
           sock->flags &= ~CARD_ACTIVE;
	}

	if(sock->flags & INT_B0) {
		if(sock_data->mmcsd_status & 0x8) {
			writel(0x14 | readl(sock->sock_addr + SOCK_MMCSD_INT_ENABLE), 
			       sock->sock_addr + SOCK_MMCSD_INT_ENABLE);
			sock->flags |= FLAG_V2;
		}

		if(sock_data->mmcsd_status & 0x10) sock->flags &= ~FLAG_V2;
		if(sock_data->mmcsd_status & 0x4) sock->flags |= FLAG_V2;

		sock_data->cmd_status = sock_data->next_cmd_status;
		sock->flags |= MMCSD_EVENT;
		wake_up_all(&sock->hw_wq);
		sock->flags &= ~INT_B0;
	}
}

void dummy_release(struct device* dev)
{
}

void tifmxx_mmcsd_insert(struct tifmxx_socket* sock)
{
	extern struct bus_type tifmxx_bus_type;
	sock->c_dev.parent = &sock->host->dev->dev;
	snprintf(sock->c_dev.bus_id, BUS_ID_SIZE, "tifm%d", sock->id);
	sock->c_dev.bus = &tifmxx_bus_type;
	sock->c_dev.release = dummy_release;
	
	if(!device_register(&sock->c_dev)) {
		struct tifmxx_mmcsd_private* mmc_data;
		struct mmc_host* mmc_h;

		sock->c_dev.platform_data = mmc_alloc_host(sizeof(struct tifmxx_mmcsd_private),
							   &sock->c_dev);

		mmc_h = sock->c_dev.platform_data;
		if(mmc_h) {
			mmc_h->ops = &tifmxx_mmcsd_ops;
			mmc_h->ocr_avail = MMC_VDD_32_33|MMC_VDD_33_34;
			mmc_h->caps = MMC_CAP_4_BIT_DATA;
			mmc_h->max_hw_segs = 1;
			mmc_h->max_phys_segs = 1;
			mmc_h->max_sectors = 0x3f;
			mmc_h->max_seg_size = mmc_h->max_sectors << 9;
			mmc_h->f_max = 80000000;
			mmc_h->f_min = 10000;

			mmc_data = mmc_priv(mmc_h);
			mmc_data->sock = sock;
			sock->eject = tifmxx_mmcsd_eject;
			sock->signal_int = tifmxx_mmcsd_signal_int;
			sock->process_int = tifmxx_mmcsd_process_int;

			sock->flags = 0;

			mmc_add_host(mmc_h);
			return;
		}
		else device_unregister(&sock->c_dev);
	}
	// otherwise do nothing - may be problems will go away by themselves
}
