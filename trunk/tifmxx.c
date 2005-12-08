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
#include <linux/spinlock.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dbg.h>

#define DRIVER_NAME "tifmxx"
#define DRIVER_VERSION "0.1"

#define FM_DEBUG(x...) printk(KERN_DEBUG DRIVER_NAME ": " x)

/* There is some waiting associated with some socket operations 
 * (up to 100 msec in worst case). So it's better to have independent
 * tasklets for each socket.
 */
struct tifmxx_sock_data
{
	spinlock_t               *lock;
	char __iomem             *sock_addr;  // pointer to socket registers

	struct scsi_cmnd         *srb;        // current command

	unsigned                 sock_status; // current socket status
	struct tasklet_struct    work_thread;
};

struct tifmxx_data
{
	spinlock_t               *lock;
	struct pci_dev           *dev;	
	char __iomem             *mmio_base;

	unsigned                 max_sockets;
	struct tifmxx_sock_data  *sockets[4];
};

static struct pci_device_id tifmxx_pci_tbl [] = 
{
	{ PCI_VENDOR_ID_TI, 0x8033, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 
    	  0 }, /* xx21 - the one I have */
  	{ PCI_VENDOR_ID_TI, 0x803B, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 
    	  0 }, /* xx12 - should be also supported */
  	{ }
};

static irqreturn_t 
tifmxx_interrupt (int irq, void *dev_instance, struct pt_regs *regs)
{
  	FM_DEBUG("Got interrupt\n");
  	return IRQ_HANDLED;
}

static void
tifmxx_work_thread(unsigned long data)
{
	struct tifmxx_sock_data *sock = (struct tifmxx_sock_data*)data;	
  	FM_DEBUG("Work thread called\n");
}

static int 
tifmxx_scsi_queue(struct scsi_cmnd *srb, void (*done)(struct scsi_cmnd*))
{
	struct tifmxx_data *fm = 
		(struct tifmxx_data*)srb->device->host->hostdata;

	unsigned long f1, f2; 

	FM_DEBUG("%s called\n", __FUNCTION__);

	spin_lock_irqsave(fm->lock, f1);
	struct tifmxx_sock_data *c_socket = fm->sockets[srb->device->id];

	// check that the target exists at all
	if(c_socket == NULL)
	{
		// bad target
		printk(KERN_ERR DRIVER_NAME ": Invalid target = %d\n", 
		       srb->device->id);
		srb->result = DID_BAD_TARGET << 16;
		done(srb);
		spin_unlock_irqrestore(fm->lock, f1);
		return 0;
	}
	
	// check that the target has no pending commands
	
	spin_lock_irqsave(c_socket->lock, f2);	
	if(c_socket->srb != NULL)
	{
		printk(KERN_ERR DRIVER_NAME ": SCSI queue full, target = %d\n", 
		       srb->device->id);
		
		spin_unlock_irqrestore(c_socket->lock, f2);
		spin_unlock_irqrestore(fm->lock, f1);  
                return SCSI_MLQUEUE_DEVICE_BUSY;
	}
	
	srb->scsi_done = done;
	c_socket->srb = srb;
	//get the command running	
	tasklet_schedule(&csocket->work_thread);
	spin_unlock_irqrestore(c_socket->lock, f2);	
	spin_unlock_irqrestore(fm->lock, f1);  
	return 0;
}

static int 
tifmxx_scsi_eh_abort(struct scsi_cmnd *srb)
{
	FM_DEBUG("eh_abort called\n");
	return SUCCESS;
}

static int 
tifmxx_scsi_eh_device_reset(struct scsi_cmnd *srb)
{
	FM_DEBUG("eh_device_reset called\n");
	return SUCCESS;
}

static int 
tifmxx_scsi_eh_bus_reset(struct scsi_cmnd *srb)
{
	FM_DEBUG("eh_bus_reset called\n");
	return SUCCESS;
}

static int 
tifmxx_scsi_slave_config(struct scsi_device *sdev)
{
	FM_DEBUG("slave_config called\n");
	return 0;
}

static int 
tifmxx_scsi_info(struct Scsi_Host *host, char *buffer, char **start,
		 off_t offset, int length, int inout)
{
	FM_DEBUG("info called\n");
	return 0;
}

static const char* 
tifmxx_scsi_host_info(struct Scsi_Host *host)
{
	return "SCSI emulation for TI FlashMedia storage controller";
}

static struct scsi_host_template tifmxx_host_template =
{
	.module                  = THIS_MODULE,
	.name                    = "TI FlashMedia Controller",
	.proc_name               = "TI FlashMedia Controller",
	.proc_info               = tifmxx_scsi_info,
	.info                    = tifmxx_scsi_host_info,
	.queuecommand            = tifmxx_scsi_queue,
	.eh_abort_handler        = tifmxx_scsi_eh_abort,
	.eh_device_reset_handler = tifmxx_scsi_eh_device_reset,
	.eh_bus_reset_handler    = tifmxx_scsi_eh_bus_reset,
	.slave_configure         = tifmxx_scsi_slave_config,	
	.can_queue               = 1,
	.this_id                 = -1, 
	.sg_tablesize            = SG_ALL,
	.max_sectors             = 240, 
	.cmd_per_lun             = 1, 
	.emulated                = 1,
	.skip_settle_delay       = 1
//.sdev_attrs	
};

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
	spin_lock_init(fm->lock);	

	host->max_id = fm->max_sockets;
	host->max_lun = 0;
	host->max_channel = 0;

	/* This device has only one memory region */
	printk(KERN_INFO DRIVER_NAME": Trying to map %lx bytes from address %lx\n",
	       pci_resource_len(pdev, 0), pci_resource_start(pdev, 0));
	mmio_base = ioremap_nocache(pci_resource_start(pdev, 0), 
				    pci_resource_len(pdev, 0));
	if (mmio_base == NULL) 
	{
                rc = -ENOMEM;
                goto err_out_free_host;
        }
	
	fm->mmio_base = mmio_base;
	pci_set_drvdata(pdev, host);

        /* Cxx21::Initialize */
	writel(0xffffffff, mmio_base + 0x0c);
	writel(0x8000000f, mmio_base + 0x08);

	rc = scsi_add_host(host, &pdev->dev); if (rc) goto err_out_iounmap;
	rc = request_irq(pdev->irq, tifmxx_interrupt, SA_SHIRQ, "tifmxx", fm); 
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
  	return rc;
}

static void 
tifmxx_remove (struct pci_dev *pdev)
{
	struct Scsi_Host *host = pci_get_drvdata(pdev); 
	struct tifmxx_data *fm = (struct tifmxx_data*)host->hostdata;

	unsigned long f1, f2, cnt;
	
	free_irq(fm->dev->irq, fm); // no more interrupts
	spin_lock_irqsave(fm->lock, f1);
	for(cnt = 0; cnt < fm->max_sockets; cnt++)
	{
		struct tifmxx_sock_data *c_socket = fm->sockets[cnt];
		if(c_socket)
		{
			spin_lock_irqsave(c_socket, f2);
			fm->sockets[cnt] = 0;
			tasklet_kill(&c_socket->work_thread);
			//! tell things to hardware if needed
			if(c_socket->srb)
			{
				srb->result = DID_ABORT << 16;
				srb->scsi_done(srb);
			}
			//! may be call scsi_remove_device
			spin_unlock_irqrestore(c_socket, f2);
			kfree(c_socket);
		}		
	}	
	
	// reset hardware
	/* Cxx21::~Cxx21 */
	writel(0xffffffff, mmio_base + 0x0c);
	
	spin_unlock_irqrestore(fm->lock, f1);
	
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
tifmxx_init (void)
{
  	return pci_module_init( &tifmxx_driver );
}

static void __exit 
tifmxx_exit (void)
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
