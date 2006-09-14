

char
CMS::WaitForMSINT()
{
	char rc;

	if(KeSynchronizeExecution(&CMS::HandleMSINT, &dwMS_STATUS, card_int)) return 0;

	for(int cnt = 6; cnt; cnt--) {
		rc = sub_1FEE0(&ms_event_x0c8, -5000000);
		if(vara_4) return 0x86;
		if(KeSynchronizeExecution(&CMS::HandleMSINT, &dwMS_STATUS, card_int)) return 0;
	}
	if(!(0x2000 & read32(base_addr + 0x18c))) return rc;
}

char
CMS::sub219F0(char arg_1, short arg_2)
{
	int t_val = read32(base_addr + 0x190) & 0xfffeffff;

	if(arg_1 == 2 || arg_1 == 13) t_val |= 0x10000;
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(&CMS::ClearStatus, &dwMS_STATUS, card_int);
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
			if(var_x0e3[0x10] == 0x88 || !var_x104) lvar_x38 = 0x88;
			else lvar_x38 = 0x80;

		}
		else lvar_x48[cnt] = var_x0e3[cnt]; 
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
		if(!KeSynchronizeExecution(CMS::HandleError, &dwMS_STATUS, card_int))
		{
			rc = 0x47;
			break;
		}
		if(KeSynchronizeExecution(CMS::HandleRDY, &dwMS_STATUS, card_int))
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
CMS::Cmd(char arg_1, char arg_2, char arg_3, char arg_4)
{
	char rc;

	if(arg_4 && (rc = WriteRegisters())) return rc;
	write32(base_addr + 0x190, var_x104 | 0x2707);
	write32(base_addr + 0x190, 0x100 | read32(base_addr + 0x190));
	write32(base_addr + 0x188, arg_1);
	write32(base_addr + 0x190, 0x100 | read32(base_addr + 0x190));
	write32(base_addr + 0x188, 0);
	int t_val = read32(base_addr + 0x190);
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(&CMS::ClearStatus, &dwMS_STATUS, card_int);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0xe001);
	rc = WaitForRDY();
	write32(base_addr + 0x190, t_val & 0xfffeffff);
	if(rc) return 0x81;
	if(arg_2 && (rc = WaitForMSINT())) return rc;
	if(arg_3) rc = sub21DA0;
	return rc;
}

char
CMS::sub22260()
{
	write32(base_addr + 0x190, var_x104 | 0x2607);
	int t_val = read32(base_addr + 0x190);
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(&CMS::ClearStatus, &dwMS_STATUS, card_int);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0x7001);
	char rc = WaitForRDY();
	write32(base_addr + 0x190, t_val & 0xfffeffff);
	var_x0e3[1] = read32(base_addr + 0x188);
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
CMS::ReadPage(short arg_1, char arg_2, char arg_3, int arg_4)
{
	if(arg_3 & 0x20)
	{
		var_x0e3[20] = 0x20;
		var_x0e3[16] = var_x104 ? 0x80 : 0x88; 
	}
	if(arg_3 & 0x01)
	{
		var_x0e3[20] = 0;
		var_x0e3[16] = var_x104 ? 0x80 : 0x88;
	}
22847: ...
}
