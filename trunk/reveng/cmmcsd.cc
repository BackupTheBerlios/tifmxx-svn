#include "cmmcsd.h"

CMMCSD::CMMCSD(char *_base_addr)
       :CFlash(_base_addr), wBlockLen(0x200), cmmcsd_var_2(0), 
	cmmcsd_var_1(0), cmmcsd_var_4(0), muiMediaID(3), byStatus(0),
	pCurrentParam(0), cmmcsd_var_7(0), muiExecute_DMAPagesTotal(0), cmmcsd_var_9(0)
{
	KeInitializeEvent(cmmcsd_event_1, 0, 0);
	write32(base_addr + 0x118, 0);
}

CMMCSD::~CMMCSD()
{
	write32(base_addr + 0x118, 0);
	~CFlash();
	//! free memory
}

int
CMMCSD::vtbl02()
{
	int r_val = CFlash::vtbl02();

	if(vara_0 != 0)
	{
		if(cmmcsd_var_10 & 0x8)
		{ 
			write32(base_addr + 0x118, 0x14 | read32(base_addr + 0x118));
			cmmcsd_var_2 = 1;
		}

		if(cmmcsd_var_10 & 0x10) cmmcsd_var_2 = 0;

		if(cmmcsd_var_10 & 0x4) cmmcsd_var_2 = 1;

		byStatus = cmmcsd_var_11;
		KeSetEvent(cmmcsd_event_1, 0, 0);
		vara_0 = 0;
	}
	return r_val;
}

char
CMMCSD::vtbl03(char arg_1, char arg_2)
{
	char r_val = CFlash::vtbl03(arg_1, arg_2);

	if(arg_2 != 0)
	{
		cmmcsd_var_10 = read32(base_addr + 0x114);
		cmmcsd_var_11 = byStatus | cmmcsd_var_10;
		write32(base_addr + 0x114, cmmcsd_var_10);
	}

	return r_val | arg_2;
}

char*
CMMCSD::LongName()
{
	if(muiMediaID == 0x13) return "MULTIMEDIACARD";
	else return "SDCARD";
}

char*
CMMCSD::Name()
{
	if(muiMediaID == 0x13) return "MMC";
	else return "SD";
}

char
CMMCSD::RescueRWFail()
{
	CFlash::RescueRWFail();
	return InitializeCard();
}

// assume all time deltas in 0.1us
char
CMMCSD::InitializeCard()
{
	write32(base_addr + 0x168, 0x2);
	mwClkSpeed = 0x3c;
	write32(base_addr + 0x110, 0xb);
	long t1, t2; 
	char r_val = 0x2f;
	
	KeQuerySystemTime(&t1);

	do
	{
		KeQuerySystemTime(&t2);
		if(0x1 & read32(base_addr + 0x16c))
		{
			r_val = 0;
			break; // reset ok
		}
		
	} while(t2 - t1 < 2500000);
	if(r_val) return r_val; // time-out on reset

	write32(base_addr + 0x12c, 0);
	write32(base_addr + 0x110, mwClkSpeed | 0x800);
	write32(base_addr + 0x130, 0x8000);
	write32(base_addr + 0x118, 0x41e9);
	write32(base_addr + 0x138, 0x20 | read32(base_addr + 0x138));
	write32(base_addr + 0x11c, 0x40);
	write32(base_addr + 0x120, 0x7ff);
	// card initialization sequence
	write32(base_addr + 0x104, 0x80);
	write32(base_addr + 0x110, mwClkSpeed | 0x800);
	sub_0_1FEE0(cmmcsd_event_1, -1000000); // arg_2: 0xfff0bdc0

	if(vara_4 != 0) return 0x86;

	if(0 != (r_val = DetectCardType())) return r_val;

	if(muiMediaID == 0x43) // SDIO card
	{
		write16zx(base_addr + 0x4, 0x80);
		return 0x2e;
	}
	else if(muiMediaID == 0x23 && (SDSwitch & 0x1)) // SD card
	{
		write16zx(base_addr + 0x4, 0x80);
		muiMediaID = 0x43;
		return 0x2e;
	}
	else if(muiMediaID == 0x13 && (SDSwitch & 0x2))
	{
		write16zx(base_addr + 0x4, 0x80);
		muiMediaID = 0x43;
		return 0x2e;
	}
	
	if(0 != (r_val = Standby())) return r_val;
	if(0 != (r_val = ReadCSDInformation())) return r_val;

	write32(base_addr + 0x110, mwClkSpeed | ( 0xffc0 & read32(base_addr + 0x110)));
	
	if(0 != (r_val = GetCHS())) return r_val;
	
	ReportMediaModel(); // print some debug info

	if(0 != (r_val = ExecCardCmd(0x7, dwRCA, 0x2900))) return r_val;
	if(0 != (r_val = ExecCardCmd(0x10, wReadBlockLen, 0x2100))) return r_val;

	write32(base_addr + 0x128, wReadBlockLen - 1);
	if(muiMediaID == 0x23)
	{
		write16zx(base_addr + 0x4, 0x100 | read16(base_addr + 0x4));
		ClkFreq = 24000000; 
		if(0 != (r_val = ExecCardCmd(0x37, dwRCA, 0x2100))) return r_val;
		if(0 != (r_val = ExecCardCmd(0x2a, 0, 0x2100))) return r_val;

		char lvar_x30 = 0;
		ExecParam lvar_x38 = {0xd, 0, 0x40, 1, 1, 1, 0, 0};

		if(0 != (r_val = Execute(&lvar_x38, 0, &lvar_x30, &lvar_x60))) return r_val;
		if(0xff00 & (((short)lvar_x63 << 0x8) + lvar_x64)) return 0x27;
		
		lvar_x30 = 0;
		lvar_x38 = {0x33, 0, 0x8, 1, 1, 1, 0, 0};

		if(0 != (r_val = Execute(&lvar_x38, 0, &lvar_x30, &lvar_x50))) return r_val;

		if(0x4 & (0xf & lvar_x51))
		{
			if(0 != (r_val = ExecCardCmd(0x37, dwRCA, 0x2100))) return r_val;
			if(0 != (r_val = ExecCardCmd(0x6, 0x2, 0x2100))) return r_val;
			write32(base_addr + 0x110, 0x8800 | mwClkSpeed);
		}
		WriteProtected |= (0x200 & read16(base_addr + 0x8)) ? 0x1 : 0; 		
	} //1F4FC
	
	for(short cnt = 0; cnt < 10000; cnt++)
	{
		if(0 != (r_val = GetState(&lvar_x31, 0))) break;
		if(lvar_x31 != 0x4) continue;
		if(cmmcsd_var_14 == 1) break;		
	}

	vara_2 = 1;
	return r_val;
}

char 
CMMCSD::ReadSectors(int arg_1, short *arg_2, short *arg_3, char arg_4) // arg_4 unused?
{	
	char r_val;

	if(arg_1 != -1)
	{
		if(arg_1 >= dwBlocks) return 0x82;		
		if(!*arg_2) return 0;
		if(*arg_2 > 0x800) return 0x2b;
				
		char mdwMMCSD_STATUS; // lvar_x20;

		for(short cnt = 0; cnt < 10000; cnt++)
		{
			if(0 != (r_val = GetState(&mdwMMCSD_STATUS, 0))) break;
			if(mdwMMCSD_STATUS != 4) continue;
			if(cmmcsd_var_14 == 1) goto x1F61F;
		}

		if(mdwMMCSD_STATUS != 4) goto x1F62D; //decode this condition

x1F61F:		if(r_val != 0 || cmmcsd_var_14 == r_val)
		{
x1F62D:			if(r_val == 0x86) return 0x86; // thread killed
			else if(0 != (r_val = InitializeCard())) return r_val;
		}
x1F65C:
		write32(base_addr + 0x12c, *arg_2 - 1);
		write32(base_addr + 0x128, wReadBlockLen - 1);
		write32(base_addr + 0x130, 0x8000);
		KeSynchronizeExecution(card_int, sub_0_1C100, &byStatus); // wait for ISR connected to card_int?
		if(*arg_2 == 1) // read single block
			r_val = ExecCardCmd(0x11, arg_1 << byReadBlockLen, 0xb100);
		else // read multiple blocks
			r_val = ExecCardCmd(0x12, arg_1 << byReadBlockLen, 0xb100);
		
		if(r_val) return r_val;
	} //1F72A

	if(*arg_2 > *arg_3)
	{
		*arg_2 -= *arg_3;
		*arg_3 = 0;
		return r_val;
	}
	if(0 != (r_val = WaitForBRS())) return r_val; // BRS error

	if(*arg_2 != 1 || arg_1 == -1)
	{ // 1F776
		r_val = ExecCardCmd(0xc, 0, 0x2900); // stop transmission
		if(r_val)
		{
			if(r_val == 0x86) return r_val; // card removed
			if(cmmcsd_var_9 == 0)
			{
				cmmcsd_var_9 = 1;
				if(0 != (r_val = InitializeCard())) return r_val;
			}
		} 
		else // dead clause?
		{ // 1F7E2
			if(r_val == 0x86) return r_val;
		}
	}	
// 1F7D5
	*arg_3 -= *arg_2;
	*arg_2 = 0;
	return r_val;
}

// Has errors!!!
char
CMMCSD::WriteSectors(int arg_1, short *arg_2, short *arg_3)
{
	char r_val;

	WriteProtected |= (0x200 & read16(base_addr + 0x8)) ? 1 : 0;
	if(WriteProtected) return 0xc1; // card is write protected

	if(arg_1 != -1)
	{
		if(arg_1 >= dwBlocks) return 0x82;
		if(!*arg_2) return 0;
		if(*arg_2 > 0x800) return 0x2b;
		
		char mdwMMCSD_STATUS; // lvar_x20;

		for(short cnt = 0; cnt < 10000; cnt++)
		{
			if(0 != (r_val = GetState(&mdwMMCSD_STATUS, 0))) break;
			if(mdwMMCSD_STATUS != 4) continue;
			if(cmmcsd_var_14 == 1) goto x1F92C;
		}

		if(mdwMMCSD_STATUS != 4) goto x1F93A; //decode this condition

x1F92C:		if(r_val != 0 || cmmcsd_var_14 == r_val)
		{
x1F93A:			if(r_val == 0x86) return 0x86; // thread killed
			else if(0 != (r_val = InitializeCard())) return r_val;
		}
x1F955:
		write32(base_addr + 0x12c, *arg_2 - 1);
		write32(base_addr + 0x128, wReadBlockLen - 1);
		write32(base_addr + 0x130, 0x80);
		KeSynchronizeExecution(card_int, sub_0_1C100, &byStatus); // wait for ISR connected to card_int?
		if(*arg_2 == 1) // write single block
			r_val = ExecCardCmd(0x18, arg_1 << byWriteBlockLen, 0x3100);
		else // write multiple blocks
			r_val = ExecCardCmd(0x19, arg_1 << byWriteBlockLen, 0x3100);

		if(r_val) return r_val;
	}
// 1FA18
	if(*arg_2 <= *arg_3)
	{
		if(0 != (r_val = WaitForBRS())) return r_val; // BRS error

		if(*arg_2 != 1 || arg_1 == -1)
		{ // 1FA7D
			mdwMMCSD_STATUS = 0;
			for(short cnt = 0; cnt < 10000; cnt++)
			{
				r_val = GetState(&mdwMMCSD_STATUS, 0);
				if(r_val) break;
				if(mdwMMCSD_STATUS == 7) continue;
				if(cmmcsd_var_14 == 1)
				{
					r_val = ExecCardCmd(0xc, 0, 0x2900);
					break;
				}
			}
			if(r_val) return r_val;
		}
		// 1FB51	
		mdwMMCSD_STATUS = 0;
		for(short cnt = 0; cnt < 10000; cnt++)
		{
			r_val = GetState(&mdwMMCSD_STATUS, 0);
			if(r_val) break;
			if(mdwMMCSD_STATUS != 4) continue;
			if(cmmcsd_var_14 == 1) goto x1FB9B;
		}
		if(mdwMMCSD_STATUS != 4) goto x1FBA9;
	x1FB9B: if(!r_val && cmmcsd_var_14 != r_val) goto x1FBD4;
	x1FBAB: if(r_val == 0x86) return 0x86;
		if(0 != (r_val = InitializeCard())) return r_val;
	x1FBD4: *arg_3 -= *arg_2;
		*arg_2 = 0;
	}
	else
	{ // 1FBE6
		*arg_2 -= *arg_3;
		*arg_3 = 0;
	}

	KeSynchronizeExecution(card_int, sub_0_27DE0, &byStatus);
	return r_val;
}

char
CMMCSD::WriteProtectedWorkaround()
{
	return 0;
}

char
CMMCSD::DetectCardType()
{
	char r_val, cnt_1 = 0;

	while(1)
	{
		write32(base_addr + 0x10c, 0);
		write32(base_addr + 0x108, 0);
		KeSynchronizeExecution(card_int, sub_0_27DE0, &byStatus);
		KeClearEvent(cmmcsd_event_1);

		write32(base_addr + 0x104, 0x1305);
		if(0x20 != (r_val = WaitForEOC()))
		{ // 1D0D2
			// SDIO card
			if(r_val) break;
			muiMediaID = 0x43;
			return 0;
		}
		
		if(cnt_1 >= 2 || muiMediaID == 3 ) break;
		cnt_1++;		
	}

	int cnt_2 = 3;

	do
	{ // 1D010:
		write32(base_addr + 0x104, 0x80);
		sub_1_FEEE0(&cmmcsd_event_1, -1000000); // if return != 0 print message
		cnt_1 = 0;
	
		while(1)
		{
			write32(base_addr + 0x10c, 0);
			write32(base_addr + 0x108, 0);
			KeSynchronizeExecution(card_int, sub_0_27DE0, &byStatus);
			KeClearEvent(cmmcsd_event_1);
			write32(base_addr + 0x104, 0);
			if(0x20 != (r_val = WaitForEOC()))
			{
				if(!r_val) 
				{ // 1D11C
					r_val = ExecCardCmd(0x37, 0, 0x2100);
					if(r_val == 0x20)
					{
						//MMC card
						ExecCardCmd(0, 0, 0);
						muiMediaID = 0x13;
						return 0;						
					}
					
					if(r_val == 0x86) return r_val;
					if(r_val) return r_val;
					//SD card
					muiMediaID = 0x23;
					return 0;
				}
				break;
			}
			if(cnt_1 >= 2 || muiMediaID == 3) break;
			cnt_1++;
		}

		cnt_2--;
	} while(cnt_2);

	return r_val;			
}

char
CMMCSD::Standby()
{
	long t1, t2; 
	int lvar_x20 = (muiMediaID == 0x23) ? 0x29 : 0x1;
	char r_val = 0, cnt_1 = 0, cnt_2 = 0, cnt_3 = 0;

	KeQuerySystemTime(&t1);
	do
	{
		if(cnt_2 && muiMediaID == 0x23)
		{
			cnt_1 = 0;

			while(1)
			{
				write32(base_addr + 0x10c, 0);
				write32(base_addr + 0x108, 0);
				KeSynchronizeExecution(card_int, sub_0_27DE0, &byStatus);
				KeClearEvent(cmmcsd_event_1);
				write32(base_addr + 0x104, 0x2137);
				if(0x20 != (r_val = WaitForEOC()))
				{
					if(r_val) return r_val;
					break;
				}				
				if(cnt_1 >= 2 || muiMediaID == 3) return r_val;
				cnt_1 ++;
			}
		}

		cnt_2 = 1;
		cnt_1 = 0;

		while(1)	
		{
			write32(base_addr + 0x10c, 0x80fc);
			write32(base_addr + 0x108, 0);
			KeSynchronizeExecution(card_int, sub_0_27DE0, &byStatus);
			KeClearEvent(cmmcsd_event_1);
			write32(base_addr + 0x104, (lvar_x20 & 0x3f) | 0x1300);
			if(0x20 != (r_val = WaitForEOC())) break;
			if(cnt_1 >= 2 || muiMediaID == 3) return cnt_3 ? 0x28 : 0x27;	
			cnt_1 ++;
		};

		cnt_3 = 1;
		if(r_val) return r_val;
		KeQuerySystemTime(&t2);
		if(sub_0_1C1F0(0x1f, 0x1f, 0)) break;
	} while(t2 - t1 <= 10000000);
	
	if(!sub_0_1C1F0(0x1f, 0x1f, 0) return 0x2d;
	//send ALL_SEND_CID
	if(0 != (r_val = ExecCardCmd(0x2, 0, 0x1200))) return r_val;
	//send SEND_REL_ADDR
	if(0 != (r_val = ExecCardCmd(0x3, 0x20000, 0x1600))) return r_val;

	if(muiMediaID == 0x13)
	{
		dwRCA = 0x20000;
		return 0;
	}
	if(muiMediaID == 0x23)
	{
		dwRCA = sub_0_1C1F0(0x1f, 0, 0) & 0xffff0000;
		return dwRCA ? 0 : 0x2c;
	}
	return 0x83;
}

char
CMMCSD::ReadCSDInformation()
{
	char r_val, cnt_1 = 0, cnt_2 = 0, match;
	short pRespBuffer[8]; // rsp + 0x48

	do
	{
		write32(base_addr + 0x10c, dwRCA >> 16);
		write32(base_addr + 0x108, dwRCA & 0xffff);
		KeSynchronizeExecution(card_int, sub_0_27DE0, &byStatus);
		KeClearEvent(cmmcsd_event_1);
		write32(base_addr + 0x104, 0x2209);
		if(0x20 != (r_val = WaitForEOC())) break;
	} while(cnt_1 < 3 && muiMediaID !=3);
	
	if(r_val == 0x21)
	{
		cnt_1 = 0; cnt_2 = 0;
		if(muiMediaID != 0x13) return r_val;
		do
		{
			pRespBuffer[cnt_2] = sub_0_1C1F0(cnt_1 + 0xf, cnt_1, 1);
			cnt_1 += 16;
			cnt_2++;
		}while(cnt_1 <= 0x70);
		
		cnt_1 = 0;
		
		do
		{
			write32(base_addr + 0x10c, dwRCA >> 16);
			write32(base_addr + 0x108, dwRCA & 0xffff);
			KeSynchronizeExecution(card_int, sub_0_27DE0, &byStatus);
			KeClearEvent(cmmcsd_event_1);
			write32(base_addr + 0x104, 0x2209);
			if(0x20 != (r_val = WaitForEOC())) break;
		} while(cnt_1 < 3 && muiMediaID !=3);

		if(r_val == 0x21)
		{
			cnt_1 = 0; cnt_2 = 0; match = 1;
			do
			{
				if(!match || pRespBuffer[cnt_2] != sub_0_1C1F0(cnt_1 + 0xf, cnt_1, 1))
					match = 0;
				else match = 1;
				cnt_1 += 16;
				cnt_2++;
			}while(cnt_1 <= 0x70);
			if(!match) return r_val;
		}
		else if(r_val) return r_val;
	}
	else if(r_val) return rval;

	

	int lvar_x28[] = {10000, 100000, 1000000, 10000000};	

	//! char byCSD = sub_0_1C1F0(0x7f, 0x7e, 1); // CSD version
	TAAC = sub_0_1C1F0(0x77, 0x70, 1);
	NSAC = sub_0_1C1F0(0x6f, 0x68, 1);
	R2W_FACTOR = sub_0_1C1F0(0x1c, 0x1a, 1);

	ReadTimeOut = (NSAC + TAAC) * 10;
	
	WriteTimeOut = R2W_FACTOR * ReadTimeOut;

	char t_TRAN_SPEED = sub_0_1C1F0(0x67, 0x60, 1);
	int dwTranSpeed = 10000;
	char lvar_x38[] = {10, 10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80};

	if(t_TRANS_SPEED & 0x7 <= 3) dwTranSpeed = lvar_x28[t_TRANS_SPEED & 0x3];
	
	dwTranSpeed *= lvar_x38[(t_TRANS_SPEED >> 0x3) & 0xf];
	mwClkSpeed = ClkFreq / dwTranSpeed;
	if(mwClkSpeed * dwTranSpeed < ClkFreq) mwClkSpeed ++;

	if(!mwClkSpeed) mwClkSpeed = 1;
	
	byReadBlockLen = sub_0_1C1F0(0x53, 0x50, 1);
	wReadBlockLen = 1 << byReadBlockLen;
	byWriteBlockLen = sub_0_1C1F0(0x19, 0x16, 1);
	wWriteBlockLen = 1 << byWriteBlockLen;
	mdwSize = sub_0_1C1F0(0x49, 0x3e, 1);
	mbySizeMult = sub_0_1C1F0(0x31, 0x2f, 1);
	mbWriteProtected = (sub_0_1C1F0(0xd, 0xd, 1) == 1); // tmp wp
	mbCRSImplemented = (sub_0_1C1F0(0x4c, 0x4c, 1) == 1); // aka DSR
	return 0;
}

char
CMMCSD::GetCHS()
{	
	mdwBlocks = (1 << (mbySizeMult + 2)) * (mdwSize + 1);
	int l_dwCardCapacity = wReadBlockLen * mdwBlocks;
	mwSize = l_dwCardCapacity >> 0x14;
	if(mwSize <= 2)
	{
		mwHeadCount = 2;
		mwSectorsPerTrack = 16;
	}
	if(mwSize > 2 && mwSize <= 16)
	{
		mwHeadCount = 2;
		mwSectorsPerTrack = 32;
	}
	if(mwSize > 16 && mwSize <= 32)
	{
		mwHeadCount = 4;
		mwSectorsPerTrack = 32;
	}
	if(mwSize > 32 && mwSize <= 128)
	{
		mwHeadCount = 8;
		mwSectorsPerTrack = 32;
	}
	if(mwSize > 128 && mwSize <= 256)
	{
		mwHeadCount = 16;
		mwSectorsPerTrack = 32;
	}
	if(mwSize > 256 && mwSize <= 504)
	{
		mwHeadCount = 16;
		mwSectorsPerTrack = 63;
	}
	if(mwSize > 504 && mwSize <= 1008)
	{
		mwHeadCount = 32;
		mwSectorsPerTrack = 63;
	}
	if(mwSize > 1008 && mwSize <= 2016)
	{
		wReadBlockLen >> 1;
		mwHeadCount = 64;
		mwSectorsPerTrack = 63;
	}
	if(mwSize > 2016 && mwSize <= 2048)
	{
		wReadBlockLen >> 1;
		mwHeadCount = 128;
		mwSectorsPerTrack = 63;
	}
	if(mwSize > 2048)
	{
		//Size exceeds CHS table
		mwHeadCount = 128;
		mwSectorsPerTrack = 64;
	}
	
	mwCylinders = l_dwCardCapacity / (mwSectorsPerTrack * mwHeadCount * wReadBlockLen);
	return ReadSerialNumber();
}

char
CMMCSD::ReadSerialNumber()
{
	char r_val, char cnt = 0;

	do
	{
		write32(base_addr + 0x10c, dwRCA >> 16);
		write32(base_addr + 0x108, dwRCA & 0xffff);
		KeSynchronizeExecution(card_int, sub_0_27DE0, &byStatus);
		KeClearEvent(cmmcsd_event_1);
		write32(base_addr + 0x104, 0x220a);
	
		if(0x20 != (r_val = WaitForEOC()))
		{ // 1DCF5
			if(r_val) break;
			cnt_1 = 0; char cnt_2 = 0;
			
			do
			{
				mpSerialNumber[cnt_2] = sub_0_1C1F0(0x7f - cnt_1, 0x78 - cnt_1, 1);
				cnt_2 ++; cnt_1 += 8;
			} while(cnt_1 < 0x48);
			mpSerialNumber[9] = '0'; mpSerialNumber[10] = '0'; mpSerialNumber[11] = '0';
			short lvar_x22 = sub_0_1C1F0(0x37, 0x30, 1); // revision
			SerialNumber = sub_0_1C1F0(0x2f, 0x18, 1);  // PSN must start at 0x10 - bug?
			sprintf(&mpSerialNumber[12], "%04X%04X", lvar_x22, SerialNumber);
			mpSerialNumber[20] = 0;
			return 0;
		}
		
		cnt ++;
	} while(muiMediaID != 3 && cnt < 3);
	return r_val;
}

char
CMMCSD::ExecCardCmd(char arg_1, int arg_2, short arg_3)
{
	char r_val, cnt = 0;

	do
	{
		write32(base_addr + 0x10c, (arg_2 >> 16) & 0xffff);
		write32(base_addr + 0x108, arg_2 & 0xffff);
		KeSynchronizeExecution(card_int, sub_0_27DE0, &byStatus);
		KeClearEvent(cmmcsd_event_1);
		write32(base_addr + 0x104, (arg_3 & 0xff80) | (arg_1 & 0x3f));
		if(0x20 != (r_val = WaitForEOC())) break;
		cnt ++;		
	} while(cnt < 3 && muiMediaID != 3);

	return r_val;
}

char 
CMMCSD::Execute(ExecParam *pParam, int uiDMAPhysicalAddress, char *uiDMAPageCount, char *pData)
{
	char r_val;

	if(pParam)
	{
		pCurrentParam = pParam;
		muiExecute_DMAPagesProcessed = 0;
		//muiExecute_DMAPages total: 
		muiExecute_DMAPagesTotal= pParam->uiDataTransferLength >> 9;		
	}
	
	short lvar_1 = 0;

	if(uiDMAPhysicalAddress)
	{
		muiExecute_DMAPagesProcessed += *uiDMAPageCount;
		if(*uiDMAPageCount > 63) return 0xc0;
		if(vara_6) return 0xc3; // transfer still in progress
		vara_6 = 1;
		if(*uiDMAPageCount)
		{
			if(pParam)
			{				
				// first DMA, clear FIFO, Enable interrupt
				write16zx(base_addr + 0x18, 0xffff);
				write16zx(base_addr + 0x24, 0x1);
				write16zx(base_addr + 0x14, 0x5);
				vara_5 = 0;
				if(pParam->bDIR)
					write16zx(base_addr + 0x130, 0x8000);
				else
				{
					lvar_1 = 0x8000;
					write16zx(base_addr + 0x130, 0x80);
				}
				// set block length
				write32(base_addr + 0x12c, muiExecute_DMAPagesTotal - 1);
				write32(base_addr + 0x128, wReadBlockLen - 1);
				KeSynchronizeExecution(card_int, sub_0_1C100, &byStatus);				
			}
		// 1E7BD
			write32(base_addr + 0xc, uiDMAPhysicalAddress);						
			write16zx(base_addr + 0x10, 0x1 | lvar_1 | (0x80 & read16(base_addr + 0x10)) 
							| (*uiDMAPageCount << 8));			
		}
	}
	else // 1E804
	{
		write32(base_addr + 0x130, 0);
		write32(base_addr + 0x118, 0x0c00 | read32(base_addr + 0x118));
	}
	// 1E83D
	if(pParam)
	{
		//pParam->uiCommandIndex       - 0x000
		//pParam->uiCommandArgument    - 0x004
		//pParam->uiDataTransferLength - 0x008
		//pParam->uiResponseType       - 0x00c
		//pParam->bDIR                 - 0x00d
		//pParam->bApp                 - 0x00e
                //pParam->bRESP                - 0x00f
		//pParam->bBLKM                - 0x010
		if(pParam->bAPP) ExecCardCmd(0x37, dwRCA, 0x2100); // send SD escape for extended command

		short lvar_2 = 0;

		switch(pParam->uiResponseType - 1)
		{
			case 0: lvar_2 = 0x100; break; // 1E94C
			case 1: lvar_2 = 0x200; break; // 1E958
			case 2: lvar_2 = 0x300; break; // 1E964
			case 3: lvar_2 = 0x100; break; // 1E94C
			case 4: lvar_2 = 0x900; break; // 1E952
			case 5: lvar_2 = 0x600; break; // 1E95E
			case 6: lvar_2 = 0x300; break; // 1E964
		}
		// 1E96D
		if(pParam->uiCommandIndex == 0x3 && !pParam->bApp) lvar_2 = 0x600;
		if(pParam->uiCommandIndex == 0x7 && !pParam->bApp) lvar_2 = 0x900;
		lvar_2 |= sub_0_1E1B0(pParam->uiCommandIndex, pParam->bApp);
		if(pParam->bDIR && 0x3000 == (lvar_2 & 0x3000)) lvar_2 |= 0x8000;
		// 1E9D6
		if(pData)
		{
			write32(base_addr + 0x12c, 0);
			if(pParam->uiDataTransferLength)
				write32(base_addr + 0x128, pParam->uiDataTransferLength - 1);
		}
		r_val = ExecCardCmd(pParam->uiCommandIndex, pParam->uiCommandArgument, lvar_2);
	}
	// 1EA53
	if(uiDMAPhysicalAddress)
	{
		if(muiExecute_DMAPagesProcessed >= muiExecute_DMAPagesTotal)
		{
			if(!r_val)
			{ 
				if(pCurrentParam->bBLKM != 0)
				{
					if(0 != (r_val = WaitForRBS()) || 0 != (r_val = ExecCardCmd(0xc, 0, 0x2900)))
					{
						write16zx(base_addr + 0x18, 0xffff);
						write16zx(base_addr + 0x10, 0x2);
						return r_val;
					}			
				}
				// 1EB2D
				if(!pCurrentParam->bDIR) r_val = WaitForCard();
			}
		}
		// 1EB56
		if(r_val)
		{
			write16zx(base_addr + 0x18, 0xffff);
			write16zx(base_addr + 0x10, 0x2);
			return r_val;
		}	
		// 1EB97
		if(!KeSynchronizeExecution(card_int, sub_0_1C050, &var_2))
		{
			do
			{
				if(0 != (r_val = sub_0_1FEE0(&event_2, -10000000))) || vara_4)
				{
					write16zx(base_addr + 0x18, 0xffff);
					write16zx(base_addr + 0x10, 0x2);
					vara_6 = 0;
					return (vara_4 != 0) ? 0x86 : r_val;
				}
			} while(!KeSynchronizeExecution(card_int, sub_0_1C050, &var_2))				
		}
		// 1EC00
		KeSynchronizeExecution(card_int, sub_0_27DE0, &var_2);
		vara_6 = 0;
	}
	// non DMA transfer
	if(!pData || r_val) return r_val;
	lvar_1 = pCurrentParam->uiDataTransferLength;
	if(pCurrentParam->bRESP)
	{
		if(pCurrentParam->uiResponseType != 2 && pCurrentParam->uiDataTransferLength != 0) lvar_1 --;
		
		if(lvar_1)
		{
			char lvar_3 = (lvar_1 << 3) - 1;			
			do
			{
				*pData = sub_0_1C1F0(lvar_3, lvar_3 - 7, (pCurrentParam->uiResponseType == 2));
				lvar_3 -= 8; pData ++; lvar_1 --;
			} while(lvar_1);
		}
		if(pCurrentParam->uiResponseType != 2 && pCurrentParam->uiDataTransferLength != 0) *pData = 0;
		return r_val;
	}
	else
	{ // 1ED32 direct byte access to card
		short lvar_4;
		if(pCurrentParam->bDIR)
		{
			if(lvar_1)
			{			
				do
				{
					WaitForAF();
					lvar_4 = (short)read32(base_addr + 0x124);					
					lvar_1 --;
					*pData = lvar_4 & 0xff;
					if(!lvar_1) break;
					pData += 2;
					*(pData - 1) = (lvar_4 >> 8) & 0xff;
					lvar_1 --;					
				} while(lvar_1);				
			}
		}
		else
		{ // 1EDD1
			write32(base_addr + 0x118, 0x14 | read32(base_addr + 0x118));
			mbCardBusy = 1;
			if(lvar_1)
			{
				do
				{
					lvar_4 = *pData; pData ++; lvar_1 --;
					if(lvar_1)
					{
						lvar_4 |= *pData << 8;
						pData ++; lvar_1 --;
					}
					WaitForAE();
					write32(base_addr + 0x124, lvar_4);					
				} while(lvar_1);
			}
			if(!pCurrentParam->bAPP)
			{
				if(0 != (r_val = WaitForCard())) return r_val;
			}
		}
		// 1ED84
		write32(base_addr + 0x118, 0xfffff3ff & read32(base_addr + 0x118));
		return r_val;
	}
}

char 
CMMCSD::GetState(char *byCardState, char arg_2)
{
	char r_val, cnt = 0;
	long l_interval;

	do
	{
		write32(base_addr + 0x10c, dwRCA >> 16);
		write32(base_addr + 0x108, dwRCA & 0xffff);
		KeSynchronizeExecution(card_int, sub_0_27DE0, &byStatus);
		KeClearEvent(cmmcsd_event_1);
		write32(base_addr + 0x104, 0x210d);
		if(0x20 != (r_val = WaitForEOC())) break;
		cnt ++;		
	} while(muiMediaID != 3 && cnt < 3);
	*byCardState = sub_0_1C1F0(0xc, 0x9, 0);
	Ready = sub_0_1C1F0(0x8, 0x8, 0);
	if(arg_2)
	{
		// print thingy
	}
	if(muiMediaID == 0x13)
	{
		if(mwSize > 2 && mwSize <= 16)
		{
			l_interval = -100000;
			KeDelayExecutionThread(0, 0, l_interval);
			return r_val;
		}
	}
	if(!Ready) 
	{
		l_interval = -500;
		KeDelayExecutionThread(0, 0, l_interval);
	}

	return r_val;		
}

char 
CMMCSD::WaitForEOC()
{
	char r_val = 0;

	if(KeSynchronizeExecution(card_int, sub_0_1C050, &byStatus)) return 0;
	
	do
	{
		if(r_val) break;
		if(vara_4) return 0x86;
		if(KeSynchronizeExecution(card_int, sub_0_1C060, &byStatus))
		{
			if(byStatus & 0x00004000) r_val = 0x2a; // Status error, may be bit clear
			if(byStatus & 0x00000080) r_val = 0x20; // timeout error
			if(byStatus & 0x00000100) r_val = 0x21; // CRC error	
 		}
		if(!r_val)
		{
			r_val = sub_0_1FEE0(&cmmcsd_event_1, -10000000);
			if(vara_4) return 0x86;
			if(KeSynchronizeExecution(card_int, sub_0_1C060, &byStatus))
			{
				if(byStatus & 0x00004000) r_val = 0x2a; // Status error, may be bit clear
				if(byStatus & 0x00000080) r_val = 0x20; // timeout error
				if(byStatus & 0x00000100) r_val = 0x21; // CRC error	
				return r_val;
			}
		}
	} while(!KeSynchronizeExecution(card_int, sub_0_1C050, &byStatus));

	if(r_val != 0x87) return r_val;
	if(1 & read32(base_addr + 0x114)) return 0; // success
	return r_val;
}

char 
CMMCSD::WaitForBRS()
{
	char r_val = 0;

	if(KeSynchronizeExecution(card_int, sub_0_1C0D0, &byStatus)) return 0;
	
	do
	{
		if(r_val) break;
		if(vara_4) return 0x86;
		if(KeSynchronizeExecution(card_int, sub_0_1C0D0, &byStatus))
		{
			if(byStatus & 0x00004000) r_val = 0x2a; // Status error, may be bit clear
			if(byStatus & 0x00000080) r_val = 0x20; // timeout error
			if(byStatus & 0x00000100) r_val = 0x21; // CRC error	
			
		}
		if(!r_val)
		{
			r_val = sub_0_1FEE0(&cmmcsd_event_1, -20000000);
			if(vara_4) return 0x86;
			if(KeSynchronizeExecution(card_int, sub_0_1C0D0, &byStatus))
			{
				if(byStatus & 0x00004000) r_val = 0x2a; // Status error, may be bit clear
				if(byStatus & 0x00000080) r_val = 0x20; // timeout error
				if(byStatus & 0x00000100) r_val = 0x21; // CRC error	
				return r_val;
			}
		}

	} while(!KeSynchronizeExecution(card_int, sub_0_1C0D0, &byStatus));
	
	if(r_val != 0x87) return r_val;
	if(0x8 & read32(base_addr + 0x114)) return 0; // success
	return r_val;
}

// case with bit field spanning 3 reads is unhandled
int 
CMMCSD::sub_0_1C1F0(char bf_end, char bf_start, char reg)
{
	if(bf_end < bf_start || bf_end > 0x7f || bf_end > bf_start + 0x20) return 0;
	
	edi = bf_start;
	r14d = bf_end >> 4;
	esi = bf_end;
	r13 = this;
	ebp = bf_start >> 4;
	r12d = (op) ? 0 : 6;
	
	int r_val, r_mask;

	short lvar_x40[16] = {0x1, 0x3, 0x7, 0xf, 0x1f, 0x3f, 0x7f, 0xff, 
			      0x1ff, 0x3ff, 0x7ff, 0xfff, 0x1fff, 0x3fff, 0x7fff, 0xffff};

	short lvar_x20[16] = {0xffff, 0xfffe, 0xfffc, 0xfff8, 0xfff0, 0xffe0, 0xffc0, 0xff80,
			      0xff00, 0xfe00, 0xfc00, 0xf800, 0xf000, 0xe000, 0xc000, 0x8000};

	

	r_val = lvar_x40[bf_end - ((bf_end >> 4) << 4)] & 
		read32(base_addr + 0x144 + (((bf_end >> 4) + (op ? 0 : 6)) << 2));

	if((bf_end >> 4) != (bf_start >> 4))
	{
		r_val <<= 0x10;
		r_mask = lvar_x20[bf_start - ((bf_start >> 4) << 4)] &
			 read32(base_addr + 0x144 + (((bf_start >> 4) + (op ? 0 : 6)) << 2));
		
	}
	else
	{
		rval &= lvar_x20[bf_start - ((bf_start >> 4) << 4)];
		r_mask = r_val;
	}
	r_val |= r_mask;
	r_val >>= bf_start - ((bf_start >> 4) << 4);
	return r_val;	
}

short 
CMMCSD::sub_0_1E1B0(char arg_1, char arg_2)
{
	if(arg_1 > 0x3f) return 0;
	short lvar_x00[64] = {0, 0, 0, 0, 0, 0, 0x2000, 0, 0, 0, 0, 0, 0, 0x3000, 0, 0,
			      0, 0, 0x3000, 0, 0, 0, 0x3000, 0x2000, 0, 0x3000, 0x3000, 0, 0, 0, 0, 0,
			      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x3000, 0x3000, 0x3000, 0x3000, 0x3000,
			      0x3000, 0x2000, 0, 0x3000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

	short lvar_x80[64] = {0, 0, 0x1000, 0x1000, 0, 0, 0, 0x2000,
			      0, 0x2000, 0x2000, 0, 0x2000, 0x2000, 0, 0x2000,
			      0x2000, 0x3000, 0x3000, 0, 0, 0, 0, 0,
			      0x3000, 0x3000, 0, 0x3000, 0x2000, 0x2000, 0x3000, 0,
			      0x2000, 0x2000, 0, 0, 0, 0, 0x2000, 0,
			      0, 0, 0x3000, 0, 0, 0, 0, 0,
			      0, 0, 0, 0, 0, 0, 0, 0x2000,
			      0x3000, 0, 0, 0, 0, 0, 0, 0};

	return (arg_2 ? lvar_x00[arg_1] : lvar_x80[arg_1]);
}

char 
CMMCSD::WaitForCard()
{
	char r_val = 0;

	if(mbCardBusy)
	{
		do
		{
			if(r_val) { r_val = 0; break; } // timeout
			if(vara_4) return 0x86;
			r_val = sub_0_1FEE0(&cmmcsd_event_1, -10000000);			
		} while(mbCardBusy);		
	}
	if(r_val) r_val = 0; // timeout
	write32(base_addr + 0x118, 0xffffffeb & read32(base_addr + 0x118));
	return r_val;
}

// low confidence, need checking
char 
CMMCSD::WaitForAF()
{
	char r_val = 0;

	if(KeSynchronizeExecution(card_int, sub_0_1C0F0, &byStatus)) return 0;
	
	do
	{
		if(r_val) break;
		if(vara_4) return 0x86;
		if(KeSynchronizeExecution(card_int, sub_0_1C060, &byStatus))
		{
			if(byStatus & 0x00004000) r_val = 0x2a; // Status error, may be bit clear
			if(byStatus & 0x00000080) r_val = 0x20; // timeout error
			if(byStatus & 0x00000100) r_val = 0x21; // CRC error	
			
		}
		
		if(!r_val) r_val = sub_0_1FEE0(&cmmcsd_event_1, -10000);
	} while(!KeSynchronizeExecution(card_int, sub_0_1C0F0, &byStatus));
	
	if(r_val != 0x87) return r_val;
	return (r_val == 0x87) ? 0 : r_val;
}

char 
CMMCSD::WaitForAE()
{
	char r_val = 0;

	if(KeSynchronizeExecution(card_int, sub_0_1C0E0, &byStatus)) return 0;
	
	do
	{
		if(r_val) break;
		if(vara_4) return 0x86;
		if(KeSynchronizeExecution(card_int, sub_0_1C060, &byStatus))
		{
			if(byStatus & 0x00004000) r_val = 0x2a; // Status error, may be bit clear
			if(byStatus & 0x00000080) r_val = 0x20; // timeout error
			if(byStatus & 0x00000100) r_val = 0x21; // CRC error	
			
		}
		
		if(!r_val) r_val = sub_0_1FEE0(&cmmcsd_event_1, -10000);
	} while(!KeSynchronizeExecution(card_int, sub_0_1C0E0, &byStatus));
	
	if(r_val != 0x87) return r_val;
	return (r_val == 0x87) ? 0 : r_val;
}

void 
CMMCSD::sub_0_1EEA0(char *arg_1)
{
	arg_1[0] = var_xd7;
	arg_1[1] = var_xd6;
}