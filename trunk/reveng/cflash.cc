#include "cflash.h"

CFlash::CFlash(char *_base_addr)
       :base_addr(_base_addr), ClkFreq(20000000), is_xx12(0)
{
	if(0x8 & read16(base_addr + 0x8))
	{ // card inserted
		write16zx(base_addr + 0x28, 0x7);
		write16zx(base_addr + 0x24, 0x1);
		muiMediaID = (read16(base_addr + 0x8) >> 0x4) & 0x7;
	
	}
	else muiMediaID = 0; // card not inserted

	var_2 = 0;
	vara_0 = 0; vara_1 = 0; vara_2 = 0; WriteProtected = 1;
	vara_4 = 0; vara_5 = 0; vara_6 = 0; vara_7 = 0;

	var_3 = 0; var_4 = -1; var_5 = 0;
	KeInitializeEvent(event_1, 0, 0);
	KeInitializeEvent(event_2, 0, 0);
	Size = 0; var_7 = 0;		
}

CFlash::~CFlash()
{
	if(WriteProtected == 0) KeSetEvent(event_1, 0, 0);
	WriteProtected = 1;	
}

char
CFlash::CloseWrite()
{
	return 0;
}

int
CFlash::vtbl02()
{
	int r_val = 0;
	if(vara_1 == 0) return 0;
	if(var_2 & 0x4)
	{
		r_val = 0x200;
		vara_6 = 1;
	}
	else KeSetEvent(event_2, 0, 0); // wake up threads waiting here

	if(var_2 & 0x1) r_val |= 0x0100;
	vara_2 = 0;
	return r_val;
} 

char
CFlash::vtbl03(char arg_1, char arg_2)
{
	vara_1 = arg_1; vara_0 = arg_2; //INT_B1, INT_B0
	char r_val = arg_1;

	if(arg_1 == 0) return 0;

	var_2 = read16(base_addr + 0x20);
	write16zx(base_addr + 0x20, var_2);
	return r_val;
}

char
CFlash::RescueRWFail()
{
	write16zx(base_addr + 4, 0x0e00);

	long t1, t2; 

	KeQuerySystemTime(&t1);
	do
	{
		KeQuerySystemTime(&t2);
		if(!(0x80 & read16(base_addr + 0x8))) break;  // check this
	} while(t2 - t1 < 10000000); // some "performance counter" - how much time to wait?
	// socket shall be down by now

	ClkFreq = 20000000;
	KeQuerySystemTime(&t1);
	do { KeQuerySystemTime(&t2); } while (t2 - t1 < 3000000);

	char uiVoltage = (char)read16(base_addr + 0x8) & 0x7;
	if(!is_xx12)
	{
		muiMediaID = (read16(base_addr + 0x8) >> 0x4) & 0x7;
		if(muiMediaID == 1) KeStallExecutionProcessor(50000); // 50 msec

		write16zx(base_addr + 0x4, read16(base_addr + 0x4) | 0x40);
		KeStallExecutionProcessor(10000); // 10msec
		write16zx(base_addr + 0x4, (short)uiVoltage | 0x0c40);	
	}
	else write16zx(base_addr +0x4, (short)uiVoltage | 0x0c00);

	if(!(0x80 & read16(base_addr + 0x8)))
	{
		KeQuerySystemTime(&t1);
		
		while(!(0x80 & read16(base_addr + 0x8)))
		{
			if(t2 - t1 > 10000000) break;
			KeQuerySystemTime(&t2);
			if(0x80 & read16(base_addr + 0x8)) break;
		}				
	}
	// socket should be powered up again
	if(!is_xx12) write16zx(base_addr + 0x4, read16(base_addr + 0x4) & 0xffbf);
	
	write16zx(base_addr + 0x28, 0x7);
	write16zx(base_addr + 0x24, 0x1);
	write16zx(base_addr + 0x18, 0xffff);
	write16zx(base_addr + 0x14, 0x5);
	vara_5 = 0;
	return 0;
}

char
CFlash::GetMediaID()
{
	return muiMediaID;
}

void 
CFlash::sub_0_1FE70()
{
	if(!vara_4) KeSetEvent(&event_1, 0, 0);
	vara_4 = 1;	
}

char 
CFlash::Read(int uiLBAStart, short *uiReadSectorCountStart, int uiDMAPhysicalAddress, char *uiDMAPageCount)
{
	if(*uiDMAPageCount > 0x3f) return 0xc0;
	char lvar_r14 = 0; char r_val; short l_sectorcount;
	short lvar_xc0 = *uiReadSectorCountStart;
	char lvar_xb8 = *uiDMAPageCount;
	short lvar_xc8;

	do
	{
		if(uiLBAStart == -1) lvar_r14 = 1;
		else var_10 = *uiReadSectorCountStart;

		r_val = CloseWrite(); // virtual call
		vara_6 = 1;
		if(r_val)
		{
			// read(base_addr + 0x8) == SOCKET_PRESENT_STATE
			write16zx(base_addr + 0x18, 0xffff);
			write16zx(base_addr + 0x10, 0x2);
			return r_val;
		}
		if(!var_10 || !*uiDMAPageCount)
		{
			vara_6 = 0;
			return 0;
		}
		if(uiLBAStart != -1) // read first dma, clear fifo, enable interrupt
		{
			write16zx(base_addr + 0x18, 0xffff);
			write16zx(base_addr + 0x24, 0x1);
			write16zx(base_addr + 0x14, 0x5);
			vara_5 = 0;
		}

		write32(base_addr + 0xc, uiDMAPhysicalAddress);
		write16zx(base_addr + 0x10, 0x1 | (*uiDMAPageCount << 8));
		lvar_xc8 = var_10;

		while(1)
		{
			l_sectorcount = var_10;
			if(!var_10 || !*uiDMAPageCount)
			{
				if(!KeSynchronizeExecution(card_int, sub_0_1C050, &var_2))
				{
					PRKEVENT lvar_x48[2] = {event_1, event_2};
					long lvar_xd0 = -1000000;

					do
					{
						KeWaitForMultipleObjects(2, &lvar_48, 1, 0, 0, 0, &lvar_xd0, 0);
						KeClearEvent(event_1);
						if(vara_4) { vara_6 = 0; return 0x86; }
				
					} while(!KeSynchronizeExecution(card_int, sub_0_1C050, &var_2))
				}
	
				KeSynchronizeExecution(card_int, sub_0_27DE0, &var_2);
				vara_6 = 0;
				return 0;
			}
			r_val = ReadSectors(uiLBAStart, &var_10, uiDMAPageCount, lvar_r14);
			if(!r_val && !vara_5) r_val = 0x2c;
			if(r_val == 0x68) break;
			else if(r_val)
			{
				// read error, abort dma
				write16zx(base_addr + 0x18, 0xffff);
				write16zx(base_addr + 0x10, 0x2);
				vara_6 = 0;
				return r_val;
			}
			if(uiLBAStart != -1)
			{
				uiLBAStart += l_sectorcount - var_10;
			}
			lvar_14 = 1;
		}
		// SM correctable error retry
		*uiReadSectorCountStart = lvar_xc0;
		*uiDMAPageCount = lvar_xb8;
		muiSMReadSector = muiReadSectorCountStart + var_5 - lvar_xc8;
		var_10 = lvar_xc8;
		write16zx(base_addr + 0x10, 0x2);
		write16zx(base_addr + 0x18, 0xffff);
		write16zx(base_addr + 0x24, 0x1);
		write16zx(base_addr + 0x14, 0x5);
		vara_6 = 0; vara_5 = 0;		
	} while(*uiDMAPageCount <= 0x3f);
	return 0x0c;
}


char //edi, r14, r15, r13
CFlash::Write(int uiLBAStart, short *uiWriteSectorCountStart, int uiDMAPhysicalAddress, char *uiDMAPageCount)
{
	char r_val, lvar_r12;

	if(mbWriteProtected) return 0xc1;	
	if(*uiDMAPageCount > 0x3f) return 0xc0;
	vara_6 = 1;

	if(uiLBAStart == -1)
		lvar_r12 = 1;
	else
	{
		var_x84 = var_x82 = *uiWriteSectorCountStart;
		var_x90 = uiLBAStart;
	}

	if(var_x82 < *uiDMAPageCount) *uiDMAPageCount = var_x82;
	if(var_x82 == 0 || *uiDMAPageCount == 0) { vara_6 = 0; return 0; }

	if(uiLBAStart != -1)
	{
		if(var_7 + 1 != uiLBAStart || 1 == GetMediaID() || 4 == GetMediaID())
		{
			r_val = CloseWrite();
			vara_6 = 1;

			if(!r_val)
			{
				//CloseWrite error, aborting DMA
				write16zx(base_addr + 0x18, 0xffff);
				write16zx(base_addr + 0x10, 0x2);
				vara_6 = 0;
				return r_val;
			}
		}

		var_7 = uiLBAStart + *uiWriteSectorCountStart - 1;
		
		write16zx(base_addr + 0x18, 0xffff);
		write16zx(base_addr + 0x24, 0x1);
		write16zx(base_addr + 0x14, 0x5);
		vara_5 = 0;
	}	
	// 0x209A5
	write32(base_addr + 0xc, uiDMAPhysicalAddress);
	write16zx(base_addr + 0x10, 0x8001 | (*uiDMAPageCount << 0x8));
	InitializeWriteBlocks();
	if(var_x82)
	{
		do
		{
			if(!*uiDMAPageCount) break;
			short l_oldvar_x82 = var_x82;
			lvar_r12 = WriteSectors(uiLBAStart, &var_x82, uiDMAPageCount, lvar_r12);
			if(!lvar_r12 && vara_5) lvar_r12 = 0xc2;
			if(lvar_r12)
			{
				write16zx(base_addr + 0x18, 0xffff);
				write16zx(base_addr + 0x10, 0x2);
				if(lvar_r12 == 0x82) lvar_r12 = 0;
				vara_6 = 0;
				return lvar_r12;
			}
			if(uiLBAStart != -1) uiLBAStart += l_oldvar_x82 - var_x82;
			lvar_r12 = 1;
		} while(var_x82);
	}
	// 0x20A3D
	if(!KeSynchronizeExecution(card_int, sub_0_1C050, &var_2)
	{
		lvar_r12 = 0;
		do
		{
			PKEVENT lvar_x40[] = {&event_1, &event_2};
			long lvar_xc0 = -1000000;
			KeWaitForMultipleObjects(2, &lvar_x40, 1, 0, 0, 0, &lvar_xc0, 0);
			KeClearEvent(&event_2);
			if(vara_4)
			{
				vara_6 = 0;
				return 0x86;
			}						
		} while(!KeSynchronizeExecution(card_int, sub_0_1C050, &var_2));
	}
	KeSynchronizeExecution(card_int, sub_0_27DE0, &var_2));
	vara_6 = 0;
	return 0;
}

void 
CFlash::InitializeWriteBlocks()
{
	var_xa4 = 0;
	sub_0_2ECE0(&var_x94, 0, 2 * ((mwSize < 0x10) ? 8 : 4)); // or another way round?
	// sub_0_2ECE0 - library function, used by <operator new> and such
}

struct tigd* 
CFlash::GetGeometry(struct tigd *arg_1)
{
	arg_1->off_x0 = mwCylinders;
	arg_1->off_x4 = mwHeadCount;
	arg_1->off_x8 = mwSectorsPerTrack;
	arg_1->off_xc = 0x200; // block size
	return arg_1;
}

void 
CFlash::GetSMDmaParams(int arg_1, int arg_2, int arg_3)
{
	var_xa8 = arg_1;
	var_xb0 = arg_2;
	var_xb4 = arg_3;
}

void
CFlash::Power(char arg_1)
{
	short r_val = read16(base_addr + 0x4);
	
	if(arg_1) r_val |= 0x40; // power on
	else r_val &= 0xffbf; // power off
	
	write16zx(base_addr + 0x4, r_val);
}

char
CFlash::sub_0_1FEE0(PRKEVENT c_event, int time_out)
{
	PRKEVENT event_array[] = {&event_1, c_event};
	NTSTATUS r_val = KeWaitForMultipleObjects(2, event_array, 1, 0, 0, 0, arg_2, ??);
	KeClearEvent(c_event);
	return (r_val == WAIT_TIMEOUT) ? 0x87 : 0; // wait timeout
}

CSocket::CSocket(char *_base_addr)
	:base_addr(_base_addr), ClkFreq(20000000), is_xx12(0)
{
	if(!(0x8 & read16(base_addr + 0x8)))
		muiMediaID = 0; // card not inserted
	else // card inserted
	{
		write16zx(base_addr + 0x28, 0x7);
		write16zx(base_addr + 0x24, 0x1);
		short t_id = read16(base_addr + 0x8);
		muiMediaID = (t_id >> 4) & 0x7;
	}
}

void
CSocket::SocketPowerCtrl()
{
	write16zx(base_addr + 0x4, 0x0e00); // reset socket
	
	long t1, t2; 

	KeQuerySystemTime(&t1);
	do
	{
		KeQuerySystemTime(&t2);
		if(!(0x80 & read16(base_addr + 0x8))) break;  // check this
	} while(t2 - t1 < 10000000); // win32 version uses KeQuerySystemTime with 0.1us units
	// socket shall be down by now
	
	ClkFreq = 20000000;
	if(!(0x8 & (char)read16(base_addr + 0x8)) 
	{
		muiMediaID = 0;
		return;
	}

	char uiVoltage = (char)read16(base_addr + 0x8) & 0x7;
	if(!is_xx12)
	{
		muiMediaID = ((char)read16(base_addr + 0x8) >> 0x4) & 0x7;
		if(muiMediaID == 1) KeStallExecutionProcessor(40000); // SmartMedia - 40 msec
		
		write16zx(base_addr + 0x4, read16(base_addr + 0x4) | 0x40);
		KeStallExecutionProcessor(10000); // 10msec
		write16zx(base_addr + 0x4, (short)uiVoltage | 0x0c40);		
	}
	else // 21231
		write16zx(base_addr +0x4, (short)uiVoltage | 0x0c00);

	// socket is re-enabled
	KeQuerySystemTime(&t1);
	do
	{
		KeQuerySystemTime(&t2);
		if(0x80 & read16(base_addr + 0x8)) break; 
	} while(t2 - t1 < 10000000);
	// socket shall be powered by now
	
	if(!is_xx12)
		write16zx(base_addr + 0x4, read16(base_addr + 0x4) & 0xffbf);
	
	write16zx(base_addr + 0x28, 7);
	write16zx(base_addr + 0x24, 1);
	muiMediaID = (read16(base_addr + 0x8) >> 0x4) & 0x7;	
}

void 
CFlash::sub_0_1FD80(long interval)
{
	long lvar_x40 = interval;
	KeDelayExecutionThread(0, 0, &lvar_x40);
}
