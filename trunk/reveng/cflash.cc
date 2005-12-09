#include "cflash.h"

CFlash::CFlash(char *_base_addr)
       :base_addr(_base_addr), var_1(0x01312d00), is_xx12(0)
{
	if(0x8 & read16(base_addr + 0x8))
	{ // card inserted
		write16zx(base_addr + 0x28, 0x7);
		write16zx(base_addr + 0x24, 0x1);
		muiMediaID = (read16(base_addr + 0x8) >> 0x4) & 0x7;
	
	}
	else // card not inserted
		muiMediaID = 0;

	var_2 = 0;
	arr_1 = {0, 0, 0, 1, 0, 0, 0, 0};
	var_3 = 0; var_4 = -1; var_5 = 0;
	KeInitializeEvent(event_1, 0, 0);
	KeInitializeEvent(event_2, 0, 0);
	Size = 0; var_7 = 0;		
}

CFlash::~CFlash()
{
	if(arr_1[3] == 0) KeSetEvent(event_1, 0, 0);
	arr_1[3] = 1;	
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
	if(arr_1[1] == 0) return 0;
	if(var_2 & 0x4)
	{
		r_val = 0x200;
		arr_1[6] = 1;
	}
	else KeSetEvent(event_2, 0, 0); // wake up threads waiting here

	if(var_2 & 0x1) r_val |= 0x0100;
	arr_1[2] = 0;
	return r_val;
} 

char
CFlash::vtbl03(char arg_1, char arg_2)
{
	arr_1[1] = arg_1; arr_1[0] = arg_2;
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

	long t1, t2; t1 = sys_var_0014[0];

	do
	{
		t2 = sys_var_0014[0];
		if(!(0x80 & read16(base_addr + 0x8))) break;  // check this
	} while(t2 - t1 < 10000000); // some "performance counter" - how much time to wait?
	// socket shall be down by now

	var_1 = 0x01312d00;
	t1 = sys_var_0014[0];
	do { t2 = sys_var_0014[0]; } while (t2 - t1 < 3000000);

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
	// Here we see an exact verbatim disassembly of original code.
	// It's not my fault!
	if(!(0x80 & read16(base_addr + 0x8)))
	{
		t1 = sys_var_0014[0];
		
		while(!(0x80 & read16(base_addr + 0x8)))
		{
			if(t2 - t1 > 10000000) break;
			t2 = sys_var_0014[0];
			if(0x80 & read16(base_addr + 0x8)) break;
		}				
	}
	// socket should be powered up again
	if(!is_xx12) write16zx(base_addr + 0x4, read16(base_addr + 0x4) & 0xffbf);
	
	write16zx(base_addr + 0x28, 0x7);
	write16zx(base_addr + 0x24, 0x1);
	write16zx(base_addr + 0x18, 0xffff);
	write16zx(base_addr + 0x14, 0x5);
	arr_1[5] = 0;
	return 0;
}

char
CFlash::GetMediaID()
{
	return muiMediaID;
}

CSocket::CSocket(char *_base_addr)
	:base_addr(_base_addr), var_1(0x01312d00), is_xx12(0)
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
	
	long t1, t2; t1 = sys_var_0014[0];

	do
	{
		t2 = sys_var_0014[0];
		if(!(0x80 & read16(base_addr + 0x8))) break;  // check this
	} while(t2 - t1 < 10000000); // some "performance counter" - how much time to wait?
	// socket shall be down by now
	
	var_1 = 0x01312d00;
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
	do
	{
		t2 = sys_var_0014[0];
		if(0x80 & read16(base_addr + 0x8)) break; 
	} while(t2 - t1 < 10000000);
	// socket shall be powered by now
	
	if(!is_xx12)
		write16zx(base_addr + 0x4, read16(base_addr + 0x4) & 0xffbf);
	
	write16zx(base_addr + 0x28, 7);
	write16zx(base_addr + 0x24, 1);
	muiMediaID = (read16(base_addr + 0x8) >> 0x4) & 0x7;	
}

CMMCSD::CMMCSD(char *_base_addr)
       :CFlash(_base_addr), wBlockLen(0x200), cmmcsd_var_2(0), 
	cmmcsd_var_1(0), cmmcsd_var_4(0), muiMediaID(3), cmmcsd_var_5(0),
	cmmcsd_var_6(0), cmmcsd_var_7(0), cmmcsd_var_8(0), cmmcsd_var_9(0)
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

	if(arr_1[0] != 0)
	{
		if(cmmcsd_var_10 & 0x8)
		{ 
			write32(base_addr + 0x118, 0x14 | read32(base_addr + 0x118));
			cmmcsd_var_2 = 1;
		}

		if(cmmcsd_var_10 & 0x10) cmmcsd_var_2 = 0;

		if(cmmcsd_var_10 & 0x4) cmmcsd_var_2 = 1;

		cmmcsd_var_5 = cmmcsd_var_11;
		KeSetEvent(cmmcsd_event_1, 0, 0);
		arr_1[0] = 0;
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
		cmmcsd_var_11 = cmmcsd_var_5 | cmmcsd_var_10;
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

char
CMMCSD::InitializeCard()
{
	write32(base_addr + 0x168, 0x2);
	cmmcsd_var_12 = 0x3c;
	write32(base_addr + 0x110, 0xb);
	long t1, t2; t1 = sys_var_0014[0];
	char r_val = 0x2f;
	
	do
	{
		t2 = sys_var_0014[0];
		if(0x1 & read32(base_addr + 0x16c))
		{
			r_val = 0;
			break; // reset ok
		}
		
	} while(t2 - t1 < 2500000);
	if(r_val) return r_val; // time-out on reset

	write32(base_addr + 0x12c, 0);
	write32(base_addr + 0x110, cmmcsd_var_12 | 0x800);
	write32(base_addr + 0x130, 0x8000);
	write32(base_addr + 0x118, 0x41e9);
	write32(base_addr + 0x138, 0x20 | read32(base_addr + 0x138));
	write32(base_addr + 0x11c, 0x40);
	write32(base_addr + 0x120, 0x7ff);
	// card initialization sequence
	write32(base_addr + 0x104, 0x80);
	write32(base_addr + 0x110, cmmcsd_var_12 | 0x800);
	sub_0_1FEE0(cmmcsd_event_1, -1000000); // arg_2: 0xfff0bdc0

	if(arr_1[4] != 0) return 0x86;

	if(0 != (r_val = DetectCardType())) return r_val;

	if(muiMediaID == 0x43) // SDIO card
	{
		write16zx(base_addr + 0x4, 0x80);
		return 0x2e;
	}
	else if(muiMediaID == 0x23 && (need_sw & 0x1)) // SD card
	{
		write16zx(base_addr + 0x4, 0x80);
		muiMediaID = 0x43;
		return 0x2e;
	}
	else if(muiMediaID == 0x13 && (need_sw & 0x2))
	{
		write16zx(base_addr + 0x4, 0x80);
		muiMediaID = 0x43;
		return 0x2e;
	}
	
	if(0 != (r_val = Standby())) return r_val;
	if(0 != (r_val = ReadCSDInformation())) return r_val;

	write32(base_addr + 0x110, cmmcsd_var_12 | ( 0xffc0 & read32(base_addr + 0x110)));
	
	if(0 != (r_val = GetCHS())) return r_val;
	
	ReportMediaModel(); // print some debug info

	if(0 != (r_val = sub_0_1CE40(0x7, dwRCA, 0x2900))) return r_val;
	if(0 != (r_val = sub_0_1CE40(0x10, wBlockLen, 0x2100))) return r_val;

	write32(base_addr + 0x128, wBlockLen - 1);
	if(muiMediaID == 0x23)
	{
		write16zx(base_addr + 0x4, 0x100 | read16(base_addr + 0x4));
		var_1 = 24000000; // 0x16e3600
		if(0 != (r_val = sub_0_1CE40(0x37, dwRCA, 0x2100))) return r_val;
		if(0 != (r_val = sub_0_1CE40(0x2a, 0, 0x2100))) return r_val;

		char lvar_x30 = 0;
		char lvar_x38 = 0xd;
		int lvar_x3C = 0;
		int lvar_x40 = 0x40;
		char lvar_x44 = 1, lvar_x45 = 1, lvar_x46 = 1, lvar_x47 = 0, lvar_x48 = 0;

		if(0 != (r_val = Execute(&lvar_x38, 0, &lvar_x30, &lvar_x60))) return r_val;
		if(0xff00 & (((short)lvar_x63 << 0x8) + lvar_x64)) return 0x27;
		
		lvar_x30 = 0;
		lvar_x38 = 0x33;
		lvar_x3C = 0;
		lvar_x40 = 0x8;
		lvar_x44 = 1, lvar_x45 = 1, lvar_x46 = 1, lvar_47 = 0, lvar_48 = 0;
		if(0 != (r_val = Execute(&lvar_x38, 0, &lvar_x30, &lvar_x50))) return r_val;

		if(0x4 & (0xf & lvar_x51))
		{
			if(0 != (r_val = sub_0_1CE40(0x37, dwRCA, 0x2100))) return r_val;
			if(0 != (r_val = sub_0_1CE40(0x6, 0x2, 0x2100))) return r_val;
			write32(base_addr + 0x110, 0x8800 | cmmcsd_var_12);
		}
		WriteProtected |= (0x200 & read16(base_addr + 0x8)) ? 0x1 : 0; 		
	} //1F4FC
	
	for(short cnt = 0, cnt < 10000, cnt++)
	{
		if(0 != (r_val = GetState(&lvar_x31, 0))) break;
		if(lvar_x31 != 0x4) continue;
		if(cmmcsd_var_14 == 1) break;		
	}

	vara_2 = 1;
	return r_val;
}

char
CMMCSD::WriteProtectedWorkaround()
{
	return 0;
}
