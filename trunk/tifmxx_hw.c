/* TI FlashMedia Driver
 *
 * Maintained by: Alex Dubov, oakad@yahoo.com
 * http://tifmxx.berlios.de
 *
**-----------------------------------------------------------------------------
**
**  This program is free software; you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation; either version 2 of the License, or
**  (at your option) any later version.
**
**  This program is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with this program; if not, write to the Free Software
**  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
**-----------------------------------------------------------------------------
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include "tifmxx.h"

unsigned int SDSwitch = 0;

module_param(SDSwitch, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(SDSwitch, "0 - default. Switch to SDIO, 1 - Switch to SD or SDIO, 2 - switch to MMC or SDIO, "
			   "3 - switch to SD or MMC or SDIO");

static struct pci_device_id tifmxx_pci_tbl [] = 
{
	{ PCI_VENDOR_ID_TI, 0x8033, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 
    	  0 }, /* xx21 - the one I have */
  	{ PCI_VENDOR_ID_TI, 0x803B, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 
    	  0 }, /* xx12 - should be also supported */
  	{ }
};

unsigned int
tifmxx_get_media_id(struct tifmxx_sock_data *sock)
{
	unsigned int rc;
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	rc = sock->media_id;
	spin_unlock_irqrestore(&sock->lock, f);
	return rc;
}

void
tifmxx_set_media_id(struct tifmxx_sock_data *sock, unsigned int media_id)
{
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	sock->media_id = media_id;
	spin_unlock_irqrestore(&sock->lock, f);
}

void
tifmxx_set_flag(struct tifmxx_sock_data *sock, unsigned int flag_mask)
{
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	sock->flags |= flag_mask;
	spin_unlock_irqrestore(&sock->lock, f);
}

void
tifmxx_clear_flag(struct tifmxx_sock_data *sock, unsigned int flag_mask)
{
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	sock->flags &= ~flag_mask;
	spin_unlock_irqrestore(&sock->lock, f);
}

int
tifmxx_test_flag(struct tifmxx_sock_data *sock, unsigned int flag_mask)
{
	int rc;
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	rc = (sock->flags & flag_mask) ? 1 : 0;
	spin_unlock_irqrestore(&sock->lock, f);
	return rc;
}

static irqreturn_t 
tifmxx_interrupt(int irq, void *dev_instance, struct pt_regs *regs)
{
	struct tifmxx_data *fm = (struct tifmxx_data*)dev_instance;
	unsigned int irq_status, cnt, card_irq_status;
	unsigned long f;
	
	irq_status = readl(fm->mmio_base + 0x14);
	if(irq_status && (~irq_status))
	{
		FM_DEBUG("Got interrupt\n");
		fm->irq_status = irq_status;    // this is written only here and is ever used only by bh to test
						// for new cards
		if(irq_status & 0x80000000)		
		{
			writel(0x80000000, fm->mmio_base + 0xc);
			for(cnt = 0; cnt < fm->max_sockets; cnt++)
			{
				card_irq_status = 0 | (((irq_status >> (cnt + 16)) & 1) ? INT_B1 : 0) |
						  (((irq_status >> (cnt + 8)) & 1) ? INT_B0 : 0);
				if(card_irq_status)
				{
					spin_lock_irqsave(&fm->sockets[cnt].lock, f);
					if(CARD_PRESENT & fm->sockets[cnt].flags)
						fm->sockets[cnt].signal_irq(&fm->sockets[cnt], card_irq_status);
					spin_unlock_irqrestore(&fm->sockets[cnt].lock, f);
				}
			}
		}
		writel(irq_status, fm->mmio_base + 0x14);
		schedule_work(&fm->isr_bh);
		return IRQ_HANDLED;
	}
  	return IRQ_NONE;
}

static unsigned int
tifmxx_sock_cycle_power(char __iomem *sock_addr, unsigned int sock_flags)
{
	unsigned int p_reg;
	u64 t;

	writel(0x0e00, sock_addr + 0x4);
	
	t=get_jiffies_64();
	while(get_jiffies_64() - t < msecs_to_jiffies(1000))
	{
		if(!(0x80 & readl(sock_addr + 0x8))) break;
		msleep(10);
	}
	
	p_reg = readl(sock_addr + 0x8);
	if(!(0x8 & p_reg)) return 0;


	if(sock_flags & XX12_SOCKET)
		writel((p_reg & 0x7) | 0x0c00, sock_addr + 0x4);
	else
	{
		// SmartMedia cards need extra 40 msec
		if(1 == ((readl(sock_addr + 0x8) >> 4) & 7)) msleep(40); 
		writel(readl(sock_addr + 0x4) | 0x0040, sock_addr + 0x4);
		msleep(10);
		writel((p_reg & 0x7) | 0x0c40, sock_addr + 0x4);
	}

	t=get_jiffies_64();
	while(get_jiffies_64() - t < msecs_to_jiffies(1000))
	{
		if(0x80 & readl(sock_addr + 0x8)) break;
		msleep(10);
	}
	
	if(!(sock_flags & XX12_SOCKET)) writel(readl(sock_addr + 0x4) & 0xffbf, sock_addr + 0x4);

	writel(0x0007, sock_addr + 0x28);
	writel(0x0001, sock_addr + 0x24);

	return (readl(sock_addr + 0x8) >> 4) & 7;
}

static void
tifmxx_detect_card(struct tifmxx_data *fm, unsigned int sock_num)
{
	struct tifmxx_sock_data *sock = &fm->sockets[sock_num];
	char __iomem *sock_addr;
	unsigned int sock_flags;
	unsigned int media_id;
	unsigned long f;
	
	sock_addr = sock->sock_addr;
	sock_flags = sock->flags;

	if(!(sock->flags & CARD_PRESENT)) // detect and insert new card
	{
		media_id = tifmxx_sock_cycle_power(sock_addr, sock_flags);
		switch(media_id)
		{
			case 1:
				FM_DEBUG("SmartMedia card inserted, not supported\n");
				break;
			case 2:
				FM_DEBUG("MemoryStick card inserted, not supported\n");
				break;
			case 3:
				FM_DEBUG("MMC/SD in socket %d detected\n", sock_num);
				tifmxx_mmcsd_init(sock);
				return;
		}

	}
	else // remove old one
	{
		spin_lock_irqsave(&sock->lock, f);
		sock->flags |= CARD_REMOVED;
		wake_up_all(&sock->irq_ack);
		spin_unlock_irqrestore(&sock->lock, f);
	}
	writel(0x00010100 << sock_num, fm->mmio_base + 0xc);
	writel(0x00010100 << sock_num, fm->mmio_base + 0x8);
}

static void
tifmxx_isr_bh(void *data)
{
	struct tifmxx_data *fm = (struct tifmxx_data*)data;
	unsigned int cnt, irq_status;
	unsigned long f;

  	FM_DEBUG("Bottom half\n");

	irq_status = fm->irq_status;
	// insert/remove
	for(cnt = 0; cnt < fm->max_sockets; cnt++)
	{
	
		spin_lock_irqsave(&fm->sockets[cnt].lock, f);
		if(fm->sockets[cnt].flags & CARD_PRESENT) fm->sockets[cnt].process_irq(&fm->sockets[cnt]);
		spin_unlock_irqrestore(&fm->sockets[cnt].lock, f);
	}

	for(cnt = 0; cnt < fm->max_sockets; cnt++)
	{
		if(irq_status & (1 << cnt))
		{
			tifmxx_detect_card(fm, cnt);
		}
	}
	writel(0x80000000, fm->mmio_base + 0x8);
}

extern struct scsi_host_template tifmxx_host_template;

static inline void
tifmxx_sock_data_init(struct Scsi_Host *fm_host)
{
	struct tifmxx_data *fm = (struct tifmxx_data*)fm_host->hostdata;
	struct tifmxx_sock_data *sock;
	int cnt;

	for(cnt = 0; cnt < fm->max_sockets; cnt++)
	{
		sock = &fm->sockets[cnt];

		memset(sock, 0, sizeof(struct tifmxx_sock_data));

		spin_lock_init(&sock->lock);

		INIT_WORK(&sock->do_scsi_cmd, tifmxx_eval_scsi, (void*)sock);

		init_waitqueue_head(&sock->irq_ack);

		sock->sock_addr = fm->mmio_base + (((unsigned long)cnt + 1) << 10);
		sock->host = fm_host;
		sock->sock_id = cnt;
		sock->flags |= (fm->max_sockets == 2) ? XX12_SOCKET : 0;
		sock->flags |= (SDSwitch & 0x1) ? ALLOW_SD : 0;
		sock->flags |= (SDSwitch & 0x2) ? ALLOW_MMC : 0;
	}
	
	// 4 socket FM device has special ability on socket 2
	if(fm->max_sockets == 4)
		fm->sockets[2].flags |= MS_SOCKET;
}

static int 
tifmxx_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{	
  	int rc;
	struct Scsi_Host *host = NULL;
	struct tifmxx_data *fm = NULL;
	void __iomem *mmio_base = NULL;
  	int pci_dev_busy = 0;

	printk(KERN_INFO DRIVER_NAME": probe called\n");
  	
	rc = pci_set_dma_mask(pdev, DMA_32BIT_MASK); if(rc) return rc;
	rc = pci_enable_device(pdev); if(rc) return rc;

	pci_set_master(pdev); // pcilynx sets this, so we do

  	rc = pci_request_regions(pdev, DRIVER_NAME);
  	if (rc) { pci_dev_busy = 1; goto err_out; }
	
	pci_intx(pdev, 1);

	host = scsi_host_alloc(&tifmxx_host_template, sizeof(*fm));
	if (!host) 
	{
                rc = -ENOMEM;
		goto err_out_int;
        }

	fm = (struct tifmxx_data*)host->hostdata;
	memset(fm, 0, sizeof(struct tifmxx_data));
	fm->dev = pdev;	
	
	fm->max_sockets = (pdev->device == 0x803B) ? 2 : 4;
	INIT_WORK(&fm->isr_bh, tifmxx_isr_bh, (void*)fm);

	
	host->max_id = fm->max_sockets;
	host->max_lun = 0;
	host->max_channel = 0;

	/* This device has only one memory region */
	mmio_base = ioremap(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0));
	if (mmio_base == NULL) 
	{
                rc = -ENOMEM;
                goto err_out_free_host;
        }
	
	fm->mmio_base = mmio_base;
	
	tifmxx_sock_data_init(host);

	pci_set_drvdata(pdev, host);

	rc = scsi_add_host(host, &pdev->dev); 
	if (rc) goto err_out_iounmap;
	
	rc = request_irq(pdev->irq, tifmxx_interrupt, SA_SHIRQ, DRIVER_NAME, fm); 
	if (rc) goto err_out_remove_host;			
	
	writel(0xffffffff, mmio_base + 0x0c);
	writel(0x8000000f, mmio_base + 0x08);
	//! detect cards, add scsi devices

	return 0; 

err_out_remove_host:
	scsi_remove_host(host);
err_out_iounmap:
	iounmap(mmio_base);
err_out_free_host:
	scsi_host_put(host);
err_out_int:
	pci_intx(pdev, 0);
	pci_release_regions(pdev);
err_out:
  	if (!pci_dev_busy) pci_disable_device(pdev);

	FM_DEBUG("probe failed\n");
  	return rc;
}

static void 
tifmxx_remove(struct pci_dev *pdev)
{
	struct Scsi_Host *host = pci_get_drvdata(pdev); 
	struct tifmxx_data *fm = (struct tifmxx_data*)host->hostdata;

	writel(0xffffffff, fm->mmio_base + 0x0c);
	free_irq(fm->dev->irq, fm);
	
	scsi_remove_host(host);	
	pci_intx(pdev, 0);
	pci_release_regions(pdev);
	scsi_host_put(host);
	pci_disable_device(pdev);
}

static struct pci_driver tifmxx_driver = 
{
	.name = DRIVER_NAME,
	.id_table = tifmxx_pci_tbl,
	.probe = tifmxx_probe,
	.remove = tifmxx_remove,
};

static int __init 
tifmxx_init(void)
{
  	return pci_register_driver(&tifmxx_driver);
}

static void __exit 
tifmxx_exit(void)
{
  	pci_unregister_driver(&tifmxx_driver);
}

MODULE_AUTHOR("Alex Dubov");
MODULE_DESCRIPTION("TI FlashMedia low level driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, tifmxx_pci_tbl);
MODULE_VERSION(DRIVER_VERSION);

module_init(tifmxx_init);
module_exit(tifmxx_exit);
