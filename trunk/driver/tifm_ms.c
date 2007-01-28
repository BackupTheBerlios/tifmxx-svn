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

/* The meaning of the bit majority in this constant is unknown. */
#define TIFM_MS_SERIAL       0x04010

#define TIFM_MS_SYS_LATCH    0x00100
#define TIFM_MS_SYS_NOT_RDY  0x00800
#define TIFM_MS_SYS_DATA     0x10000

/* Hardware flags */
enum {
	FIFO_RDY = 0x0001, /* fifo operation finished  */
	EJECT    = 0x0002, /* drop all commands        */
	ERR_TO   = 0x0100, /* command time-out         */
	ERR_CRC  = 0x0200, /* crc error                */
	READY    = 0x1000, /* TPC completed            */
	CARD_INT = 0x2000  /* card operation completed */
};

struct tifm_ms {
	struct tifm_dev     *dev;
	unsigned int        state;
	unsigned int        desired_state;
	unsigned int        mode_mask;
	unsigned long       timeout_jiffies;

	struct tasklet_struct finish_tasklet;
	struct timer_list         timer;
	struct memstick_request   *req;
//	wait_queue_head_t     notify;

};

/* Called from interrupt handler */
static void tifm_ms_signal_irq(struct tifm_dev *sock,
			       unsigned int sock_irq_status)
{
	struct tifm_ms *host;
	unsigned int host_status = 0, fifo_status = 0;

	spin_lock(&sock->lock);
	host = memstick_priv((struct memstick_host*)tifm_get_drvdata(sock));

	if (sock_irq_status & TIFM_IRQ_FIFOMASK(1)) {
		fifo_status = readl(sock->addr + SOCK_DMA_FIFO_STATUS);
		writel(fifo_status, sock->addr + SOCK_DMA_FIFO_STATUS);

		host->state |= fifo_status & FIFO_RDY;
	}

	if (sock_irq_status & TIFM_IRQ_CARDMASK(1)) {
		host_status = readl(sock->addr + SOCK_MS_STATUS);
		writel(TIFM_MS_SYS_NOT_RDY | readl(sock->addr + SOCK_MS_SYSTEM),
		       sock->addr + SOCK_MS_SYSTEM);

		if (!host->req)
			goto done;

		if (host_status & ERR_TO)
			host->req->error = MEMSTICK_ERR_TIMEOUT;
		else if (host_status & ERR_CRC)
			host->req->error = MEMSTICK_ERR_TIMEOUT;

		if (host->req->error) {
			writel(TIFM_FIFO_INT_SETALL,
			       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
			writel(TIFM_DMA_RESET, sock->addr + SOCK_DMA_CONTROL);
			goto done;
		}
		host->state |= host_status & (READY | CARD_INT);	
	}

	if ((host->state & host->desired_state) != host->desired_state) {
		spin_unlock(&sock->lock);
		return;
	}
done:
	tasklet_schedule(&host->finish_tasklet);
	spin_unlock(&sock->lock);
	return;
}

static void tifm_ms_request(struct memstick_host *msh,
			    struct memstick_request *req)
{
	struct tifm_ms *host = memstick_priv(msh);
	struct tifm_dev *sock = host->dev;
	unsigned int cnt;
	unsigned int *data_ptr = (unsigned int*)req->short_data;
	unsigned int cmd = (req->tpc & 0xf) << 12;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);

	if (host->state & EJECT) {
		spin_unlock_irqrestore(&sock->lock, flags);
		goto err_out;
	}

	if (host->req) {
		printk(KERN_ERR DRIVER_NAME ": unfinished request detected\n");
		spin_unlock_irqrestore(&sock->lock, flags);
		goto err_out;
	}

	host->state = 0;
	host->desired_state = READY;
	host->desired_state |= req->need_card_int ? CARD_INT : 0;

	/* The meaning of the bit majority in this constant is unknown. */
	writel(host->mode_mask | 0x2607, sock->addr + SOCK_MS_SYSTEM);

	if (req->sg) {
		if (req->data_dir == WRITE) {
		} else {
		}

		writel(TIFM_MS_SYS_DATA | readl(sock->addr + SOCK_MS_SYSTEM),
		       sock->addr + SOCK_MS_SYSTEM);
		host->desired_state |= FIFO_RDY;
	}

	if (req->short_data_dir == WRITE) {
		for (cnt = 0; cnt < req->short_data_len; cnt += 4) {
			writel(TIFM_MS_SYS_LATCH
			       | readl(sock->addr + SOCK_MS_SYSTEM),
			       sock->addr + SOCK_MS_SYSTEM);
			writel(data_ptr[cnt >> 2], sock->addr + SOCK_MS_DATA);
		}
	} else {
		cmd |= req->short_data_len & 0x3ff;
	}

	host->req = req;
	mod_timer(&host->timer, jiffies + host->timeout_jiffies);

	writel(TIFM_CTRL_LED | readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);

	writel(TIFM_MS_SYS_NOT_RDY | readl(sock->addr + SOCK_MS_SYSTEM),
	       sock->addr + SOCK_MS_SYSTEM);

	writel(cmd, sock->addr + SOCK_MS_COMMAND);
	spin_unlock_irqrestore(&sock->lock, flags);
	return;

err_out:
	req->error = MEMSTICK_ERR_TIMEOUT;
	memstick_request_done(msh, req);
}

static void tifm_ms_end_request(unsigned long data)
{
	struct tifm_ms *host = (struct tifm_ms*)data;
	struct tifm_dev *sock = host->dev;
	struct memstick_host *msh = tifm_get_drvdata(sock);
	struct memstick_request *req;
	unsigned int *data_ptr;
	unsigned int cnt;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);

	del_timer(&host->timer);
	req = host->req;
	host->req = NULL;

	if (!req) {
		printk(KERN_ERR DRIVER_NAME ": no request to complete?\n");
		spin_unlock_irqrestore(&sock->lock, flags);
		return;
	}

	if (req->short_data_dir == READ) {
		data_ptr = (unsigned int*)req->short_data;
		for (cnt = 0; cnt < req->short_data_len; cnt += 4)
			data_ptr[cnt >> 2] = readl(sock->addr + SOCK_MS_DATA);
	}

	if (req->sg) {
		writel((~TIFM_MS_SYS_DATA) & readl(sock->addr + SOCK_MS_SYSTEM),
		       sock->addr + SOCK_MS_SYSTEM);

	}
	writel((~TIFM_CTRL_LED) & readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);

	spin_unlock_irqrestore(&sock->lock, flags);

	memstick_request_done(msh, req);
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

static void tifm_ms_terminate(struct tifm_ms *host)
{
	struct tifm_dev *sock = host->dev;
	unsigned long flags;

	spin_lock_irqsave(&sock->lock, flags);
	host->state |= EJECT;
	if (host->req) {
		writel(TIFM_FIFO_INT_SETALL,
		       sock->addr + SOCK_DMA_FIFO_INT_ENABLE_CLEAR);
		writel(0, sock->addr + SOCK_DMA_FIFO_INT_ENABLE_SET);
		tasklet_schedule(&host->finish_tasklet);
	}
	spin_unlock_irqrestore(&sock->lock, flags);
}

static void tifm_ms_abort(unsigned long data)
{
	struct tifm_ms *host = (struct tifm_ms*)data;

	printk(KERN_ERR DRIVER_NAME
	       ": card failed to respond for a long period of time");

	tifm_ms_terminate(host);
	tifm_eject(host->dev);
}

static int tifm_ms_initialize_host(struct tifm_ms *host)
{
	struct tifm_dev *sock = host->dev;

	host->mode_mask = 0x4010;
	writel(0x8000, sock->addr + SOCK_MS_SYSTEM);
	writel(0x0a00, sock->addr + SOCK_MS_SYSTEM);
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
		printk(KERN_WARNING DRIVER_NAME ": card gone, unexpectedly\n");
		return rc;
	}

	msh = memstick_alloc_host(sizeof(struct tifm_ms), &sock->dev);
	if (!msh)
		return -ENOMEM;

	host = memstick_priv(msh);
	tifm_set_drvdata(sock, msh);
	host->dev = sock;
	host->timeout_jiffies = msecs_to_jiffies(1000);
	tasklet_init(&host->finish_tasklet,
		     tifm_ms_end_request,
		     (unsigned long)host);

	setup_timer(&host->timer, tifm_ms_abort, (unsigned long)host);

	msh->request = tifm_ms_request;
	msh->set_ios = tifm_ms_ios;
	sock->signal_irq = tifm_ms_signal_irq;
	rc = tifm_ms_initialize_host(host);

	if (!rc)
		rc = memstick_add_host(msh);
	if (rc)
		goto out_free_memstick;

	return 0;
out_free_memstick:
	memstick_free_host(msh);
	return rc;
}

static void tifm_ms_remove(struct tifm_dev *sock)
{
	struct memstick_host *msh = tifm_get_drvdata(sock);
	struct tifm_ms *host = memstick_priv(msh);

	del_timer_sync(&host->timer);
	tifm_ms_terminate(host);
//	wait_event_timeout(host->notify, host->flags & EJECT_DONE,
//			   host->timeout_jiffies);
	tasklet_kill(&host->finish_tasklet);
	memstick_remove_host(msh);

	writel(0x0a00, sock->addr + SOCK_MS_SYSTEM);
	writel(0xffffffff, sock->addr + SOCK_MS_STATUS);

	/* The meaning of the bit majority in this constant is unknown. */
	writel(0xfff8 & readl(sock->addr + SOCK_CONTROL),
	       sock->addr + SOCK_CONTROL);

	tifm_set_drvdata(sock, NULL);
	memstick_free_host(msh);
}

#define tifm_ms_suspend NULL
#define tifm_ms_resume NULL

static tifm_media_id tifm_ms_id_tbl[] = {
	FM_MS, 0
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
