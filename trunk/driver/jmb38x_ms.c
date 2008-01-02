/*
 * JMicron JMB38x MemoryStick card reader
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
	void __iomem            *addr;
	spinlock_t              lock;
	int                     id;
	char                    host_id[DEVICE_ID_SIZE];
	int                     irq;
	unsigned short          exit:1,
				no_dma:1;
	unsigned long           timeout_jiffies;

	struct timer_list       timer;
	struct memstick_request *req;
};

struct jmb38x_ms {
	struct pci_dev        *pdev;
	int                   num_slots;
	struct memstick_host  *hosts[];
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

#define INT_TPC_ERR            0x00080000
#define INT_CRC_ERR            0x00040000
#define INT_TIMER_TO           0x00020000
#define INT_TPC_HSK_TO         0x00010000
#define INT_ANY_ERR            0x00008000
#define INT_SRAM_WRDY          0x00000080
#define INT_SRAM_RRDY          0x00000040
#define INT_CARD_RM            0x00000010
#define INT_CARD_INS           0x00000008
#define INT_DMA_BOUNDARY       0x00000004
#define INT_TRAN_END           0x00000002
#define INT_TPC_END            0x00000001


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

static int jmb38x_ms_issue_cmd(struct jmb38x_ms_host *host)
{
	return 0;
}

static irqreturn_t jmb38x_ms_isr(int irq, void *dev_id)
{
	struct memstick_host *msh = dev_id;
	struct jmb38x_ms_host *host = memstick_priv(msh);
	unsigned int irq_status;

	spin_lock(&host->lock);

	irq_status = readl(host->addr + INT_STATUS);

	if (irq_status == 0 || irq_status == (~0)) {
		spin_unlock(&host->lock);
		return IRQ_NONE;
	}



	if (irq_status & (INT_CARD_INS | INT_CARD_RM))
		memstick_detect_change(msh);

	writel(0xffffffff, host->addr + INT_STATUS);

	spin_unlock(&host->lock);
	return IRQ_HANDLED;
}

static void jmb38x_ms_request(struct memstick_host *msh)
{
	struct jmb38x_ms_host *host = memstick_priv(msh);
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&host->lock, flags);
	if (host->req) {
		printk(KERN_ERR "%s : unfinished request detected\n",
		       host->host_id);
		spin_unlock_irqrestore(&host->lock, flags);
		BUG();
		return;
	}

	if (host->exit) {
		do {
			rc = memstick_next_req(msh, &host->req);
			if (!rc)
				host->req->error = -ETIME;
		} while (!rc);
		spin_unlock_irqrestore(&host->lock, flags);
		return;
	}

	do {
		rc = memstick_next_req(msh, &host->req);
	} while (!rc && jmb38x_ms_issue_cmd(host));

	spin_unlock_irqrestore(&host->lock, flags);
	return;
}

static void jmb38x_ms_ios(struct memstick_host *msh, struct memstick_ios *ios)
{
}

static void jmb38x_ms_abort(unsigned long data)
{
	struct jmb38x_ms_host *host = (struct jmb38x_ms_host*)data;

	printk(KERN_ERR
	       "%s : card failed to respond for a long period of time "
	       "(%x)\n",
	       host->host_id, host->req ? host->req->tpc : 0);
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

static int jmb38x_ms_alloc_hosts(struct jmb38x_ms *jm)
{
	int cnt, h_cnt = 0;
	struct jmb38x_ms_host *host;

	for (cnt = 0; cnt < PCI_ROM_RESOURCE; ++cnt) {
		if (!(IORESOURCE_MEM & pci_resource_flags(jm->pdev, cnt)))
			continue;

		if (256 != pci_resource_len(jm->pdev, cnt))
			continue;

		jm->hosts[h_cnt]
			= memstick_alloc_host(sizeof(struct jmb38x_ms_host),
					      &jm->pdev->dev);

		if (!jm->hosts[h_cnt])
			goto err_out;

		jm->hosts[h_cnt]->request = jmb38x_ms_request;
		jm->hosts[h_cnt]->set_ios = jmb38x_ms_ios;

		host = memstick_priv(jm->hosts[h_cnt]);

		host->addr = ioremap(pci_resource_start(jm->pdev, cnt),
				     pci_resource_len(jm->pdev, cnt));
		if (!host->addr)
			goto err_out;

		spin_lock_init(&host->lock);
		host->irq = jm->pdev->irq;
		host->id = h_cnt;
		setup_timer(&host->timer, jmb38x_ms_abort, (unsigned long)host);

		h_cnt++;
	}

	return 0;

err_out:
	for (; h_cnt >= 0; --h_cnt) {
		if (jm->hosts[h_cnt]) {
			host = memstick_priv(jm->hosts[h_cnt]);

			if (host->addr)
				iounmap(host->addr);

			memstick_free_host(jm->hosts[h_cnt]);
			jm->hosts[h_cnt] = NULL;
		}
	}

	return -ENOMEM;
}

static int jmb38x_ms_add_host(struct memstick_host *msh)
{
	struct jmb38x_ms_host *host = memstick_priv(msh);
	int rc = 0;

	snprintf(host->host_id, DEVICE_ID_SIZE, DRIVER_NAME ":slot%d",
		 host->id);

	rc = request_irq(host->irq, jmb38x_ms_isr, IRQF_SHARED, host->host_id,
			 msh);
	if (rc)
		goto err_out_unmap;

	rc = memstick_add_host(msh);
	if (rc)
		goto err_out_free_irq;

	return 0;

err_out_free_irq:
	free_irq(host->irq, msh);
err_out_unmap:
	iounmap(host->addr);
	memstick_free_host(msh);

	return rc;
}

static void jmb38x_ms_remove_host(struct memstick_host *msh)
{
	struct jmb38x_ms_host *host = memstick_priv(msh);

	memstick_remove_host(msh);

	writel(readl(host->addr + HOST_CONTROL)
	       & ~(HOST_CONTROL_CLOCK_EN | HOST_CONTROL_POWER_EN),
	       host->addr + HOST_CONTROL);

	mmiowb();
	free_irq(host->irq, msh);
	iounmap(host->addr);

	memstick_free_host(msh);
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
		     + cnt * sizeof(struct memstick_host*), GFP_KERNEL);
	if (!jm) {
		rc = -ENOMEM;
		goto err_out_int;
	}

	jm->pdev = pdev;
	jm->num_slots = cnt;
	pci_set_drvdata(pdev, jm);

	rc = jmb38x_ms_alloc_hosts(jm);
	if (rc)
		goto err_out_free;

	for (cnt = 0; cnt < jm->num_slots; ++cnt) {
		rc = jmb38x_ms_add_host(jm->hosts[cnt]);

		if (rc) {
			jm->hosts[cnt] = NULL;
			printk(KERN_ERR "%s: error %d adding slot %d\n",
			       pdev->dev.bus_id, rc, cnt);
		}
	}

	rc = -ENODEV;

	for (cnt = 0; cnt < jm->num_slots; ++cnt) {
		if (jm->hosts[cnt]) {
			rc = 0;
			break;
		}
	}

	if (!rc)
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

	for (cnt = 0; cnt < jm->num_slots; ++cnt) {
		if (jm->hosts[cnt])
			jmb38x_ms_remove_host(jm->hosts[cnt]);
	}

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
