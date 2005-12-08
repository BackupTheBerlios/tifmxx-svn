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
	var_6 = 0; var_7 = 0;		
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

