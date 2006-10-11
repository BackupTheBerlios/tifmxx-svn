static char
CMSPro::HandleDATA(char *arg_1)
{
	return ((*arg_1) >> 1) & 1;
}

static char
CMSPro::sub217b0(char *arg_1)
{
	return ((*arg_1) >> 2) & 1;
}

char
CMSPro::GetInt()
{
	write32(base_addr + 0x190, 0x2607 | var_x104);
	int t_val = read32(base_addr + 0x190);
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(card_int, ClearStatus, &dwMS_STATUS);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0x7001);
	char rc = WaitForRDY();
	write32(base_addr + 0x190, t_val & 0xfffeffff);
	ms_regs.status.interrupt = read32(base_addr + 0x188);
	read32(base_addr + 0x188);
	return rc;
}

CMSPro::CMSPro(char *_base_addr, char _serial_mode)
       :CMS(_base_addr, _serial_mode), muiMediaID(0x22)
{
}

char
CMSPro::ExCmd(char arg_1, short arg_2, int arg_3)
{
	int lvar_esi = (((arg_3 & 0xff0000) | (arg_3 >> 0x10)) >> 8) | (((arg_3 & 0xff00) | (arg_3 << 0x10)) << 8);
	write32(base_addr + 0x190, var_x104 | 0x2707);
	write32(base_addr + 0x190, 0x100 | read32(base_addr + 0x190));
	write32(base_addr + 0x188, arg_1 | ((arg_2 & 0xff) << 0x10) | (arg_2 & 0xff00) | (lvar_esi << 0x18));
	write32(base_addr + 0x190, 0x100 | read32(base_addr + 0x190));
	write32(base_addr + 0x188, lvar_esi >> 8);
	int t_val = read32(base_addr + 0x190);
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(card_int, ClearStatus, &dwMS_STATUS);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0x9007);
	char rc = WaitForRDY();
	write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
	if(rc) return 0x81;
	rc = WaitForMSINT();
	if(rc) return rc;
	if(KeSynchronizeExecution(card_int, sub1c050, &dwMS_STATUS)) return 0x81;
	return 0;
}

char
CMSPro::ReadSector(int arg_1, short arg_2, char arg_3)
{
	char rc;
	if(arg_3)
	{
		rc = ExCmd(0x20, arg_2, arg_1);
		if(rc) return rc;
	}
	else
	{
		rc = WaitForMSINT();
		if(rc) return rc;
	}

	if(!mbParallelInterface)
	{
		rc = GetInt();
		if(rc) return rc;
		if(!(ms_regs.status.interrupt & 0x20)) return 0x4e;
	}
	else
	{
		if(!KeSynchronizeExecution(card_int, &CMSPro::HandleDATA, &dwMS_STATUS) &&
		   !(2 & read32(base_addr + 0x18c))) return 0x4e;
	}
	if(KeSynchronizeExecution(card_int, sub217b0, &dwMS_STATUS)) return 0x51;
	rc = sub219f0(2, 0x200);
	return rc;
}

char
CMSPro::WriteSector(int arg_1, short arg_2, char arg_3)
{
	char rc;
	if(arg_3)
	{
		if(var_x0e1 == 3 && (bySegments + 1 == arg_1))
		{
			rc = WaitForMSINT();
			if(rc) return rc;
		}
		else
		{
			CloseWrite();
			rc = ExCmd(0x21, 0, arg_1);
			if(rc) return rc;
		}
	}
	else
	{
		rc = WaitForMSINT();
		if(rc) return rc;
	}
	bySegments = arg_1;
	if(mbParallelInterface)
	{
		if(!KeSynchronizeExecution(card_int, &CMSPro::HandleDATA, &dwMS_STATUS)
		   && !(2 & read32(base_addr + 0x18c))) return 0x4e;
	}
	else
	{
		rc = GetInt();
		if(rc) return rc;
		if(!(0x20 & ms_regs.status.interrupt)) return 0x4e;
	}
	if(KeSynchronizeExecution(card_int, sub217b0, &dwMS_STATUS)) return 0x51;
	rc = sub219f0(0xd, 0x200);
	if(rc) return rc;
	if(arg_2 == 1) var_x0e1 = 3;
	return 0;
}

char
CMSPro::CloseWrite()
{
	char rc;

	if(var_x0e1 != 3) return 0;
	rc = WaitForMSINT(); vara_6 = 0;
	if(rc) return rc;
	rc = Cmd(0x25, 1, 0, 0);
	if(rc) return rc;
	var_x0e1 = 0;
	return 0;
}

char
CMSPro::GetCHS(arg_1)
{
	mwCylinders = be16_to_cpu(arg_1->x0);
	mwSectorsPerTrack = be16_to_cpu(arg_1->x8);
	mwHeadCount = be16_to_cpu(arg_1->x2);
	return 0;
}

char
CMSPro::SwitchToParallelIF()
{
	char rc;
	var_x104 = 0x4010;
	for(int cnt = 0;  cnt <=2; cnt++)
	{
		ms_regs.param.system = 0;
		rc = WriteRegisters();
		if(!rc) break;
	}
	if(rc) return 0x4c;
	var_x104 = 0;
	return 0;
}

char
CMSPro::ConfirmCPUStartup()
{
	long long t1 = KeQuerySystemTime();
	do
	{
		write32(base_addr + 0x190, var_x104 | 0x2607);
		int t_val = read32(base_addr + 0x190);
		KeClearEvent(&ms_event_x0c8);
		KeSynchronizeExecution(card_int, ClearStatus, &dwMS_STATUS);
		write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
		write32(base_addr + 0x184, 0x7001);
		for(int cnt = 2; cnt >= 0; cnt--)
		{
			rc = sub1fee0(&ms_event_x0c8, -5000000);
			if(vara_4)
			{
				rc = 0x86;
				break;
			}
			if(!KeSynchronizeExecution(card_int, HandleError, &dwMS_STATUS))
			{
				rc = 0x47;
				break;
			}
			if(KeSynchronizeExecution(card_int, HandleRDY, &dwMS_STATUS))
			{
				rc = 0;
				break;
			}
			if(!cnt)
			{
				rc = read32(base_addr + 0x18c) & 0x1000 ? 0 : 0x87;
			}
		}
		write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
		ms_regs.status.interrupt = read32(base_addr + 0x188);
		read32(base_addr + 0x188);
		if(rc) return rc;
		if((0x80 & ms_regs.status.interrupt) && !(0x40 & ms_regs.status.interrupt)) return 0;
	}while(KeQuerySystemTime() - t1 < 1000000);
	return 0x4d;
}

char
CMSPro::Format(char arg_1)
{
	ms_regs.param.block_address[0] = 1;
	ms_regs.param.block_address[1] = arg_1 ? 0 : 1;
	char rc = WriteRegisters();
	if(rc) return rc;
	Cmd(0x10, 0, 1, 0);
	int cnt = 3, t_val, lvar_r12, lvar_r14 = 0, lvar_r13 = 1;
	do
	{
		while(!KeSynchronizeExecution(card_int, HandleMSINT, &dwMS_STATUS))
		{
			rc = sub1fee0(&ms_event_x0c8, -5000000);
			if(vara_4) return 0x86;
			cnt--;
			if(!cnt)
			{
				if(!(0x2000 & readl(base_addr + 0x18c)) && rc) return rc;
			}
		}
		if(KeSynchronizeExecution(card_int, sub1c050, &dwMS_STATUS)) return 0x81; // CMDNK
		lvar_r12 = read32(base_addr + 0x18c);
		if(!(t_val & 0x8))
		{
			write16(base_addr + 0x24, 1);
			write16(base_addr + 0x10, 0x100);
			t_val = read32(base_addr + 0x190);
			KeClearEvent(&ms_event_x0c8);
			KeSynchronizeExecution(card_int, ClearStatus, &dwMS_STATUS);
			write32(base_addr + 0x190, t_val | 0x10000 | 0x0800);
			write32(base_addr + 0x184, 0x2200);
			rc = WaitForRDY();
			write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190);
			if(rc) return rc;
			lvar_r13 = (sub1fc40(0) << 24)(sub1fc40(1) << 16)(sub1fc40(2) << 8) | sub1fc40(3);
			lvar_r14 = (sub1fc40(4) << 24)(sub1fc40(5) << 16)(sub1fc40(6) << 8) | sub1fc40(7);
		}
	}while(lvar_r13 != lvar_r14 || lvar_r12);
	return Cmd(0xcc, 1, 0, lvar_r12 & 0xff);
}

char
CMSPro::RescueRWFail()
{
	char rc = CFlash::RescueRWFail();
	write32(base_addr + 0x190, 0x8000);
	write32(base_addr + 0x190, 0x0a00);
	write32(base_addr + 0x18c, 0xffffffff);
	dwMS_STATUS = 0;
	int interval = -1000000;
	KeDelayExecutionThread(0, 0, &interval);
	var_x104 = 0x4010;
	return InitializeCard(); 
}

CMSPro::~CMSPro()
{
	write32(base_addr + 0x190, 0x0a00);
	write32(base_addr + 0x18c, 0xffffffff);
	dwMS_STATUS = 0;
}

char
CMSPro::MediaModel()
{
	char rc;

	for(int cnt = 0; cnt < 0x400; cnt++)
	{
		write16(base_addr + 0x24, 1);
		write16(base_addr + 0x10, 0x100);
		rc = ExCmd(0x24, 1, cnt >> 9);
		if(rc) return rc;
		if(mbParallelInterface)
		{
			if(KeSynchronizeExecution(card_int, HandleDATA, &dwMS_STATUS)) goto 27403;
			return 0x4e; // breq error
		}
		write32(base_addr + 0x190, 0x2607 | var_x104);
		t_val = read32(base_addr + 0x190);
		KeClearEvent(&ms_event_x0c8);
		KeSynchronizeExecution(card_int, ClearStatus, &dwMS_STATUS);
		write32(base_addr + 0x190, (0xfffeffff & t_val) | 0x0800);
		write32(base_addr + 0x184, 0x7001);
		for(cnt1= 2; cnt1 >= 0; cnt1--)
		{
			rc = sub1fee0(ms_event_x0c8, -5000000);
			if(vara_4)
			{
				rc = 0x86;
				break;
			}
			if(!KeSynchronizeExecution(card_int, HandleError, &dwMS_STATUS))
			{
				rc = 0x47;
				break;
			}
			if(KeSynchronizeExecution(card_int, HandleRDY, &dwMS_STATUS))
			{
				rc = 0;
				break;
			}
		}
		if(!cnt1 && !(0x1000 & read32(base_addr + 0x18c))) rc = 0x87;
		else rc = 0;
		write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190);
		ms_regs.status.interrupt = read32(base_addr + 0x188);
		read32(base_addr + 0x188);
		if(rc) return rc;
		if(!(ms_regs.status.interrupt & 0x20)) return 0x4e;
27403:
		if(KeSynchronizeExecution(card_int, sub217b0, &dwMS_STATUS)) return 0x51;
		t_val = read32(base_addr + 0x190);
		KeClearEvent(&ms_event_x0c8);
		KeSynchronizeExecution(card_int, ClearStatus, &dwMS_STATUS);
		write32(base_addr + 0x190, t_val | 0x10000 | 0x0800);
		write32(base_addr + 0x184, 0x2200);
		for(cnt1= 2; cnt1 >= 0; cnt1--)
		{
			rc = sub1fee0(ms_event_x0c8, -5000000);
			if(vara_4)
			{
				rc = 0x86;
				break;
			}
			if(!KeSynchronizeExecution(card_int, HandleError, &dwMS_STATUS))
			{
				rc = 0x47;
				break;
			}
			if(KeSynchronizeExecution(card_int, HandleRDY, &dwMS_STATUS))
			{
				rc = 0;
				break;
			}
		}
		if(!cnt1 && !(0x1000 & read32(base_addr + 0x18c))) rc = 0x87;
		else rc = 0;
		write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190);
		if(rc) return rc;
		for(cnt1 = 0; cnt1 < 0x200; cnt1++)
		{
			lvar_x438[cnt++] = sub1fc40(cnt1);
		}
	}

	char byDevInfEntryCount = lvar_x438[4];
	if(byDevInfEntryCount < 1 || byDevInfEntryCount > 12) return 0x50;
	for(cnt = 16; cnt; cnt += 12)
	{
		int byID = *(lvar_x438 + cnt)[8];
		int wAddress = *(lvar_x438 + cnt)[3] + (*(lvar_x438 + cnt)[2] << 8);
		int wSize = *(lvar_x438 + cnt)[7] + (*(lvar_x438 + cnt)[6] << 8);
		if(wAddress >= 0x1a0 && wSize && wAddress + wSize < 0xf800)
		{
			if(byID == 16) // system information
			{
				SerialNumber = *(lvar_x438 + wAddress)[17] + ((*(lvar_x438 + wAddress)[16] + (*(lvar_x438 + wAddress)[15] + (*(lvar_x438 + wAddress)[14] << 8)) << 8) << 8);
			}
			else if(byID == 32)
			{
				//MBR entry
			}
			else if(byID == 33)
			{
				//PBR entry
			}
			else if(byID == 48)
			{
				//PC card information
				rc = GetCHS(*(lvar_x438 + wAddress)[0]);
			}
			else if(byID == 64)
			{
				//information block
			}
			else
			{
				//invalid
			}
		}
		byDevInfEntryCount--;
		if(rc || !byDevInfEntryCount) break;
	}
	return rc;
}

char
CMSPro::InitializeCard()
{
	vara_2 = 0;
	char rc = sub22370(0x1f001f00);
	if(rc) return rc;
	rc = sub21da0();
	if(rc) return rc;
	if(mbParallelInterface && (rc = SwitchToParallelIF())) return rc;
	rc = ConfirmCPUStartup();
	if(rc) return rc;
	if(mbParallelInterface)
	{
		write16(base_addr + 4, 0x100 | read16(base_addr + 4));
	}
	rc = MediaModel();
	if(rc) return rc;
	rc = sub21da0();
	if(rc) return rc;
	vara_2 = 1;
	mbWriteProtected = ms_regs.status.status0 & 1;
	return 0;
}
