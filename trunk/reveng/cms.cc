short
CMS::sub21510(short arg_1)
{
	if(arg_1 >= 0x2000) return -1;
	return var_x172[arg_1];
}

char
CMS::WaitForMSINT()
{
	char rc;

	if(KeSynchronizeExecution(&CMS::HandleMSINT, &dwMS_STATUS, card_int)) return 0;

	for(int cnt = 6; cnt; cnt--) {
		rc = sub_1FEE0(&ms_event_x0c8, -5000000);
		if(vara_4) return 0x86;
		if(KeSynchronizeExecution(&CMemoryStick::HandleMSINT, &dwMS_STATUS, card_int)) return 0;
	}
	if(!(0x2000 & read32(base_addr + 0x18c))) return rc;
}

char
CMS::sub219F0(char arg_1, short arg_2)
{
	int t_val = read32(base_addr + 0x190) & 0xfffeffff;

	if(arg_1 == 2 || arg_1 == 13) t_val |= 0x10000;
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(&CMemoryStick::ClearStatus, &dwMS_STATUS, card_int);
	write32(base_addr + 0x190, t_val | 0x0800);
	write32(base_addr + 0x184, ((arg_1 & 0xf) << 12) | (arg_2 & 0x3ff));
	char rc = WaitForRDY();
	write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
	return rc;
}

char
CMS::WriteRegisters()
{
	int cnt;
	char rc;
	char lvar_x48[32];

	write32(base_addr + 0x190, var_x104 | 0x2707);
	for(cnt = 0; cnt < 32; cnt++)
	{
		if(cnt == 0x10 || muiMediaID == 0x12)
		{
			lvar_x48[0x10] = (ms_regs.param.system == 0x88 || !var_x104) ? 0x88 : 0x80; // status.system
		}
		else lvar_x48[cnt] = ((char*)&ms_regs)[cnt]; 
	}
	for(cnt = 0; cnt < 8; cnt++)
	{
		write32(base_addr + 0x190, 0x100 | read32(base_addr + 0x190));
		write32(base_addr + 0x188, ((int*)lvar_x48)[cnt]);
	}
	int t_val = read32(base_addr + 0x190);

	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(&CMemoryStick::ClearStatus, &dwMS_STATUS, card_int);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0xb01f);

	for(cnt=3; cnt; cnt--)
	{
		rc = sub_1FEE0(&ms_event_x0c8, -5000000);
		if(vara_4)
		{
			rc = 0x86;
			break;
		}
		if(!KeSynchronizeExecution(CMemoryStick::HandleError, &dwMS_STATUS, card_int))
		{
			rc = 0x47;
			break;
		}
		if(KeSynchronizeExecution(CMemoryStick::HandleRDY, &dwMS_STATUS, card_int))
		{
			rc = 0;
			break;
		}
	}
	if(!cnt) rc = !(0x1000 & read32(base_addr + 0x18c)) ? 0x87 : 0;
	write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
	return rc;
}

char
CMS::Cmd(char opcode, char arg_2, char arg_3, char arg_4)
{
	char rc;

	if(arg_4 && (rc = WriteRegisters())) return rc;
	write32(base_addr + 0x190, var_x104 | 0x2707);
	write32(base_addr + 0x190, 0x100 | read32(base_addr + 0x190));
	write32(base_addr + 0x188, opcode);
	write32(base_addr + 0x190, 0x100 | read32(base_addr + 0x190));
	write32(base_addr + 0x188, 0);
	int t_val = read32(base_addr + 0x190);
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(&CMemoryStick::ClearStatus, &dwMS_STATUS, card_int);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0xe001); // SET_CMD
	rc = WaitForRDY();
	write32(base_addr + 0x190, t_val & 0xfffeffff);
	if(rc) return 0x81;
	if(arg_2 && (rc = WaitForMSINT())) return rc;
	if(arg_3) rc = sub21DA0;
	return rc;
}

char
CMS::GetInt()
{
	write32(base_addr + 0x190, var_x104 | 0x2607);
	int t_val = read32(base_addr + 0x190);
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(&CMemoryStick::ClearStatus, &dwMS_STATUS, card_int);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0x7001);
	char rc = WaitForRDY();
	write32(base_addr + 0x190, t_val & 0xfffeffff);
	ms_regs.status.interrupt = read32(base_addr + 0x188) & 0xff;
	read32(base_addr + 0x188);
	return rc;
}

CMS::CMS(char *_base_addr, int _serial_mode)
    :CMemoryStick(_base_addr), dwMS_STATUS(0), mbParallelInterface(_serial_mode), var_x0e1(0), var_x0e2(0),
     var_x104(0x4010), var_x108(0), var_x10c(0)
{
	KInitializeEvent(ms_event_x0c8);
	write32(base_addr + 0x190, 0x8000);
	write32(base_addr + 0x190, 0x0a00);
	write32(base_addr + 0x18c, 0xffffffff);
}

char
CMS::ReadPage(short wPhyBlock, char byPage, char arg_3, int arg_4)
{
	char rc;
	if(arg_3 & 0x20)
	{
		ms_regs.param.cp = 0x20;
		ms_regs.param.system = var_x104 ? 0x80 : 0x88; 
	}
	if(arg_3 & 0x01)
	{
		ms_regs.param.cp = 0;
		ms_regs.param.system = var_x104 ? 0x80 : 0x88;
	} 
	if(!(arg_3 & 0x04))
	{
		ms_regs.param.block_address[0] = 0;
		ms_regs.param.block_address[1] = wPhyBlock >> 8;
		ms_regs.param.block_address[2] = wPhyBlock & 0xff;
		ms_regs.param.page_address = byPage;

		if(vara_7)
		{
			if((rc = GetInt())) return rc;
		}
		if((rc = WriteRegisters())) return rc;
		write32(base_addr + 0x190, var_x104 | 0x2707);
		write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
		write32(base_addr + 0x188, 0x00aa); // READ
		write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
		write32(base_addr + 0x188, 0);
		int t_val = read32(base_addr + 0x190);
		KeClearEvent(&ms_event_x0c8);
		KeSynchronizeExecution(&CMemoryStick::ClearStatus, &dwMS_STATUS, card_int);
		write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
		write32(base_addr + 0x184, 0xe001); // SET_CMD + ?
		rc = WaitForRDY();
		write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
		if(rc) return 0x81;
		if((rc = WaitForMSINT())) return rc;
	}
	else
	{
		if((rc = WaitForMSINT())) return rc;
	}
	if((rc = GetInt())) return rc;
	if(ms_regs.status.interrupt & 0x01) // CMDNK
	{
		//print registers
		return 0x49;
	}
	if(!(ms_regs.status.interrupt & 0x80) && !(arg_3 & 0x05)) return 0x4f; // CED not set
	if(!(ms_regs.status.interrupt & 0x20)) return 0x4e; // BREQ not set

	// 0x33 - BLK_END
	if((arg_3 & 0x08) && (byBlockSize != (arg_3 + 1)) && (rc = Cmd(0x33, 1, 0, 0))) return rc; // CMD_BLOCK_END error
	if(ms_regs.status.interrupt & 0x40) // INT_ERR is set
	{
		if((rc = sub21DA0())) return rc;
		if(!(ms_regs.status.status1 & 0x10))
		{
			if(ms_regs.status.status1 & 0x04) return 0x40; //UCEX
			if(ms_regs.status.status1 & 0x01) return 0x40; //UCFG
			if(ms_regs.status.status1 & 0x08) return 0x40; //EXER
		}
		else
		{ // UCDT
			char l_OV = (~ms_regs.extra_data.overwrite_flag) | 0x3f;
			if(arg_3 & 0x05) Cmd(0x33, 1, 0, 0);
			if(l_OV == 0xff) return 0x43;
			ms_regs.extra_data.overwrite_flag = l_OV;
			ms_regs.param.block_address[2] = wPhyBlock & 0xff;
			ms_regs.param.block_address[1] = wPhyBlock >> 8;
			ms_regs.param.block_address[0] = 0;
			ms_regs.param.page_address = byPage;
			var_x0f3 = var_x104 ? 0x80 : 0x88;
			var_x0f7 = 0x80;
			if(!(rc = WriteRegisters()))
			{
				write32(base_addr + 0x190, var_x104 | 0x2707);
				write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
				write32(base_addr + 0x188, 0x0055); // WRITE
				write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
				write32(base_addr + 0x188, 0);
				int t_val = read32(base_addr + 0x190);
				KeClearEvent(&ms_event_x0c8);
				KeSynchronizeExecution(&CMemoryStick::ClearStatus, &dwMS_STATUS, card_int);
				write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
				write32(base_addr + 0x184, 0xe001); // SET_CMD + ?
				rc = WaitForRDY();
				write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
				if(!rc && !(rc = WaitForMSINT())) sub21DA0();

				ms_regs.extra_data.overwrite_flag = 0x7f;
				ms_regs.param.block_address[2] = wPhyBlock & 0xff;
				ms_regs.param.block_address[1] = wPhyBlock >> 8;
				ms_regs.param.block_address[0] = 0;
				var_x0f3 = var_x104 ? 0x80 : 0x88;
				ms_regs.param.page_address = 0;
				var_x0f7 = 0x80;
				if((rc = WriteRegisters())) return 0x43;
				write32(base_addr + 0x190, var_x104 | 0x2707);
				write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
				write32(base_addr + 0x188, 0x0055);
				write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
				write32(base_addr + 0x188, 0);
				int t_val = read32(base_addr + 0x190);
				KeClearEvent(&ms_event_x0c8);
				KeSynchronizeExecution(&CMemoryStick::ClearStatus, &dwMS_STATUS, card_int);
				write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
				write32(base_addr + 0x184, 0xe001);
				rc = WaitForRDY();
				write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
				if(rc) return 0x43;
				if(!(rc = WaitForMSINT())) sub21DA0();
				return 0x43;
			}
		}
	}

	if(arg_4)
	{
		if((rc = sub219F0(2, 0x200))) return rc;
		if(arg_3 & 8)
		{
			for(int cnt = 0; cnt < 1000; cnt++)
			{
				if((rc = GetInt())) return rc;
				if(ms_regs.status.interrupt & 0x80) goto proceed;
			}
			return 0x4f;
		}
	}
proceed:
	if(arg_3 & 0x05) return rc;
	if((rc = sub21DA0())) return rc;
	if((ms_regs.extra_data.overwrite_flag & 0x60) == 0x60) return 0;
	if((arg_3 & 0x02) && (ms_regs.extra_data.overwrite_flag == 0xc0)) return 0;
	if(wPhyBlock <= 2) return 0;

	ms_regs.extra_data.overwrite_flag = 0x7f;
	ms_regs.param.block_address[2] = wPhyBlock & 0xff;
	ms_regs.param.block_address[1] = wPhyBlock >> 8;
	ms_regs.param.block_address[0] = 0;
	ms_regs.param.system = var_x104 ? 0x80 : 0x88;
	ms_regs.param.page_address = 0;
	ms_regs.param.cp = 0x80;
	if((rc = WriteRegisters())) return 0x42;
	write32(base_addr + 0x190, var_x104 | 0x2707);
	write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
	write32(base_addr + 0x188, 0x0055);
	write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
	write32(base_addr + 0x188, 0);
	int t_val = read32(base_addr + 0x190);
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(&CMemoryStick::ClearStatus, &dwMS_STATUS, card_int);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0xe001);
	rc = WaitForRDY();
	write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
	if(rc) return 0x42;
	if(!(rc = WaitForMSINT())) sub21DA0();
	return 0x42;
}

char
CMD::FindBootBlocks()
{
	int bb_cnt = 0;
	short phy_block = 0;
	char rc0, rc1;

	for(int cnt = 0; cnt < 10; cnt++)
	{
		write32(base_addr + 0x24, 1);
		write32(base_addr + 0x10, 0x100);
		if((rc0 = ReadPage(phy_block, 0, 0x22, 1)))
		{ // error - reset
			if(!WriteRegisters())
			{
				write32(base_addr + 0x190, var_x104 | 0x2707);
				write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
				write32(base_addr + 0x188, 0x003c);
				write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
				write32(base_addr + 0x188, 0);
				int t_val = read32(base_addr + 0x190);
				KeClearEvent(&ms_event_x0c8);
				KeSynchronizeExecution(&CMemoryStick::ClearStatus, &dwMS_STATUS, card_int);
				write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
				write32(base_addr + 0x184, 0xe001);
				rc1 = WaitForRDY();
				write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
				if(!rc1) sub21DA0();
			}
			var_x104 = 0x4010;
			sub22370(0x1f001f00);
		}
		if(!rc0 && !sub21DA0())
		{
			if(ms_regs.extra_data.management_flag != 0x40)
			{
				if(1 == (read32(base_addr + 0x200) >> 16)) boot_blocks[bb_cnt++] = phy_block;
			}
		}
		else {/* print */};
		phy_block++;
		if(bb_cnt >= 2) break;
	}
	if(!bb_cnt || rc0) return 0xff;
	return 0;
}

char
CMS::MediaModel()
{
	char rc;

	write16(base_addr + 0x24, 1);
	write16(base_addr + 0x10, 0x100);
	if((rc = ReadPage(boot_blocks, 0, 0x22, 1))) return rc;
	if((rc = sub21DA0())) return rc;
	byBlockSize = sub1FC40(0x1a3) * 2;
	mwPhyBlocks = (sub1FC40(0x1a4) << 8) + sub1FC40(0x1a5);
	mwEffectiveBlocks = (sub1FC40(0x1a6) << 8) + sub1FC40(0x1a7);
	mwUserBlocks = mwEffectiveBlocks - 4;
	if(mwPhyBlocks == 0x200) mwSize = 4;
	else if(mwPhyBlocks == 0x400) mwSize = 8;
	else if(mwPhyBlocks == 0x800) mwSize = 16;
	else if(mwPhyBlocks == 0x1000) mwSize = 32;
	else if(mwPhyBlocks == 0x2000) mwSize = 64;
	if(byBlockSize == 32) mwSize *= 2;

	bySegments = mwPhyBlocks >> 9;
	SerialNumber = (sub1FC40(0x1b5) << 16) | (sub1FC40(0x1b6) << 8) | sub1FC40(0x1b7);
	return 0;
}

char
CMS::EraseBlock(short block_address)
{
	char rc = 0;

	regs.param.block_address[2] = block_address & 0xff;
	regs.param.block_address[1] = (block_address >> 8) & 0xff;
	regs.param.block_address[0] = 0;
	regs.param.cp = 0x20;
	regs.param.system = var_x104 ? 0x80 : 0x88;
	if(WriteRegisters()) return 0x81;
	write32(base_addr + 0x190, 0x2707 | var_x104);
	write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
	write32(base_addr + 0x188, 0x0099);
	write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
	write32(base_addr + 0x188, 0);
	int t_val = read32(base_addr + 0x190);
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(&CMemoryStick::ClearStatus, &dwMS_STATUS, card_int);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0xe001);
	rc = WaitForRDY();
	write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
	if(rc || WaitForMSINT() || sub21DA0()) return 0x81;
	if(regs.status.interrupt != 1) return 0x81; // no ack
	if(!(regs.status.interrupt & 0x80)) return 0x81; // proc error
	if(block_address >= 0x2000) return 0;
	
	this->off_x4172[(block_address >> 3)] &= ~((block_address >> 3) << (block_address & 7));
	return 0;
}

char
CMS::sub23BA0(short *arg_1, char arg_2)
{
	char lvar_r11 = arg_2;
	short lvar_r12 = 0;

	do
	{
		if(-1 != (lvar_r9 = var_x11e[lvar_r11]))
		{
			lvar_r10 = lvar_r11 << 9;
			lvar_r8 = lvar_r10 + 0x200;
			lvar_bx = lvar_r9 + 1 >=  lvar_r8 ? lvar_r8 : lvar_r9 + 1;
			do
			{
				if(lvar_bx < 0x2000)
				{
					lvar_dl = this->off_x4172[lvar_bx >> 3] & (1 << (lvar_bx & 7));
					if(!lvar_dl)
					{
						if(lvar_bx != lvar_r9)
						{
							var_x11e[lvar_r11] = lvar_bx;
							EraseBlock(lvar_bx);
							return 0;
						}
						else break;
					}
				}
				if(lvar_bx == lvar_r9) break;
				lvar_bx++;
				if(lvar_bx < lvar_r8) continue;
				lvar_bx = lvar_r10;
			}while(1);

		}
		lvar_r11++;
		lvar_r11 = lvar_r11 >= bySegments ? lvar_r12 : lvar_r11;
		*arg_1 = var_x11e[lvar_r11];

	}while(lvar_r11 != arg_2)
	return 0x84;
}

char
CMS::sub23CC0(short arg_1)
{
	char rc;

	if(arg_1 != FFPhyBlock) return 0;
	if((rc = sub23BA0(&lvar_x38, 0))) return rc;
	FFPhyBlock = lvar_x38;
	for(short cnt = 0; cnt < mwUserBlocks; cnt++)
	{
		lvar_cx = cnt < 0x2000 ? var_x172[cnt] ? 0xffff;
		if(lvar_cx == arg_1 || cnt < 0x2000) var_x172[cnt] = FFPhyBlock;
	}
	return 0;
}

char
CMS::WritePage(short arg_1, short block_address, char arg_3, char arg_4, char arg_5)
{
	char rc;

	if(block_address == boot_blocks[0] || block_address == boot_blocks[1]) return 0x85;
	if(arg_4 & 0x80)
	{
		regs.param.cp = 0x20;
		regs.param.system = var_x104 ? 0x80 : 0x88;
	}
	if(arg_4 & 1)
	{
		regs.param.cp = 0;
		regs.param.system = var_x104 ? 0x80 : 0x88;
	}
	if(!(arg_4 & 4))
	{
		regs.extra_data.overwrite_flag = 0xff;
		regs.extra_data.management_flag = 0xff;
		regs.extra_data.logical_address = cpu_to_be16(arg_1);
		regs.param.block_address[2] = block_address & 0xff;
		regs.param.block_address[1] = (block_address >> 8) & 0xff;
		regs.param.block_address[0] = 0;
		regs.param.page_address = arg_3;
		if(vara_7)
		{
			if((rc = GetInt())) return rc;
			//print ...
		}
		if((rc = WriteRegisters())) return rc;
		write32(base_addr + 0x190, 0x2707 | var_x104);
		write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
		write32(base_addr + 0x188, 0x0055);
		write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
		write32(base_addr + 0x188, 0);
		int t_val = read32(base_addr + 0x190);
		KeClearEvent(&ms_event_x0c8);
		KeSynchronizeExecution(&CMemoryStick::ClearStatus, &dwMS_STATUS, card_int);
		write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
		write32(base_addr + 0x184, 0xe001);
		rc = WaitForRDY();
		write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
		if(rc) return 0x81;
		if((rc = WaitForMSINT())) return rc;
	}
	else
	{
		if((rc = WaitForMSINT())) return rc;
	}
	
	if((rc = GetInt())) return rc;
	if(1 & regs.status.interrupt) return 0x49; // no ack
	if(0x40 & regs.status.interrupt) return 0x44; // int error
	if(!arg_5) return 0;
	write32(base_addr + 0x190, 0x10100 | (read32(base_addr + 0x190) & 0xffffefff));
	if((rc = sub219F0(0x0d, 0x200))) return 0x81;
	if(!(arg_4 & 8) & !(arg_4 & 0x20)) return 0;
	if((rc = WaitForMSINT())) return rc;
	if(arg_4)
	{
		if((rc = Cmd(0x33, 1, 0, 0))) return rc;
	}
	for(int cnt = 0; cnt < 1000; cnt++)
	{
		if((rc = GetInt())) return rc;
		if(regs.status.interrupt & 0x80) return 0;
	}
	return 0x4f;
}

char
CMS::CopyPages(short arg_1, short *arg_2, char arg_3, char arg_4, short arg_5)
{
	char rc;
	if(var_x0e1 == 0)
	{
		var_x140 = *arg_2;
		var_x142 = arg_1;
		if(*arg_2 != var_x144 && byBlockSize > 0) memset(var_x148, 0, byBlockSize);
		var_x144 = -1;
		if(FFPhyBlock == *arg_2 || var_148[arg_3] == 0)
		{
			var_x0e1 = 1;
			int lvar_ax = 0x1ed;
			for(int cnt = 0; cnt < bySegments; cnt++)
			{
				if(arg_1 <= lvar_ax)
				{
					if((rc = sub23BA0(&var_x140, cnt))) return rc;
					break;
				}
				lvar_ax += 0x1f0;
			}
			if(var_x140 < 0x2000)
			{
				bad_blocks[var_x140 >> 3] |= 1 << (var_x140 & 7);
			}
			if(byBlockSize > 0)
			{
				memset(var_x148, 1, byBlockSize);
			}
			if(var_x146 != -1) EraseBlock(var_x146);
			var_x146 = -1;
		}
		var_x140 = *arg_2;
		*arg_2 = var_x140;
		var_x144 = var_x140;
	}
	if(var_x0e1 == 1)
	{
		var_x168 = arg_3;
		var_x16a = byBlockSize;
		var_x146 = *arg_2;
		for(int cnt = 0; cnt < arg_3; cnt++)
		{
			if(0x81 == (rc = ReadPage(*arg_2, cnt, 0x20, 0))) break;
			if((rc = WritePage(arg_1, var_x140, cnt, 0x20, 0)))
			{
				rc = 0x46;
				break;
			}
		}
		if(cnt == arg_3) rc = 0x45;
		if(arg_3) memset(&var_x148, 0, arg_3);
		var_x16a = byBlockSize > var_x168 + arg_5 ? byBlockSize : var_x168 + arg_5;
		if(rc == 0x45 || rc == 0) var_x0e1 = 2;
		
	}
	if(var_x0e1 == 3)
	{
		if(*arg_2 != var_x144) return 0x52;
		if(arg_3 != var_x16c + 1) return 0x52;
		var_x168 = arg_3;
		var_x16a = arg_5 + arg_3 < byBlockSize ? byBlockSize : arg_5 + arg_3;
		var_x0e1 = 2;
		rc = 0;
	}
	if(var_x0e1 == 2)
	{
		char lvar_cl = !arg_5 || !arg_3 ? 4 : 1;
		if(arg_3 == byBlockSize - 1 ||  
	}
	24699:
	
}

char
CMS::CloserWrite()
{
	if(var_x0e1 != 3)
	{
		vara_6 = 0;
		return 0;
	}

	int lvar_x40;
	vara_6 = 1;
	var_x0e1 = 4;
	char rc = CopyPages(0, &lvar_x40, 0, 0, 0); 
	vara_6 = 0;
	return rc;
}

char
CMS::MakeLUT()
{
	char rc; 

	if(var_x16e >= mwPhyBlocks)
	{
		var_x170 = 1;
		return 0;
	}
	if(var_x16e < 0x2000 || bad_blocks[var_x16e >> 3] & (1 << (var_x16e & 7)))
	{
		write16(base_addr + 0x24, 1);
		write16(base_addr + 0x10, 0x100);
		rc = ReadPage(var_x16e, 0, 0x20, 1);
		if(vara_4) return 0x86;
		if(rc)
		{
			write32(base_addr + 0x190, var_x104 | 0x2707);
			write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
			write32(base_addr + 0x188, 0x003c);
			write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
			write32(base_addr + 0x188, 0);
			int t_val = read32(base_addr + 0x190);
			KeClearEvent(&ms_event_x0c8);
			KeSynchronizeExecution(ClearStatus, &dwMS_STATUS, card_int);
			write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
			write32(base_addr + 0x184, 0xe001);
			rc = WaitForRDY();
			write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
			if(!rc) sub21DA0();
			varx_x104 = 0x4010;
			sub22370(0x1f001f00);
		}
		else
		{
			sub21DA0();
			if((ms_regs.extra_data.overwrite_flag | 0x1f) != 0xff)
			{
				lvar_si = (ms_regs.extra_data.overwrite_flag >> 5) & 3
				lvar_r13 = ms_regs.extra_data.overwrite_flag >> 7
				if(!(rc = WriteRegisters()))
				{
					write32(base_addr + 0x190, var_x104 | 0x2707);
					write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
					write32(base_addr + 0x188, 0x003c);
					write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
					write32(base_addr + 0x188, 0);
					int t_val = read32(base_addr + 0x190);
					KeClearEvent(&ms_event_x0c8);
					KeSynchronizeExecution(ClearStatus, &dwMS_STATUS, card_int);
					write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
					write32(base_addr + 0x184, 0xe001);
					rc = WaitForRDY();
					write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
					if(!rc) sub21DA0();
				}
				varx_x104 = 0x4010;
				sub22370(0x1f001f00);
				lvar_r12 = 0xff;
			}
		}
		if(lvar_r13 || lvar_si != 3 || !lvar_r12)
		{
			lvar_x28 = logical_address[1] + (logical_address[0] << 8);
			if(lvar_x28 == 0xffff)
			{
				if(var_x11e[var_x16e >> 9] == 0xffff) var_x11e[var_x16e >> 9] = var_x16e; 
			}
			else
			{
				if(lvar_x28 < 0x2000)
				{
					if(var_x172[lvar_x28] != 0xffff)
					{
						lvar_dx = var_x172[lvar_x28];
						if(ms_regs.extra_data.overwrite_flag & 0x10) var_x172[lvar_x28] = var_x16e;
						else
						{
							if(var_x16e <= lvar_dx) lvar_dx = var_x16e;
							else var_x172[lvarx_28] = var_x16e; 
						}
						if((rc = EraseBlock(lvar_dx))) return rc;
					}
					else var_x172[lvar_x28] = var_x16e;
				}
				if(var_x16e < 0x2000) bad_blocks[var_x16e >> 3] |=  1 << (var_x16e & 3);
			}
		}
		else
		{
			if(var_x16e < 0x2000) bad_blocks[var_x16e >> 3] |=  1 << (var_x16e & 3);
		}
	}

	if(var_x16e + 1 != mwPhyBlocks) return 0;
	int cnt;
	for(cnt = 0; cnt < bySegments; cnt++)
	{
		FFPhyBlock = var_x11e[cnt];
		if(FFPhyBlock != 0xffff) break;
	}
	if(FFPhyBlock == 0xffff || cnt == bySegments) return 0x84;
	for(cnt = 0; cnt < mwUserBlocks; cnt++)
	{
		if(cnt < 0x2000 && var_x172[cnt] == 0xffff)
		{
			var_x172[cnt] = FFPhyBlock;
		}
	}
	if(FFPhyBlock < 0x2000) bad_blocks[FFPhyBlock >> 3] |= 1 << (FFPhyBlock & 7);
	var_x170 = 1;
	var_x16e = 0;
	return 0;
}

char
CMS::GetCHS()
{
	write16(base_addr + 0x24, 1);
	write16(base_addr + 0x10, 0x100);
	char rc = ReadPage(boot_blocks[0], 2, 0x22, 1);
	sub21DA0();
	if(rc) return rc;
	mwHeadCount = sub1FD00(0x83);
	mwSectorsPerTrack = sub1FD00(0x86);
	mwCylinders = (mwUserBlocks * byBlockSize) / (mwHeadCount * mwSectorsPerTrack);
	return 0;  
}

char
CMS::InitializeLUT()
{
	memset(var_x172, 0xff, 0x4000);
	memset(bad_blocks, 0, 0x400);
	memset(var_x11e, 0xff, 0x20);
	var_x170 = 0;
	write16(base_addr + 0x24, 1);
	write16(base_addr + 0x10, 0x100);
	if(ReadPage(boot_blocks[0], 1, 0x22, 1) || sub21DA0()) return 0xff;
	//defective blocks:
	for(int cnt = 0;  cnt < 0x200; cnt++)
	{
		lvar_x18 = be_to_cpu16(sub1FD00(cnt));
		if(lvar_x18 == 0xffff) break;
		if(mwPhyBlocks >= lvar_x18 && 0x2000 > lvar_x18)
		{
			bad_blocks[lvar_x18 >> 3] |= 1 << (lvar_x18 & 7);
		}
	}
	if(boot_blocks[0] != 0xffff && boot_blocks[0] < 0x2000)
	{
		bad_blocks[boot_blocks[0] >> 3] |= 1 << (boot_blocks[0] & 7);
	}
	if(boot_blocks[1] != 0xffff && boot_blocks[1] < 0x2000)
	{
		bad_blocks[boot_blocks[1] >> 3] |= 1 << (boot_blocks[1] & 7);
	}
	return 0;
}

char
CMS::SwitchToParallelIF()
{
	char rc;
	var_x104 = 0x4010;
	ms_regs.param.system = 0x88;
	rc = WriteRegisters();
	ms_regs.param.system = 0x88;
	rc = WriteRegisters();
	if(rc)
	{
		// parallel mode
		write16(base_addr + 0x4, 0x100 | read16(base_addr + 0x4));
		var_x104 = 0;
		var_x029 = 0x80;
	}
	else
	{
		// serial mode
		var_x104 = 0x4010;
		var_x029 = 0x12;
	}
	return 0;
}

char
CMS::InitializeCard()
{
	var_x170 = 0;
	vara_2 = 0;
	boot_blocks[0] = 0xffff; boot_blocks[1] = 0xffff;
	FFPhyBlock = 0xffff;
	write32(base_addr + 0x190, var_x104 | 0x2707);
	write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
	write32(base_addr + 0x188, 0x003c);
	write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
	write32(base_addr + 0x188, 0);
	int t_val = read32(base_addr + 0x190);
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(ClearStatus, &dwMS_STATUS, card_int);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0xe001);
	rc = WaitForRDY();
	write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
	if(rc)
	{
		write32(base_addr + 0x190, var_x104 | 0x2707);
		write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
		write32(base_addr + 0x188, 0x003c);
		write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
		write32(base_addr + 0x188, 0);
		int t_val = read32(base_addr + 0x190);
		KeClearEvent(&ms_event_x0c8);
		KeSynchronizeExecution(ClearStatus, &dwMS_STATUS, card_int);
		write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
		write32(base_addr + 0x184, 0xe001);
		rc = WaitForRDY();
		write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
		if(rc) return 0x81;
	}
	if((rc = sub22370(0x1f001f00))) return rc;
	if((rc = FindBootBlocks())) return rc;
	if((rc = MediaModel())) return rc;
	if((rc = GetCHS())) return rc;
	if((rc = InitializeLUT())) return rc;
	if((rc = sub21DA0())) return rc;
	mbWriteProtected = (ms_regs.status.status0 & 1);
	while(!(rc = MakeLUT() && !var_x170) {};
	if(rc) return rc;
	if(mbParallelInterface) SwitchToParallelIF();
	vara_2 = 1;
	return 0;
}

char
CMS::RescueRWFail()
{
	CFlash::RescueRWFail();
	write32(base_addr + 0x190, 0x8000);
	write32(base_addr + 0x190, 0x0a00);
	dwMS_STATUS = 0;
	write32(base_addr + 0x18c, 0xffffffff);
	var_x104 = 0x4010;
	dwMS_STATUS = 0;
	write32(base_addr + 0x18c, 0xffffffff);
	var_x104 = 0x4010;
	return InitializeCard();
}

CMS::~CMS()
{
	write32(base_addr + 0x190, 0x0a00);
	write32(base_addr + 0x18c, 0xffffffff);
	dwMS_STATUS = 0;
}

char
CMS::ReadSector(int dwSector, short arg_2, char arg_3)
{
	int wLogBlock; // si
	long wPhyBlock; // bp
	int byPage; //r13
	char rc;

	if(byBlockSize == 16)
	{
		wLogBlock = arg_1 >> 4;
	}
	else if(byBlockSize == 32)
	{
		wLogBlock = arg_1 >> 5;
	}
	else
	{
		wLogBlock = arg_1 / byBlockSize;
	}
	if(wLogBlock >=  mwUserBlocks) return 0x82;
	byPage = arg_1 % byBlockSize;
	wPhyBlock = wLogBlock < 0x2000 ? var_x172[lvar_esi] : -1;
	lvar_r12 = !arg_3 || !byPage ? 1 : 4;
	if(byPage == byBlockSize - 1 || arg_2 == 1) lvar_r12 |= 8;
	if(!arg_3)
	{
		if(arg_2 == 1) lvar_r12 = 32;
	}
	int cnt = 0;
	while(!var_x170)
	{
		if(cnt >= 0x800) return 0x4a;
		if(wPhyBlock == -1) break;
		MakeLUT();
		wPhyBlock = sub21510(wLogBlock);
	}
	lvar_r12 = ReadPage(wPhyBlock, byPage, lvar_r12, 1);
	if(!lvar_r12) return lvar_r12;
	//Reset on read error:
	rc = WriteRegisters();
	if(!rc)
	{
		write32(base_addr + 0x190, var_x104 | 0x2707);
		write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
		write32(base_addr + 0x188, 0x3c);
		write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
		write32(base_addr + 0x188, 0);
		int t_val = read32(base_addr + 0x190);
		KeClearEvent(&ms_event_x0c8);
		KeSynchronizeExecution(ClearStatus, &dwMS_STATUS, card_int);
		write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
		write32(base_addr + 0x184, 0xe001);
		rc = WaitForRDY();
		write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
		if(!rc) sub21DA0();
	}
	var_x104 = 0x4010;
	sub22370(0x1f001f00);
	if(lvar_r12 == 0x43 || lvar_r12 == 0x40) // data or extra data error
	{
		sub23CC0(wPhyBlock);
		if(wLogBlock >= 0x2000) return lvar_r12;
		var_x172[wLogBlock] = FFPhyBlock;
	}
	return lvar_r12;
}

char
CMS::WriteSector(int arg_1, short arg_2, char arg_3)
{
	int wLogBlock; // ebx
	long wPhyBlock, wPhyBlock_r; // bp
	int byPage; //r12
	char rc;

	if(byBlockSize == 16)
	{
		wLogBlock = arg_1 >> 4;
	}
	else if(byBlockSize == 32)
	{
		wLogBlock = arg_1 >> 5;
	}
	else
	{
		wLogBlock = arg_1 / byBlockSize;
	}
	if(wLogBlock >=  mwUserBlocks) return 0x82;
	byPage = arg_1 % byBlockSize;
	wPhyBlock = wLogBlock < 0x2000 ? var_x172[lvar_esi] : -1;
	while(!var_x170)
	{
		if(wPhyBlock == -1) break;
		MakeLUT();
		wPhyBlock = sub21510(wLogBlock);
	}
	wPhyBlock_r = wPhyBlock;
	rc = CopyPages(wLogBlock, &wPhyBlock, byPage, arg_2, arg_3);
	if(rc == 0x52)
	{
		rc = CloserWrite();
		if(!rc) rc = CopyPages(wLogBlock, &wPhyBlock, byPage, arg_2, arg_3);
	}
	if(!rc)
	{
		if(wPhyBlock == wPhyBlock_r || wLogBlock >= 0x2000) return 0;
		var_x172[wLogBlock] = wPhyBlock;
		return 0;
	}
	//write error
	if(rc != 0x81) return rc;
	if(!WriteRegisters())
	{
		write32(base_addr + 0x190, var_x104 | 0x2707);
		write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
		write32(base_addr + 0x188, 0x3c);
		write32(base_addr + 0x190, 0x0100 | read32(base_addr + 0x190));
		write32(base_addr + 0x188, 0);
		int t_val = read32(base_addr + 0x190);
		KeClearEvent(&ms_event_x0c8);
		KeSynchronizeExecution(ClearStatus, &dwMS_STATUS, card_int);
		write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
		write32(base_addr + 0x184, 0xe001);
		rc = WaitForRDY();
		write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
		if(!rc) sub21DA0();

	}
	var_x104 = 0x4010;
	sub22370(0x1f001f00);
	return 0x81;
}
