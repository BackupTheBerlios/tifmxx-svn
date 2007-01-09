
void
CSM::sub20000(short arg_1)
{
	if(var_x0a4 < 8)
	{
		mwWrittenBlocks[var_x0a4] = arg_1;
		var_x0a4++;
	}
}

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
    :CFlash(_base_addr), dwSM_STATUS(0), var_x0c4(0), var_x0c8(0), var_x112(0), var_x16c(0), mdwDMAaddressReg(0),
     mwDMAcontrolReg(0), var_x148({0}), mpFirstEmptyPhyBlock({0}), var_x15d(0), var_x15e(0), mbWriteBlockPos(0), var_x162(0), var_x16e(0),
     var_x10a(-1), var_x10c(-1), mwHeadSrcPhyBlock(-1), var_x110(-1), var_x113(-1), var_x114(-1), mwTailSrcPhyBlock(-1), var_x118(-1),
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

	KeSynchronizeExecution(card_int, ClearStatus, &dwSM_STATUS);
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

char
CSM::MediaModel()
{
	int t_val;
	if(mwSize >= 64)
	{
		switch(mwSize)
		{
			case 64:
				mbyZones = 4;
				break;
			case 128:
				mbyZones = 8;
				break;
			case 256:
				mbyZones = 16;
				break;
			case 512:
				mbyZones = 32;
				break;
			case 1024:
				mbyZones = 64;
				break;
			case 2048:
				mbyZones = 128;
				break;
		}
		t_val = 3;
		mwPageSize = 528;
		mbyLogBlockSize = 32;
		mbyPhyBlockSize = 32;
		mwLogBlocks = 1000;
		mwPhyBlocks = 1024;
	}
	else if(mwSize <= 32)
	{
		switch(mwSize)
		{
			case 1: // 28794
				t_val = 0;
				mbyZones = 1;
				mwPhyBlocks = 256;
				mwLogBlocks = 250;
				mbyPhyBlockSize = 16;
				mbyLogBlockSize = 8;
				mwPageSize = 264;
				break;
			case 2: // 287b1
				t_val = 0;
				mbyZones = 1;
				mwPhyBlocks = 512;
				mwLogBlocks = 500;
				mbyPhyBlockSize = 16;
				mbyLogBlockSize = 8;
				mwPageSize = 264;
				break;
			case 4: // 287e8
				t_val = 1;
				mbyZones = 1;
				mwPhyBlocks = 512;
				mwLogBlocks = 500;
				mbyPhyBlockSize = 16;
				mbyLogBlockSize = 16;
				mwPageSize = 528;
				break;
			case 8: // 28822
				t_val = 1;
				mbyZones = 1;
				mwPhyBlocks = 1024;
				mwLogBlocks = 1000;
				mbyPhyBlockSize = 16;
				mbyLogBlockSize = 16;
				mwPageSize = 528;
				break;
			case 16: // 2885c
				t_val = 1;
				mbyZones = 1;
				mwPhyBlocks = 512;
				mwLogBlocks = 500;
				mbyPhyBlockSize = 32;
				mbyLogBlockSize = 32;
				mwPageSize = 528;
				break;
			case 32: // 2886a
				t_val = 1;
				mbyZones = 2;
				mwPhyBlocks = 512;
				mwLogBlocks = 500;
				mbyPhyBlockSize = 32;
				mbyLogBlockSize = 32;
				mwPageSize = 528;
				break;
			default: // 288d0
				return 0x67;
		}
	}
	else return 0x67;

	write32(base_addr + 0x9c, read32(base_addr + 0x9c) & 0xfffffffc | t_val);
	int SM_CONTROL = read32(base_addr + 0x9c);
	return 0;
}

char
CSM::GetEmptyPhyBlock(char zone, short *arg_2)
{
	short wPhyBlock = mpFirstEmptyPhyBlock[zone];
	if(wPhyBlock == -1) return 0x84;
	short wNextEmptyPhyBlock = wPhyBlock + 1;
	if(t_block > mwPhyBlocks) t_block = 0;
	do
	{
		if(zone < mbyZones && wNextEmptyPhyBlock < mwPhyBlocks)
		{
			if(!(var_x150[(mwPhyBlocks >> 3) * zone + (wNextEmptyPhyBlock >> 3)] & (1 << (wNextEmptyPhyBlock & 7)))) break;
		}
		if(wNextEmptyPhyBlock == wPhyBlock) break;

		wNextEmptyPhyBlock++;
		if(wNextEmptyPhyBlock < mwPhyBlocks) continue;
		t_block = 0;
	}while(1);

	if(wNextEmptyPhyBlock == wPhyBlock) // no free blocks
	{
		mbWriteProtected = 1;
		return 0x84;
	}

	mpFirstEmptyPhyBlock[zone] = wNextEmptyPhyBlock;
	*arg_2 = wNextEmptyPhyBlock;
	return 0;
}

char
CSM::sub28b80(char arg_1, short arg_2, char arg_3)
{
	char rc;

	if(mwPageSize < 512) arg_3 += arg_3;
	write32(base_addr  + 0xa0, (mwPhyBlocks * arg_1 + arg_2) * mbyPhyBlockSize + arg_3);
	KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
	write32(base_addr + 0x94, 0x109);
	rc = sub1fee0(&sm_event_x0d0, -1000000);
	if(rc) return rc;
	if(vara_4) return 0x86;
	if(KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) return 0x6a;
	if(KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) return 0x62;
	return 0;
}

char
CSM::sub28cb0(short *arg_1)
{
	int t_val = read32(base_addr + 0xb4);
	int x_val = (read32(base_addr + 0xb8) << 8) | (read32(base_addr + 0xbc) >> 24);
	int cnt = 0;
	for(int t_valx = t_val >> 1; t_valx; t_valx >> 1) { if(t_valx & 1) cnt++; };
	if((cnt & 0xfffffffe) == (t_val & 1))
	{
		*arg_1 = (t_val >> 1) & 0x3ff;
		if(*arg_1 < mwPhyBlocks) return 0;
	}

	cnt = 0;
	for(int x_valx = x_val >> 1; x_valx; x_valx >> 1) { if(x_valx & 1) cnt++; };
	if((cnt & 0xfffffffe) == (x_val & 1))
	{
		*arg_1 = (x_val >> 1) & 0x3ff;
		if(*arg_1 < mwPhyBlocks) return 0;
	}
	return 0x66;
}

char
CSM::ReadPage(char byZone, short wPhyBlock, char byPage, char arg_4)
{
	char rc = 0;
	if(mwPageSize < 512) byPage += byPage;
	write32(base_addr + 0x9c, 0xffffffdf & read32(base_addr + 0x9c));
	write32(base_addr + 0x9c, 0x10 | read32(base_addr + 0x9c));
	if(arg_4)
	{
		write32(base_addr + 0x9c, 0xffffffef & read32(base_addr + 0x9c));
		write32(base_addr + 0x9c, 0x80 | read32(base_addr + 0x9c));
		write32(base_addr + 0x9c, 0xffffff7f & read32(base_addr + 0x9c));
		write16(base_addr + 0x24, 1);
		write16(base_addr + 0x10, 0x100);
		write32(base_addr + 0xa0, (mwPhyBlocks * byZone + wPhyBlock) * mbyPhyBlockSize + byPage);
		KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
		write32(base_addr + 0x94, 0x108);
		for(int cnt = 0; cnt < 0x80, cnt++)
		{
			while(!(0x200 & read32(base_addr + 0x98)))
			{
				sleep(-64);
				if(vara_4) return 0x86;
			}
			write32(base_addr + 0x200 + (cnt << 2), read32(base_addr + 0xa4));
		}
		if(sub1fee0(sm_event_x0d0, -1000000)) return 0x87;
		if(vara_4) return 0x86;
		if(KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) return 0x6a;
		if(!KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) return 0;
		rc = 0x62;
	}
	else
	{
		write32(base_addr + 0xa0, (mwPhyBlocks * byZone + wPhyBlock) * mbyPhyBlockSize + byPage);
		KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
		write32(base_addr + 0x94, 0x108);
		if(sub1fee0(sm_event_x0d0, -1000000)) return 0x87;
		if(vara_4) rc = 0x86;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) return 0x62;
	}
	if(!var_x15c)
	{
		if(KeSynchronizeExecution(card_int, &CSM::CheckLogicErr, dwSM_STATUS))
		{
			short wLogBlock;
			if(sub28cb0(&wLogBlock)) return 0x66;
			//->print
			rc = 0;
		}
		if(KeSynchronizeExecution(card_int, &CSM::CheckUnCorrEcc, dwSM_STATUS))
		{
			return 0x69;	
		}
		if(KeSynchronizeExecution(card_int, &CSM::CheckCorrEcc, dwSM_STATUS))
		{
			return 0x68;	
		}
		if(KeSynchronizeExecution(card_int, &CSM::CheckDSErr, dwSM_STATUS))
		{
			return 0x69;	
		}
	}
	return 0;
}

char
CSM::MarkBadBlock(char byZone, short wPhyBlock, char arg_3)
{
	int t_val = arg_3 ? 0xfff0ffff : 0xff00ffff;
	char rc = 0;
	char c_page;

	for(int cnt = 0; cnt < mbyLogBlockSize; cnt++)
	{
		KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
		write32(base_addr + 0x94, 0); // CMD RST CHIP
		rc = sub1fee0(sm_event_x0d0, -1000000);
		if(!rc && vara_4) rc = 0x86;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) rc = 0x62;

		c_page = mwPageSize < 512 ? cnt * 2 : cnt;
		write32(base_addr + 0xa0, (mwPhyBlocks * byZone + wPhyBlock) * mbyPhyBlockSize + c_page);
		KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
		write32(base_addr + 0x94, 0x109);
		rc = sub1fee0(sm_event_x0d0, -1000000);
		if(!rc && vara_4) rc = 0x86;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) rc = 0x62;
		write32(base_addr + 0x9c, 0x20 | read32(base_addr + 0x9c));
		write32(base_addr + 0xb4, t_val & read32(base_addr + 0xb4)); // SM_REDUNDANT_DATA

		write32(base_addr + 0xa0, (mwPhyBlocks * byZone + wPhyBlock) * mbyPhyBlockSize + c_page);
		KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
		write32(base_addr + 0x94, 0x10d);
		rc = sub1fee0(sm_event_x0d0, -1000000);
		if(!rc && vara_4) rc = 0x86;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) rc = 0x62;
		if(rc) break;
	}
	return rc;
}

char
CSM::FillUpBlock(char arg_1, short arg_2, short arg_3, char arg_4, char arg_5)
{
	int c_page;
	char rc = 0;
	while(arg_4 < arg_5)
	{
		int t_val = arg_2 | 0xfffffc00;
		KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
		write32(base_addr + 0x94, 0);
		rc = sub1fee0(sm_event_x0d0, -1000000);
		if(!rc && vara_4) rc = 0x86;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) rc = 0x62;

		c_page = mwPageSize < 512 ? arg_4 * 2 : arg_4;
		write32(base_addr + 0xa0, (mwPhyBlocks * arg_1 + arg_3) * mbyPhyBlockSize + c_page);
		KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
		write32(base_addr + 0x94, 0x109);
		rc = sub1fee0(sm_event_x0d0, -1000000);
		if(!rc && vara_4) rc = 0x86;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) rc = 0x62;
		write32(base_addr + 0xb0, 0xffffffff);
		write32(base_addr + 0xb4, 0xffffffff);
		write32(base_addr + 0xb8, 0xffffffff);
		write32(base_addr + 0xbc, 0xffffffff);
		write32(base_addr + 0xc0, t_val);

		write32(base_addr + 0xa0, (arg_1 * byZone + arg_3) * mbyPhyBlockSize + c_page);
		KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
		write32(base_addr + 0x94, 0x10d);
		rc = sub1fee0(sm_event_x0d0, -1000000);
		if(!rc && vara_4) rc = 0x86;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) rc = 0x62;
		arg_4++;
	}

	KeSynchronizeExecution(card_int, ClearStatus, dwSM_STATUS);
	write32(base_addr + 0x94, 0);
	rc = sub1fee0(sm_event_x0d0, -1000000);
	if(!rc && vara_4) rc = 0x86;
	if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
	if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) rc = 0x62;
	return rc;
}

short
CSM::PhyBlockDefective()
{
	int cnt;
	char rc = 0;

	for(cnt = 0; cnt < 0x18; cnt++)
	{
		if(found) return c_block;
		write32(base_addr + 0xa0, cnt * mbyPhyBlockSize);
		write32(base_addr + 0x94, 0x109);
		rc = sub1fee0(sm_event_x0d0, -1000000);
		if(!rc && vara_4) rc = 0x86;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) rc = 0x62;
		if(read32(base_addr + 0x98) & 0x8000 || vara_4) return cnt;
	}
	return 0x18;
}

void
CSM::sub29ec0()
{
	switch(mwSize)
	{
		case 1:
			mwHeadCount = 4;
			mwSectorsPerTrack = 4;
			mwCylinders = 125;
			break;
		case 2:
			mwHeadCount = 4;
			mwSectorsPerTrack = 8;
			mwCylinders = 125;
			break;
		case 4:
			mwHeadCount = 4;
			mwSectorsPerTrack = 8;
			mwCylinders = 250;
			break;
		case 8:
			mwHeadCount = 4;
			mwSectorsPerTrack = 16;
			mwCylinders = 250;
			break;
		case 16:
			mwHeadCount = 4;
			mwSectorsPerTrack = 16;
			mwCylinders = 500;
			break;
		case 32:
			mwHeadCount = 8;
			mwSectorsPerTrack = 16;
			mwCylinders = 500;
			break;
		case 64:
			mwHeadCount = 8;
			mwSectorsPerTrack = 32;
			mwCylinders = 500;
			break;
		case 128:
			mwHeadCount = 16;
			mwSectorsPerTrack = 32;
			mwCylinders = 500;
			break;
		case 256:
			mwHeadCount = 16;
			mwSectorsPerTrack = 32;
			mwCylinders = 1000;
			break;
		case 512:
			mwHeadCount = 16;
			mwSectorsPerTrack = 63;
			mwCylinders = 1015;
			break;
		case 1024:
			mwHeadCount = 33;
			mwSectorsPerTrack = 63;
			mwCylinders = 985;
			break;
		case 2048:
			mwHeadCount = 66;
			mwSectorsPerTrack = 63;
			mwCylinders = 985;
			break;
		default:
			mwHeadCount = 0;
			mwSectorsPerTrack = 0;
			mwCylinders = 0;
			break;
	}
}

char
CSM::sub29ff0()
{
	if(!(var_x148 = kzalloc(2 * mwLogBlocks * mbyZones))) return 0x6c;
	if(!(var_x150 = kzalloc(mwPhyBlocks * mbyZones / 8))) return 0x6c;
	if(!(mpFirstEmptyPhyBlock = kzalloc(2 * mbyZones))) return 0x6c;
	if(!(var_x100 = kzalloc(2 * mbyZones))) return 0x6c;
	for(int zcnt = 0; zcnt < mbyZones; zcnt++)
	{
		for(int bcnt = 0; bcnt < mwLogBlocks; bcnt++) var_x148[bcnt * zcnt] = -1;
		for(int bcnt = 0; bcnt < (mwPhyBlocks / 8); bcnt++) var_x150[(mwPhyBlocks >> 3) * zcnt + bcnt] = 0;
	}
	return 0;
}

char
CSM::RescueRWFail()
{
	char rc = CFlash::RescueRWFail(); 
	if(!rc) rc = InitializeCard();
	return rc;
}

char
CSM::CheckCIS()
{
	short lvar_bp = var_x108;
	char rc;
	int t_val;

	int dwCISData[3] = {0x1d90301, 0xdf0218ff, 0x2001}; // x030
	int dwCISCardData[3]; // x040
	int lvar_x48[3];

	write32(base_addr + 0x9c, read32(base_addr + 0x9c) & 0xff7fffff);
	for(int byPage = 0; byPage < mbyLogBlockSize; byPage++)
	{
		rc = ReadPage(0, lvar_bp, byPage, 1);
		if(rc == 0x68 || rc == 0x86) break;
	}
	if(rc && rc != 0x68) return rc;

	for(int cnt = 0; cnt < 0x80; cnt++)
	{
		while(!(0x100 & read32(base_addr + 0x98)))
		{
			sub_1fd80(64); // sleep
			if(vara_4) return 0x86;
		}
		t_val = read32(base_addr + cnt << 2 + 0x200);
		if(cnt == 2 || cnt == 0x42) t_val &= 0xffff;
		if(cnt >= 0 && cnt <= 2) dwCISCardData[cnt] = t_val;
		if(cnt >= 0x40 && cnt <= 0x42) lvar_x48[cnt - 0x40] = t_val;
	}

	int lvar_r12;
	for(int cnt = 0; cnt < 3; cnt++)
	{
		if(rc) break;
		if(dwCISCardData[cnt] != dwCISData[cnt] && lvar_x48[cnt] != dwCISData[cnt]) rc = 0x6e;
	}
	write32(base_addr + 0x9c, read32(base_addr + 0x9c) | 0x800000);
	return rc;
}

char
CSM::EccCorrectData(int dwECCregister)
{
	int byColBitAddr = 0;
	int byLineAddr = 0;
	short lvar_r12 = 0;
	char byData;
	int lvar_bl = 1;

	if(dwECCregister & 0x80000000 || dwECCregister & 0x8000) return 0x69;
	if(dwECCregister & 0x4000)
	{
		byColBitAddr = (dwECCregister >> 8) & 7;
		byLineAddr = dwECCregister & 0xff;
	}
	else if(dwECCregister & 0x40000000)
	{
		byColBitAddr = (dwECCregister >> 24) & 7;
		byLineAddr = dwECCregister >> 16;
		lvar_r12 = 0x100;
	}
	byData = sub1fc40(byLineAddr + lvar_r12);
	if(byColBitAddr)
	{
		for(int cnt = byColBitAddr; cnt; cnt--) lvar_bl <<= 1;
	}

	sub1fc80(byLineAddr + lvar_r12, byData ^ lvar_bl);
	return 0;
}

CSM::~CSM()
{
	write32(base_addr + 0x90, 0xffffffff);
	// free everything
}

char CSM::EraseBlock(char byZone, short wPhyBlock)
{
	char rc = 0;
	if(var_x160 != -1)
	{
		// UNC or DS error
		if(var_x15f < mbyZones && var_x160 < mwPhyBlocks)
		{
			var_x150[(mwPhyBlocks >> 3) * var_x15f + (var_x160 >> 3)] |= 1 << (var_x160 & 7);
		}
		var_x160 = -1;
		var_x15f = -1;
	}
	else
	{
		if(byZone < mbyZones && wPhyBlock < mwPhyBlocks)
		{
			var_x150[(mwPhyBlocks >> 3) * byZone + (wPhyBlock >> 3)] &= ~(1 << (wPhyBlock & 7));
		}
	}
	write32(base_addr + 0xa0, (mwPhyBlocks * byZone + wPhyBlock) * mbyPhyBlockSize);
	KeSynchronizeExecution(card_int, ClearStatus, &dwSM_STATUS);
	write32(base_addr + 0x94, 4);
	rc = sub1fee0(&sm_event_x0d0, -1000000);
	if(!rc && vara_4) rc = 0x86;
	if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, &dwSM_STATUS)) rc = 0x6a;
	if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, &dwSM_STATUS)) rc = 0x62;
	if(rc && !var_x16e)
	{
		write32(base_addr + 0x18, 0xffff);
		write32(base_addr + 0x10, 2);
		write32(base_addr + 0x9c, 0xffffffef & read32(base_addr + 0x9c));
		write32(base_addr + 0x9c, 0x80 | read32(base_addr + 0x9c));
		write32(base_addr + 0x9c, 0xffffff7f & read32(base_addr + 0x9c));
		var_x11a = mbyLogBlockSize;
		var_x113 = -1;
		var_x114 = -1;
		mwTailSrcPhyBlock = -1;
		var_x118 = -1;
		var_x10a = -1;
		var_x10c = -1;
		mwHeadSrcPhyBlock = -1;
		var_x110 = -1;
		var_x112 = 0;
		//print mwWrittenBlocks content
		for(; mbWriteBlockPos; mbWriteBlockPos--)
		{
			if(byZone < mbyZones && mwWrittenBlocks[mbWriteBlockPos] < mwPhyBlocks)
			{
				var_x150[(mwPhyBlocks >> 3) * byZone + (mwWrittenBlocks[mbWriteBlockPos] >> 3)] &= ~(1 << (mwWrittenBlocks[mbWriteBlockPos] & 7));
			}
			write32(base_addr + 0xa0, (mwPhyBlocks * byZone + mwWrittenBlocks[mbWriteBlockPos]) * mbyPhyBlockSize);
			KeSynchronizeExecution(card_int, ClearStatus, &dwSM_STATUS);
			write32(base_addr + 0x94, 4);
			if(sub1fee0(sm_event_x0d0, -1000000)) continue;
			if(vara_4) continue;
			if(KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, &dwSM_STATUS)) continue;
			if(KeSynchronizeExecution(card_int, &CSM::CheckDataErr, &dwSM_STATUS)) continue;
		}
		MarkBadBlock(byZone, wPhyBlock, 1);
		rc = 0x6d;
	}
	return rc;
}

char
CSM::WritePage(char arg_1, short arg_2, short arg_3, char arg_4, char arg_5)
{
	char rc = 0;
	int l_status;

	if(mwPageSize < 512)
	{
		arg_4 <<= 1;
		var_x15e <<= 1;
	}
	for(int cnt = 0; rc; cnt++)
	{
		write32(base_addr + 0xc0, 0xfffffc00 | arg_2);
		if(var_x15d == 1 && var_x15e == arg_4)
		{
			write32(base_addr + 0xb4, 0xffffff & read32(base_addr + 0xb4));
			var_x15d = 0;
			var_x15e = 0;
			// print redundant data
		}
		write32(base_addr + 0x9c, 0x20 | read32(base_addr + 0x9c));
		write32(base_addr + 0x9c, 0x10 | read32(base_addr + 0x9c));
		if(arg_5)
		{
			//2aeed
			write32(base_addr + 0x9c, 0xffffffef & read32(base_addr + 0x9c));
			write32(base_addr + 0x10, 0x8100);
			write32(base_addr + 0x9c, 0x80 | read32(base_addr + 0x9c));
			write32(base_addr + 0x9c, 0xffffff7f & read32(base_addr + 0x9c));
			write32(base_addr + 0xa0, (mwPhyBlocks * arg_1 + arg_2) * mbyPhyBlockSize);
			KeSynchronizeExecution(card_int, ClearStatus, &l_status);
			write32(base_addr + 0x94, 0x10c);
			for(int cnt = 0; cnt < 0x80; cnt++)
			{
				while(0x100 & read32(base_addr + 0x98))
				{
					sub1fd80(-100); // sleep
					if(vara_4) return 0x86;
				}
				write32(base_addr + 0xa4, read32(base_addr + 0x200 + 4 * cnt));
			}
		}
		else
		{
			write32(base_addr + 0xa0, (mwPhyBlocks * arg_1 + arg_3) * mbyPhyBlockSize);
			KeSynchronizeExecution(card_int, ClearStatus, &l_status);
			write32(base_addr + 0x94, 0x10c);
		}

		rc = sub1fee0(&sm_event_x0d0, -1000000);
		if(!rc && vara_4) rc = 0x86;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, &l_status)) rc = 0x6a;
		if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, &l_status)) rc = 0x62;

		if(rc)
		{
			if(KeSynchronizeExecution(card_int, &CSM::CheckFlashErr, &l_status))
			{
				//writepage failed
				write32(base_addr + 0x18, 0xffff);
				write32(base_addr + 0x10, 2);
				write32(base_addr + 0x9c, 0xffffffef & read32(base_addr + 0x9c));
				write32(base_addr + 0x9c, 0x80 | read32(base_addr + 0x9c));
				write32(base_addr + 0x9c, 0xffffff7f & read32(base_addr + 0x9c));
				var_x11a = mbyLogBlockSize;
				var_x113 = -1;
				var_x114 = -1;
				mwTailSrcPhyBlock = -1;
				var_x118 = -1;
				var_x10a = -1;
				var_x10c = -1;
				mwHeadSrcPhyBlock = -1;
				var_x110 = -1;
				var_x112 = 0;
				//print mwWrittenBlocks content
				for(; mbWriteBlockPos; mbWriteBlockPos--)
				{
					if(EraseBlock(arg_1, mwWrittenBlocks[mbWriteBlockPos]))
					{
						var_x16e = 0;
						return 0x6d;
					}
				}
				MarkBadBlock(arg_1, arg_2, 1);
				return 0x6d;
			}
			var_x16e = 0;
		}
	}

	return rc;
}

char
CSM::WritePhyBlock(char byZone, short wLogBlock, short *arg_3, char byPage, char bySectorCount)
{
	char rc = 0;
	short wFFPhyBlock;
	short wPhyBlock = *arg_3;
	short wNewPhyBlock;
	//r15, arg_2 - wLogBlock
	//r12, arg_4 - byPage
	//r14, arg_5 - bySectorCount

	if(!bySectorCount) bySectorCount = 0x80;
	if(bySectorCount > 0x80) bySectorCount = 0x80;
	if(byZone != var_x126) var_x124 = -1;
	var_x126 = byZone;

	if(wPhyBlock != var_x124 && mbyLogBlockSize > 0)
	{
		for(int cnt = 0; cnt < mbyLogBlockSize; cnt++) var_x127[cnt] = 0;
	}

	wFFPhyBlock = var_x100[byZone];
	var_x124 = -1;
	if(wPhyBlock != wFFPhyBlock && var_x127[byPage])
	{
		wNewPhyBlock = wPhyBlock;
		bySectorCount = byZone;
		wPhyBlock = 1;
	}
	else
	{
		if(var_x16a != -1)
		{
			rc = EraseBlock(var_x16d);
			if(rc) return 0x6d;
			var_x16a = -1;
		}
		rc = GetEmptyPhyBlock(byZone, &wNewPhyBlock);
		if(rc) return rc;
		if(wNewPhyBlock == var_x100[byZone])
		{
			rc = GetEmptyPhyBlock(byZone, &wNewPhyBlock);
		}
		EraseBlock(byZone, wNewPhyBlock);
		sub20000(wNewPhyBlock);
		if(byZone < mbyZones && wNewPhyBlock < mwPhyBlocks)
		{
			var_x150[(mwPhyBlocks >> 3) * byZone + (wNewPhyBlock >> 3)] |= 1 << (wNewPhyBlock & 7);
		}
		for(int cnt = 0; cnt < mbyLogBlockSize; cnt++) var_x127[cnt] = 1;
		if(wPhyBlock == wNewPhyBlock)
		{
			bySectorCount = byZone;
			wPhyBlock = 1;
		}
		else
		{
			if(wPhyBlock == wFFPhyBlock)
			{
				if(!byPage)
				{
					bySectorCount = byZone;
					wPhyBlock = 1;
				}
				else
				{
					wPhyBlock = 1;
					bySectorCount = FillUpBlock(byZone, wLogBlock, wNewPhyBlock, byPage, 0);
				}
			}
			else
			{
				if((bySectorCount < mbyLogBlockSize && bySectorCount > 0) || byPage)
				{
					//2b585
					if(byPage) memset(var_x127, 0, byPage);
					for(int cnt = bySectorCount + byPage; cnt < mbyLogBlockSize; cnt++) var_x127[cnt] = 0;
					if(mwHeadSrcPhyBlock == -1)
					{
						// Head sectors
						var_x10a = byZone;
						var_x10c = wLogBlock;
						mwHeadSrcPhyBlock = wPhyBlock;
						var_x110 = wNewPhyBlock;
						var_x112 = byPage;
					}
				}
				//2b60e
				if(byPage + bySectorCount <= mbyLogBlockSize)
					bySectorCount = 0;
				else
				{
					// Tail sectors
					var_x113 = byZone;
					var_x114 = wLogBlock;
					mwTailSrcPhyBlock = wPhyBlock;
					var_x11a = byPage + bySectorCount;
					var_x118 = wNewPhyBlock;
					bySectorCount = 1;
					var_x16c = 2;
				}
				if(byPage)
				{
					var_x16c = 1;
					mdwDMAaddressReg = read32(base_addr + 0xc);
					mwDMAcontrolReg = read16(base_addr + 0x10);
					write16(base_addr + 0x10, 2);
					rc = this->CloseWrite();
					if(rc) return rc;
					write16(base_addr + 0x18, 0xffff);
					write16(base_addr + 0x24, 1);
					write16(base_addr + 0x14, 5);
					write32(base_addr + 0xc, mdwDMAaddressReg);
					write16(base_addr + 0x10, mwDMAcontrolReg);
					write16(base_addr + 0x10, 1 | read16(base_addr + 0x10));
					var_x16c = 2;
				}

				if(!bySectorCount)
				{
					rc = EraseBlock(byZone, wPhyBlock);
					if(rc) return 0x6d;
				}
				if(byZone >= mbyZones || wLogBlock >= mwLogBlocks)
				{
					wPhyBlock = 1;
					bySectorCount = byZone;
				}
				else
				{
					wPhyBlock = 1;
					bySectorCount = byZone;
					var_x148[mwLogBlocks * bySectorCount + wLogBlock] = -1;
				}
			}
		}
	}
	//2b904
	rc = WritePage(byZone, wLogBlock, wNewPhyBlock, 0, byPage);
	var_x127[byPage] = 0;
	if(rc)
	{
		if(byZone < mbyZones && wNewPhyBlock < mwPhyBlocks)
		{
			var_x150[(mwPhyBlocks >> 3) * byZone + (wNewPhyBlock >> 3)] |= wPhyBlock << (wNewPhyBlock & 7);
		}
	}
	else
	{
		*arg_3 = wNewPhyBlock;
		var_x124 = wNewPhyBlock;
		if(var_x120 == (var_x084 + var_x090 - 1) && mwTailSrcPhyBlock == -1)
		{
			byPage++;
			rc = FillUpBlock(byZone, wLogBlock, wNewPhyBlock, 0, mbyLogBlockSize, byPage);
			var_x124 = -1;
		}
	}
	return rc;
}

char
CSM::WriteSector(int arg_1, short bySectorCount)
{
	char rc = 0;
	short byPage = arg_1 % mbyLogBlockSize; // r14
	short byZone = (arg_1 / mbyLogBlockSize) / mwLogBlocks; // r12
	short wLogBlock = (arg_1 / mbyLogBlockSize) % mwLogBlocks;
	
	if(lvar_r12 >= mbyZones) return 0x82;
	short wPhyBlock = wLogBlock < mwLogBlocks ? var_x148[mwLogBlocks * byZone + wLogBlock] : -1;
	bySectorCount = bySectorCount > 0x80 ? 0x80 : bySectorCount;
	for(int cnt = 0; !cnt; cnt++)
	{
		rc = WritePhyBlock(byZone, wLogBlock, &wPhyBlock, byPage, bySectorCount);
		if(!rc)
		{
			if(byZone < mbyZones && wLogBlock < mwLogBlocks)
				var_x148[mwLogBlocks * byZone + wLogBlock] = wPhyBlock;
			return 0;
		}
	}
	if(rc == 0x81) ResetCard();
	return rc;
}

char
CSM::MakeLUT()
{
	char rc = 0;
	short wPhyBlock = 0; // r15
	short wOldPhyBlock = 0;
	short wLogBlock = 0;  // rsp + 0x34, ebp
	char lvar_x30 = 1;   // rsp + 0x30
	char byZone = 0;     // sil
	short wLastPageLogBlock = 0;

	do { // 2bc41 <- 2c65a
		if (byZone >= mbyZones || wPhyBlock >= mwPhyBlocks) {
			var_x147 = 1;
			break;
		}

		if (lvar_x30) { // 2bc5e -> 2bce8
			for (wLogBlock = 0; wLogBlock < mwLogBlocks; wLogBlock++) {
				if (byZone < mbyZones && wLogBlock < mwLogBlocks)
					var_x148[byZone * mwLogBlocks + wLogBlock] = -1;
			}
			wPhyBlock = 0;
			mpFirstEmptyPhyBlock[byZone] = -1;
		} // 2bce8

		if (!byZone || wPhyBlock == var_x108) {
			if (byZone < mbyZones && wPhyBlock < mwPhyBlocks)
				var_x150[wPhyBlock >> 3] |= 1 << (wPhyBlock & 7);
		} else { //2bceb, 2bcf5 -> 2c4d1
			write32(base_addr + 0xa0, (mwPhyBlocks * byZone + wPhyBlock) * mbyPhyBlockSize);
			KeSynchronizeExecution(card_int, ClearStatus, &dwSM_STATUS);
			write32(base_addr + 0x94, 0x109);
			rc = sub1fee0(&sm_event_x0d0, -1000000);
			if (!rc && vara_4)
				rc = 0x86;
			if (!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, &dwSM_STATUS))
				rc = 0x6a;
			if (!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, &dwSM_STATUS))
				rc = 0x62;
			if (rc) {
				if (0x8000 & read32(base_addr + 0x98))
					muiBadBlocks++;
				rc = 0;
			}

			if (0x17ff != (0x17ff & read32(base_addr + 0xb4))) { // 2be3f
				if (byZone < mbyZones && wPhyBlock < mwPhyBlocks)
					var_x150[(mwPhyBlocks >> 3) * byZone + (wPhyBlock >> 3)] |= 1 << (wPhyBlock & 7);

				if (!(0x8000 & read32(base_addr + 0x98))) { // 2bea0
					if (!sub28cb0(&wLogBlock) && wLogBlock != -1) { // 2bedc
						if (byZone < mbyZones && wLogBlock < mwLogBlocks && var_x148[mwLogBlocks * byZone + wLogBlock] != -1) {
							rc = sub28b80(byZone, wPhyBlock, mbyLogBlockSize - 1);
							if (rc) {
								if (rc == 0x86)
									return rc;
								wOldPhyBlock = (byZone < mbyZones && wLogBlock < mwLogBlocks) ? var_x148[mwLogBlocks * byZone + wLogBlock] : -1;
								rc = sub28b80(byZone, wOldPhyBlock, mbyLogBlockSize - 1);
								if (rc) {
									if (rc == 0x86)
										return rc;
								} else { // 2c00a
									if (EraseBlock(byZone, wPhyBlock))
										return 0x6d;
								}
							} else { // 2c03d
								wLastPageLogBlock = 0x3ff & (read32(base_addr + 0xb4) >> 1);
								if (wLastPageLogBlock == wLogBlock) {
									wOldPhyBlock = (byZone < mbyZones && wLogBlock < mwLogBlocks) ? var_x148[mwLogBlocks * byZone + wLogBlock] : -1;
									rc = sub28b80(byZone, wOldPhyBlock, mbyLogBlockSize - 1);
									if (rc) {
										if (rc == 0x86)
											return rc;
										if (byZone < mbyZones && wLogBlock < mwLogBlocks)
											var_x148[mwLogBlocks * byZone + wLogBlock] = wPhyBlock;
										if (EraseBlock(byZone, wOldPhyBlock))
											return 0x6d;
									} else { // 2c162
										wLastPageLogBlock = 0x3ff & (read32(base_addr + 0xb4) >> 1);
										if (wLastPageLogBlock == wLogBlock) {
											if (EraseBlock(byZone, wPhyBlock))
												return 0x6d;
										} else { // 2c1fc
											if (byZone < mbyZones && wLogBlock < mwLogBlocks)
												var_x148[mwLogBlocks * byZone + wLogBlock] = wPhyBlock;
											if (EraseBlock(byZone, wOldPhyBlock))
												return 0x6d;
										}
									}
								} else { // 2c266
									wOldPhyBlock = (byZone < mbyZones && wLogBlock < mwLogBlocks) ? var_x148[mwLogBlocks * byZone + wLogBlock] : -1;
									rc = sub28b80(byZone, wOldPhyBlock, mbyLogBlockSize - 1);
									if (rc) {
										if (rc == 0x86)
											return rc;
									} else { // 2c301
										if (EraseBlock(byZone, wPhyBlock))
											return 0x6d;
									}
								}
							}
						} else { // 2c336
							if (byZone < mbyZones && wLogBlock < mwLogBlocks)
								var_x148[mwLogBlocks * byZone + wLogBlock] = wPhyBlock;
						}
					}
				} else { // 2c391
					// print stuff
				}
			} else { // 2c3aa
				int cnt = 0;
				int t_val = read32(base_addr + 0xb4) >> 0x10;
				while (t_val) {
					cnt += t_val & 1;
					t_val >>= 1;
				}

				if (cnt >= 7) {
					if (mpFirstEmptyPhyBlock[byZone] == -1)
						mpFirstEmptyPhyBlock[byZone] = wPhyBlock;

					if (byZone < mbyZones && wPhyBlock < mwPhyBlocks)
						var_x150[(mwPhyBlocks >> 3) * byZone + (wPhyBlock >> 3)] &= ~(1 << (wPhyBlock & 7));
				} else { // 2c473
					if (byZone < mbyZones && wPhyBlock < mwPhyBlocks)
						var_x150[(mwPhyBlocks >> 3) * byZone + (wPhyBlock >> 3)] |= 1 << (wPhyBlock & 7);
				}
			}
		} // 2c4d1
		wPhyBlock++;
		if (wPhyBlock != mwPhyBlocks)
			continue; // -> 2c64e

		var_x100[byZone] = mpFirstEmptyPhyBlock[byZone];
		for (wLogBlock = 0; wLogBlock < mwLogBlocks; wLogBlock++) {
			if (byZone >= mbyZones || wLogBlock >= mwLogBlocks || var_x148[mwLogBlocks * byZone + wLogBlock] == -1) {
				if (byZone < mbyZones && wLogBlock < mwLogBlocks)
					var_x148[mwLogBlocks * byZone + wLogBlock) = var_x100[mwLogBlocks * byZone + wLogBlock];
			}
		} // 2c5c2

		if (byZone < mbyZones && var_x100[byZone] < mwPhyBlocks)
			var_x150[(mwPhyBlocks >> 3) * byZone + (var_x100[byZone] >> 3)] |= 1 << (var_x100[byZone] & 7);

		byZone++;
		lvar_x30 = 1;
		wPhyBlock = 0;
		if (byZone == mbyZones) {
			var_x147 = lvar_x30;
			wPhyBlock = sub27950(0, 0);
		}
	} while (!var_x147); // 2c65a -> 2bc41
	return 0;
}

void
CSM::EraseCard()
{
	char rc = 0;

	for (char byZone = 0; byZone < mbyZones; byZone++) { // r13b
		for (short wPhyBlock = 0; wPhyBlock < mwPhyBlocks; wPhyBlock++) { // si
			write32(base_addr + 0xa0, (mwPhyBlocks * byZone + wPhyBlock) * mbyPhyBlockSize);
			KeSynchronizeExecution(card_int, ClearStatus, &dwSM_STATUS);
			write32(base_addr + 0x94, 0x109);
			rc = sub1fee0(&sm_event_x0d0, -1000000);
			if(!rc && vara_4) rc = 0x86;
			if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS)) rc = 0x6a;
			if(!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS)) rc = 0x62;
			// print stuff if rc
			if (!(0x8000 & read32(base_addr + 0x98))) {
				EraseBlock(byZone, wPhyBlock);
			}
		}
	}
}

char
CSM::InitializeCard()
{
	char rc = 0;

	sub1fd80(-1500000);
	write32(base_addr + 0x9c, 0x3fb8004);
	write32(base_addr + 0x84, 7);

	ResetCard();

	rc = ReadID();
	if (rc)
		return rc;

	rc = ReadXDId();
	if (rc)
		return rc;

	rc = MediaModel();
	if (rc)
		return rc;

	rc = sub29ec0();
	if (rc)
		return rc;

	if (vara_2)
		return 0;

	rc = sub29ff0();
	if (rc)
		return rc;

	ReportMediaModel();
	var_x108 = PhyBlockDefective();
	if (var_x108 > 0x17) {
		return vara_4 ? 0x86 : 0x6e;
	}

	if (SMCISEnable) {
		var_x15c = 1;
		rc = sub29ec0();
		var_x15c = 0;
		if (rc)
			return rc;
	}

	InitializeWriteblocks();
	var_x147 = 0;
	rc = MakeLUT();
	if (rc)
		return rc;

	write32(base_addr + 0x9c, 0x10 | read32(base_addr + 0x9c));
	SerialNumber = mwSize;
	if (muiMediaID == 4)
		SerialNumber++;

	if (0x200 & read32(base_addr + 8)) { // write protected
		vara_2 = 1;
		mbWriteProtected = 1;
		return 0;
	}

	mbWriteProtected = 0;
	if(muiMediaID == 1 || is_xx12 == 0) { // write protected workaround not necessary
		vara_2 = 1;
		return 0;
	}

	if (this->WriteProtectedWorkaround())
		mbWriteProtected = 1;

	vara_2 = 1;
	return 0;
}

char
CSM::WriteProtectedWorkaround()
{
	char rc = 0;
	short lvar_di = mpFirstEmptyPhyBlock[mbyZones - 1]; 

	if(lvar_di == -1) {
		mbWriteProtected = 1;
		return 0;
	}
	KeSynchronizeExecution(card_int, ClearStatus, &dwSM_STATUS);
	write32(base_addr + 0x94, 0);
	rc = sub1fee0(&sm_event_x0d0, -1000000);
	if (!rc && vara_4)
		rc = 0x86;
	if (!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS))
		rc = 0x6a;
	if (!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS))
		rc = 0x62;
	rc = EraseBlock(mbyZones - 1, lvar_di);
	if (rc)
		return rc;

	WaitForCMD(0, 0, 1); // SM_CMD_RST_CHIP
	rc = WritePage(mbyZones - 1, 0, lvar_di, 0, 1);
	if (rc)
		return rc;

	write32(base_addr + 0xb0, 0xffffffff);
	write32(base_addr + 0xb4, 0xffffffff);
	write32(base_addr + 0xb8, 0xffffffff);
	write32(base_addr + 0xbc, 0xffffffff);

	WaitForCMD(0, 0, 1);
	rc = ReadPage(mbyZones - 1, 0, lvar_di, 1);
	if (rc)
		return rc;

	if (0x1001 == read32(base_addr + 0xb4)) {
		mbWriteProtected = 0;
		return EraseBlock(mbyZones - 1, lvar_di);
	}

	mbWriteProtected = 1;
	return 0;
}

char
CSM::WriteCIS()
{
	char rc = 0;
	int Data[] = {0x01d90301, 0xdf0218ff, 0x04002001, 0x21000000,
		      0x22010402, 0x22010102, 0x07040203, 0x0301051A,
		      0x1B0F0200, 0xA1C0C008, 0x00085501, 0xC10A1B20,
		      0x55019941, 0xFFFFF064, 0x820C1B20, 0x61EA1841,
		      0xF60701F0, 0x1BEE0103, 0x1841830C, 0x017061EA,
		      0x01037607, 0x051415EE, 0x20202000, 0x20202020,
		      0x20202000, 0x2E300020, 0x14FF0030, 0x0000FF00,
		      0xFFFFFFFF, 0x0000FFFF, 0x00C3CC0C, 0xC3CC0C00};

	write16(base_addr + 0x24, 1);
	short uiOffset; // di
	char index = 0; // bpl

	for (uiOffset = 0; uiOffset < 128; uiOffset++) {
		sub1fd80(-100);
		if (uiOffset < 28) {
			write32(base_addr + 0x200 + (uiOffset >> 2), Data[index++]);
		}

		if ((uiOffset > 27 && uiOffset < 64) || (uiOffset >= 27 && uiOffset < 125)) {
			write32(base_addr + 0x200 + (uiOffset >> 2), 0);
		}

		if (uiOffset == 64) {
			write32(base_addr + 0x300, 0xffffffff);
		} else if (uiOffset == 65) {
			write32(base_addr + 0x304, 0xffff);
			index = 0;
		} else {
			if (uiOffset <= 93) {
				write32(base_addr + 0x200 + (uiOffset >> 2), Data[index++]);
			}

			if (uiOffset == 126) {
				write32(base_addr + 0x3f8, 0xc3cc0c);
				index = 0;
			} else if (uiOffset == 127) {
				write32(base_addr + 0x3fc, 0xc3cc0c00);
				index = 0;
			}
		}
	} // 2d27b
	KeSynchronizeExecution(card_int, ClearStatus, &dwSM_STATUS);
	write32(base_addr + 0x94, 0);
	rc = sub1fee0(&sm_event_x0d0, -1000000);
	if (!rc && vara_4)
		rc = 0x86;
	if (!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS))
		rc = 0x6a;
	if (!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS))
		rc = 0x62;
	rc = WritePage(0, 0, var_x108, 0, 1);
	return rc ? 0x6e : 0;
}

char
CSM::CISFixOrCheck(char uiFixCheckCIS)
{
	char rc;

	CFlash::RescueRWFail();
	KeSynchronizeExecution(card_int, ClearStatus, &dwSM_STATUS);
	write32(base_addr + 0x94, 0);
	rc = sub1fee0(&sm_event_x0d0, -1000000);
	if (!rc && vara_4)
		rc = 0x86;
	if (!rc && KeSynchronizeExecution(card_int, &CSM::CheckTimeOut, dwSM_STATUS))
		rc = 0x6a;
	if (!rc && KeSynchronizeExecution(card_int, &CSM::CheckDataErr, dwSM_STATUS))
		rc = 0x62;
	if (uiFixCheckCIS) {
		write32(base_addr + 0x9c, 0x3fb8004);
		write32(base_addr + 0x84, 7);
		ResetCard();
		rc = ReadID();
		if (rc)
			return rc;
		rc = MediaModel();
		if (rc)
			return rc;
		rc = sub29ec0();
		if (rc)
			return rc;
		EraseCard();
		var_x108 = PhyBlockDefective();
		if (var_x108 > 0x17)
			return vara_4 ? 0x86 : 0x6e;
		rc = EraseBlock(0, var_x108);
		if (rc)
			return rc;
		rc = WriteCIS();
	} else { // 2d5be
		write16(base_addr + 0x24, 1);
		write32(base_addr + 0x9c, 0x3fb8004);
		write32(base_addr + 0x84, 7);
		ResetCard();
		rc = ReadID();
		if (rc)
			return rc;
		rc = MediaModel();
		if (rc)
			return rc;
		rc = sub29ec0();
		if (rc)
			return rc;
		var_x108 = PhyBlockDefective();
		if (var_x108 > 0x17)
			return vara_4 ? 0x86 : 0x6e;
		var_x15c = 1;
		rc = CheckCIS();
		var_x15c = 0;
	}
	return rc;
}

char
CSM::WriteSectors(int arg_1, short *arg_2, char *arg_3)
{
	char rc;

	if (arg_1 != -1)
		var_x120 = arg_1;

	rc = WriteSector(var_x120, *arg_2);
	if (rc)
		return rc;
	++var_x120;
	--(*arg_2);
	--(*arg_3);
	return 0;
}

char
CSM::ProcessCorrectableError(char byZone, short wLogBlock, short *wPhyBlock, char arg_4, char arg_5)
{
	char rc;
	short wNewPhyBlock;

	write16(base_addr + 0x10, 2);
	rc = GetEmptyPhyBlock(byZone, &wNewPhyBlock);
	if (rc)
		return rc;
	if (var_x100[byZone] == wNewPhyBlock) {
		GetEmptyPhyBlock(byZone, &wNewPhyBlock);
	}

	rc = CopyPages(byZone, wLogBlock, wNewPhyBlock, *wPhyBlock, 0, arg_4, 1);
	if (rc)
		return rc;

	ReadPage(byZone, *wPhyBlock, arg_4, 1);
	rc = EccCorrectData(read32(base_addr + 0xac);
	if (rc)
		return rc;

	rc = WritePage(byZone, wLogBlock, wNewPhyBlock, arg_4, 1);
	if (rc)
		return rc;

	arg_4++;
	rc = CopyPages(byZone, wLogBlock, wNewPhyBlock, *wPhyBlock, arg_4, mbyLogBlockSize, 1);
	if (rc)
		return rc;

	if (arg_5)
		sub27910(byZone, wLogBlock, wNewPhyBlock);

	if (EraseBlock(byZone, *wPhyBlock))
		return 0x6d;

	if (byZone < mbyZones && *wPhyBlock < mwPhyBlocks)
		var_x150[(mwPhyBlocks >> 3) * byZone + (*wPhyBlock >> 3)] |= 1 << (*wPhyBlock & 7);

	*wPhyBlock = wNewPhyBlock;
	return 0;
}

char
CSM::ReadSector(int arg_1)
{
	char rc = 0;

	char byPage = arg_1 % mbyLogBlockSize;
	char byZone = (arg_1 / mbyLogBlockSize) / mwLogBlocks;
	short wLogBlock = (arg_1 / mbyLogBlockSize) % mwLogBlocks;

	if (byZone >= mbyZones)
		return 0x82;

	short wPhyBlock = wLogBlock < mwLogBlocks ? var_x148[mwLogBlocks * byZone + wLogBlock] : -1;
	rc = ReadPage(byZone, wPhyBlock, byPage, 0);
	if (rc == 0x62 || rc == 0x66)
		return rc;
	if (rc == 0x68) {
		var_x162 = 1;
		rc = ProcessCorrectableError(byZone, wLogBlock, &wPhyBlock, byPage, 0);
		var_x162 = 0;
		return rc ? rc : 0x68;
	}
	if (rc == 0x69) {
		if (byZone < mbyZones && wLogBlock < mwLogBlocks)
			var_x148[mwLogBlocks * byZone + wLogBlock] = var_x100[byZone];
	}
	return rc;
}

char
CSM::CopyPagesDMA(char byZone, short arg_2, short dstPhyBlock/*r8*/, short srcPhyBlock/*bp*/, char srcPage, char dstPage, char arg_7)
{
	char rc = 0;
	short numPages = dstPage - srcPage; // r15
	short cnt;

	while(1) { // -> 2de1a
		write16(base_addr + 0x10, 2);
		write16(base_addr + 0x18, 0xffff);
		write16(base_addr + 0x24, 1);
		vara_5 = 0;
		write16(base_addr + 0x14, 5);
		write32(base_addr + 0xc, var_x0b0);
		write16(base_addr + 0x10, (numPages << 8) | 1);

		for (cnt = srcPage; cnt < dstPage; cnt++) { // ->2dd2c
			if (rc)
				break;
			rc = ReadPage(byZone, srcPhyBlock, cnt, 0);
			if (rc == 0x69) { // UNC or DS error
				var_x15d = 1;
				var_x15e = cnt;
				var_x160 = dstPhyBlock;
				var_x15f = byZone;
				rc = 0;
			} else if (rc == 0x66 || rc == 0x62) {
				rc = 0;
			}
		}

		if (rc != 0x68)
			break;

		cnt--;
		var_x162 = 1;
		rc = ProcessCorrectableError(byZone, arg_2, &srcPhyBlock, cnt, 1);
		var_x162 = 0;
		if (rc)
			return rc;
		if (arg_7) {
			if (mwTailSrcPhyBlock == mwHeadSrcPhyBlock) {
				mwTailSrcPhyBlock = mwHeadSrcPhyBlock = srcPhyBlock;
			} else {
				mwHeadSrcPhyBlock = srcPhyBlock;
			}
		} else {
			mwTailSrcPhyBlock = srcPhyBlock;
		}
		write16(base_addr + 0x18, 0xffff);
		write16(base_addr + 0x10, 2);
	}

	if (rc) {
		write16(base_addr + 0x18, 0xffff);
		write16(base_addr + 0x10, 2);
		return rc;
	}

	while (!KeSynchronizeExecution(card_int, sub_1c050, &var_2)) {
		sub1fee0(&event_2, -1000000);
		if (vara_4)
			return 0x86;
	}
	KeSynchronizeExecution(card_int, ClearStatus, &var_2);
	write16(base_addr + 0x18, 0xffff);
	write16(base_addr + 0x24, 1);
	write16(base_addr + 0x14, 5);
	write32(base_addr + 0xc, var_x0b0);
	write16(base_addr + 0x10, (numPages << 8) | 0x8001);

	for (cnt = srcPage; cnt < dstPage; cnt++) {
		if (rc)
			break;
		rc = WritePage(byZone, arg_2, srcPhyBlock, cnt, 0);
		
	}
	if (rc) {
		write16(base_addr + 0x18, 0xffff);
		write16(base_addr + 0x10, 2);
		return rc;
	}

	while (!KeSynchronizeExecution(card_int, sub_1c050, &var_2)) {
		sub1fee0(&event_2, -1000000);
		if (vara_4)
			return 0x86;
	}
	KeSynchronizeExecution(card_int, ClearStatus, &var_2);
	write16(base_addr + 0x18, 0xffff);
	write16(base_addr + 0x24, 1);

	if (var_x15d == 1 || var_x15e == cnt) {
		var_x15d = 0;
		var_x15e = 0;
	}
	return 0;
}

char
CSM::CopyPages(char byZone/*r12*/, short arg_2, short dstPhyBlock, short srcPhyBlock/*si*/, char srcPage, char dstPage, char arg_7)
{
	char rc = 0;
	short cnt;

	while (1) { // <- 2e2f3
		for (cnt = srcPage; cnt < dstPage; cnt++) { // <- 2e217
			rc = ReadPage(byZone, srcPhyBlock, cnt, 1);
			if (rc == 0x68)
				break;
			if (rc == 0x69) {
				var_x15e = cnt;
				var_x160 = srcPhyBlock;
				var_x15f = byZone;
			}
			if (WritePage(byZone, arg_2, dstPhyBlock, cnt, 1))
				return 0x65;
		}

		if (var_x162)
			return 0x68; // already processing correctable error

		var_x162 = 1;
		rc = ProcessCorrectableError(byZone, arg_2, &srcPhyBlock, cnt, 1);
		var_x162 = 0;
		if (rc)
			return rc;
		if (arg_7) {
			if (vax_116 == mwHeadSrcPhyBlock) {
				mwHeadSrcPhyBlock = srcPhyBlock;
				mwTailSrcPhyBlock = srcPhyBlock;
			} else {
				mwHeadSrcPhyBlock = srcPhyBlock;
			}
		} else {
			if (vax_116 == mwHeadSrcPhyBlock)
				mwHeadSrcPhyBlock = srcPhyBlock;
		}
	}
	write16(base_addr + 0x18, 0xffff);
	write16(base_addr + 0x24, 1);
	if (var_x15d || var_x15e == cnt) {
		var_X15d = 0;
		var_x15e = 0;
	}
	return 0;
}

char
CSM::CloseWrite()
{
	char rc = 0;
	char byNLoops;
	int tval1, tval2;
	char srcPage/*cl*/, dstPage/*dil*/;

	vara_6 = 1;
	while (mwHeadSrcPhyBlock != -1) { // -> 2e5f7
		if (var_x16c != 1)
			break;
		byNLoops = 1;
		tval1 = 2 * (var_x0b4 >> 10); tval2 = var_X112;
		while (tval1 < tval2) {
			tval2 -= tval1;
			byNLoops++;
		}
		if (var_x0b4 == -1 || mwPageSize < 0x200) {
			rc = CopyPages(var_x10a, var_x10c, var_x110, mwHeadSrcPhyBlock, 0, var_x112, 1);
		} else {
			tval1 = 2 * (var_x0b4 >> 10);
			srcPage = 0;
			dstPage = var_x112;
			for (; byNLoops; byNLoops--) {
				if (rc)
					break;
				if (var_x112 < tval1) {
					srcPage = 0;
					dstPage = 0;
				} else if (var_x112 > tval1) {
					dstPage += var_x112;
				} else {
					dstPage = tval1 + srcPage;
					lvar_si = var_x112 - dstPage;
				}
				rc = CopyPagesDMA(var_x10a, var_x10c, var_x110, mwHeadSrcPhyBlock, lvar_cl, lvar_dil, 1);
			}
		}
		if (!rc) {
			if (mwHeadSrcPhyBlock != mwTailSrcPhyBlock) {
				var_x16a = mwHeadSrcPhyBlock;
				var_x16d = var_x10a;
			}
			break;
		}
		if (rc != 0x6d) {
			vara_6 = 0;
			return rc;
		}
		rc = GetEmptyPhyBlock(var_x10a, &var_x110);
		if (rc)
			return rc;
		vara_6 = 1;
	}

	while (mwTailSrcPhyBlock != -1) { // <- 2e7d7
		if (var_x16c != 2)
			break;
		byNLoops = 1;
		tval1 = 2 * (var_x0b4 >> 10); tval2 = var_X112;
		while (tval1 < tval2) {
			tval2 -= tval1;
			byNLoops++;
		}
		if (var_x0b4 == -1 || mwPageSize < 0x200) {
			rc = CopyPages(var_x113, var_x114, var_x118, mwTailSrcPhyBlock, var_x11a, mbyLogBlockSize, 0); 
		} else {
			tval1 = (var_x0b4 * 2) >> 0xa;
			srcPage = mbyLogBlockSize - var_x11a;

			for (; byNLoops; byNLoops--) {
				if (tval1 > srcPage) {
					dstPhyBlock = var_x11a;
					dstPage = mbyLogBlockSize;
				} else if (tval1 < srcPage) {
					dstPage = mbyLogBlockSize;
				} else {
					dstPage = tval1 + dstPhyBlock;
					srcPage = mbyLogBlockSize - dstPage;
				}
				rc = CopyPagesDMA(var_x113, var_x114, var_x118, mwTailSrcPhyBlock, dstPhyBlock, dstPage, 0);
			}
		}
		if (rc) {
			if (rc != 0x6d) {
				vara_6 = 0l
				return rc;
			}
			rc = GetEmptyPhyBlock(var_x113, &var_x118);
			if (rc)
				return rc;
		} else {
			if (EraseBlock(var_x113, mwTailSrcPhyBlock))
				return 0x6d;
			break;
		}
	}
	
	var_x10a = -1;
	var_x10c = -1;
	mwHeadSrcPhyBlock = -1;
	var_x110 = -1;
	var_x112 = 0;
	vara_6 = 0;
	return 0;
}

char
CSM::ReadSectors(int arg_1, short *arg_2, char *arg_3, char arg_4)
{
	char rc;

	if (arg_1 != -1)
		var_x11c = arg_1;
	if (muiSMReadSector != -1) {
		var_x11c = muiSMReadSector;
		muiSMReadSector = -1;
	}
	rc = ReadSector(var_x11c, *arg_2, arg_4);
	if (!rc) {
		var_x11c++;
		*arg_2--;
		*arg_3--;
	}
	return rc;
}
