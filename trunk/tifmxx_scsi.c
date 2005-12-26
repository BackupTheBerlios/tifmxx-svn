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

#include "tifmxx.h"

static int 
tifmxx_scsi_queue(struct scsi_cmnd *srb, void (*done)(struct scsi_cmnd*))
{
	struct tifmxx_data *fm = 
		(struct tifmxx_data*)srb->device->host->hostdata;
	struct tifmxx_sock_data *sock = 0;
	unsigned long f;
	int rc = 0;

	FM_DEBUG("%s called\n", __FUNCTION__);

	read_lock(&fm->lock);
	sock = (srb->device->id < fm->max_sockets) ? fm->sockets[srb->device->id] : 0;

	spin_lock_irqsave(&sock->lock, f);
	if(sock->flags & CARD_PRESENT)
	{
		if(sock->srb != NULL)
		{
			printk(KERN_ERR DRIVER_NAME ": SCSI queue full, target = %d\n", srb->device->id);
			rc = SCSI_MLQUEUE_DEVICE_BUSY;
		}
		else
		{
			srb->scsi_done = done;
			sock->srb = srb;
			//get the command running	
			schedule_work(&sock->do_scsi_cmd);
		}
	}
	else 
	{
		// bad target
		printk(KERN_ERR DRIVER_NAME ": Invalid target = %d\n", srb->device->id);
		srb->result = DID_BAD_TARGET << 16;
		done(srb);
	}

	spin_unlock_irqrestore(&sock->lock, f);
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
	.max_sectors             = 64, 
	.cmd_per_lun             = 1, 
	.emulated                = 1,
	.skip_settle_delay       = 1
//.sdev_attrs	
};

/* Mostly copied from scsi_debug/mk_sense_buffer */
static inline void
tifmxx_fill_sense_buffer(struct tifmxx_sock_data *sock, int key, int asc, int asq)
{
	unsigned char *sbuff = sock->srb->sense_buffer;

	memset(sbuff, 0, SCSI_SENSE_BUFFERSIZE);
	sbuff[0] = 0x70;  /* fixed, current */
	sbuff[2] = key;
	sbuff[7] = 0xa;   /* implies 18 byte sense buffer */
	sbuff[12] = asc;
	sbuff[13] = asq;
}

static void
tifmxx_eval_inquiry(struct tifmxx_sock_data *sock)
{
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	if(!(sock->flags & CARD_PRESENT))
	{
		// bad target
		printk(KERN_ERR DRIVER_NAME ": Target %d removed.\n", sock->sock_id);
		sock->srb->result = DID_ABORT << 16;
		sock->srb->done(sock->srb);
		sock->srb = 0;
	}
	else if(!(sock->flags & CARD_READY))
	{
		//! check card info
		
	}
	//! fill inquiry response
	spin_unlock_irqrestore(&sock->lock, f);
}

void
tifmxx_eval_scsi(void *data)
{
	struct tifmxx_sock_data *sock = (struct tifmxx_sock_data*)data;
	unsigned long f;

	read_lock(&((struct tifmxx_data*)sock->host->hostdata)->lock);
	spin_lock_irqsave(&sock->lock, f);
	switch(*(unsigned char*)sock->srb->cmnd)
	{
		case INQUIRY:
			spin_unlock_irqrestore(&sock->lock, f);
			tifmxx_eval_inquiry(sock);
			break;
		case REQUEST_SENSE:
		case SEND_DIAGNOSTIC:
		case TEST_UNIT_READY:
		default:
			FM_DEBUG("scsi opcode 0x%x not supported\n", *(unsigned char*)sock->srb->cmnd);
			tifmxx_fill_sense_buffer(sock, ILLEGAL_REQUEST, INVALID_OPCODE, 0);
			sock->srb->result = (DRIVER_SENSE << 24) | SAM_STAT_CHECK_CONDITION;
			sock->srb->done(sock->srb);
			sock->srb = 0;
			spin_unlock_irqrestore(&sock->lock, f);
	}
	read_unlock(&((struct tifmxx_data*)sock->host->hostdata)->lock);
}
