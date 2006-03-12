#include "tifm.h"

#define DRIVER_NAME "tifm_7xx1"
#define DRIVER_VERSION "0.2"

static irqreturn_t tifm_7xx1_isr(int irq, void *dev_id, struct pt_regs *regs)
{
	struct tifm_adapter *fm = (struct tifm_adapter*)dev_id;

	unsigned int irq_status = readl(fm->addr + FM_INTERRUPT_STATUS);
	unsigned int sock_irq_status, cnt;

	if(irq_status && (~irq_status))
	{
		fm->irq_status = irq_status;

		if(irq_status & 0x80000000) {
			writel(0x80000000, fm->addr + FM_CLEAR_INTERRUPT_ENABLE);
			spin_lock(&fm->lock);
			for(cnt = 0; cnt <  fm->max_sockets; cnt++) {
				sock_irq_status = 0
						| (((irq_status >> (cnt + 16)) & 1) ? 0x2 : 0)
						| (((irq_status >> (cnt + 8)) & 1) ? 0x1 : 0);
				if(sock_irq_status && fm->sockets[cnt])
					fm->sockets[cnt]->signal_irq(fm->sockets[cnt], sock_irq_status);
			}
			spin_unlock(&fm->lock);
		}
		writel(irq_status, fm->addr + FM_INTERRUPT_STATUS);
		schedule_work(&fm->isr_bh);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

static void tifm_7xx1_detect_card(struct tifm_adapter *fm, unsigned int sock)
{
}

static void tifm_7xx1_bh(void *dev_id)
{
	struct tifm_adapter *fm = (struct tifm_adapter*)dev_id;
	unsigned int irq_status = fm->irq_status, cnt;
	unsigned long f;

	spin_lock_irqsave(&fm->lock, f);
	for(cnt = 0; cnt <  fm->max_sockets; cnt++)
		if(fm->sockets[cnt]) fm->sockets[cnt]->process_irq(fm->sockets[cnt]);
	spin_unlock_irqrestore(&fm->lock, f);
	
	for(cnt = 0; cnt <  fm->max_sockets; cnt++)
		if(irq_status & (1 << cnt)) tifm_7xx1_detect_card(fm, cnt);
	writel(0x80000000, fm->addr + FM_SET_INTERRUPT_ENABLE);
	
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
	INIT_WORK(&fm->isr_bh, tifm_7xx1_bh, (void*)fm);
	pci_set_drvdata(dev, fm);

	fm->addr = ioremap(pci_resource_start(dev, 0), pci_resource_len(dev, 0));
	if(!fm->addr) goto err_out_free;

	rc = request_irq(dev->irq, tifm_7xx1_isr, SA_SHIRQ, DRIVER_NAME, fm);
	if(rc) goto err_out_unmap;

	rc = tifm_add_adapter(fm);
	if(rc) goto err_out_irq;

	writel(0xffffffff, fm->addr + FM_CLEAR_INTERRUPT_ENABLE);
	writel(0x8000000f, fm->addr + FM_SET_INTERRUPT_ENABLE);

	
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
	
	if(!fm) return;

	tifm_remove_adapter(fm);	
	pci_set_drvdata(dev, NULL);

	writel(0xffffffff, fm->addr + FM_CLEAR_INTERRUPT_ENABLE);
	free_irq(dev->irq, fm);
	cancel_delayed_work(&fm->isr_bh);
	flush_scheduled_work();

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
