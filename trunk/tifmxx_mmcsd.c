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

static void
tifmxx_mmcsd_clean_up(struct tifmxx_sock_data *sock)
{
	struct scsi_device *s_dev;	

	cancel_delayed_work(&sock->do_scsi_cmd);
	flush_scheduled_work();

	if(sock->srb)
	{
		sock->srb->result = DID_ABORT << 16;
		sock->srb->scsi_done(sock->srb);
	}
	
	s_dev = scsi_device_lookup(sock->host, 0, sock->sock_id, 0);
	if(s_dev)
	{
		scsi_remove_device(s_dev);
		scsi_device_put(s_dev);
	}
	
	sock->clean_up = 0;
	sock->process_irq = 0;
	sock->signal_irq = 0;
	if(sock->mmcsd_p) kfree(sock->mmcsd_p);
	sock->flags &= 0xffff0000;
}

static void
tifmxx_mmcsd_process_irq(struct tifmxx_sock_data *sock)
{
	unsigned int rc = 0;

	if(!(sock->flags & INT_B1))
	{
		if(sock->r_var_2 & 0x4) { rc = 0x0200; sock->flags |= CARD_BUSY; }
		else wake_up_all(&sock->irq_ack); // signal event_2
		
		if(sock->r_var_2 & 0x4) rc |= 0x0100;
		sock->flags &= ~CARD_READY;
	}
	
	if(sock->flags & INT_B0)
	{
		if(sock->mmcsd_p->r_var_10 & 0x8)
		{
			writel(0x0014 | readl(sock->sock_addr + 0x118), sock->sock_addr + 0x118);
			sock->mmcsd_p->r_var_2 = 1;
		}
		if(sock->mmcsd_p->r_var_10 & 0x10) sock->mmcsd_p->r_var_2 = 0;
		if(sock->mmcsd_p->r_var_10 & 0x4) sock->mmcsd_p->r_var_2 = 1;
		sock->flags |= CARD_EVENT;
		wake_up_all(&sock->irq_ack);
		sock->flags &= ~INT_B0;
	}
}

static void
tifmxx_mmcsd_signal_irq(struct tifmxx_sock_data *sock, unsigned int card_irq_status)
{
	sock->flags &= ~(INT_B0 | INT_B1); sock->flags |= card_irq_status;
	
	if(!(sock->flags & INT_B1))
	{
		sock->r_var_2 = readl(sock->sock_addr + 0x20);
		writel(sock->r_var_2, sock->sock_addr + 0x20);
	}

	if(sock->flags & INT_B0)
	{
		sock->mmcsd_p->r_var_10 = readl(sock->sock_addr + 0x114);
		sock->mmcsd_p->r_var_11 = sock->mmcsd_p->r_var_10 | sock->mmcsd_p->r_var_5;
		writel(sock->mmcsd_p->r_var_10, sock->sock_addr + 0x114);
	}
}

static int
tifmxx_mmcsd_initialize(struct tifmxx_sock_data *sock, unsigned long *irq_flags)
{
	int rc;

	writel(0x0002, sock->sock_addr + 0x168);
	sock->mmcsd_p->clk_speed = 60;
	writel(0x000b, sock->sock_addr + 0x110);

	spin_unlock_irqrestore(&sock->lock, *irq_flags);
	msleep(3);
	spin_lock_irqsave(&sock->lock, *irq_flags);

	if(!(0x1 & readl(sock->sock_addr + 0x16c))) return 0x2f; //! failed

	writel(0x0000, sock->sock_addr + 0x12c);
	writel(sock->mmcsd_p->clk_speed | 0x0800, sock->sock_addr + 0x110);
	writel(0x8000, sock->sock_addr + 0x130);
	writel(0x41e9, sock->sock_addr + 0x118);
	writel(0x0020 | readl(sock->mmcsd_p->sock_addr + 0x138), sock->sock_addr + 0x138);
	writel(0x0040, sock->sock_addr + 0x11c);
	writel(0x07ff, sock->sock_addr + 0x120);
	writel(0x0080, sock->sock_addr + 0x104);
	writel(sock->clk_speed | 0x0800, sock->sock_addr + 0x110);

	spin_unlock_irqrestore(&sock->lock, *irq_flags);
	wait_event_timeout(&sock->irq_ack, (sock->flags & (CARD_EVENT | CARD_REMOVED)), msecs_to_jiffies(100));
	spin_lock_irqsave(&sock->lock, *irq_flags);

	sock->flags &= ~CARD_EVENT;
	if(sock->flags & CARD_REMOVED) return 0x86; //! card removed
	if(0 != (rc = tifmxx_mmcsd_detect_card_type(sock))) return rc;

	if(sock->media_id == 0x43 || (sock->media_id == 0x23 && (sock->flags & ALLOW_SD)) ||
	   (sock->media_id == 0x13 && (sock->flags & ALLOW_MMC)))
	{
		writel(0x0080, sock->sock_addr + 0x4);
		sock->media_id = 0x43;
		return 0x2e;
	}
	
	if(0 != (rc = tifmxx_mmcsd_standby(sock))) return rc;
	if(0 != (rc = tifmxx_mmcsd_read_csd_info(sock))) return rc;
	//...	
}

void
tifmxx_mmcsd_init(struct tifmxx_sock_data *sock)
{
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	
	if(0x8 & readl(sock->sock_addr + 0x8)) // card still inside
	{
		sock->mmcsd_p = kmalloc(sizeof(struct tifmxx_mmcsd_data), GFP_KERNEL);
		if(sock->mmcsd_p)
		{
			sock->clean_up = tifmxx_mmcsd_clean_up;
			sock->process_irq = tifmxx_mmcsd_process_irq;
			sock->signal_irq = tifmxx_mmcsd_signal_irq;
			
			writel(0, sock->sock_addr + 0x118);
			sock->flags |= CARD_PRESENT | CARD_RO;
			scsi_add_device(sock->host, 0, sock->sock_id, 0);
		}
		else FM_DEBUG("kmalloc failed in MMC/SD constructor\n");	
	}
	spin_unlock_irqrestore(&sock->lock, f);
}

