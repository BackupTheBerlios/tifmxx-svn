#include "tifmxx.h"

static int 
tifmxx_scsi_queue(struct scsi_cmnd *srb, void (*done)(struct scsi_cmnd*))
{
	struct tifmxx_data *fm = 
		(struct tifmxx_data*)srb->device->host->hostdata;
	struct tifmxx_sock_data *c_socket = 0;
	unsigned long f;
	int rc = 0, no_target = 0;

	FM_DEBUG("%s called\n", __FUNCTION__);

	read_lock(&fm->lock);
	c_socket = (srb->device->id < fm->max_sockets) ? fm->sockets[srb->device->id] : 0;

	// check that the target exists at all
	if(c_socket)
	{
		write_lock_irqsave(&c_socket->lock, f);
		if(c_socket->flags & CARD_PRESENT)
		{
			if(c_socket->srb != NULL)
			{
				printk(KERN_ERR DRIVER_NAME ": SCSI queue full, target = %d\n", srb->device->id);
				rc = SCSI_MLQUEUE_DEVICE_BUSY;
			}
			else
			{
				srb->scsi_done = done;
				c_socket->srb = srb;
				//get the command running	
				schedule_work(&c_socket->do_scsi_cmd);
			}
		}
		else no_target = 1;
		write_unlock_irqrestore(&c_socket->lock, f);
	}
	else no_target = 1;

	if(no_target)
	{
		// bad target
		printk(KERN_ERR DRIVER_NAME ": Invalid target = %d\n", srb->device->id);
		srb->result = DID_BAD_TARGET << 16;
		done(srb);
	}
	
	read_unlock(&fm->lock);
	return rc;
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

struct scsi_host_template tifmxx_host_template =
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
