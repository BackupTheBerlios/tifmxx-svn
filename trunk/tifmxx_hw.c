/* TI FlashMedia Driver
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

/* History:
 *

 * Revision 0.1   2005-12-05   Alex Dubov (oakad@yahoo.com)
 * 
 * Initial release, nothing works yet. 

 *	
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include "tifmxx.h"

static struct pci_device_id tifmxx_pci_tbl [] = 
{
	{ PCI_VENDOR_ID_TI, 0x8033, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 
    	  0 }, /* xx21 - the one I have */
  	{ PCI_VENDOR_ID_TI, 0x803B, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 
    	  0 }, /* xx12 - should be also supported */
  	{ }
};

static irqreturn_t 
tifmxx_interrupt(int irq, void *dev_instance, struct pt_regs *regs)
{
	struct tifmxx_data *fm = (struct tifmxx_data*)dev_instance;
	unsigned int irq_status, cnt, card_irq_status;
	//unsigned long f;
	irqreturn_t rc = IRQ_NONE;

	read_lock(&fm->lock);
	
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
				card_irq_status = 0 | ((irq_status >> (cnt + 15)) & 2) | 
						  ((irq_status >> (cnt + 8)) & 1);
				if(card_irq_status)
				{
					FM_DEBUG("Signal irq status %x to card %d\n", card_irq_status, cnt);
				}
			}
		}
		writel(irq_status, fm->mmio_base + 0x14);
		schedule_work(&fm->isr_bh);
		rc = IRQ_HANDLED;
	}
	read_unlock(&fm->lock);
  	return rc;
}

static void
tifmxx_detect_card(struct tifmxx_data *fm, unsigned int sock_num)
{
}

static void
tifmxx_isr_bh(void *data)
{
	struct tifmxx_data *fm = (struct tifmxx_data*)data;
	unsigned int cnt, irq_status;

  	FM_DEBUG("Bottom half\n");

	read_lock(&fm->lock);
	irq_status = fm->irq_status;
	//! process active sockets	
	// insert/remove
	for(cnt = 0; cnt < fm->max_sockets; cnt++)
	{
		if(irq_status & (1 << cnt))
		{
			tifmxx_detect_card(fm, cnt);
		}
	}
	writel(0x80000000, fm->mmio_base + 0x8);
	read_unlock(&fm->lock);
}

static void
tifmxx_eval_scsi(void *data)
{
	struct tifmxx_sock_data *sock = (struct tifmxx_sock_data*)data;

}

extern struct scsi_host_template tifmxx_host_template;

static int
tifmxx_sock_data_init(struct tifmxx_data *fm)
{
	struct tifmxx_sock_data *c_socket;
	int cnt;

	if(fm->max_sockets > MAX_SUPPORTED_SOCKETS) return 0;

	for(cnt = 0; cnt < fm->max_sockets; cnt++)
	{
		c_socket = fm->sockets[cnt] = 
			kmalloc(sizeof(struct tifmxx_sock_data), GFP_KERNEL);
		if(!c_socket) goto clean_up_and_fail;
		memset(c_socket, 0, sizeof(struct tifmxx_sock_data));
		rwlock_init(&c_socket->lock);
		INIT_WORK(&c_socket->do_scsi_cmd, tifmxx_eval_scsi,
			  (void*)c_socket);
		c_socket->sock_addr = fm->mmio_base + 
				      (((unsigned long)cnt + 1) << 10);
		c_socket->host = fm;
	}
	
	// 4 socket FM device has special ability on socket 2
	if(fm->max_sockets == 4)
		fm->sockets[2]->flags |= MS_SOCKET;

	return 1;

clean_up_and_fail:
	for(cnt = 0; fm->sockets[cnt]; cnt++) kfree(fm->sockets[cnt]);
	return 0;
}

static void
tifmxx_sock_data_fini(struct tifmxx_data *fm)
{
	unsigned int cnt;
	unsigned long f;	

	for(cnt = 0; cnt < fm->max_sockets; cnt++)
	{
		struct tifmxx_sock_data *c_socket = fm->sockets[cnt];
		if(c_socket)
		{
			write_lock_irqsave(&c_socket->lock, f);
			fm->sockets[cnt] = 0;
			//! change this to wait for completion
			cancel_delayed_work(&c_socket->do_scsi_cmd);
			flush_scheduled_work();

			if(c_socket->flags & CARD_PRESENT)
			{
				c_socket->finalize(c_socket);

				if(c_socket->srb)
				{
					c_socket->srb->result = DID_ABORT << 16;
					c_socket->srb->scsi_done(c_socket->srb);
				}
				//! kick scsi device
			}
			
			write_unlock_irqrestore(&c_socket->lock, f);
			kfree(c_socket);
		}
	}
}

static int 
tifmxx_probe (struct pci_dev *pdev, const struct pci_device_id *ent)
{	
  	int rc;
	struct Scsi_Host *host = NULL;
	struct tifmxx_data *fm = NULL;
	void __iomem *mmio_base = NULL;
  	int have_msi = 0, pci_dev_busy = 0;

	printk(KERN_INFO DRIVER_NAME": probe called\n");
  	
	rc = pci_enable_device(pdev); if (rc) return rc;

	//! pci_set_master(pdev); // do we need this?

  	rc = pci_request_regions(pdev, DRIVER_NAME);
  	if (rc) { pci_dev_busy = 1; goto err_out; }
	
	have_msi = (pci_enable_msi(pdev) == 0)?1:0; // and this?
	if (!have_msi) pci_intx(pdev, 1);

	host = scsi_host_alloc(&tifmxx_host_template, sizeof(*fm));
	if (!host) 
	{
                rc = -ENOMEM;
		goto err_out_msi;
        }

	fm = (struct tifmxx_data*)host->hostdata;
	memset(fm, 0, sizeof(struct tifmxx_data));
	fm->dev = pdev;	
	fm->max_sockets = (pdev->device == 0x803B) ? 2 : 4;
	rwlock_init(&fm->lock);
	INIT_WORK(&fm->isr_bh, tifmxx_isr_bh, (void*)fm);

	if(!tifmxx_sock_data_init(fm)) 
	{
		tifmxx_sock_data_fini(fm);
		goto err_out_free_host;
	}

	host->max_id = fm->max_sockets;
	host->max_lun = 0;
	host->max_channel = 0;

	/* This device has only one memory region */
	mmio_base = ioremap_nocache(pci_resource_start(pdev, 0), 
				    pci_resource_len(pdev, 0));
	if (mmio_base == NULL) 
	{
                rc = -ENOMEM;
                goto err_out_free_host;
        }
	
	fm->mmio_base = mmio_base;
	pci_set_drvdata(pdev, host);

	writel(0xffffffff, mmio_base + 0x0c);
	writel(0x8000000f, mmio_base + 0x08);

	rc = scsi_add_host(host, &pdev->dev); if (rc) goto err_out_iounmap;
	rc = request_irq(pdev->irq, tifmxx_interrupt, SA_SHIRQ, DRIVER_NAME, fm); 
	if (rc) goto err_out_remove_host;			
	
	//! detect cards, add scsi devices

	return 0; 

err_out_remove_host:
	scsi_remove_host(host);
err_out_iounmap:
	iounmap(mmio_base);
err_out_free_host:
	scsi_host_put(host);
err_out_msi:
	if (have_msi)
		pci_disable_msi(pdev);
	else
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
	unsigned long f;
	
	free_irq(fm->dev->irq, fm); // no more interrupts

	//! wait for everything to complete
	write_lock_irqsave(&fm->lock, f);
	
	cancel_delayed_work(&fm->isr_bh); 

	tifmxx_sock_data_fini(fm);

	writel(0xffffffff, fm->mmio_base + 0x0c);
	
	write_unlock_irqrestore(&fm->lock, f);
	
	scsi_remove_host(host);	
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
  	return pci_module_init( &tifmxx_driver );
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
