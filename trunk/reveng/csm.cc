
void
CSM::sub27910(char arg_1, short arg_2, short arg_3)
{
	if(arg_1 >=  mbyZones || arg_2 >= mwLogBlocks) return;
	var_x148[mwLogBlocks * arg_1 + arg_2] = arg_3;
}

short
CSM::sub27950(char arg_1, short arg_2)
{
	if(arg_1 >=  mbyZones || arg_2 >= mwLogBlocks) return -1;
	return var_x148[mwLogBlocks * arg_1 + arg_2];
}

CSM::CSM(char *_base_addr)
    :CFlash(_base_addr), dwSM_STATUS(0), var_x0c4(0), var_x0c8(0), var_x112(0), var_x16c(0), var_x164(0),
     var_x168(0), var_x148({0}), var_x0f8({0}), var_x15d(0), var_x15e(0), var_x0a4(0), var_x162(0), var_x16e(0),
     var_x10a(-1), var_x10c(-1), var_x10e(-1), var_x110(-1), var_x113(-1), var_x114(-1), var_x116(-1), var_x118(-1),
     var_x11a(32), var_x124(-1), var_x126(-1), var_x16a(-1), var_x16d(-1), var_x160(-1), var_x15f(-1)
{
	KeInitializeEvent(&sm_event_x0d0, 0, 0);
	write32(base_addr + 0x90, 0xffffffff);

}

char
CSM::Isr(char arg_1, char arg_2)
{
	char rc;

	rc = CFlash::Isr(arg_1, arg_2);
	if(arg_2)
	{
		var_x0c4 = dwSM_STATUS | read32(base_addr + 0x98);
		var_x0c8 |= read32(base_addr + 0x90);
		write32(base_addr + 0x90, var_x0c8);
	}
	return rc | arg_2;
}

int
CSM::IsrDpc()
{
	int rc = CFlash::IsrDpc();
	if(vara_0)
	{
		dwSM_STATUS = var_x0c4;
		KeSetEvent(&sm_event_x0d0, 0, 0);
		vara_0 = 0;
	}
	return rc;
}

char
CSM::CheckTimeOut(int *status_var)
{
	return *status_var & 4 ? 1 : 0;
}

char
CSM::CheckDataErr(int *status_var)
{
	return *status_var & 2 ? 1 : 0;
}

char
CSM::CheckLogicErr(int *status_var)
{
	return *status_var & 0x03800000 ? 1 : 0;
}

char
CSM::CheckDSErr(int *status_var)
{
	return *status_var & 0x00400000 ? 1 : 0;
}

char
CSM::CheckCorrEcc(int *status_var)
{
	return *status_var & 0x00100000 ? 1 : 0;
}

char
CSM::CheckUnCorrEcc(int *status_var)
{
	return *status_var & 0x00080000 ? 1 : 0;
}

char
CSM::CheckFlashErr(int *status_var)
{
	return *status_var & 0x00010000 ? 1 : 0;
}

char
CSM::WaitForCMD(char arg_1, char arg_2, char arg_3)
{
	char rc;

	KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
	write32(base_addr + 0x94, (arg_1 & 0xf) | (arg_2 << 8));
	if(!arg_3) return 0;
	rc = sub1fee0(&sm_event_x0d0, -1000000);
	if(rc) return rc;
	if(vara_4) return 0x86;
	if(KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) return 0x6a;
	if(KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) return 0x62;
	return 0;
}

void
CSM::ResetCard()
{
	char rc;
	for(int cnt = 0; cnt < 256; cnt++)
	{
		KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
		write32(base_addr + 0x94, 2);
		rc = sub1fee0(&sm_event_x0d0, -1000000);
		if(!rc && vara_4) rc = 0x86;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) rc = 0x62;
		int dwCardStatusID = read32(base_addr + 0xa8);
		if(rc == 0x6a) rc = 0;
		if(dwCardStatusID & 0x40 || rc) break;
	}
}

char
CSM::ReadXDId()
{
	char rc;
	if(!is_xx12)
	{
		KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
		write32(base_addr + 0x94, 7);
		rc = sub1fee0(&sm_event_x0d0, -1000000);
		if(rc) return rc;
		if(vara_4) return 0x86;
		if(KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
		if(KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) rc = 0x62;
		char byXDIdCode = (read32(base_addr + 0xa8) >> 8) & 0xff;
		int SM_CARD_STATUS_ID = read32(base_addr + 0xa8);
		if(byXDIdCode == 0xb5)
		{
			muiMediaID = 4; // new style xD card
			return 0;
		}
		if(!SMEnable) return 0x67;
		return 0; // SmartMedia card
	}
	else
	{
		if(0x0800 & read16(base_addr + 0x8))
		{
			rc = WaitForCMD(7, 0, 1);
			if(rc) return rc;
			char byXDIdCode = (read32(base_addr + 0xa8) >> 8) & 0xff;
			int SM_CARD_STATUS_ID = read32(base_addr + 0xa8);
			if(byXDIdCode == 0xb5)
			{
				muiMediaID = 4; // new style xD card
				return 0;
			}
			else return 0x67;
		}
		else
		{
			if(!SMEnable) return 0x67;
			muiMediaID = 1;
			return 0;
		}
	}
}

char
CSM::ReadID()
{
	char rc;
	KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
	write32(base_addr + 0x94, 1);
	rc = sub1fee0(&sm_event_x0d0, -1000000);
	if(!rc && vara_4) rc = 0x86;
	if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
	if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) rc = 0x62;
	if(!rc)
	{
		char byIDCode = read32(base_addr + 0xa8) >> 0x18;
		int SM_CARD_STATUS_ID = read32(base_addr + 0xa8);
		switch(byIDCode - 100)
		{
			case 0:
			case 134: // 2851f
				mwSize = 2;
				break;
			case 7:
			case 127:
			case 129: // 28527
				mwSize = 4;
				break;
			case 10:
			case 132:
			case 136: // 28517
				mwSize = 1;
				break;
			case 13: // 28557
				mwSize = 256;
				break;
			case 15: // 28537
				mwSize = 16;
				break;
			case 17: // 2853f
				mwSize = 32;
				break;
			case 18: // 28547
				mwSize = 64;
				break;
			case 21: // 2854f
				mwSize = 128;
				break;
			case 111: // 28567
				mwSize = 1024;
				break;
			case 113: // 2856f
				mwSize = 2048;
				break;
			case 120: // 2855f
				mwSize = 512;
				break;
			case 130: // 2852f
				mwSize = 8;
				break;
		default: // 28577
			mwSize = 0;
		}
	}
	return !mwSize ? 0x67 : rc;
}
