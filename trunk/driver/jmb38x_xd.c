/*
 *  JMicron jmb38x xD picture card reader
 *
 *  Copyright (C) 2008 JMicron Technology Corporation <www.jmicron.com>
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

#include "linux/xd_card.h"
/*
#undef dev_dbg

#define dev_dbg(dev, format, arg...)          \
        dev_printk(KERN_EMERG , dev , format , ## arg)
*/
#define PCI_DEVICE_ID_JMICRON_JMB38X_XD 0x2384
#define DRIVER_NAME "jmb38x_xd"

enum {
	DMA_ADDRESS       = 0x00,
	HOST_CONTROL      = 0x04,
	COMMAND           = 0x08,
	MEDIA_ADDRESS_LO  = 0x0c,
	MEDIA_ADDRESS_HI  = 0x10,
	MCOMMAND          = 0x14,
	MADDRESS          = 0x18,
	MREAD             = 0x1c,
	MDATA             = 0x20,
	PIN_STATUS        = 0x24,
	ID_CODE           = 0x28,
	RDATA0            = 0x2c,
	RDATA1            = 0x30,
	RDATA2            = 0x34,
	RDATA3            = 0x38,
	ECC               = 0x3c,
	INT_STATUS        = 0x40,
	INT_STATUS_ENABLE = 0x44,
	INT_SIGNAL_ENABLE = 0x48,
	TIMER             = 0x4c,
	TIMER_CONTROL     = 0x50,
	PAD_OUTPUT_ENABLE = 0x54,
	PAD_PU_PD         = 0x58,
	CLOCK_CONTROL     = 0x5c,
	DEBUG_PARAM       = 0x60,
	LED_CONTROL       = 0x64,
	CARD_DEBOUNCE     = 0x68,
	VERSION           = 0x6c
};

#define JMB38X_XD_EXTRA_DATA_SIZE 16
struct jmb38x_xd_host {
	struct pci_dev          *pdev;
	void __iomem            *addr;
	spinlock_t              lock;
	char                    id[DEVICE_ID_SIZE];
	unsigned short          page_size;
	unsigned short          extra_size;
	unsigned int            host_ctl;
	unsigned long           timeout_jiffies;
	struct timer_list       timer;
	struct xd_card_request  *req;
	unsigned char           cmd_flags;
	unsigned char           extra_data[JMB38X_XD_EXTRA_DATA_SIZE];
};

enum {
	CMD_READY    = 0x01,
	DATA_READY   = 0x02
};

#define PAD_OUTPUT_ENABLE_XD0 0x00004fcf
#define PAD_OUTPUT_ENABLE_XD1 0x00005fff
#define PAD_OUTPUT_DISABLE_XD 0x000000c0

#define PAD_PU_PD_OFF         0x7FFF0000
#define PAD_PU_PD_ON_XD       0x4f8f1030

#define HOST_CONTROL_RESET_REQ      0x80000000
#define HOST_CONTROL_PAGE_CNT_MASK  0x00ff0000
#define HOST_CONTROL_PAGE_CNT_SHIFT 16
#define HOST_CONTROL_LED_TYPE       0x00008000
#define HOST_CONTROL_4P_MODE        0x00004000
#define HOST_CONTROL_COPY_BACK      0x00002000
#define HOST_CONTROL_CLK_DIV2       0x00001000
#define HOST_CONTROL_RW             0x00000800
#define HOST_CONTROL_LED            0x00000400
#define HOST_CONTROL_POWER_EN       0x00000200
#define HOST_CONTROL_CLOCK_EN       0x00000100
#define HOST_CONTROL_RESET          0x00000080
#define HOST_CONTROL_SLOW_CLK       0x00000040
#define HOST_CONTROL_CS_EN          0x00000020
#define HOST_CONTROL_DATA_DIR       0x00000010
#define HOST_CONTROL_ECC_EN         0x00000008
#define HOST_CONTROL_ADDR_SIZE_MASK 0x00000006
#define HOST_CONTROL_AC_EN          0x00000001

#define PIN_STATUS_XDINS            0x00000002
#define PIN_STATUS_XDRB             0x00000001

#define ECC_CODE1_ERR               0x00008000
#define ECC_DATA1_ERR               0x00004000
#define ECC_CORR1                   0x00002000
#define ECC_GOOD1                   0x00001000
#define ECC_CODE0_ERR               0x00000800
#define ECC_DATA0_ERR               0x00000400
#define ECC_CORR0                   0x00000200
#define ECC_GOOD0                   0x00000100
#define ECC_XD_STATUS_MASK          0x000000ff

#define INT_STATUS_OC_ERROR         0x00000080
#define INT_STATUS_ECC_ERROR        0x00000040
#define INT_STATUS_DMA_BOUNDARY     0x00000020
#define INT_STATUS_TIMER_TO         0x00000010
#define INT_STATUS_MEDIA_OUT        0x00000008
#define INT_STATUS_MEDIA_IN         0x00000004
#define INT_STATUS_EOTRAN           0x00000002
#define INT_STATUS_EOCMD            0x00000001

#define INT_STATUS_ALL              0x000000ff

#define CLOCK_CONTROL_MMIO          0x00000008
#define CLOCK_CONTROL_62_5MHZ       0x00000004
#define CLOCK_CONTROL_50MHZ         0x00000002
#define CLOCK_CONTROL_40MHZ         0x00000001

static int jmb38x_xd_issue_cmd(struct xd_card_host *host)
{
	struct jmb38x_xd_host *jhost = xd_card_priv(host);
	unsigned int p_cnt = 0;

	if (!(PIN_STATUS_XDINS & readl(jhost->addr + PIN_STATUS))) {
		dev_dbg(host->dev, "no media status\n");
		jhost->req->error = -ETIME;
		return jhost->req->error;
	}

	writel(jhost->req->addr, jhost->addr + MEDIA_ADDRESS_LO);
	writel(jhost->req->addr >> 32, jhost->addr + MEDIA_ADDRESS_HI);

	if (jhost->req->flags & XD_CARD_REQ_DATA) {
		if (1 != pci_map_sg(jhost->pdev, &jhost->req->sg, 1,
				    jhost->req->flags & XD_CARD_REQ_DIR
				    ? PCI_DMA_TODEVICE
				    : PCI_DMA_FROMDEVICE)) {
			jhost->req->error = -ENOMEM;
			return jhost->req->error;
		}

		writel(sg_dma_address(&jhost->req->sg),
		       jhost->addr + DMA_ADDRESS);
		p_cnt = sg_dma_len(&jhost->req->sg) / jhost->page_size;
		p_cnt <<= HOST_CONTROL_PAGE_CNT_SHIFT;
		dev_dbg(host->dev, "trans %llx, %llx, %d, %08x\n",
			jhost->req->addr,
			sg_dma_address(&jhost->req->sg),
			sg_dma_len(&jhost->req->sg), p_cnt);
	}

	if ((jhost->req->flags & XD_CARD_REQ_EXTRA)
	    && (jhost->req->flags & XD_CARD_REQ_DIR)) {
		xd_card_get_extra(host, jhost->extra_data,
				  jhost->extra_size);
		writel(*(unsigned int *)(jhost->extra_data),
		       jhost->addr + RDATA0);
		writel(*(unsigned int *)(jhost->extra_data + 4),
		       jhost->addr + RDATA1);
		writel(*(unsigned int *)(jhost->extra_data + 8),
		       jhost->addr + RDATA2);
		writel(*(unsigned int *)(jhost->extra_data + 12),
		       jhost->addr + RDATA3);
	}

	if (!(jhost->req->flags & XD_CARD_REQ_NO_ECC)) {
		writel(INT_STATUS_ECC_ERROR
		       | readl(jhost->addr + INT_SIGNAL_ENABLE),
		       jhost->addr + INT_SIGNAL_ENABLE);
		writel(INT_STATUS_ECC_ERROR
		       | readl(jhost->addr + INT_STATUS_ENABLE),
		       jhost->addr + INT_STATUS_ENABLE);
	} else {
		writel((~INT_STATUS_ECC_ERROR)
		       & readl(jhost->addr + INT_SIGNAL_ENABLE),
		       jhost->addr + INT_SIGNAL_ENABLE);
		writel((~INT_STATUS_ECC_ERROR)
		       & readl(jhost->addr + INT_STATUS_ENABLE),
		       jhost->addr + INT_STATUS_ENABLE);
	}

	/* The controller has a bug, requiring IDs to be "written", not "read".
	 */
	if ((jhost->req->flags & XD_CARD_REQ_DIR)
	    || (jhost->req->flags & XD_CARD_REQ_ID))
		jhost->host_ctl &= ~HOST_CONTROL_DATA_DIR;
	else
		jhost->host_ctl |= HOST_CONTROL_DATA_DIR;

	writel(jhost->host_ctl | HOST_CONTROL_LED
	       | (p_cnt & HOST_CONTROL_PAGE_CNT_MASK),
	       jhost->addr + HOST_CONTROL);

	mod_timer(&jhost->timer, jiffies + jhost->timeout_jiffies);
	jhost->req->error = 0;
	jhost->cmd_flags = 0;

	writel(jhost->req->cmd << 16, jhost->addr + COMMAND);

	dev_dbg(host->dev, "issue command %02x, %08x\n", jhost->req->cmd,
		readl(jhost->addr + HOST_CONTROL));
	return 0;
}

static void jmb38x_xd_read_id(struct jmb38x_xd_host *jhost)
{
	unsigned int id_val = readl(jhost->addr + ID_CODE);

	memset(jhost->req->id, 0, jhost->req->count);

	if (jhost->req->count >= 4) {
		*(unsigned int *)(jhost->req->id) = id_val;
	} else {
		switch (jhost->req->count) {
		case 3:
			((unsigned char *)jhost->req->id)[2]
				= (id_val >> 16) & 0xff;
		case 2:
			((unsigned char *)jhost->req->id)[1]
				= (id_val >> 8) & 0xff;
		case 1:
			((unsigned char *)jhost->req->id)[0]
				= id_val & 0xff;
		}
	}
}

static void jmb38x_xd_complete_cmd(struct xd_card_host *host, int last)
{
	struct jmb38x_xd_host *jhost = xd_card_priv(host);
	unsigned int host_ctl;
	int rc;

	if (!last)
		del_timer(&jhost->timer);

	host_ctl = readl(jhost->addr + HOST_CONTROL);
	dev_dbg(&jhost->pdev->dev, "c control %08x\n", host_ctl);
	dev_dbg(&jhost->pdev->dev, "c hstatus %08x\n",
		readl(jhost->addr + ECC));


	writel((~(HOST_CONTROL_LED | HOST_CONTROL_PAGE_CNT_MASK)) & host_ctl,
	       jhost->addr + HOST_CONTROL);

	if (jhost->req->flags & XD_CARD_REQ_ID)
		jmb38x_xd_read_id(jhost);

	if (jhost->req->flags & XD_CARD_REQ_STATUS)
		jhost->req->status = readl(jhost->addr + ECC)
				     & ECC_XD_STATUS_MASK;

	if (jhost->req->flags & XD_CARD_REQ_DATA) {
		host_ctl &= HOST_CONTROL_PAGE_CNT_MASK;
		host_ctl >>= HOST_CONTROL_PAGE_CNT_SHIFT;

		jhost->req->count = sg_dma_len(&jhost->req->sg)
				    - host_ctl * jhost->page_size;

		writel(0, jhost->addr + DMA_ADDRESS);
		pci_unmap_sg(jhost->pdev, &jhost->req->sg, 1,
			     jhost->req->flags & XD_CARD_REQ_DIR
			     ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE);

	}

	if ((jhost->req->flags & XD_CARD_REQ_EXTRA)
	    && !(jhost->req->flags & XD_CARD_REQ_DIR)) {
		*(unsigned int *)(jhost->extra_data)
			= readl(jhost->addr + RDATA0);
		*(unsigned int *)(jhost->extra_data + 4)
			= readl(jhost->addr + RDATA1);
		*(unsigned int *)(jhost->extra_data + 8)
			= readl(jhost->addr + RDATA2);
		*(unsigned int *)(jhost->extra_data + 12)
			= readl(jhost->addr + RDATA3);

		xd_card_set_extra(host, jhost->extra_data,
				  jhost->extra_size);
	}

	if (!last) {
		do {
			rc = xd_card_next_req(host, &jhost->req);
		} while (!rc && jmb38x_xd_issue_cmd(host));
	} else {
		do {
			rc = xd_card_next_req(host, &jhost->req);
			if (!rc)
				jhost->req->error = -ETIME;
		} while (!rc);
	}
}


static irqreturn_t jmb38x_xd_isr(int irq, void *dev_id)
{
	struct xd_card_host *host = dev_id;
	struct jmb38x_xd_host *jhost = xd_card_priv(host);
	unsigned int irq_status, p_cnt;

	spin_lock(&jhost->lock);
	irq_status = readl(jhost->addr + INT_STATUS);
	dev_dbg(host->dev, "irq_status = %08x\n", irq_status);
	if (irq_status == 0 || irq_status == (~0)) {
		spin_unlock(&host->lock);
		return IRQ_NONE;
	}

	if (jhost->req) {
		if (irq_status & INT_STATUS_TIMER_TO)
			jhost->req->error = -ETIME;
		else if (irq_status & INT_STATUS_ECC_ERROR)
			jhost->req->error = -EILSEQ;

		if (irq_status & INT_STATUS_EOCMD) {
			jhost->cmd_flags |= CMD_READY;

			if (!(jhost->req->flags & XD_CARD_REQ_DATA))
				jhost->cmd_flags |= DATA_READY;
		}

		if (irq_status & INT_STATUS_EOTRAN)
			jhost->cmd_flags |= DATA_READY;

		if ((jhost->req->flags & XD_CARD_REQ_DATA)
		    && (irq_status & INT_STATUS_DMA_BOUNDARY)) {
			p_cnt = readl(jhost->addr + HOST_CONTROL);
			p_cnt &= HOST_CONTROL_PAGE_CNT_MASK;
			p_cnt >>= HOST_CONTROL_PAGE_CNT_SHIFT;
			p_cnt = sg_dma_len(&jhost->req->sg)
				- p_cnt * jhost->page_size;
			dev_dbg(host->dev, "dma boundary %d, %d\n",
				sg_dma_len(&jhost->req->sg), p_cnt);
			writel(sg_dma_address(&jhost->req->sg) + p_cnt,
			       jhost->addr + DMA_ADDRESS);
		}
	}

	if (irq_status & (INT_STATUS_MEDIA_IN | INT_STATUS_MEDIA_OUT)) {
		dev_dbg(host->dev, "media changed\n");
		xd_card_detect_change(host);
	}

	writel(irq_status, jhost->addr + INT_STATUS);

	if (jhost->req
	    && (((jhost->cmd_flags & CMD_READY)
		 && (jhost->cmd_flags & DATA_READY))
		|| jhost->req->error))
		jmb38x_xd_complete_cmd(host, 0);

	spin_unlock(&jhost->lock);
	return IRQ_HANDLED;
}

static void jmb38x_xd_abort(unsigned long data)
{
	struct xd_card_host *host = (struct xd_card_host *)data;
	struct jmb38x_xd_host *jhost = xd_card_priv(host);
	unsigned long flags;

	dev_dbg(&jhost->pdev->dev, "abort\n");
	spin_lock_irqsave(&jhost->lock, flags);
	if (jhost->req) {
		jhost->req->error = -ETIME;
		jmb38x_xd_complete_cmd(host, 1);
	}
	spin_unlock_irqrestore(&jhost->lock, flags);
}

static void jmb38x_xd_request(struct xd_card_host *host)
{
	struct jmb38x_xd_host *jhost = xd_card_priv(host);
	unsigned long flags;
	int rc;

	dev_dbg(host->dev, "new request\n");
	spin_lock_irqsave(&jhost->lock, flags);
	if (jhost->req) {
		spin_unlock_irqrestore(&jhost->lock, flags);
		BUG();
		return;
	}

	do {
		rc = xd_card_next_req(host, &jhost->req);
	} while (!rc && jmb38x_xd_issue_cmd(host));

	spin_unlock_irqrestore(&jhost->lock, flags);
}

static void jmb38x_xd_reset(struct jmb38x_xd_host *jhost)
{
	writel(HOST_CONTROL_RESET_REQ | readl(jhost->addr + HOST_CONTROL),
	       jhost->addr + HOST_CONTROL);
	mmiowb();

	while (HOST_CONTROL_RESET_REQ & readl(jhost->addr + HOST_CONTROL)) {
		ndelay(20);
		dev_dbg(&jhost->pdev->dev, "reset 1\n");
	}

	writel(HOST_CONTROL_RESET | readl(jhost->addr + HOST_CONTROL),
	       jhost->addr + HOST_CONTROL);
	mmiowb();

	while (HOST_CONTROL_RESET & readl(jhost->addr + HOST_CONTROL)) {
		ndelay(20);
		dev_dbg(&jhost->pdev->dev, "reset 2\n");
	}
}

static void jmb38x_xd_set_param(struct xd_card_host *host,
				enum xd_card_param param,
				int value)
{
	struct jmb38x_xd_host *jhost = xd_card_priv(host);
	unsigned int t_val;

	switch(param) {
	case XD_CARD_POWER:
		if (value == XD_CARD_POWER_ON) {
			/* jmb38x_xd_reset(jhost); */
			writel(CLOCK_CONTROL_MMIO | CLOCK_CONTROL_40MHZ,
			       jhost->addr + CLOCK_CONTROL);

			writel(PAD_OUTPUT_ENABLE_XD0 << 16,
			       jhost->addr + PAD_PU_PD);
			writel(PAD_OUTPUT_ENABLE_XD0,
			       jhost->addr + PAD_OUTPUT_ENABLE);

			msleep(60);

			jhost->host_ctl = HOST_CONTROL_POWER_EN
					  | HOST_CONTROL_CLOCK_EN
					  | HOST_CONTROL_ECC_EN
					  | HOST_CONTROL_AC_EN;
			writel(jhost->host_ctl, jhost->addr + HOST_CONTROL);

			msleep(1);

			writel(PAD_PU_PD_ON_XD, jhost->addr + PAD_PU_PD);
			writel(PAD_OUTPUT_ENABLE_XD1,
			       jhost->addr + PAD_OUTPUT_ENABLE);
			dev_dbg(host->dev, "power on\n");
			writel(INT_STATUS_ALL, jhost->addr + INT_SIGNAL_ENABLE);
			writel(INT_STATUS_ALL, jhost->addr + INT_STATUS_ENABLE);
			mmiowb();
		} else if (value == XD_CARD_POWER_OFF) {
			jhost->host_ctl &= ~HOST_CONTROL_RW;
			writel(jhost->host_ctl, jhost->addr + HOST_CONTROL);

			dev_dbg(host->dev, "p1\n");
			jmb38x_xd_reset(jhost);

			dev_dbg(host->dev, "p2\n");
			msleep(1);

			dev_dbg(host->dev, "p3\n");
			if (readl(jhost->addr + PAD_OUTPUT_ENABLE))
				writel(PAD_OUTPUT_ENABLE_XD0,
				       jhost->addr + PAD_OUTPUT_ENABLE);

			msleep(1);
			dev_dbg(host->dev, "p4\n");

			jhost->host_ctl &= ~(HOST_CONTROL_POWER_EN
					     | HOST_CONTROL_CLOCK_EN);
			writel(jhost->host_ctl, jhost->addr + HOST_CONTROL);

			msleep(1);

			dev_dbg(host->dev, "p5\n");
			writel(PAD_PU_PD_OFF, jhost->addr + PAD_PU_PD);

			dev_dbg(host->dev, "p6\n");
			writel(PAD_OUTPUT_DISABLE_XD,
			       jhost->addr + PAD_OUTPUT_ENABLE);
			msleep(60);
			writel(0, jhost->addr + PAD_OUTPUT_ENABLE);
			dev_dbg(host->dev, "power off\n");
			writel(INT_STATUS_ALL, jhost->addr + INT_SIGNAL_ENABLE);
			writel(INT_STATUS_ALL, jhost->addr + INT_STATUS_ENABLE);
			mmiowb();
		}
		break;
	case XD_CARD_CLOCK:
		if (value == XD_CARD_SLOW)
			jhost->host_ctl |= HOST_CONTROL_SLOW_CLK;
		else if (value == XD_CARD_NORMAL)
			jhost->host_ctl &= ~HOST_CONTROL_SLOW_CLK;

		writel(jhost->host_ctl, jhost->addr + HOST_CONTROL);
		break;
	case XD_CARD_PAGE_SIZE:
		jhost->page_size = 0xfff & value;
		dev_dbg(host->dev, "set page size %d\n", jhost->page_size);
		t_val = readl(jhost->addr + DEBUG_PARAM);
		t_val &= ~0xfff;
		t_val |= jhost->page_size;
		writel(t_val, jhost->addr + DEBUG_PARAM);
		break;
	case XD_CARD_EXTRA_SIZE:
		BUG_ON(value > JMB38X_XD_EXTRA_DATA_SIZE);
		jhost->extra_size = value;
		t_val = readl(jhost->addr + DEBUG_PARAM);
		t_val &= ~(0xff << 16);
		t_val |= value << 16;
		writel(t_val, jhost->addr + DEBUG_PARAM);
		break;
	case XD_CARD_ADDR_SIZE:
		BUG_ON((value < 3) || (value > 6));
		jhost->host_ctl &= ~ HOST_CONTROL_ADDR_SIZE_MASK;
		jhost->host_ctl |= (value - 3) << 1;
		writel(jhost->host_ctl, jhost->addr + HOST_CONTROL);
		break;
	};
}


#ifdef CONFIG_PM

static int jmb38x_xd_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct xd_card_host *host = pci_get_drvdata(pdev);
	int rc;

	rc = xd_card_suspend_host(host);
	if (rc)
		return rc;

	pci_save_state(pdev);
	pci_enable_wake(pdev, pci_choose_state(pdev, state), 0);
	pci_disable_device(pdev);
	pci_set_power_state(pdev, pci_choose_state(pdev, state));
	return 0;
}

static int jmb38x_xd_resume(struct pci_dev *pdev)
{
	struct xd_card_host *host = pci_get_drvdata(pdev);
	int rc;

	pci_set_power_state(pdev, PCI_D0);
	pci_restore_state(pdev);
	rc = pci_enable_device(pdev);
	if (rc)
		return rc;
	pci_set_master(pdev);

	pci_read_config_dword(pdev, 0xac, &rc);
	pci_write_config_dword(pdev, 0xac, rc | 0x00470000);
	pci_read_config_dword(pdev, 0xb0, &rc);
	pci_write_config_dword(pdev, 0xb0, rc & 0xffff0000);

	return xd_card_resume_host(host);
}

#else

#define jmb38x_ms_suspend NULL
#define jmb38x_ms_resume NULL

#endif /* CONFIG_PM */

static int jmb38x_xd_probe(struct pci_dev *pdev,
			   const struct pci_device_id *dev_id)
{
	struct xd_card_host *host;
	struct jmb38x_xd_host *jhost;
	int pci_dev_busy = 0;
	int rc;

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

	pci_read_config_dword(pdev, 0xac, &rc);
	pci_write_config_dword(pdev, 0xac, rc | 0x00470000);
	pci_read_config_dword(pdev, 0xb0, &rc);
	pci_write_config_dword(pdev, 0xb0, rc & 0xffff00ff);

	host = xd_card_alloc_host(sizeof(struct jmb38x_xd_host), &pdev->dev);

	if (!host) {
		rc = -ENOMEM;
		goto err_out_release;
	}

	jhost = xd_card_priv(host);

	jhost->addr = ioremap(pci_resource_start(pdev, 0),
			      pci_resource_len(pdev, 0));

	if (!jhost->addr) {
		rc = -ENOMEM;
		goto err_out_free;
	}

	jhost->pdev = pdev;
	jhost->timeout_jiffies = msecs_to_jiffies(1000);

	host->request = jmb38x_xd_request;
	host->set_param = jmb38x_xd_set_param;
	host->caps = XD_CARD_CAP_AUTO_ECC | XD_CARD_CAP_FIXED_EXTRA;
	
	pci_set_drvdata(pdev, host);

	snprintf(jhost->id, DEVICE_ID_SIZE, DRIVER_NAME);

	spin_lock_init(&jhost->lock);
	setup_timer(&jhost->timer, jmb38x_xd_abort, (unsigned long)host);

	rc = request_irq(pdev->irq, jmb38x_xd_isr, IRQF_SHARED, jhost->id,
			 host);

	if(!rc) {
		xd_card_detect_change(host);
		return 0;
	}

	iounmap(jhost->addr);
	pci_set_drvdata(pdev, NULL);
err_out_free:
	xd_card_free_host(host);
err_out_release:
	pci_release_regions(pdev);
err_out:
	if (!pci_dev_busy)
		pci_disable_device(pdev);
	return rc;
}

static void jmb38x_xd_remove(struct pci_dev *pdev)
{
	struct xd_card_host *host = pci_get_drvdata(pdev);
	struct jmb38x_xd_host *jhost = xd_card_priv(host);
	void __iomem *addr = jhost->addr;
	unsigned long flags;

	writel(0, jhost->addr + INT_SIGNAL_ENABLE);
	writel(0, jhost->addr + INT_STATUS_ENABLE);
	mmiowb();
	free_irq(pdev->irq, host);
	dev_dbg(&pdev->dev, "interrupts off\n");
	del_timer_sync(&jhost->timer);

	spin_lock_irqsave(&jhost->lock, flags);
	if (jhost->req) {
		jhost->req->error = -ETIME;
		jmb38x_xd_complete_cmd(host, 1);
	}
	spin_unlock_irqrestore(&jhost->lock, flags);

	xd_card_free_host(host);
	iounmap(addr);

	pci_set_drvdata(pdev, NULL);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static struct pci_device_id jmb38x_xd_id_tbl [] = {
	{ PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB38X_XD, PCI_ANY_ID,
	  PCI_ANY_ID, 0, 0, 0 },
	{ }
};

static struct pci_driver jmb38x_xd_driver = {
	.name = DRIVER_NAME,
	.id_table = jmb38x_xd_id_tbl,
	.probe = jmb38x_xd_probe,
	.remove = jmb38x_xd_remove,
	.suspend = jmb38x_xd_suspend,
	.resume = jmb38x_xd_resume
};

static int __init jmb38x_xd_init(void)
{
        return pci_register_driver(&jmb38x_xd_driver);
}

static void __exit jmb38x_xd_exit(void)
{
        pci_unregister_driver(&jmb38x_xd_driver);
}

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("JMicron jmb38x MemoryStick driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, jmb38x_xd_id_tbl);

module_init(jmb38x_xd_init);
module_exit(jmb38x_xd_exit);
