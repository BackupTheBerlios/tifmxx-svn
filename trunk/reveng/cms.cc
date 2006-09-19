

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
    :CMemoryStick(_base_addr), dwMS_STATUS(0), serial_mode(_serial_mode), var_x0e1(0), var_x0e2(0),
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
