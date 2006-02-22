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

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include "tifmxx.h"

unsigned int sd_switch = 0;

module_param(sd_switch, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(sd_switch, "0 - default. Switch to SDIO, "
			    "1 - Switch to SD or SDIO, "
			    "3 - switch to SD or MMC or SDIO");

static struct pci_device_id tifmxx_pci_tbl [] =
{
        { PCI_VENDOR_ID_TI, 0x8033, PCI_ANY_ID, PCI_ANY_ID, 0, 0,
          0 }, /* xx21 - the one I have */
        { PCI_VENDOR_ID_TI, 0x803B, PCI_ANY_ID, PCI_ANY_ID, 0, 0,
          0 }, /* xx12 - should be also supported */
        { }
};

static void tifmxx_free_host(struct kobject* kobj)
{
	struct tifmxx_host* fm_host = container_of(kobj, struct tifmxx_host, kobj);
	int cnt;
	unsigned long f;
	struct tifmxx_socket* sock;

	if(fm_host->sockets) {
		for(cnt = 0; cnt < fm_host->num_sockets; cnt++) {
			sock = fm_host->sockets[cnt];
			spin_lock_irqsave(&sock->lock, f);
			if(sock->eject) sock->eject(sock);
			spin_unlock_irqrestore(&sock->lock, f);
			kobject_put(&fm_host->sockets[cnt]->kobj);
		}
		kfree(fm_host->sockets);
	}
	kfree(fm_host);
}

static void tifmxx_free_socket(struct kobject* kobj)
{
	struct tifmxx_socket* sock = container_of(kobj, struct tifmxx_socket, kobj);
	
	kfree(sock);
}

static struct kobj_type tifmxx_host_kobj_type = {
	.release = tifmxx_free_host
};

static struct kobj_type tifmxx_socket_kobj_type = {
	.release = tifmxx_free_socket
};

static unsigned int tifmxx_sock_cycle_power(struct tifmxx_socket* sock)
{
	unsigned int p_reg;
	u64 t;

	writel(0xe00, sock->sock_addr + SOCK_CONTROL);

	// wait for card to chut down
	t = get_jiffies_64();
	while(get_jiffies_64() - t < msecs_to_jiffies(1000)) {
		if(!(0x80 & readl(sock->sock_addr + SOCK_PRESENT_STATE))) break;
		msleep(10);
	}

	p_reg = readl(sock->sock_addr + SOCK_PRESENT_STATE);
	if(!(8 & p_reg)) return 0;

	if(sock->fixed_flags & XX12_SOCKET)
		writel(0xc00 | (p_reg & 7), sock->sock_addr + SOCK_CONTROL);
	else {
		 // SmartMedia cards need extra 40 msec
		if(1 == ((readl(sock->sock_addr + SOCK_PRESENT_STATE) >> 4) & 7)) 
			msleep(40);
		writel(0x40 | readl(sock->sock_addr + SOCK_CONTROL), 
		       sock->sock_addr + SOCK_CONTROL);
		msleep(10);
		writel(0xc40 | (p_reg & 0x7), sock->sock_addr + SOCK_CONTROL);
	}

	// wait for card to power up
	t = get_jiffies_64();
	while(get_jiffies_64() - t < msecs_to_jiffies(1000)) {
		if(0x80 & readl(sock->sock_addr + SOCK_PRESENT_STATE)) break;
		msleep(10);
	}

	if(!(sock->fixed_flags & XX12_SOCKET))
		writel(0xffbf & readl(sock->sock_addr + SOCK_CONTROL),
		       sock->sock_addr + SOCK_CONTROL);

	writel(7, sock->sock_addr + SOCK_FIFO_PAGE_SIZE);
	writel(1, sock->sock_addr + SOCK_FIFO_CONTROL);
	return 7 & (readl(sock->sock_addr + SOCK_PRESENT_STATE) >> 4);
}

static void tifmxx_isr_bh(void *data)
{
	struct tifmxx_host* fm_host = (struct tifmxx_host*)data;
	unsigned int int_status = fm_host->int_status;
	unsigned int cnt, media_id;
	unsigned long f;
	struct tifmxx_socket* sock;

	kobject_get(&fm_host->kobj);

	for(cnt = 0; cnt < fm_host->num_sockets; cnt++) {
		sock = fm_host->sockets[cnt];
		kobject_get(&sock->kobj);

		spin_lock_irqsave(&sock->lock, f);
		if(sock->process_int) sock->process_int(sock);
		spin_unlock_irqrestore(&sock->lock, f);

		kobject_put(&sock->kobj);
	}
	
	for(cnt = 0; cnt < fm_host->num_sockets; cnt++) {
		sock = fm_host->sockets[cnt];
		kobject_get(&sock->kobj);

		if(int_status & (1 << cnt)) {
			spin_lock_irqsave(&sock->lock, f);
			if(!sock->eject) {
				spin_unlock_irqrestore(&sock->lock, f);
				media_id = tifmxx_sock_cycle_power(sock);
				spin_lock_irqsave(&sock->lock, f);

				switch(media_id) {
					case 1:
						DBG("SmartMedia/xD - can use an implementation, really\n");
						goto enable_interrupt;
					case 2:
						DBG("MemoryStick - can use an implementation, really\n");
						goto enable_interrupt;
					case 3:
						tifmxx_mmcsd_insert(sock);
						break;
				}
			}
			else {
				sock->eject(sock);
enable_interrupt:
				writel(0x00010100 << cnt, fm_host->mmio_base + FM_CLEAR_INTERRUPT_ENABLE);
				writel(0x00010100 << cnt, fm_host->mmio_base + FM_SET_INTERRUPT_ENABLE);
			}
			spin_unlock_irqrestore(&sock->lock, f);
		}

		kobject_put(&sock->kobj);
	}

	writel(0x80000000, fm_host->mmio_base + FM_SET_INTERRUPT_ENABLE);
	kobject_put(&fm_host->kobj);
}

static irqreturn_t
tifmxx_interrupt(int irq, void *dev_instance, struct pt_regs *regs)
{
	struct tifmxx_host* fm_host = (struct tifmxx_host*)dev_instance;
	unsigned int int_status, cnt, sock_int_status;
	unsigned long f;
	struct tifmxx_socket* sock;

	kobject_get(&fm_host->kobj);

	int_status = readl(fm_host->mmio_base + FM_INTERRUPT_STATUS);

	if(int_status && (~int_status)) {
		fm_host->int_status = int_status;
		if(int_status & 0x80000000) {
			writel(0x80000000, fm_host->mmio_base + FM_CLEAR_INTERRUPT_ENABLE);
			for(cnt = 0; cnt < fm_host->num_sockets; cnt++) {
				sock = fm_host->sockets[cnt];
				kobject_get(&sock->kobj);

				sock_int_status = 0 | (((int_status >> (cnt + 16)) & 1) ? INT_B1 : 0) 
						    | (((int_status >> (cnt + 8)) & 1) ? INT_B0 : 0);
				if(sock_int_status) {
					spin_lock_irqsave(&sock->lock, f);
					if(sock->signal_int) sock->signal_int(sock, sock_int_status);
					spin_unlock_irqrestore(&sock->lock, f);
				}
				kobject_put(&sock->kobj);
			}
		}
		writel(int_status, fm_host->mmio_base + FM_INTERRUPT_STATUS);
		schedule_work(&fm_host->isr_bh);
		kobject_put(&fm_host->kobj);
		return IRQ_HANDLED;
	}
	kobject_put(&fm_host->kobj);
	return IRQ_NONE;
}

static int tifmxx_init_sockets(struct tifmxx_host* fm_host)
{
	int cnt;
	struct tifmxx_socket* sock;

	if(NULL == (fm_host->sockets = kmalloc(fm_host->num_sockets * sizeof(struct tifmxx_socket*), 
					       GFP_KERNEL)))
		return -ENOMEM;

	memset(fm_host->sockets, 0, fm_host->num_sockets * 
		sizeof(struct tifmxx_socket*));

	for(cnt = 0; cnt < fm_host->num_sockets; cnt++) {
		if(NULL == (fm_host->sockets[cnt] = kmalloc(sizeof(struct tifmxx_socket*), GFP_KERNEL)))
			return -ENOMEM;

		sock = fm_host->sockets[cnt];
		memset(sock, 0, sizeof(struct tifmxx_socket));

		kobject_init(&sock->kobj);
		sock->kobj.ktype = &tifmxx_socket_kobj_type;
		spin_lock_init(&sock->lock);
		init_waitqueue_head(&sock->hw_wq);
		sock->sock_addr = fm_host->mmio_base + (((unsigned long)cnt + 1) << 10);
		sock->fixed_flags |= (fm_host->num_sockets == 2) ? XX12_SOCKET : 0;
		sock->fixed_flags |= (sd_switch & 1) ? ALLOW_SD : 0;
		sock->fixed_flags |= (sd_switch & 2) ? ALLOW_MMC : 0;
	}

	if(fm_host->num_sockets == 4) 
		fm_host->sockets[2]->fixed_flags |= MS_SOCKET;

	return 0;	
}

inline static int tifmxx_num_sockets_of_id(unsigned short dev_id)
{
	switch(dev_id)
	{
		case 0x8033: return 4;
		case 0x803b: return 2;
		default: return 4; //probably bad idea
	}
}

static int __devinit tifmxx_probe(struct pci_dev* pdev, 
	const struct pci_device_id* ent)
{
	int rc, pci_dev_busy = 0;
	struct tifmxx_host* fm_host = NULL;
	void __iomem *mmio_base = NULL;

	rc = pci_set_dma_mask(pdev, DMA_32BIT_MASK); if(rc) return rc;
        rc = pci_enable_device(pdev); if(rc) return rc;

        pci_set_master(pdev);

        rc = pci_request_regions(pdev, DRIVER_NAME);
        if (rc) { pci_dev_busy = 1; goto err_out; }

        pci_intx(pdev, 1);

	if(NULL == (fm_host = kmalloc(sizeof(struct tifmxx_host), GFP_KERNEL)))
		goto err_out_int;
	
	memset(fm_host, 0, sizeof(struct tifmxx_host));
	kobject_init(&fm_host->kobj);
	fm_host->kobj.ktype = &tifmxx_host_kobj_type;
	fm_host->dev = pdev;
	fm_host->num_sockets = tifmxx_num_sockets_of_id(pdev->device);
	INIT_WORK(&fm_host->isr_bh, tifmxx_isr_bh, (void*)fm_host);

	/* This device has only one memory region */
  	mmio_base = ioremap(pci_resource_start(pdev, 0), 
		pci_resource_len(pdev, 0));

	if (mmio_base == NULL) {
		rc = -ENOMEM;
		goto err_out_free_host;
	}

	fm_host->mmio_base = mmio_base;

	rc = tifmxx_init_sockets(fm_host);

	if(rc) goto err_out_remove_all;

	pci_set_drvdata(pdev, fm_host);

	rc = request_irq(pdev->irq, tifmxx_interrupt, SA_SHIRQ, DRIVER_NAME, fm_host);

	if (rc) goto err_out_remove_all;

	writel(0xffffffff, mmio_base + FM_CLEAR_INTERRUPT_ENABLE);
	writel(0x8000000f, mmio_base + FM_SET_INTERRUPT_ENABLE);

	/*! check for already present cards? */

	return 0;

err_out_remove_all:
        iounmap(mmio_base);
err_out_free_host:
	kobject_put(&fm_host->kobj);
err_out_int:
        pci_intx(pdev, 0);
        pci_release_regions(pdev);
err_out:
        if (!pci_dev_busy) pci_disable_device(pdev);
        return rc;
}


static void __devexit tifmxx_remove(struct pci_dev* pdev)
{
	struct tifmxx_host* fm_host = pci_get_drvdata(pdev);

	writel(0xffffffff, fm_host->mmio_base + FM_CLEAR_INTERRUPT_ENABLE);
	free_irq(fm_host->dev->irq, fm_host);
	pci_intx(pdev, 0);
	kobject_put(&fm_host->kobj);

	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

#undef CONFIG_PM
#ifdef CONFIG_PM
#else
#define tifmxx_suspend NULL
#define tifmxx_resume  NULL
#endif

static struct pci_driver tifmxx_driver = {
        .name = DRIVER_NAME,
        .id_table = tifmxx_pci_tbl,
        .probe = tifmxx_probe,
        .remove = __devexit_p(tifmxx_remove),
	.suspend = tifmxx_suspend,
	.resume = tifmxx_resume
};

static int __init tifmxx_init(void)
{
        return pci_register_driver(&tifmxx_driver);
}

static void __exit tifmxx_exit(void)
{
        pci_unregister_driver(&tifmxx_driver);
}

MODULE_AUTHOR("Alex Dubov <oakad@yahoo.com>");
MODULE_DESCRIPTION("TI FlashMedia low level driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, tifmxx_pci_tbl);
MODULE_VERSION(DRIVER_VERSION);

module_init(tifmxx_init);
module_exit(tifmxx_exit);
