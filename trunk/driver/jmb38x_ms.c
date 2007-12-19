/*
 *  jmb38x_ms.c - JMicron JMB38x MemoryStick card reader
 *
 *  Copyright (C) 2008 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/pci.h>

#include "linux/memstick.h"

#define PCI_DEVICE_ID_JMICRON_JMB38X_MS 0x2383
#define DRIVER_NAME "jmb38x_ms"

enum {
	DMA_ADDRESS       = 0x00,
	DMA_BLOCK         = 0x04,
	DMA_CONTROL       = 0x08,
	TPC_P0            = 0x0c,
	TPC_P1            = 0x10,
	TPC               = 0x14,
	HOST_CONTROL      = 0x18,
	DATA              = 0x1c,
	STATUS            = 0x20,
	INT_STATUS        = 0x24,
	INT_STATUS_ENABLE = 0x28,
	INT_SIGNAL_ENABLE = 0x2c,
	TIMER             = 0x30,
	TIMER_CONTROL     = 0x34,
	PAD_OUTPUT_ENABLE = 0x38,
	PAD_PU_PD         = 0x3c,
	CLOCK_DELAY       = 0x40,
	ADMA_ADDRESS      = 0x44,
	CLOCK_CONTROL     = 0x48,
	LED_CONTROL       = 0x4c,
	VERSION           = 0x50
};

struct jmb38x_ms_host {
	void __iomem *addr;
	spinlock_t   lock;
	int          id;
	char         host_id[DEVICE_ID_SIZE];
	int          irq;

};

struct jmb38x_ms {
	struct pci_dev        *pdev;
	int                   num_slots;
	struct jmb38x_ms_host hosts[];
};

#define HOST_CONTROL_RST       0x00008000
#define HOST_CONTROL_LED       0x00000400
#define HOST_CONTROL_FAST_CLK  0x00000200
#define HOST_CONTROL_DMA_RST   0x00000100
#define HOST_CONTROL_POWER_EN  0x00000080
#define HOST_CONTROL_CLOCK_EN  0x00000040

#define HOST_CONTROL_IF_SERIAL 0x0
#define HOST_CONTROL_IF_PAR4   0x1
#define HOST_CONTROL_IF_PAR8   0x3

#define PAD_PU_PD_OFF         0x7FFF0000
#define PAD_PU_PD_ON_SD_SLOTA 0x40800F0F
#define PAD_PU_PD_ON_SD_SLOTB 0x00007FFF
#define PAD_PU_PD_ON_MS_SOCK0 0x4f8f0000
#define PAD_PU_PD_ON_MS_SOCK1 0x7fff0000
#define PAD_PU_PD_ON_XD       0x4F8F0000

#define PAD_OUTPUT_ENABLE_SD  0x0F3F
#define PAD_OUTPUT_ENABLE_MS  0x0F3F
#define PAD_OUTPUT_ENABLE_XD1 0x4FCF
#define PAD_OUTPUT_ENABLE_XD2 0x5FFF

static irqreturn_t jmb38x_ms_isr(int irq, void *dev_id)
{
	return IRQ_NONE;
}

#ifdef CONFIG_PM

static int jmb38x_ms_suspend(struct pci_dev *dev, pm_message_t state)
{
#warning Ugh! Ugh!
	return 0;
}

static int jmb38x_ms_resume(struct pci_dev *dev)
{
#warning Ugh! Ugh!
	return 0;
}

#else

#define jmb38x_ms_suspend NULL
#define jmb38x_ms_resume NULL

#endif /* CONFIG_PM */

static int jmb38x_ms_count_slots(struct pci_dev *pdev)
{
	int cnt, rc = 0;

	for (cnt = 0; cnt < PCI_ROM_RESOURCE; ++cnt) {
		if (!(IORESOURCE_MEM & pci_resource_flags(pdev, cnt)))
			continue;

		if (256 != pci_resource_len(pdev, cnt))
			continue;

		++rc;
	}
	return rc;
}

static int jmb38x_ms_get_regions(struct jmb38x_ms *jm)
{
	int cnt, h_cnt = 0;

	for (cnt = 0; cnt < PCI_ROM_RESOURCE; ++cnt) {
		if (!(IORESOURCE_MEM & pci_resource_flags(jm->pdev, cnt)))
			continue;

		if (256 != pci_resource_len(jm->pdev, cnt))
			continue;

		jm->hosts[h_cnt++].addr = ioremap(pci_resource_start(jm->pdev,
								     cnt),
						  pci_resource_len(jm->pdev,
								   cnt));
		if (!jm->hosts[h_cnt - 1].addr) {
			for (--h_cnt; h_cnt >= 0; --h_cnt)
				iounmap(jm->hosts[h_cnt].addr);

			return -ENOMEM;
		}
	}
	return 0;
}

static int jmb38x_ms_host_init(struct jmb38x_ms_host *host)
{
	int rc = 0;

	spin_lock_init(&host->lock);

	snprintf(host->host_id, DEVICE_ID_SIZE, DRIVER_NAME ":slot%d",
		 host->id);

	rc = request_irq(host->irq, jmb38x_ms_isr, IRQF_SHARED, host->host_id,
			 host);
	if (rc)
		goto err_out_unmap;

	return 0;
err_out_unmap:
	iounmap(host->addr);
	host->addr = NULL;
	return rc;
}

static void jmb38x_ms_host_remove(struct jmb38x_ms_host *host)
{
	writel(readl(host->addr + HOST_CONTROL)
	       & ~(HOST_CONTROL_CLOCK_EN | HOST_CONTROL_POWER_EN),
	       host->addr + HOST_CONTROL);

	mmiowb();
	free_irq(host->irq, host);
	iounmap(host->addr);
}

static int jmb38x_ms_probe(struct pci_dev *pdev,
			   const struct pci_device_id *dev_id)
{
	struct jmb38x_ms *jm;
	int pci_dev_busy = 0;
	int rc, cnt;

	rc = pci_set_dma_mask(pdev, DMA_32BIT_MASK);
	if (rc)
		return rc;

	rc = pci_enable_device(pdev);
	if (rc)
		return rc;

	pci_set_master(pdev);

	rc = pci_request_regions(pdev, DRIVER_NAME);
	if (rc) {
		pci_dev_busy = 1;
		goto err_out;
	}

	cnt = jmb38x_ms_count_slots(pdev);
	if (!cnt) {
		rc = -ENODEV;
		pci_dev_busy = 1;
		goto err_out;
	}

	jm = kzalloc(sizeof(struct jmb38x_ms)
		     + cnt * sizeof(struct jmb38x_ms_host), GFP_KERNEL);
	if (!jm) {
		rc = -ENOMEM;
		goto err_out_int;
	}

	jm->pdev = pdev;
	jm->num_slots = cnt;
	pci_set_drvdata(pdev, jm);

	rc = jmb38x_ms_get_regions(jm);
	if (rc)
		goto err_out_free;


	for (cnt = 0; cnt < jm->num_slots; ++cnt) {
		jm->hosts[cnt].id = cnt;
		jm->hosts[cnt].irq = pdev->irq;
		rc = jmb38x_ms_host_init(&jm->hosts[cnt]);

		if (rc) {
			if (cnt) {
				for (--cnt; cnt >= 0; --cnt)
					jmb38x_ms_host_remove(&jm->hosts[cnt]);
			}
			goto err_out_free;
		}
	}

	return 0;

err_out_free:
	pci_set_drvdata(pdev, NULL);
	kfree(jm);
err_out_int:
	pci_release_regions(pdev);
err_out:
	if (!pci_dev_busy)
		pci_disable_device(pdev);
	return rc;
}

static void jmb38x_ms_remove(struct pci_dev *dev)
{
	struct jmb38x_ms *jm = pci_get_drvdata(dev);
	int cnt;

	for (cnt = 0; cnt < jm->num_slots; ++cnt)
		jmb38x_ms_host_remove(&jm->hosts[cnt]);

	pci_set_drvdata(dev, NULL);
	pci_release_regions(dev);
	pci_disable_device(dev);
	kfree(jm);
}

static struct pci_device_id jmb38x_ms_id_tbl [] = {
	{ PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB38X_MS, PCI_ANY_ID,
	  PCI_ANY_ID, 0, 0, 0 },
	{ }
};

static struct pci_driver jmb38x_ms_driver = {
	.name = DRIVER_NAME,
	.id_table = jmb38x_ms_id_tbl,
	.probe = jmb38x_ms_probe,
	.remove = jmb38x_ms_remove,
	.suspend = jmb38x_ms_suspend,
	.resume = jmb38x_ms_resume
};

static int __init jmb38x_ms_init(void)
{
        return pci_register_driver(&jmb38x_ms_driver);
}

static void __exit jmb38x_ms_exit(void)
{
        pci_unregister_driver(&jmb38x_ms_driver);
}

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("JMicron JMB38x MemoryStick driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, jmb38x_ms_id_tbl);

module_init(jmb38x_ms_init);
module_exit(jmb38x_ms_exit);
