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
		writel(cmd_arg >> 16, sock->sock_addr + 0x10c);
		writel(cmd_arg & 0xffff, sock->sock_addr + 0x108);
		sock->cmd_status = 0;
		sock->flags &= ~CARD_EVENT;
		writel((cmd & 0x3f) | (cmd_mask & 0xff80), sock->sock_addr + 0x104);
		spin_unlock_irqrestore(&sock->lock, f);
		if(0x20 != (rc = tifmxx_mmcsd_wait_for_eoc(sock))) break;
	}	
	return rc;
}

static unsigned int
tifmxx_mmcsd_extract_info_bitfld(struct tifmxx_sock_data *sock, unsigned int b_end, unsigned int b_start,
				 unsigned int card_reg)
{
	unsigned int w_end = b_end >> 4;
	unsigned int w_start = b_start >> 4;
	unsigned int rc;
	unsigned long f;

	card_reg = card_reg ? 0 : 6;
	
	if(b_end < b_start || b_end > 127 || b_end > b_start + 32) return 0;

	spin_lock_irqsave(&sock->lock, f);

	rc = (0xffff >> (15 - (b_end & 0xf))) & readl(sock->sock_addr + 0x144 + ((card_reg + w_end) << 2));

	if(w_end != w_start) 
	{
		if(w_end > w_start + 1)
		{
			rc <<= 16;
			rc |= readl(sock->sock_addr + 0x144 + ((card_reg + w_start + 1) << 2));
		}
		rc <<= 16 - (b_start & 0xf);
		rc |= (readl(sock->sock_addr + 0x144 + ((card_reg + w_start) << 2)) >> (b_start & 0xf));
	}

	spin_unlock_irqrestore(&sock->lock, f);

	return rc >> (b_start & 0xf);
}

static int
tifmxx_mmcsd_standby(struct tifmxx_sock_data *sock)
{
	int rc = 0, cnt, retflag_1 = 0, retflag_2 = 0, t_cmd = (sock->media_id == 0x23) ? 0x29 : 0x1;
	unsigned long f;
	u64 t = get_jiffies_64();

	while(get_jiffies_64() - t < msecs_to_jiffies(1000))
	{

		if(retflag_1 && sock->media_id == 0x23)
		{
			cnt = 0;
			while(1)
			{
				spin_lock_irqsave(&sock->lock, f);
				writel(0x0000, sock->sock_addr + 0x10c);
				writel(0x0000, sock->sock_addr + 0x108);
				sock->cmd_status = 0;
				sock->flags &= ~CARD_EVENT;
				writel(0x2137, sock->sock_addr + 0x104);
				spin_unlock_irqrestore(&sock->lock, f);
				if(0x20 != (rc = tifmxx_mmcsd_wait_for_eoc(sock)))
				{
					if(rc) return rc;
					break;
				}
				if(cnt >= 2 || sock->media_id == 0x3) return rc;
				cnt++;
			}
		}
		retflag_1 = 1;

		cnt = 0;
		while(1)
		{
			spin_lock_irqsave(&sock->lock, f);
			writel(0x80fc, sock->sock_addr + 0x10c);
			writel(0x0000, sock->sock_addr + 0x108);
			sock->cmd_status = 0;
			sock->flags &= ~CARD_EVENT;
			writel((t_cmd & 0x3f) | 0x1300, sock->sock_addr + 0x104);
			spin_unlock_irqrestore(&sock->lock, f);
			if(0x20 != (rc = tifmxx_mmcsd_wait_for_eoc(sock))) break;
			if(cnt >= 2 || sock->media_id == 3) return retflag_2 ? 0x28 : 0x27;	
			cnt++;
		}
		retflag_2 = 1;
		if(rc) return rc;
		if(tifmxx_mmcsd_extract_info_bitfld(sock, 0x1f, 0x1f, 0)) break;
	}
	if(!tifmxx_mmcsd_extract_info_bitfld(sock, 0x1f, 0x1f, 0)) return 0x2d;
	if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x2, 0, 0x1200))) return rc;
	if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x3, 0x20000, 0x1600))) return rc;

	if(sock->media_id == 0x13)
	{
		sock->mmcsd_p->rca = 0x20000;
		return 0;
	}
	if(sock->media_id == 0x23)
	{
		sock->mmcsd_p->rca = tifmxx_mmcsd_extract_info_bitfld(sock, 0x1f, 0, 0) & 0xffff0000;
		return sock->mmcsd_p->rca ? 0 : 0x2c;
	}
	return 0x83;
}

static int
tifmxx_mmcsd_read_serial_number(struct tifmxx_sock_data *sock)
{
	int cnt = 0, rc;	
	unsigned char l_mid;
	unsigned short l_oid;
	char l_pnm[7];
	unsigned char l_rev;
	unsigned int l_psn;	
	unsigned long f;

	do
	{
		spin_lock_irqsave(&sock->lock, f);
		writel(sock->mmcsd_p->rca >> 16, sock->sock_addr + 0x10c);
		writel(sock->mmcsd_p->rca & 0xffff, sock->sock_addr + 0x108);
		sock->cmd_status = 0;
		sock->flags &= ~CARD_EVENT;
		writel(0x220a, sock->sock_addr + 0x104);
		spin_unlock_irqrestore(&sock->lock, f);
		/* cid offset 0x78, size 1B - MID
		 *            0x68, size 2B - OID
		 *            0x38, size 6B - PNM (ASCII)
		 *            0x30, size 1B - revision
		 *            0x10, size 4B - PSN
		 */
		if(0x20 != (rc = tifmxx_mmcsd_wait_for_eoc(sock)))
		{
			if(rc) break;

			l_mid = (unsigned char)tifmxx_mmcsd_extract_info_bitfld(sock, 0x7f, 0x78, 1);
			l_oid = (unsigned short)tifmxx_mmcsd_extract_info_bitfld(sock, 0x77, 0x68, 1);
			for(cnt = 0; cnt < 6; cnt++)
				l_pnm[cnt] = tifmxx_mmcsd_extract_info_bitfld(sock, 0x67 - cnt, 0x60 - cnt, 1);
			l_pnm[6] = 0;
			l_rev = (unsigned char)tifmxx_mmcsd_extract_info_bitfld(sock, 0x37, 0x30, 1);
			l_psn = tifmxx_mmcsd_extract_info_bitfld(sock, 0x2f, 0x10, 1);
			
			spin_lock_irqsave(&sock->lock, f);
			sock->mmcsd_p->mid = l_mid;
			sock->mmcsd_p->oid = l_oid;
			memcpy(sock->mmcsd_p->pnm, l_pnm, 7);
			sock->mmcsd_p->rev = l_rev;
			sock->mmcsd_p->psn = l_psn;
			spin_unlock_irqrestore(&sock->lock, f);
			return 0;
		}

		cnt++;
	}while(sock->media_id != 3 || cnt < 3);	

	return rc;
}

static int
tifmxx_mmcsd_detect_card_type(struct tifmxx_sock_data *sock)
{
	int cnt_1, cnt_2, rc;
	unsigned long f;

	for(cnt_1 = 0; cnt_1 < 3; cnt_1++)
	{
		spin_lock_irqsave(&sock->lock, f);
		writel(0x0000, sock->sock_addr + 0x10c);
		writel(0x0000, sock->sock_addr + 0x108);
		sock->cmd_status = 0;
		sock->flags &= ~CARD_EVENT;
		writel(0x1305, sock->sock_addr + 0x104);
		spin_unlock_irqrestore(&sock->lock, f);
		if(0x20 != (rc = tifmxx_mmcsd_wait_for_eoc(sock)))
		{
			if(!rc)
			{
				spin_lock_irqsave(&sock->lock, f);
				sock->media_id = 0x43;
				spin_unlock_irqrestore(&sock->lock, f);
				return 0;
			}
		}
		if(sock->media_id == 0x3) break;
	}

	for(cnt_2 = 0; cnt_2 < 3; cnt_2++)
	{
		spin_lock_irqsave(&sock->lock, f);
		writel(0x0080, sock->sock_addr + 0x104);
		spin_unlock_irqrestore(&sock->lock, f);

		wait_event_timeout(sock->irq_ack, (sock->flags & (CARD_EVENT | CARD_REMOVED)), msecs_to_jiffies(100));
		
		for(cnt_1 = 0; cnt_1 < 3; cnt_1++)
		{
			spin_lock_irqsave(&sock->lock, f);
			writel(0x0000, sock->sock_addr + 0x10c);
			writel(0x0000, sock->sock_addr + 0x108);
			sock->cmd_status = 0;
			sock->flags &= ~CARD_EVENT;
			writel(0x0000, sock->sock_addr + 0x104);
			spin_unlock_irqrestore(&sock->lock, f);

			if(0x20 != (rc = tifmxx_mmcsd_wait_for_eoc(sock)))
			{
				if(!rc)
				{
					rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x37, 0, 0x2100);
					if(rc == 0x20)
					{
						tifmxx_mmcsd_exec_card_cmd(sock, 0, 0, 0);
						spin_lock_irqsave(&sock->lock, f);
						sock->media_id = 0x13;
						spin_unlock_irqrestore(&sock->lock, f);
						return 0;
					}

					if(rc) return rc;

					spin_lock_irqsave(&sock->lock, f);
					sock->media_id = 0x23;
					spin_unlock_irqrestore(&sock->lock, f);
					return 0;
				}
			}
			if(sock->media_id == 0x3) break;
		}
	}
}

static int
tifmxx_mmcsd_init_card(struct tifmxx_sock_data *sock)
{
	int rc = 0x2f, cnt;
	struct tifmxx_mmcsd_ecmd cmd_rq;
	char cmd_rp_1[16], cmd_rp_2[16];
	int dma_page_cnt, card_state;
	unsigned int l_read_blen, l_rca;
	unsigned long f;
	u64 t;

	spin_lock_irqsave(&sock->lock, f);
	writel(0x0002, sock->sock_addr + 0x168);
	sock->mmcsd_p->clk_speed = 60;
	writel(0x000b, sock->sock_addr + 0x110);
	spin_unlock_irqrestore(&sock->lock, f);

	t=get_jiffies_64();
	while(get_jiffies_64() - t < msecs_to_jiffies(250))
	{
		if(0x1 & readl(sock->sock_addr + 0x16c))
		{
			rc = 0;
			break;
		}
		msleep(10);
	};
	if(rc) return rc;

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

