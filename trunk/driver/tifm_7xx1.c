/*
 *  tifm_7xx1.c - TI FlashMedia driver
 *
 *  Copyright (C) 2006 Alex Dubov <oakad@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include "tifm.h"
#include <linux/interrupt.h>

#define DRIVER_NAME "tifm_7xx1"
#define DRIVER_VERSION "0.4"

static void tifm_7xx1_eject(struct tifm_adapter *fm, struct tifm_dev *sock)
{
	int cnt;
	unsigned long f;

	spin_lock_irqsave(&fm->lock, f);
	for(cnt = 0; cnt < fm->max_sockets; cnt++) {
		if(fm->sockets[cnt] == sock) {
			fm->remove_mask = 1 << cnt;
			queue_work(fm->wq, &fm->media_remover);
			break;
		}
	}
	spin_unlock_irqrestore(&fm->lock, f);
}

static void tifm_7xx1_remove_media(void *adapter)
{
	struct tifm_adapter *fm = (struct tifm_adapter*)adapter;
	unsigned long f;
	int cnt;
	struct tifm_dev *sock;

	if(!class_device_get(&fm->cdev)) return;
	spin_lock_irqsave(&fm->lock, f);
	for(cnt = 0; cnt < fm->max_sockets; cnt++) {
		if(fm->sockets[cnt] && (fm->remove_mask & (1 << cnt))) {
			DBG("Demand removing card from socket %d\n", cnt);
			sock = fm->sockets[cnt];
			fm->sockets[cnt] = 0;
			fm->remove_mask &= ~(1 << cnt);
			writel(0x00010100 << cnt, fm->addr + FM_CLEAR_INTERRUPT_ENABLE);
			writel(0x00010100 << cnt, fm->addr + FM_SET_INTERRUPT_ENABLE);

			spin_unlock_irqrestore(&fm->lock, f);
			device_unregister(&sock->dev);
			spin_lock_irqsave(&fm->lock, f);
		}
	}
	spin_unlock_irqrestore(&fm->lock, f);
	class_device_put(&fm->cdev);
}

static irqreturn_t tifm_7xx1_isr(int irq, void *dev_id, struct pt_regs *regs)
{
	struct tifm_adapter *fm = (struct tifm_adapter*)dev_id;

	unsigned int irq_status;
	unsigned int sock_irq_status, cnt;
	irqreturn_t rc = IRQ_NONE;

	spin_lock(&fm->lock);
	irq_status = readl(fm->addr + FM_INTERRUPT_STATUS);
	if(irq_status && (~irq_status))
	{
		
		if(irq_status & 0x80000000) {
			writel(0x80000000, fm->addr + FM_CLEAR_INTERRUPT_ENABLE);
			
			for(cnt = 0; cnt <  fm->max_sockets; cnt++) {
				sock_irq_status = (irq_status >> cnt) & 0x00010100;
				if(fm->sockets[cnt]) {
					if(sock_irq_status && fm->sockets[cnt]->signal_irq) {
						sock_irq_status = fm->sockets[cnt]->signal_irq(fm->sockets[cnt], sock_irq_status);
					}
					if(irq_status & (1 << cnt)) fm->remove_mask |= 1 << cnt;
				} else {
					if(irq_status & (1 << cnt)) fm->insert_mask |= 1 << cnt;
				}
			}
			
		}
		writel(irq_status, fm->addr + FM_INTERRUPT_STATUS);

		if(!fm->remove_mask && !fm->insert_mask)
			writel(0x80000000, fm->addr + FM_SET_INTERRUPT_ENABLE);
		else {
			queue_work(fm->wq, &fm->media_remover);
			queue_work(fm->wq, &fm->media_inserter);
		}
	
		rc = IRQ_HANDLED;
	}
	spin_unlock(&fm->lock);
	return rc;
}

static tifm_media_id tifm_7xx1_toggle_sock_power(char *sock_addr, int is_x2)
{
	unsigned int s_state;
	unsigned long long t;

	writel(0x0e00, sock_addr + SOCK_CONTROL);

	t = get_jiffies_64();
	while(get_jiffies_64() - t < msecs_to_jiffies(1000)) {
		if(!(0x0080 & readl(sock_addr + SOCK_PRESENT_STATE))) break;
		msleep(10);
	}

	s_state = readl(sock_addr + SOCK_PRESENT_STATE);
	if(!(0x0008 & s_state)) return FM_NULL;

	if(is_x2) {
		writel((s_state & 7) | 0x0c00, sock_addr + SOCK_CONTROL);
	} else {
		// SmartMedia cards need extra 40 msec
		if(1 == ((readl(sock_addr + SOCK_PRESENT_STATE) >> 4) & 7)) msleep(40);
		writel(readl(sock_addr + SOCK_CONTROL) | 0x0040, sock_addr + SOCK_CONTROL);
		msleep(10);
		writel((s_state & 0x7) | 0x0c40, sock_addr + SOCK_CONTROL);
	}
	
	t = get_jiffies_64();
	while(get_jiffies_64() - t < msecs_to_jiffies(1000)) {
		if((0x0080 & readl(sock_addr + SOCK_PRESENT_STATE))) break;
		msleep(10);
	}

	if(!is_x2) writel(readl(sock_addr + SOCK_CONTROL) & 0xffbf, sock_addr + SOCK_CONTROL);

	writel(0x0007, sock_addr + SOCK_FIFO_PAGE_SIZE);
	writel(0x0001, sock_addr + SOCK_FIFO_CONTROL);

	return (readl(sock_addr + SOCK_PRESENT_STATE) >> 4) & 7;
}

inline static char* tifm_7xx1_sock_addr(char *base_addr, unsigned int sock_num)
{
	return base_addr + ((sock_num + 1) << 10);
}
 
static void tifm_7xx1_insert_media(void *adapter)
{
	struct tifm_adapter *fm = (struct tifm_adapter*)adapter;
	unsigned long f;
	tifm_media_id media_id;
	char *card_name = "xx";
	int cnt, ok_to_register;
	unsigned int insert_mask;
	struct tifm_dev *new_sock = 0;

	if(!class_device_get(&fm->cdev)) return;
	spin_lock_irqsave(&fm->lock, f);
	insert_mask = fm->insert_mask;
	fm->insert_mask = 0;
	spin_unlock_irqrestore(&fm->lock, f);

	for(cnt = 0; cnt < fm->max_sockets; cnt++) {
		if(insert_mask & (1 << cnt)) {
			media_id = tifm_7xx1_toggle_sock_power(tifm_7xx1_sock_addr(fm->addr, cnt),
							       fm->max_sockets == 2);
			if(media_id) {
				ok_to_register = 0;
				if((new_sock = tifm_alloc_device(fm, cnt))) {
					new_sock->addr = tifm_7xx1_sock_addr(fm->addr, cnt);
					new_sock->media_id = media_id;
					switch(media_id)
					{
						case 1:
							card_name = "sm"; break;
						case 2:
							card_name = "ms"; break;
						case 3:
							card_name = "sd"; break;
						default:
							break;
					}
					snprintf(new_sock->dev.bus_id, BUS_ID_SIZE,
						 "tifm_%s%u:%u", card_name, fm->id, cnt);
					spin_lock_irqsave(&fm->lock, f);
					if(!fm->sockets[cnt]) {
						fm->sockets[cnt] = new_sock;
						ok_to_register = 1;
					}
					spin_unlock_irqrestore(&fm->lock, f);
					if(!ok_to_register || device_register(&new_sock->dev)) {
						spin_lock_irqsave(&fm->lock, f);
						fm->sockets[cnt] = 0;
						spin_unlock_irqrestore(&fm->lock, f);
						tifm_free_device(&new_sock->dev);
					}
				}
			}
			writel(0x00010100 << cnt, fm->addr + FM_CLEAR_INTERRUPT_ENABLE);
			writel(0x00010100 << cnt, fm->addr + FM_SET_INTERRUPT_ENABLE);
		}
	}

	writel(0x80000000, fm->addr + FM_SET_INTERRUPT_ENABLE);
	class_device_put(&fm->cdev);
}

static int tifm_7xx1_suspend(struct pci_dev *dev, pm_message_t state)
{
	struct tifm_adapter *fm = pci_get_drvdata(dev);
	unsigned long f;
	int cnt;

	spin_lock_irqsave(&fm->lock, f);
	writel(0x80000000, fm->addr + FM_CLEAR_INTERRUPT_ENABLE);
	for(cnt = 0; cnt < fm->max_sockets; cnt++) {
		if(fm->sockets[cnt]) {
			device_unregister(&fm->sockets[cnt]->dev);
			fm->sockets[cnt] = 0;
			writel(0x00010100 << cnt, fm->addr + FM_CLEAR_INTERRUPT_ENABLE);
		}
	}
	fm->remove_mask = 0;
	fm->insert_mask = 0;
	spin_unlock_irqrestore(&fm->lock, f);
	pci_set_power_state(dev, PCI_D3hot);
        pci_disable_device(dev);
        pci_save_state(dev);
	return 0;
}

static int tifm_7xx1_resume(struct pci_dev *dev)
{
	struct tifm_adapter *fm = pci_get_drvdata(dev);
	unsigned long f;

	pci_restore_state(dev);
        pci_enable_device(dev);
        pci_set_power_state(dev, PCI_D0);
        pci_set_master(dev);

	spin_lock_irqsave(&fm->lock, f);
	writel(0xffffffff, fm->addr + FM_INTERRUPT_STATUS);
	writel(0xffffffff, fm->addr + FM_CLEAR_INTERRUPT_ENABLE);
	writel(0x8000000f, fm->addr + FM_SET_INTERRUPT_ENABLE);
	spin_unlock_irqrestore(&fm->lock, f);
	return 0;
}

static int tifm_7xx1_probe(struct pci_dev *dev, const struct pci_device_id *dev_id)
{
	struct tifm_adapter *fm;
	int pci_dev_busy = 0;
	int rc;

	rc = pci_set_dma_mask(dev, DMA_32BIT_MASK); if(rc) return rc;
	rc = pci_enable_device(dev); if(rc) return rc;

	pci_set_master(dev);

	rc = pci_request_regions(dev, DRIVER_NAME);
	if (rc) { pci_dev_busy = 1; goto err_out; }

	pci_intx(dev, 1);

	if (!(fm = tifm_alloc_adapter()))
	{
		rc = -ENOMEM;
		goto err_out_int;
	}

	fm->dev = &dev->dev;
	fm->max_sockets = (dev->device == 0x803B) ? 2 : 4;
	fm->sockets = kzalloc(sizeof(struct tifm_dev*) * fm->max_sockets, GFP_KERNEL);
	if(!fm->sockets) goto err_out_free;

	INIT_WORK(&fm->media_inserter, tifm_7xx1_insert_media, (void*)fm);
	INIT_WORK(&fm->media_remover, tifm_7xx1_remove_media, (void*)fm);
	fm->eject = tifm_7xx1_eject;
	pci_set_drvdata(dev, fm);

	fm->addr = ioremap(pci_resource_start(dev, 0), pci_resource_len(dev, 0));
	if(!fm->addr) goto err_out_free;

	rc = request_irq(dev->irq, tifm_7xx1_isr, SA_SHIRQ, DRIVER_NAME, fm);
	if(rc) goto err_out_unmap;

	rc = tifm_add_adapter(fm);
	if(rc) goto err_out_irq;

	writel(0xffffffff, fm->addr + FM_CLEAR_INTERRUPT_ENABLE);
	writel(0x8000000f, fm->addr + FM_SET_INTERRUPT_ENABLE);

	fm->insert_mask = 0xf;
	
	return 0;

err_out_irq:
	free_irq(dev->irq, fm);
err_out_unmap:
	iounmap(fm->addr);
err_out_free:
	pci_set_drvdata(dev, NULL);
	tifm_free_adapter(fm);
err_out_int:
	pci_intx(dev, 0);
	pci_release_regions(dev);
err_out:
	if(!pci_dev_busy) pci_disable_device(dev);
	return rc;
}

static void tifm_7xx1_remove(struct pci_dev *dev)
{
	struct tifm_adapter *fm = pci_get_drvdata(dev);
	int cnt;
	unsigned long f;
	
	if(!fm) return;

	tifm_remove_adapter(fm);
	spin_lock_irqsave(&fm->lock, f);
	pci_set_drvdata(dev, NULL);

	writel(0xffffffff, fm->addr + FM_CLEAR_INTERRUPT_ENABLE);
	free_irq(dev->irq, fm);


	for(cnt = 0; cnt < fm->max_sockets; cnt++) {
		if(fm->sockets[cnt]) device_unregister(&fm->sockets[cnt]->dev);
		fm->sockets[cnt] = 0;
	}
	spin_unlock_irqrestore(&fm->lock, f);

	iounmap(fm->addr);
	pci_intx(dev, 0);
	pci_release_regions(dev);

	pci_disable_device(dev);
	tifm_free_adapter(fm);
}

static struct pci_device_id tifm_7xx1_pci_tbl [] = {
	{ PCI_VENDOR_ID_TI, 0x8033, PCI_ANY_ID, PCI_ANY_ID, 0, 0,
	  0 }, /* xx21 - the one I have */
        { PCI_VENDOR_ID_TI, 0x803B, PCI_ANY_ID, PCI_ANY_ID, 0, 0,
	  0 }, /* xx12 - should be also supported */
	{ }
};

static struct pci_driver tifm_7xx1_driver = {
	.name = DRIVER_NAME,
	.id_table = tifm_7xx1_pci_tbl,
	.probe = tifm_7xx1_probe,
	.remove = tifm_7xx1_remove,
	.suspend = tifm_7xx1_suspend,
	.resume = tifm_7xx1_resume,
};

static int __init tifm_7xx1_init(void)
{
	return pci_register_driver(&tifm_7xx1_driver);
}

static void __exit tifm_7xx1_exit(void)
{
	pci_unregister_driver(&tifm_7xx1_driver);
}

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("TI FlashMedia host driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, tifm_7xx1_pci_tbl);
MODULE_VERSION(DRIVER_VERSION);

module_init(tifm_7xx1_init);
module_exit(tifm_7xx1_exit);
