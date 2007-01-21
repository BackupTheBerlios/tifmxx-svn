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

struct tifm_ms {
	struct tifm_dev     *dev;

	unsigned int        cmd_tr_mode_mask;
	unsigned int        flags;

	unsigned long       timeout_jiffies;

//	struct tasklet_struct finish_tasklet;
	struct timer_list     timer;
//	struct memstick_request    *req;
//	wait_queue_head_t     notify;

//	size_t                written_blocks;
//	size_t                buffer_size;
//	size_t                buffer_pos;
};

/* Called from interrupt handler */
static void tifm_ms_signal_irq(struct tifm_dev *sock,
			       unsigned int sock_irq_status)
{
}

static void tifm_ms_request(struct memstick_host *host,
			    struct memstick_request *req)
{
}

static void tifm_ms_ios(struct memstick_host *host, struct memstick_ios *ios)
{
}

static int tifm_ms_initialize_host(struct tifm_ms *host)
{
	struct tifm_dev *sock = host->dev;

	host->cmd_tr_mode_mask = 0x4010;
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

	//setup_timer(&host->timer, tifm_ms_abort, (unsigned long)host);

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
//	tifm_sd_terminate(host);
//	wait_event_timeout(host->notify, host->flags & EJECT_DONE,
//			   host->timeout_jiffies);
//	tasklet_kill(&host->finish_tasklet);
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
