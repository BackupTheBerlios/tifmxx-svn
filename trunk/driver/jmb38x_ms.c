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
#include <linux/delay.h>

#include "linux/memstick.h"

#define PCI_DEVICE_ID_JMICRON_JMB38X_MS 0x2383
#define DRIVER_NAME "jmb38x_ms"

static int no_dma;
module_param(no_dma, bool, 0644);

enum {
	DMA_ADDRESS       = 0x00,
	BLOCK             = 0x04,
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
	struct jmb38x_ms        *chip;
	void __iomem            *addr;
	spinlock_t              lock;
	int                     id;
	char                    host_id[DEVICE_ID_SIZE];
	int                     irq;
	unsigned short          eject:1,
				no_dma:1;
	unsigned short          cmd_flags;
	unsigned int            block_pos;
	unsigned long           timeout_jiffies;
	struct timer_list       timer;
	struct memstick_request *req;
};

struct jmb38x_ms {
	struct pci_dev        *pdev;
	int                   host_cnt;
	struct memstick_host  *hosts[];
};

#define BLOCK_COUNT_MASK       0xffff0000
#define BLOCK_SIZE_MASK        0x00000fff

#define TPC_DATA_SEL           0x00008000
#define TPC_DIR                0x00004000
#define TPC_WAIT_INT           0x00002000
#define TPC_GET_INT            0x00000800
#define TPC_CODE_SZ_MASK       0x00000700
#define TPC_DATA_SZ_MASK       0x00000007

#define HOST_CONTROL_RST       0x00008000
#define HOST_CONTROL_LED       0x00000400
#define HOST_CONTROL_FAST_CLK  0x00000200
#define HOST_CONTROL_DMA_RST   0x00000100
#define HOST_CONTROL_POWER_EN  0x00000080
#define HOST_CONTROL_CLOCK_EN  0x00000040

#define HOST_CONTROL_IF_SERIAL 0x0
#define HOST_CONTROL_IF_PAR4   0x1
#define HOST_CONTROL_IF_PAR8   0x3

#define INT_STATUS_TPC_ERR      0x00080000
#define INT_STATUS_CRC_ERR      0x00040000
#define INT_STATUS_TIMER_TO     0x00020000
#define INT_STATUS_HSK_TO       0x00010000
#define INT_STATUS_FIFO_WRDY    0x00000080
#define INT_STATUS_FIFO_RRDY    0x00000040
#define INT_STATUS_MEDIA_OUT    0x00000010
#define INT_STATUS_MEDIA_IN     0x00000008
#define INT_STATUS_DMA_BOUNDARY 0x00000004
#define INT_STATUS_EOTRAN       0x00000002
#define INT_STATUS_EOTPC        0x00000001

#define PAD_OUTPUT_ENABLE_MS  0x0F3F

#define PAD_PU_PD_OFF         0x7FFF0000
#define PAD_PU_PD_ON_MS_SOCK0 0x5f8f0000
#define PAD_PU_PD_ON_MS_SOCK1 0x0f0f0000

enum {
        CMD_READY  = 0x0001,
        FIFO_READY = 0x0002,
        CARD_READY = 0x0004,
        DATA_CARRY = 0x0008
};

static int jmb38x_ms_issue_cmd(struct jmb38x_ms_host *host)
{
	unsigned char *data;
	unsigned int cnt, data_len, irq_mask, cmd, t_val;

	host->cmd_flags = 0;
	host->block_pos = 0;

	irq_mask = readl(host->addr + INT_STATUS_ENABLE);

	cmd = host->req->tpc << 16;
	cmd |= TPC_DATA_SEL;

	if (host->req->data_dir == READ)
		cmd |= TPC_DIR;
	if (host->req->need_card_int)
		cmd |= TPC_WAIT_INT;
	if (host->req->get_int_reg)
		cmd |= TPC_GET_INT;

	if (host->req->io_type == MEMSTICK_IO_SG) {
		if (!host->no_dma) {
			if (1 != pci_map_sg(host->chip->pdev, &host->req->sg, 1,
					    host->req->data_dir == READ
					    ? PCI_DMA_FROMDEVICE
					    : PCI_DMA_TODEVICE)) {
				host->req->error = -ENOMEM;
				return host->req->error;
			}
			data_len = sg_dma_len(&host->req->sg);
			irq_mask &= ~(INT_STATUS_FIFO_RRDY
				      | INT_STATUS_FIFO_WRDY);
		} else {
			data_len = host->req->sg.length;
			irq_mask |= INT_STATUS_FIFO_RRDY
				    | INT_STATUS_FIFO_WRDY;
		}

		writel(irq_mask, host->addr + INT_STATUS_ENABLE);
		writel(irq_mask, host->addr + INT_SIGNAL_ENABLE);
	} else if (host->req->io_type == MEMSTICK_IO_VAL) {
 		data = host->req->data;
		data_len = host->req->data_len;

		if (host->req->data_dir == WRITE) {
			for (cnt = 0; (data_len - cnt) >= 4; cnt += 4)
				__raw_writel(*(unsigned int *)(data + cnt),
					     host->addr + DATA);

			t_val = 0;
			switch (data_len - cnt) {
			case 3:
				t_val |= data[cnt + 2] << 16;
			case 2:
				t_val |= data[cnt + 1] << 8;
			case 1:
				t_val |= data[cnt];
				writel(t_val, host->addr + DATA);
                        }
		}
	} else
		BUG();

	writel(((1 << 16) & BLOCK_COUNT_MASK) | (data_len & BLOCK_SIZE_MASK),
	       host->addr + BLOCK);
	mod_timer(&host->timer, jiffies + host->timeout_jiffies);
	writel(HOST_CONTROL_LED | readl(host->addr + HOST_CONTROL),
	       host->addr + HOST_CONTROL);
	host->req->error = 0;

	writel(cmd, host->addr + TPC);
	dev_dbg(&host->chip->pdev->dev, "executing TPC %x, %x\n", cmd,
		cmd_mask);
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
	dev_dbg(&host->chip->pdev->dev, "irq_status = %08x\n", irq_status);

	if (irq_status & (INT_STATUS_MEDIA_IN | INT_STATUS_MEDIA_OUT)) {
		dev_dbg(&host->chip->pdev->dev, "media changed\n");

	}

	writel(irq_status, host->addr + INT_STATUS);
	spin_unlock(&host->lock);
	return IRQ_HANDLED;
}

static void jmb38x_ms_abort(unsigned long data)
{
#warning Ugh! Ugh!
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
			break;

		if (256 != pci_resource_len(pdev, cnt))
			break;

		++rc;
	}
	return rc;
}

static struct memstick_host* jmb38x_ms_alloc_host(struct jmb38x_ms *jm, int cnt)
{
	struct memstick_host *msh;
	struct jmb38x_ms_host *host;

	msh = memstick_alloc_host(sizeof(struct jmb38x_ms_host),
				  &jm->pdev->dev);
	if (!msh)
		return NULL;

	host = memstick_priv(msh);
	host->chip = jm;
	host->addr = ioremap(pci_resource_start(jm->pdev, cnt),
			     pci_resource_len(jm->pdev, cnt));
	if (!host->addr)
		goto err_out_free;

	spin_lock_init(&host->lock);
	host->id = cnt;
	snprintf(host->host_id, DEVICE_ID_SIZE, DRIVER_NAME ":slot%d",
		 host->id);
	host->irq = jm->pdev->irq;
	host->timeout_jiffies = msecs_to_jiffies(1000);

	setup_timer(&host->timer, jmb38x_ms_abort, (unsigned long)msh);

	if (!request_irq(host->irq, jmb38x_ms_isr, IRQF_SHARED, host->host_id,
			 msh))
		return msh;

	iounmap(host->addr);
err_out_free:
	kfree(msh);
	return NULL;
}

static void jmb38x_ms_free_host(struct memstick_host *msh)
{
	struct jmb38x_ms_host *host = memstick_priv(msh);

	free_irq(host->irq, msh);
	iounmap(host->addr);
	memstick_free_host(msh);
}

static int jmb38x_ms_probe(struct pci_dev *pdev,
			   const struct pci_device_id *dev_id)
{
	struct jmb38x_ms *jm;
	struct jmb38x_ms_host *host;
	int pci_dev_busy = 0;
	int rc, cnt;
	unsigned int host_ctl;

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
	jm->host_cnt = cnt;
	pci_set_drvdata(pdev, jm);

	for (cnt = 0; cnt < jm->host_cnt; ++cnt) {
		jm->hosts[cnt] = jmb38x_ms_alloc_host(jm, cnt);
		if (!jm->hosts[cnt])
			break;
		host = memstick_priv(jm->hosts[cnt]);

		host_ctl = readl(host->addr + HOST_CONTROL);
		writel(cnt ? PAD_PU_PD_ON_MS_SOCK1
			   : PAD_PU_PD_ON_MS_SOCK0, host->addr + PAD_PU_PD);

		writel(PAD_OUTPUT_ENABLE_MS, host->addr + PAD_OUTPUT_ENABLE);

		writel(host_ctl | (HOST_CONTROL_POWER_EN | HOST_CONTROL_CLOCK_EN),
		       host->addr + HOST_CONTROL);

		msleep(10);

		writel(INT_STATUS_EOTPC | INT_STATUS_EOTRAN
		       | INT_STATUS_DMA_BOUNDARY | INT_STATUS_MEDIA_OUT
		       | INT_STATUS_MEDIA_IN | INT_STATUS_TPC_ERR
		       | INT_STATUS_CRC_ERR,
		       host->addr + INT_STATUS_ENABLE);
		writel(INT_STATUS_EOTPC | INT_STATUS_EOTRAN
		       | INT_STATUS_DMA_BOUNDARY | INT_STATUS_MEDIA_OUT
		       | INT_STATUS_MEDIA_IN | INT_STATUS_TPC_ERR
		       | INT_STATUS_CRC_ERR,
		       host->addr + INT_SIGNAL_ENABLE);
	}

	if (cnt)
		return 0;

	rc = -ENODEV;

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
	struct jmb38x_ms_host *host;
	int cnt;

	for (cnt = 0; cnt < jm->host_cnt; ++cnt) {
		if (!jm->hosts[cnt])
			break;

		host = memstick_priv(jm->hosts[cnt]);

		writel(0, host->addr + INT_SIGNAL_ENABLE);
		writel(0, host->addr + INT_STATUS_ENABLE);
		writel(readl(host->addr + HOST_CONTROL)
		       & ~HOST_CONTROL_POWER_EN, host->addr +  HOST_CONTROL);
		writel(0, host->addr + PAD_OUTPUT_ENABLE);
		writel(PAD_PU_PD_OFF, host->addr + PAD_PU_PD);

		jmb38x_ms_free_host(jm->hosts[cnt]);
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
