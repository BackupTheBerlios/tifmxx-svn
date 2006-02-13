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

unsigned int
tifmxx_mmcsd_get_rca(struct tifmxx_sock_data *sock)
{
	unsigned int rc;
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	rc = sock->mmcsd_p->rca;
	spin_unlock_irqrestore(&sock->lock, f);
	return rc;
}

void
tifmxx_mmcsd_set_rca(struct tifmxx_sock_data *sock, unsigned int rca)
{
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	sock->mmcsd_p->rca = rca;
	spin_unlock_irqrestore(&sock->lock, f);
}

size_t
tifmxx_mmcsd_get_size(struct tifmxx_sock_data *sock)
{
	size_t rc;
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	rc = sock->mmcsd_p->size;
	spin_unlock_irqrestore(&sock->lock, f);
	return rc;
}

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
		if(sock->sock_status & 0x4) { rc = 0x0200; sock->flags |= CARD_BUSY; }
		else 
		{
			sock->flags |= SOCK_EVENT;
			wake_up_all(&sock->irq_ack); // signal event_2
		}
		
		if(sock->sock_status & 0x4) rc |= 0x0100;
		sock->flags &= ~CARD_ACTIVE;
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
		sock->sock_status = readl(sock->sock_addr + 0x20);
		writel(sock->sock_status, sock->sock_addr + 0x20);
	}

	if(sock->flags & INT_B0)
	{
		sock->mmcsd_p->r_var_10 = readl(sock->sock_addr + 0x114);
		sock->mmcsd_p->r_var_11 = sock->mmcsd_p->r_var_10 | sock->mmcsd_p->cmd_status;
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
	if(sock->mmcsd_p->cmd_status & 0x1)
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

		if(sock->mmcsd_p->cmd_status & 0x4000) rc = 0x2a; // ???
		if(sock->mmcsd_p->cmd_status & 0x0080) rc = 0x20; // timeout
		if(sock->mmcsd_p->cmd_status & 0x0100) rc = 0x21; // crc error

		if(!rc)
		{
			spin_unlock_irqrestore(&sock->lock, f);

			if(!wait_event_timeout(sock->irq_ack, tifmxx_test_flag(sock, CARD_EVENT | CARD_REMOVED),
					       msecs_to_jiffies(100))) rc = 0x87;
			tifmxx_clear_flag(sock, CARD_EVENT);

			spin_lock_irqsave(&sock->lock, f);
			sock->flags &= ~CARD_EVENT;
			if(sock->flags & CARD_REMOVED)
			{
				spin_unlock_irqrestore(&sock->lock, f);
				return 0x86;
			}

			if(sock->mmcsd_p->cmd_status & 0x4000) rc = 0x2a; // ???
			if(sock->mmcsd_p->cmd_status & 0x0080) rc = 0x20; // timeout
			if(sock->mmcsd_p->cmd_status & 0x0100) rc = 0x21; // crc error
			if(rc)
			{
				spin_unlock_irqrestore(&sock->lock, f);
				return rc;
			}

		}
	}while(!(sock->mmcsd_p->cmd_status & 0x1));

	if(rc == 0x87 && (0x1 & readl(sock->sock_addr + 0x114))) rc = 0;

	spin_unlock_irqrestore(&sock->lock, f);

	return rc;	
}

/* Lowest 6 bits of cmd are the actual SD command */
static int
tifmxx_mmcsd_exec_card_cmd(struct tifmxx_sock_data *sock, unsigned int cmd, unsigned int cmd_arg)
{
	unsigned long f;
	int rc, cnt = 0;

	do
	{
		spin_lock_irqsave(&sock->lock, f);
		writel(cmd_arg >> 16, sock->sock_addr + 0x10c);
		writel(cmd_arg & 0xffff, sock->sock_addr + 0x108);
		sock->mmcsd_p->cmd_status = 0;
		sock->flags &= ~CARD_EVENT;
		writel(cmd, sock->sock_addr + 0x104);
		spin_unlock_irqrestore(&sock->lock, f);
		if(0x20 != (rc = tifmxx_mmcsd_wait_for_eoc(sock))) break;
		cnt++;
	}while(cnt < 3 || tifmxx_get_media_id(sock) !=3);
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
tifmxx_mmcsd_read_csd(struct tifmxx_sock_data *sock)
{
	unsigned short l_csd[8];
	int cnt, rc;
	const unsigned int rate_exp[4] = {10000, 100000, 1000000, 10000000};
	const unsigned int rate_mantissa[16] = {0, 10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80};
	unsigned int trans_speed;
	unsigned long f;
	
	rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x2209, tifmxx_mmcsd_get_rca(sock));

	for(cnt = 0; cnt < 8; cnt++)
		l_csd[cnt] = tifmxx_mmcsd_extract_info_bitfld(sock, (cnt << 4) + 15, cnt << 4, 1);

	if(rc == 0x21) /* CRC error */
	{
		if(tifmxx_get_media_id(sock) != 0x13) return rc;

		rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x2209, tifmxx_mmcsd_get_rca(sock));
		
		if(rc == 0x21)
		{
			for(cnt = 0; cnt < 8; cnt++)
				if(l_csd[cnt] != tifmxx_mmcsd_extract_info_bitfld(sock, (cnt << 4) + 15, cnt << 4, 1))
					return rc;

		}
		else if(rc) return rc;
	}
	else if(rc) return rc;

	spin_lock_irqsave(&sock->lock, f);
	sock->mmcsd_p->read_time_out = ((l_csd[7] & 0xff) + (l_csd[6] >> 0x8)) * 0xa;
	sock->mmcsd_p->write_time_out = sock->mmcsd_p->read_time_out * ((l_csd[1] >> 0xa) & 0x7);
	sock->mmcsd_p->read_block_len = 1 << (l_csd[5] & 0xf);
	sock->mmcsd_p->log_read_block_len = l_csd[5] & 0xf;
	sock->mmcsd_p->write_block_len = 1 << ((l_csd[1] >> 6) & 0xf);
	sock->mmcsd_p->log_write_block_len = (l_csd[1] >> 6) & 0xf;
	trans_speed = 10000;
	if((l_csd[6] & 0x7) < 4) trans_speed = rate_exp[l_csd[6] & 0x7];
	trans_speed *= rate_mantissa[(l_csd[6] >> 0x3) & 0xf];
	sock->mmcsd_p->clk_speed = sock->clk_freq / trans_speed;
	if(sock->mmcsd_p->clk_speed * trans_speed < sock->clk_freq) sock->mmcsd_p->clk_speed++;
	if(!sock->mmcsd_p->clk_speed) sock->mmcsd_p->clk_speed = 1;
	sock->mmcsd_p->blocks = (((l_csd[4] & 0x3ff) << 0x2) | (l_csd[3] >> 0xe)) + 1;
	sock->mmcsd_p->blocks *= (1 << ((((l_csd[3] & 0x3) << 0x1) | (l_csd[2] >> 0xf)) + 0x2));
	sock->mmcsd_p->size = sock->mmcsd_p->blocks << (l_csd[5] & 0xf);
	if(l_csd[0] & 0xd) sock->flags |= CARD_RO;
	else sock->flags &= ~CARD_RO;
	spin_unlock_irqrestore(&sock->lock, f);
	return 0;
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

	rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x220a, tifmxx_mmcsd_get_rca(sock));
	if(!rc)
	{
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
	}
	return rc;
}

static int
tifmxx_mmcsd_get_state(struct tifmxx_sock_data *sock, unsigned int *card_state)
{
	int rc;
	size_t mb_size = tifmxx_mmcsd_get_size(sock) >> 20;

	rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x210d, tifmxx_mmcsd_get_rca(sock));
	*card_state = tifmxx_mmcsd_extract_info_bitfld(sock, 0xc, 0x9, 0);
	if(tifmxx_mmcsd_extract_info_bitfld(sock, 0x8, 0x8, 0)) tifmxx_set_flag(sock, CARD_READY);
	else tifmxx_clear_flag(sock, CARD_READY);

	if(tifmxx_get_media_id(sock) == 0x13 && mb_size >= 2 && mb_size < 16) msleep(10);
	else if(!tifmxx_test_flag(sock, CARD_READY)) udelay(50); // what's this supposed to do?

	return rc;
}

static int
tifmxx_mmcsd_standby(struct tifmxx_sock_data *sock)
{
	int rc = 0, retflag_1 = 0, retflag_2 = 0, t_cmd = (tifmxx_get_media_id(sock) == 0x23) ? 0x29 : 0x1;
	u64 t = get_jiffies_64();

	while(get_jiffies_64() - t < msecs_to_jiffies(1000))
	{

		if(retflag_1 && tifmxx_get_media_id(sock) == 0x23)
			if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x2137, 0))) return rc;

		retflag_1 = 1;

		rc = tifmxx_mmcsd_exec_card_cmd(sock, t_cmd | 0x1300, 0x80fc0000);
		if(rc == 0x20) return retflag_2 ? 0x28 : 0x27;
		else if(rc) return rc;
	
		retflag_2 = 1;	
		if(tifmxx_mmcsd_extract_info_bitfld(sock, 0x1f, 0x1f, 0)) break;
	}

	if(!tifmxx_mmcsd_extract_info_bitfld(sock, 0x1f, 0x1f, 0)) return 0x2d;
	if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x1202, 0))) return rc;
	if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x1603, 0x20000))) return rc;

	if(tifmxx_get_media_id(sock) == 0x13)
	{
		tifmxx_mmcsd_set_rca(sock, 0x20000);
		return 0;
	}

	if(tifmxx_get_media_id(sock) == 0x23)
	{
		tifmxx_mmcsd_set_rca(sock, tifmxx_mmcsd_extract_info_bitfld(sock, 0x1f, 0, 0) & 0xffff0000);
		return tifmxx_mmcsd_get_rca(sock) ? 0 : 0x2c;
	}

	return 0x83;
}

static int
tifmxx_mmcsd_detect_card_type(struct tifmxx_sock_data *sock)
{
	int cnt, rc;
	unsigned long f;

	if(0 == (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x1305, 0)))
	{
		tifmxx_set_media_id(sock, 0x43);
		return 0;
	}
	
	for(cnt = 0; cnt < 3; cnt++)
	{
		spin_lock_irqsave(&sock->lock, f);
		writel(0x0080, sock->sock_addr + 0x104);
		spin_unlock_irqrestore(&sock->lock, f);

		wait_event_timeout(sock->irq_ack, tifmxx_test_flag(sock, CARD_EVENT | CARD_REMOVED),
				   msecs_to_jiffies(100));
		tifmxx_clear_flag(sock, CARD_EVENT);

		if(0 == (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0, 0)))
		{
			rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x2137, 0);
			if(rc == 0x20)
			{
				tifmxx_mmcsd_exec_card_cmd(sock, 0, 0);
				tifmxx_set_media_id(sock, 0x13);
				return 0;
			}
			else if(rc) return rc;

			tifmxx_set_media_id(sock, 0x23);
			return 0;
		}
		
	}
}

static int
tifmxx_mmcsd_wait_for_card(struct tifmxx_sock_data *sock)
{
	int rc = 0;
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	if(sock->mmcsd_p->r_var_2)
	{
		do
		{
			if(rc) break;
			if(sock->flags & CARD_REMOVED) { rc = 0x86; break; }
			spin_unlock_irqrestore(&sock->lock, f);
			if(!wait_event_timeout(sock->irq_ack, tifmxx_test_flag(sock, CARD_EVENT | CARD_REMOVED),
					       msecs_to_jiffies(1000))) rc = 0x87;
			spin_lock_irqsave(&sock->lock, f);
			sock->flags &= ~CARD_EVENT;
			
		}while(sock->mmcsd_p->r_var_2);
		if(rc == 0x87) rc = 0;
	}

	if(!rc)
	{
		writel(readl(sock->sock_addr + 0x118) & 0xffffffeb, sock->sock_addr + 0x118);		
	}
	spin_unlock_irqrestore(&sock->lock, f);
	return rc;
}

static int
tifmxx_mmcsd_wait_for_brs(struct tifmxx_sock_data *sock)
{
	int rc = 0;
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	if(!sock->mmcsd_p->cmd_status & 0x8)
	{
		do
		{
			if(rc) break;
			if(sock->flags & CARD_REMOVED) { rc = 0x86; break; }

			if(sock->mmcsd_p->cmd_status & 0x4000) rc = 0x2a; // ???
			if(sock->mmcsd_p->cmd_status & 0x0080) rc = 0x20; // timeout
			if(sock->mmcsd_p->cmd_status & 0x0100) rc = 0x21; // crc
			
			if(!rc)
			{
				spin_unlock_irqrestore(&sock->lock, f);
				if(!wait_event_timeout(sock->irq_ack, tifmxx_test_flag(sock, CARD_EVENT | CARD_REMOVED),
						       msecs_to_jiffies(2000))) rc = 0x87;
				spin_lock_irqsave(&sock->lock, f);
				sock->flags &= ~CARD_EVENT;
				if(sock->flags & CARD_REMOVED) { rc = 0x86; break; }

				if(sock->mmcsd_p->cmd_status & 0x4000) rc = 0x2a; // ???
				if(sock->mmcsd_p->cmd_status & 0x0080) rc = 0x20; // timeout
				if(sock->mmcsd_p->cmd_status & 0x0100) rc = 0x21; // crc
				if(rc) break;		
				
			}
		}while(!sock->mmcsd_p->cmd_status & 0x8);

		if(rc == 0x87)
		{
			if(0x8 & readl(sock->sock_addr + 0x114)) rc = 0;
		}
	}
	spin_unlock_irqrestore(&sock->lock, f);
	return rc;
}

static int
tifmxx_mmcsd_wait_for_ae(struct tifmxx_sock_data *sock)
{
	int rc = 0;
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	if(!sock->mmcsd_p->cmd_status & 0x800)
	{
		do
		{
			if(rc) break;
			if(sock->flags & CARD_REMOVED) { rc = 0x86; break; }

			if(sock->mmcsd_p->cmd_status & 0x4000) rc = 0x2a; // ???
			if(sock->mmcsd_p->cmd_status & 0x0080) rc = 0x20; // timeout
			if(sock->mmcsd_p->cmd_status & 0x0100) rc = 0x21; // crc
			
			if(!rc)
			{
				spin_unlock_irqrestore(&sock->lock, f);
				if(!wait_event_timeout(sock->irq_ack, tifmxx_test_flag(sock, CARD_EVENT | CARD_REMOVED),
						       msecs_to_jiffies(1))) rc = 0x87;
				spin_lock_irqsave(&sock->lock, f);
				sock->flags &= ~CARD_EVENT;
			}
		}while(!sock->mmcsd_p->cmd_status & 0x800);

		if(rc == 0x87) rc = 0;		
	}
	spin_unlock_irqrestore(&sock->lock, f);
	return rc;
}

static int
tifmxx_mmcsd_wait_for_af(struct tifmxx_sock_data *sock)
{
	int rc = 0;
	unsigned long f;

	spin_lock_irqsave(&sock->lock, f);
	if(!sock->mmcsd_p->cmd_status & 0x400)
	{
		do
		{
			if(rc) break;
			if(sock->flags & CARD_REMOVED) { rc = 0x86; break; }

			if(sock->mmcsd_p->cmd_status & 0x4000) rc = 0x2a; // ???
			if(sock->mmcsd_p->cmd_status & 0x0080) rc = 0x20; // timeout
			if(sock->mmcsd_p->cmd_status & 0x0100) rc = 0x21; // crc
			
			if(!rc)
			{
				spin_unlock_irqrestore(&sock->lock, f);
				if(!wait_event_timeout(sock->irq_ack, tifmxx_test_flag(sock, CARD_EVENT | CARD_REMOVED),
						       msecs_to_jiffies(1))) rc = 0x87;
				spin_lock_irqsave(&sock->lock, f);
				sock->flags &= ~CARD_EVENT;
			}
		}while(!sock->mmcsd_p->cmd_status & 0x400);

		if(rc == 0x87) rc = 0;		
	}
	spin_unlock_irqrestore(&sock->lock, f);
	return rc;
}

inline unsigned int
tifmxx_mmcsd_cmd_mask(struct tifmxx_mmcsd_ecmd *cmd)
{
	const unsigned int base_cmd_mask[] = {0x0000, 0x0000, 0x1000, 0x1000, 0x0000, 0x0000, 0x0000, 0x2000,
					      0x0000, 0x2000, 0x2000, 0x0000, 0x2000, 0x2000, 0x0000, 0x2000,
					      0x2000, 0x3000, 0x3000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
					      0x3000, 0x3000, 0x0000, 0x3000, 0x2000, 0x2000, 0x3000, 0x0000,
					      0x2000, 0x2000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2000, 0x0000,
					      0x0000, 0x0000, 0x3000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
					      0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2000,
					      0x3000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
	const unsigned int escaped_cmd_mask[] = {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2000, 0x0000,
						 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x3000, 0x0000, 0x0000,
						 0x0000, 0x0000, 0x3000, 0x0000, 0x0000, 0x0000, 0x3000, 0x2000,
						 0x0000, 0x3000, 0x3000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
						 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
						 0x0000, 0x0000, 0x0000, 0x3000, 0x3000, 0x3000, 0x3000, 0x3000,
						 0x3000, 0x2000, 0x0000, 0x3000, 0x0000, 0x0000, 0x0000, 0x0000,
						 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
	
	if(cmd->cmd_index > 63) return 0;
	return (cmd->flags & CMD_APP) ? escaped_cmd_mask[cmd->cmd_index] : base_cmd_mask[cmd->cmd_index];
}

static int
tifmxx_mmcsd_execute(struct tifmxx_sock_data *sock, struct tifmxx_mmcsd_ecmd *cmd, u32 dma_phys_addr, 
                     int *dma_page_cnt, char *data)
{
	int rc = 0;
	unsigned int cmd_mask = 0, l_dtx_length = 0, l_resp_type = 0, l_cmd_flags = 0, t_val = 0, cnt = 0;
	const unsigned int resp_type_mask[] = {0x0100, 0x0200, 0x0300, 0x0100, 0x0900, 0x0600, 0x0300};
	unsigned long f;
	
	spin_lock_irqsave(&sock->lock, f);
	
	if(cmd)
	{
		sock->mmcsd_p->active_cmd = cmd;
		sock->mmcsd_p->dma_pages_processed = 0;
		sock->mmcsd_p->dma_pages_total = cmd->dtx_length >> 9;
	}

	if(dma_phys_addr)
	{
		sock->mmcsd_p->dma_pages_processed += *dma_page_cnt;
		if(*dma_page_cnt > 63)
		{
			spin_unlock_irqrestore(&sock->lock, f);
			return 0xc0;
		}
		
		if(sock->flags & CARD_BUSY)
		{
			spin_unlock_irqrestore(&sock->lock, f);
			return 0xc3;
		}

		sock->flags |= CARD_BUSY;

		if(*dma_page_cnt)
		{
			if(cmd)
			{
				writel(0xffff, sock->sock_addr + 0x18);
				writel(0x0001, sock->sock_addr + 0x24);
				writel(0x0005, sock->sock_addr + 0x14);
				sock->flags &= ~FLAG_A5;
				if(cmd->flags & CMD_DIR)
					writel(0x8000, sock->sock_addr + 0x130);
				else
				{
					writel(0x0080, sock->sock_addr + 0x130);
					cmd_mask = 0x8000;
				}

				writel(sock->mmcsd_p->dma_pages_total - 1, sock->sock_addr + 0x12c);
				writel(sock->mmcsd_p->read_block_len - 1, sock->sock_addr + 0x128);
				sock->mmcsd_p->cmd_status &= 0xfffffff7;
			}

			writel(dma_phys_addr, sock->sock_addr + 0xc);
			writel((readl(sock->sock_addr + 0x10) & 0x80) | ((*dma_page_cnt) << 8) | 0x1 | cmd_mask,
			       sock->sock_addr + 0x10);
		}
	}
	else
	{
		writel(0x0000, sock->sock_addr + 0x130);
		writel(0x0c00 | readl(sock->sock_addr + 0x118), sock->sock_addr + 0x130);
	}

	spin_unlock_irqrestore(&sock->lock, f);
	
	if(cmd)
	{
		if(cmd->flags & CMD_APP) tifmxx_mmcsd_exec_card_cmd(sock, 0x2137, tifmxx_mmcsd_get_rca(sock));
		cmd_mask = ((cmd->resp_type - 1) < 7) ? resp_type_mask[cmd->resp_type - 1] : 0;
		if(cmd->cmd_index == 3 && !(cmd->flags & CMD_APP)) cmd_mask = 0x0600;
		if(cmd->cmd_index == 7 && !(cmd->flags & CMD_APP)) cmd_mask = 0x0900;
		cmd_mask |= tifmxx_mmcsd_cmd_mask(cmd);
		if((cmd->flags & CMD_DIR) && 0x3000 == (cmd_mask & 0x3000)) cmd_mask |= 0x8000;
		if(data)
		{
			spin_lock_irqsave(&sock->lock, f);
			writel(0, sock->sock_addr + 0x12c);
			if(cmd->dtx_length) writel(cmd->dtx_length - 1, sock->sock_addr + 0x128);
			spin_unlock_irqrestore(&sock->lock, f);
		}
		rc = tifmxx_mmcsd_exec_card_cmd(sock, (cmd->cmd_index & 0x3f) | (cmd_mask & 0xff80),cmd->cmd_arg);
	}
	
	if(dma_phys_addr)
	{
		spin_lock_irqsave(&sock->lock, f);
		if(sock->mmcsd_p->dma_pages_processed >= sock->mmcsd_p->dma_pages_total)
		{
			if(!rc)
			{
				if(sock->mmcsd_p->active_cmd->flags & CMD_BLKM)
				{
					spin_unlock_irqrestore(&sock->lock, f);
					if(0 != (rc = tifmxx_mmcsd_wait_for_brs(sock)) ||
					   0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x290c, 0)))
					{
						spin_lock_irqsave(&sock->lock, f);
						writel(0xffff, sock->sock_addr + 0x18);
						writel(0x0002, sock->sock_addr + 0x10);
						spin_unlock_irqrestore(&sock->lock, f);
						return rc;
					}
					spin_lock_irqsave(&sock->lock, f);
				}
				if(sock->mmcsd_p->active_cmd->flags & CMD_DIR) 
				{
					spin_unlock_irqrestore(&sock->lock, f);
					rc = tifmxx_mmcsd_wait_for_card(sock);
					spin_lock_irqsave(&sock->lock, f);
				}
			}
		}
		
		if(rc)
		{
			writel(0xffff, sock->sock_addr + 0x18);
			writel(0x0002, sock->sock_addr + 0x10);
			spin_unlock_irqrestore(&sock->lock, f);
			return rc;
		}

		while(!sock->sock_status & 1)
		{
			spin_unlock_irqrestore(&sock->lock, f);
			if(!wait_event_timeout(sock->irq_ack, tifmxx_test_flag(sock, SOCK_EVENT | CARD_REMOVED),
					       msecs_to_jiffies(1000))) rc = 0x87;
			tifmxx_clear_flag(sock, SOCK_EVENT);
			spin_lock_irqsave(&sock->lock, f);
			if(rc || (sock->flags & CARD_REMOVED))
			{
				writel(0xffff, sock->sock_addr + 0x18);
				writel(0x0002, sock->sock_addr + 0x10);
				sock->flags &= ~CARD_BUSY;
				if(sock->flags & CARD_REMOVED) rc = 0x86;
				spin_unlock_irqrestore(&sock->lock, f);
				return rc;
			}
		}
		sock->flags &= ~CARD_BUSY;
		sock->sock_status = 0;
		spin_unlock_irqrestore(&sock->lock, f);
	}
	
	if(!data || rc) return rc;
	/* Non DMA transfer here */
	spin_lock_irqsave(&sock->lock, f);
	l_dtx_length = sock->mmcsd_p->active_cmd->dtx_length;
	l_resp_type = sock->mmcsd_p->active_cmd->resp_type;
	l_cmd_flags = sock->mmcsd_p->active_cmd->flags;
	spin_unlock_irqrestore(&sock->lock, f);

	if(l_cmd_flags & CMD_RESP)
	{
		if(l_resp_type != 2 && l_dtx_length) l_dtx_length--;

		for(cnt = 0; cnt < l_dtx_length; cnt++)
			data[cnt] = tifmxx_mmcsd_extract_info_bitfld(sock, ((l_dtx_length - cnt) << 8) - 1,
								     ((l_dtx_length - cnt) << 8) - 8,
								     l_resp_type == 2);
		if(l_resp_type != 2 && l_dtx_length) data[l_dtx_length] = 0;
	}
	else
	{
		if(l_cmd_flags & CMD_DIR)
		{
			for(cnt = 0; cnt < l_dtx_length; cnt += 2)
			{
				tifmxx_mmcsd_wait_for_af(sock);
				spin_lock_irqsave(&sock->lock, f);
				t_val = readl(sock->sock_addr + 0x124);
				spin_unlock_irqrestore(&sock->lock, f);
				data[cnt] = t_val & 0xff;
				if((cnt + 1) >= l_dtx_length) break;
				data[cnt + 1] = (t_val >> 8) & 0xff;			
			}
		}
		else
		{
			spin_lock_irqsave(&sock->lock, f);
			writel(readl(sock->sock_addr + 0x118) | 0x14, sock->sock_addr + 0x118);
			sock->mmcsd_p->r_var_2 = 1;
			spin_unlock_irqrestore(&sock->lock, f);
			for(cnt = 0; cnt < l_dtx_length; cnt += 2)
			{
				t_val = data[cnt];
				if((cnt + 1) < l_dtx_length) t_val |= data[cnt + 1] << 8;
				tifmxx_mmcsd_wait_for_ae(sock);
				spin_lock_irqsave(&sock->lock, f);
				writel(t_val, sock->sock_addr + 0x124);
				spin_unlock_irqrestore(&sock->lock, f);
			}

			if(!(l_cmd_flags & CMD_APP) && 0 != (rc = tifmxx_mmcsd_wait_for_card(sock))) return rc;
			
		}

		spin_lock_irqsave(&sock->lock, f);
		writel(readl(sock->sock_addr + 0x118) & 0xfffff3ff, sock->sock_addr + 0x118);
		spin_unlock_irqrestore(&sock->lock, f);
	}

	return rc;	
}

static int
tifmxx_mmcsd_init_card(struct tifmxx_sock_data *sock)
{
	int rc = 0x2f, cnt;
	struct tifmxx_mmcsd_ecmd cmd_rq;
	char cmd_rp_1[16], cmd_rp_2[16];
	int dma_page_cnt;
	unsigned int l_read_blen, l_rca, card_state;
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

	wait_event_timeout(sock->irq_ack, tifmxx_test_flag(sock, CARD_EVENT | CARD_REMOVED), msecs_to_jiffies(100));
	tifmxx_clear_flag(sock, CARD_EVENT);

	if(tifmxx_test_flag(sock, CARD_REMOVED)) return 0x86; //! card removed

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
	if(0 != (rc = tifmxx_mmcsd_read_csd(sock))) return rc;

	spin_lock_irqsave(&sock->lock, f);
	writel(sock->mmcsd_p->clk_speed | (0xffc0 & readl(sock->sock_addr + 0x110)), sock->sock_addr + 0x110);
	l_rca = sock->mmcsd_p->rca;
	l_read_blen = sock->mmcsd_p->read_block_len;
	spin_unlock_irqrestore(&sock->lock, f);

	if(0 != (rc = tifmxx_mmcsd_read_serial_number(sock))) return rc;	
	if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x2907, l_rca))) return rc;
	if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x2110, l_read_blen))) return rc;

	spin_lock_irqsave(&sock->lock, f);
	writel(sock->mmcsd_p->read_block_len - 1, sock->sock_addr + 0x128);

	if(sock->media_id == 0x23)
	{
		writel(0x0100 | readl(sock->sock_addr + 0x4), sock->sock_addr + 0x4);
		sock->clk_freq = 24000000;
		spin_unlock_irqrestore(&sock->lock, f);
		if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x2137, l_rca))) return rc;
		if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x212a, 0))) return rc;
		
		dma_page_cnt = 0;
		cmd_rq = ((struct tifmxx_mmcsd_ecmd){0x000d, 0x0000, 0x0040, 0x0001, CMD_DIR | CMD_APP});
		if(0 != (rc = tifmxx_mmcsd_execute(sock, &cmd_rq, 0, &dma_page_cnt, cmd_rp_1))) return rc;
		if(0xff00 & ((cmd_rp_1[3] << 8) + cmd_rp_1[4])) return 0x27;
		
		dma_page_cnt = 0;
		cmd_rq = ((struct tifmxx_mmcsd_ecmd){0x0033, 0x0000, 0x0008, 0x0001, CMD_DIR | CMD_APP});
		if(0 != (rc = tifmxx_mmcsd_execute(sock, &cmd_rq, 0, &dma_page_cnt, cmd_rp_2))) return rc;
		if(0x4 & (0xf & cmd_rp_2[1]))
		{
			if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x2137, l_rca))) return rc;
			if(0 != (rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x2106, 2))) return rc;
			spin_lock_irqsave(&sock->lock, f);
			sock->flags |= (0x0200 & readl(sock->sock_addr + 0x8)) ? CARD_RO : 0;
			spin_unlock_irqrestore(&sock->lock, f);
		}
		spin_lock_irqsave(&sock->lock, f);
	}
	spin_unlock_irqrestore(&sock->lock, f);

	// why do they need so many retries?
	for(cnt = 0; cnt < 10000; cnt++)
        {
		if(0 != (rc = tifmxx_mmcsd_get_state(sock, &card_state))) break;

                if(card_state != 0x4) continue;
                if(tifmxx_test_flag(sock, CARD_READY)) break;
        }

	tifmxx_set_flag(sock, CARD_ACTIVE);
        return rc;
}

static int
tifmxx_mmcsd_close_write(struct tifmxx_sock_data* sock)
{
	return 0;
}

static int
tifmxx_mmcsd_read_sect(struct tifmxx_sock_data *sock, int lba_start, int* sector_count, int* dma_page_count, 
		       int resume)
{
	unsigned long f;
	int rc = 0, cnt;
	unsigned int card_state;
	int lba_offset;
	
	spin_lock_irqsave(sock->lock, f);
	if(lba_start != -1)
	{
		if(lba_start >= sock->mmcsd_p->blocks)
		{
			spin_unlock_irqrestore(sock->lock, f);
			return 0x82;
		}
		
		if(*sector_count == 0)
		{
			spin_unlock_irqrestore(sock->lock, f);
			return 0;
		}

		if(*sector_count > 2048)
		{
			spin_unlock_irqrestore(sock->lock, f);
			return 0x2b;
		}

		lba_offset = lba_start << sock->mmcsd_p->log_read_block_len;

		spin_unlock_irqrestore(sock->lock, f);

		for(cnt = 0; cnt < 10000; cnt++)
		{
			if(0 != (rc = tifmxx_mmcsd_get_state(sock, &card_state))) break;
			if(card_state != 0x4) continue;
			if(tifmxx_test_flag(sock, CARD_READY)) break;
		}
		if(card_state != 0x4 || rc || !tifmxx_test_flag(sock, CARD_READY))
		{
			if(rc == 0x86) return rc;
			if(0 != (rc = sock->init_card(sock))) return rc;
		}

		spin_lock_irqsave(sock->lock, f);

		writel(*sector_count - 1, sock->sock_addr + 0x12c);
		writel(sock->mmcsd_p->read_block_len - 1, sock->sock_addr + 0x128);
		writel(0x8000, sock->sock_addr + 0x130);
		sock->mmcsd_p->cmd_status &= 0xfffffff7;

		spin_unlock_irqrestore(sock->lock, f);

		rc = (*sector_count == 1) ? tifmxx_mmcsd_exec_card_cmd(sock, 0xb111, lba_offset)
					  : tifmxx_mmcsd_exec_card_cmd(sock, 0xb112, lba_offset);
		if(rc) return rc;

		spin_lock_irqsave(sock->lock, f);
	}

	if(*sector_count > *dma_page_count)
	{
		*sector_count -= *dma_page_count;
		*dma_page_count = 0;
	}
	else
	{
		spin_unlock_irqrestore(sock->lock, f);

		if(0 != (rc = tifmxx_mmcsd_wait_for_brs(sock))) return rc;
		
		spin_lock_irqsave(sock->lock, f);

		if(*sector_count != 1 || lba_start == -1)
		{
			spin_unlock_irqrestore(sock->lock, f);

			rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x290c, 0); // stop transmission
			if(rc == 0x86) return rc; // card removed

			
			if(rc && !tifmxx_test_flag(sock, R_INIT))
			{
				if(0 != (rc = sock->init_card(sock))) return rc;
			}

			spin_lock_irqsave(sock->lock, f);	
		}
		*dma_page_count -= *sector_count;
		*dma_page_count = 0;
	}

	spin_unlock_irqrestore(sock->lock, f);
	return rc;
}

static int
tifmxx_mmcsd_write_sect(struct tifmxx_sock_data *sock, int lba_start, int* sector_count, int* dma_page_count, 
			int resume)
{
	unsigned long f;
	int rc = 0, cnt;
	unsigned int card_state;
	int lba_offset;
	
	spin_lock_irqsave(sock->lock, f);
	if((sock->flags & CARD_RO) || (0x0200 & readl(sock->sock_addr + 0x8)))
	{
		spin_unlock_irqrestore(sock->lock, f);
		return 0xc1; // card is write protected
	}

	if(lba_start != -1)
	{
		if(lba_start >= sock->mmcsd_p->blocks) rc = 0x82;
		else if(*sector_count > 2048) rc = 0x2b;
		
		if(rc || *sector_count == 0)
		{
			spin_unlock_irqrestore(sock->lock, f);
			return rc;
		}
		
		lba_offset = lba_start << sock->mmcsd_p->log_write_block_len;

		spin_unlock_irqrestore(sock->lock, f);

		for(cnt = 0; cnt < 10000; cnt++)
		{
			if(0 != (rc = tifmxx_mmcsd_get_state(sock, &card_state))) break;
			if(card_state != 0x4) continue;
			if(tifmxx_test_flag(sock, CARD_READY)) break;
		}
		if(card_state != 0x4 || rc || !tifmxx_test_flag(sock, CARD_READY))
		{
			if(rc == 0x86) return rc;
			if(0 != (rc = sock->init_card(sock))) return rc;
		}

		if(tifmxx_get_media_id(sock) == 0x23)
		{
			cnt = 0;
			do
			{
				rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x2137, tifmxx_mmcsd_get_rca(sock));
				cnt++;
				if(cnt > 2) break;
			}while(rc == 0x25);

			if(!rc) rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x2117, *sector_count);
		}

		spin_lock_irqsave(sock->lock, f);

		writel(*sector_count - 1, sock->sock_addr + 0x12c);
		writel(sock->mmcsd_p->write_block_len - 1, sock->sock_addr + 0x128);
		writel(0x0080, sock->sock_addr + 0x130);
		sock->mmcsd_p->cmd_status &= 0xfffffff7;

		spin_unlock_irqrestore(sock->lock, f);

		rc = (*sector_count == 1) ? tifmxx_mmcsd_exec_card_cmd(sock, 0x3118, lba_offset)
					  : tifmxx_mmcsd_exec_card_cmd(sock, 0x3119, lba_offset);
		if(rc) return rc;

		spin_lock_irqsave(sock->lock, f);
	}
	//x1fa18
	spin_unlock_irqrestore(sock->lock, f);
	if(*sector_count <= *dma_page_count)
	{
		
		if(0 != (rc = tifmxx_mmcsd_wait_for_brs(sock))) return rc;
		
		if(*sector_count == 1 || lba_start == -1)
		{
			for(cnt = 0; cnt < 10000; cnt++)
			{
				if(0 != (rc = tifmxx_mmcsd_get_state(sock, &card_state))) break;
				if(card_state == 0x7) continue;
				if(tifmxx_test_flag(sock, CARD_READY))
				{
					rc = tifmxx_mmcsd_exec_card_cmd(sock, 0x290c, 0);
					break;
				}
			}

			if(rc) return rc;
		}
		//x1fb51
		for(cnt = 0; cnt < 10000; cnt++)
		{
			if(0 != (rc = tifmxx_mmcsd_get_state(sock, &card_state))) break;
			if(card_state != 0x4) continue;
			if(tifmxx_test_flag(sock, CARD_READY)) break;
		}

		if(card_state != 0x4 || rc || !tifmxx_test_flag(sock, CARD_READY))
		{
			if(rc == 0x86) return rc;
			if(0 != (rc = sock->init_card(sock))) return rc;
		}	
		
		//x1fbd4
		*dma_page_count -= *sector_count;
		*sector_count = 0;
	}
	else
	{
		*sector_count -= *dma_page_count;
		*dma_page_count = 0;
	}

	spin_lock_irqsave(sock->lock, f);
	sock->mmcsd_p->cmd_status = 0;
	spin_unlock_irqrestore(sock->lock, f);

	return rc;
}

void
tifmxx_mmcsd_init(struct tifmxx_sock_data* sock)
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
			sock->close_write = tifmxx_mmcsd_close_write;
			sock->read_sect = tifmxx_mmcsd_read_sect;
			sock->write_sect = tifmxx_mmcsd_write_sect;
			
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

