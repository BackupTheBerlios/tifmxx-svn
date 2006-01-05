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
	unsigned long f;
	
	spin_lock_irqsave(&sock->lock, f);
	writel(0xfff8 & readl(sock->sock_addr + 0x4), sock->sock_addr + 0x4);
	writel(0, sock->sock_addr + 0x118);
	sock->flags |= CARD_REMOVED;
	wake_up_all(&sock->irq_ack);	
	
	sock->clean_up = 0;
	sock->process_irq = 0;
	sock->signal_irq = 0;
	if(sock->mmcsd_p) kfree(sock->mmcsd_p);
	sock->flags &= (~CARD_PRESENT);
	spin_unlock_irqrestore(&sock->lock, f);
}

static void
tifmxx_mmcsd_process_irq(struct tifmxx_sock_data *sock)
{
	unsigned int rc = 0;

	if(!(sock->flags & INT_B1))
	{
		if(sock->r_var_2 & 0x4) { rc = 0x0200; sock->flags |= CARD_BUSY; }
		else 
		{
			sock->flags |= SOCK_EVENT;
			wake_up_all(&sock->irq_ack); // signal event_2
		}
		
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

/* This function deserves to be streamlined a bit */
static int
tifmxx_mmcsd_wait_for_eoc(struct tifmxx_sock_data *sock)
{
	unsigned long f;
	int rc = 0;
	
	spin_lock_irqsave(&sock->lock, f);
	if(sock->cmd_status & 0x1)
	{
		spin_unlock_irqrestore(&sock->lock, f);
		return 0;
	}

	do
	{
		if(rc) break;
		if(sock->flags & CARD_REMOVED)
		{
			spin_unlock_irqrestore(&sock->lock, f);
			return 0x86;
		}

		if(sock->cmd_status & 0x4000) rc = 0x2a; // ???
		if(sock->cmd_status & 0x0080) rc = 0x20; // timeout
		if(sock->cmd_status & 0x0100) rc = 0x21; // crc error

		if(!rc)
		{
			spin_unlock_irqrestore(&sock->lock, f);

			if(!wait_event_timeout(sock->irq_ack, (sock->flags & (CARD_EVENT | CARD_REMOVED)),
					       msecs_to_jiffies(100))) rc = 0x87;

			spin_lock_irqsave(&sock->lock, f);
			sock->flags &= ~CARD_EVENT;
			if(sock->flags & CARD_REMOVED)
			{
				spin_unlock_irqrestore(&sock->lock, f);
				return 0x86;
			}

			if(sock->cmd_status & 0x4000) rc = 0x2a; // ???
			if(sock->cmd_status & 0x0080) rc = 0x20; // timeout
			if(sock->cmd_status & 0x0100) rc = 0x21; // crc error
			if(rc)
			{
				spin_unlock_irqrestore(&sock->lock, f);
				return rc;
			}

		}
	}while(!(sock->cmd_status & 0x1));
	spin_unlock_irqrestore(&sock->lock, f);

	if(rc == 0x87 && (0x1 & readl(sock->sock_addr + 0x114))) return 0;
	return rc;	
}

static int
tifmxx_mmcsd_exec_card_cmd(struct tifmxx_sock_data *sock, unsigned int cmd, unsigned int cmd_arg, 
			   unsigned int cmd_mask)
{
	unsigned long f;
	int rc, cnt;

	for(cnt = 0; cnt < 3; cnt++)
	{
		spin_lock_irqsave(&sock->lock, f);
		writel((cmd_arg >> 16) & 0xffff, sock->sock_addr + 0x10c);
		writel(cmd_arg & 0xffff, sock->sock_addr + 0x108);
		sock->cmd_status = 0;
		sock->flags &= ~CARD_EVENT;
		writel((cmd & 0x3f) | (cmd_mask & 0xff80), sock->sock_addr + 0x104);
		spin_unlock_irqrestore(&sock->lock, f);
		if(0x20 != (rc = tifmxx_mmcsd_wait_for_eoc(sock))) break;
	}	
	return rc;
}

static int
tifmxx_mmcsd_init_card(struct tifmxx_sock_data *sock)
{
	int rc, cnt;
	struct tifmxx_mmcsd_ecmd cmd_rq;
	char cmd_rp_1[16], cmd_rp_2[16];
	int dma_page_cnt, card_state;
	unsigned int l_read_blen, l_rca;
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	writel(0x0002, sock->sock_addr + 0x168);
	sock->mmcsd_p->clk_speed = 60;
	writel(0x000b, sock->sock_addr + 0x110);
	spin_unlock_irqrestore(&sock->lock, f);

	msleep(3);

	if(!(0x1 & readl(sock->sock_addr + 0x16c))) return 0x2f; //! failed

	spin_lock_irqsave(&sock->lock, f);
	writel(0x0000, sock->sock_addr + 0x12c);
	writel(sock->mmcsd_p->clk_speed | 0x0800, sock->sock_addr + 0x110);
	writel(0x8000, sock->sock_addr + 0x130);
	writel(0x41e9, sock->sock_addr + 0x118);
	writel(0x0020 | readl(sock->sock_addr + 0x138), sock->sock_addr + 0x138);
	writel(0x0040, sock->sock_addr + 0x11c);
	writel(0x07ff, sock->sock_addr + 0x120);
	writel(0x0080, sock->sock_addr + 0x104);
	writel(sock->mmcsd_p->clk_speed | 0x0800, sock->sock_addr + 0x110);
	spin_unlock_irqrestore(&sock->lock, f);

	wait_event_timeout(sock->irq_ack, (sock->flags & (CARD_EVENT | CARD_REMOVED)), msecs_to_jiffies(100));
	sock->flags &= ~CARD_EVENT;

	if(sock->flags & CARD_REMOVED) return 0x86; //! card removed

	if(0 != (rc = tifmxx_mmcsd_detect_card_type(sock))) return rc;

	spin_lock_irqsave(&sock->lock, f);
	if(sock->media_id == 0x43 || (sock->media_id == 0x23 && (sock->flags & ALLOW_SD)) ||
	   (sock->media_id == 0x13 && (sock->flags & ALLOW_MMC)))
	{
		writel(0x0080, sock->sock_addr + 0x4);
		sock->media_id = 0x43;
		spin_unlock_irqrestore(&sock->lock, f);
		return 0x2e;
	}
	spin_unlock_irqrestore(&sock->lock, f);

	if(0 != (rc = tifmxx_mmcsd_standby(sock))) return rc;
	if(0 != (rc = tifmxx_mmcsd_read_csd_info(sock))) return rc;

	spin_lock_irqsave(&sock->lock, f);
	writel(sock->mmcsd_p->clk_speed | (0xffc0 & readl(sock->sock_addr + 0x110)), sock->sock_addr + 0x110);
	l_rca = sock->mmcsd_p->rca;
	l_read_blen = sock->mmcsd_p->read_blen;
	spin_unlock_irqrestore(&sock->lock, f);

	if(0 != (rc = tifmxx_mmcsd_read_serial_number(sock))) return rc;	
	if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x0007, l_rca, 0x2900))) return rc;
	if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x0010, l_read_blen, 0x2100))) return rc;

	spin_lock_irqsave(&sock->lock, f);
	writel(sock->mmcsd_p->read_blen - 1, sock->sock_addr + 0x128);

	if(sock->media_id == 0x23)
	{
		writel(0x0100 | readl(sock->sock_addr + 0x4), sock->sock_addr + 0x4);
		sock->clk_freq = 24000000;
		spin_unlock_irqrestore(&sock->lock, f);
		if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x0037, l_rca, 0x2100))) return rc;
		if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x002a, 0, 0x2100))) return rc;
		
		dma_page_cnt = 0;
		cmd_rq = ((struct tifmxx_mmcsd_ecmd){0x000d, 0x0000, 0x0040, 0x0001, CMD_DIR | CMD_APP});
		if(0 != (rc = tifmxx_mmcsd_execute(&cmd_rq, 0, &dma_page_cnt, cmd_rp_1))) return rc;
		if(0xff00 & ((cmd_rp_1[3] << 8) + cmd_rp_1[4])) return 0x27;
		
		dma_page_cnt = 0;
		cmd_rq = ((struct tifmxx_mmcsd_ecmd){0x0033, 0x0000, 0x0008, 0x0001, CMD_DIR | CMD_APP});
		if(0 != (rc = tifmxx_mmcsd_execute(&cmd_rq, 0, &dma_page_cnt, cmd_rp_2))) return rc;
		if(0x4 & (0xf & cmd_rp_2[1]))
		{
			if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x0037, l_rca, 0x2100))) return rc;
			if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x0006, 0x0002, 0x2100))) return rc;
			spin_lock_irqsave(&sock->lock, f);
			sock->flags |= (0x0200 & readl(sock->sock_addr + 0x8)) ? CARD_RO : 0;
			spin_unlock_irqrestore(&sock->lock, f);
		}
		spin_lock_irqsave(&sock->lock, f);
	}
	
	// why do they need so many retries?
	for(cnt = 0; cnt < 10000; cnt++)
        {
		spin_unlock_irqrestore(&sock->lock, f);
		rc = tifmxx_mmcsd_get_state(sock, &card_state, 0);
		spin_lock_irqsave(&sock->lock, f);

                if(rc) break;
                if(card_state != 0x4) continue;
                if(sock->mmcsd_p->r_var_14 == 1) break;
        }

	sock->flags |= CARD_READY;
	spin_unlock_irqrestore(&sock->lock, f);
        return rc;
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
			sock->init_card = tifmxx_mmcsd_init_card;
			
			writel(0, sock->sock_addr + 0x118);
			sock->flags &= 0xffff0000; // kill volatile flags
			sock->flags |= CARD_PRESENT | CARD_RO;
			sock->clk_freq = 20000000;
			scsi_add_device(sock->host, 0, sock->sock_id, 0);
		}
		else FM_DEBUG("kmalloc failed in MMC/SD constructor\n");	
	}
	spin_unlock_irqrestore(&sock->lock, f);
}

