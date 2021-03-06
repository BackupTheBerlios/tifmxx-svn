#include "cmemorystick.h"

char
CFlash::sub1FC40(short arg_1)
{
	int rv;
	rv = read32(base_addr + 0x200 + (arg_1 & 0xfffc));
	return ((char*)&rv)[arg_1 & 3];
}

bool
ClearStatus(int *status_var)
{
	*status_var = 0;
	return 1;
}


bool
CMemoryStick::HandleError(int *status_var)
{
	if(*status_var & 0x0100) return false; // TOE
	if(*status_var & 0x0200) return false; // CRC
	return 1;
}

bool
CMemoryStick::HandleMSINT(int *status_var)
{
	return (*status_var & 0x2000);
}

bool
CMemoryStick::HandleRDY(int *status_var)
{
	return (*status_var & 0x1000);
}

char
CMemoryStick::WaitForRDY()
{
	char rc;

	for(int cnt = 3; cnt; cnt--) {
		rc = sub_1FEE0(&ms_event_x0c8, -5000000);
		if(vara_4) return 0x86;
		if(!KeSynchronizeExecution(&CMS::HandleError, &dwMS_STATUS, card_int)) return 0x47; // error - TOE/CRC
		if(KeSynchronizeExecution(&CMS::HandleRDY, &dwMS_STATUS, card_int)) return 0;
	}
	if(!(0x1000 & read32(base_addr + 0x18c))) return 0x87; // timeout
	return 0;
}

char
CMemoryStick::Isr(char arg_1, char arg_2)
{
	char rc = CFlash::Isr(arg_1, arg_2);
	if (arg_2) {
		dwMS_STATUS |= read32(base_addr + 0x18c);
		write32(base_addr + 0x190, 0x0800 | read32(base_addr + 0x190));
	}
	return rc | arg_2;
}

int
CMemoryStick::IsrDpc()
{
	int rc = CFlash::IsrDpc();
	if(vara_0) {
		KeSetEvent(ms_event_x0c8);
		vara_0 = 0;
	}
	return rc;
}

char
CMemoryStick::sub21DA0() // read registers
{
	char lvar_x38[32];

	write32(base_addr + 0x190, var_x104 | 0x2607);
	int t_val = read32(base_addr + 0x190);
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(&CMemoryStick::ClearStatus, &dwMS_STATUS, card_int);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0x401f);
	char rc = WaitForRDY();
	write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
	if(rc) return rc;

	for(int cnt = 0; cnt < 8; cnt++)
	{
		((int*)lvar_x38)[cnt] = read32(base_addr + 0x188);
	}
	memcpy((char*)&ms_regs, lvar_x38, 32);
	return 0;
}

char
CMemoryStick::sub22370(int arg_1) // set rw reg address
{
	write32(base_addr + 0x190, var_x104 | 0x2707);
	write32(base_addr + 0x190, read32(base_addr + 0x190) | 0x0100);
	write32(base_addr + 0x188, arg_1);
	write32(base_addr + 0x190, read32(base_addr + 0x190) | 0x0100);
	write32(base_addr + 0x188, 0);
	int t_val = read32(base_addr + 0x190);
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(&CMS::ClearStatus, &dwMS_STATUS, card_int);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0x8004);
	char rc = WaitForRDY();
	write32(base_addr + 0x190, 0xfffeffff & read32(base_addr + 0x190));
	return rc;
}

char
CMemoryStick::ReadSectors(int arg_1, short *arg_2, char *arg_3, char arg_4)
{
	char rc;

	if(arg_1 != -1) var_x108 = arg_1;
	if(!(rc = ReadSector(var_x108, *arg_2, arg_4)))
	{
		var_x108++;
		(*arg_2)--;
		(*arg_3)--;
	}
	return rc;
}

char
CMemoryStick::WriteSectors(int arg_1, short *arg_2, char *arg_3, char arg_4)
{
	char rc;

	if(arg_1 != -1) var_x10c = arg_1;
	if(!(rc = WriteSector(var_x10c, *arg_2, arg_4)))
	{
		var_x10c++;
		(*arg_2)--;
		(*arg_3)--;
	}
	return rc;
}

char
CMemoryStick::InitializeCard()
{
	char rc = 0;
	if((rc = sub22370(0x1f001f00))) return rc;
	if((rc = sub21DA0())) return rc;
	muiMediaID = 0x12; var_x029 = 0x12;
	if (!(ms_regs.status.type & 1)) return 0;
	if (ms_regs.status.category) return 0;
	if (ms_regs.status.class > 3) return 0;
	muiMediaID = 0x22; // MSPro device
	return 0;
}

CMemoryStick::CMemoryStick(char *_base_addr, char serial_mode)
	     : CFlash(_base_addr)
{
	sub1FEE50(&ms_event_x0c8); // initialize
	var_x104 = 0x4010;
	var_x0e1 = 0;
	var_x0e2 = 0;
	var_x108 = 0;
	var_x10c = 0;
	mbParallelInterface = serial_mode;
	write32(base_addr + 0x190, 0x8000);
	write32(base_addr + 0x190, 0x0a00);
	write32(base_addr + 0x18c, 0xffffffff);
	dwMS_STATUS = 0;
}

CMemoryStick::~CMemoryStick()
{
	write32(base_addr + 0x190, 0x0a00);
	write32(base_addr + 0x18c, 0xffffffff);
	dwMS_STATUS = 0;
}
