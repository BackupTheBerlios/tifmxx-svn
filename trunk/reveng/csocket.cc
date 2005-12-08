#include "csocket.h"

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
		if(muiMediaID == 1)
		{
			//! wait for 40msec - SmartMedia
		}
		
		write16zx(base_addr + 0x4, read16(base_addr + 0x4) | 0x40);
		//! wait for 10msec
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

