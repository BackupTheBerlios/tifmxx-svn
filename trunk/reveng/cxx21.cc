#include "cxx21.h"

Cxx21::Cxx21(char *_base_addr)
      :base_addr(_base_addr), irq_status(0), var_2(0), num_sockets(4), d_socket(2)
{
	for(int c = 4; c>0; c--) { mpFlash[c] = 0; sock_valid[c] = 0; }
}

Cxx21::~Cxx21()
{
    	write32(iomem + 0xc, 0xffffffff);
	for(int cnt = 0; cnt < num_sockets; cnt++)
		if(mpFlash[cnt]) mpFlash[cnt]->vtable[0](1);
}

void
Cxx21::GetNumSockets(int _num_sockets) // 104C:803B <- 2, other <- 4
{
	num_sockets = _num_sockets;
	if(num_sockets != 4) d_socket = 0;
}

void
Cxx21::Initialize()
{
	write32(iomem + 0xc, 0xffffffff);
        write32(iomem + 0x8, 0x8000000f);
}

char
Cxx21::sub_0_1A100()
{
	char r_val, lvar_1, lvar_2;
	unsigned int lvar_3 = read32(base_addr + 0x14);
	if(lvar_3 == 0 || lvar_3 == 0xffffffff) return 0;
	irq_status = lvar_3;
	
	if(lvar_3 & 0x80000000) r_val = 1;
	if(!var_2 && r_val)
	{
		write32(base_addr + 0xc, 0x80000000);
		
		if(mpFlash[0])
		{
			lvar_1 = (lvar_3 & 0x00010000) ? 1 : 0;
			lvar_2 = (lvar_3 & 0x00000100) ? 1 : 0;
			if(lvar_1 || lvar_2)
				mpFlash[0]->vtbl03(lvar_1, lvar_2);
		}
		if(mpFlash[1])
		{
			lvar_1 = (lvar_3 & 0x00020000) ? 1 : 0;
			lvar_2 = (lvar_3 & 0x00000200) ? 1 : 0;
			if(lvar_1 || lvar_2)
				mpFlash[1]->vtbl03(lvar_1, lvar_2);
		}
		if(mpFlash[2])
		{
			lvar_1 = (lvar_3 & 0x00040000) ? 1 : 0;
			lvar_2 = (lvar_3 & 0x00000400) ? 1 : 0;
			if(lvar_1 || lvar_2)
				mpFlash[2]->vtbl03(lvar_1, lvar_2);
		}
		if(mpFlash[3])
		{
			lvar_1 = (lvar_3 & 0x00080000) ? 1 : 0;
			lvar_2 = (lvar_3 & 0x00000800) ? 1 : 0;
			if(lvar_1 || lvar_2)
				mpFlash[3]->vtbl03(lvar_1, lvar_2);
		}
	} // 1A206
	
	write32(base_addr + 0x14, irq_status);
	return r_val;
}

char
Cxx21::CardDetection(char socket)
{
	char *socket_ptr = base_addr + (((long)socket + 1) << 10); // == r13
	char m_id;
	CSocket *t_socket;
	char r_val;
	
	if(mpFlash[socket] == 0 || 0x43 != (m_id = mpFlash[socket]->GetMediaID()))
	{ // 1A2Af
		t_socket = new CSocket(socket_ptr); //constructor0, t_socket == rbp
		if(!t_socket) return 0x88;
		if(num_sockets != 4) t_socket->is_xx12 = 1;
		t_socket->SocketPowerCtrl();
		m_id = t_socket->GetMediaID();
		delete t_socket;
	}
	// 1A328
	if(m_id == 1)
	{ // 1A4EC
		mpFlash[socket] = new CSM(socket_ptr);
		if(mpFlash[socket]) sock_valid[socket] = 1;
		else return 0x88;
	} 
	else if(m_id == 2)
	{ // 1A455
		mpFlash[socket] = new CMSBase(socket_ptr, (MSPEnable) ? (socket == d_socket) : 0);
		if(mpFlash[socket]) sock_valid[socket] = 1;
		else return 0x88;
	}
	else if(m_id == 3)
	{ // 1A3F9
		mpFlash[socket] = new CMMCSD(socket_ptr);
		if(mpFlash[socket]) sock_valid[socket] = 1;
		else return 0x88;
	}
	else
	{ // 1A344
		r_val = 0x83;
		if(mpFlash[socket] != 0) mpFlash[socket]->sub_0_1FE70();
		if(socket == 0)
		{
			write32(base_addr + 0xc, 0x00010100);
			sock_valid[socket] = 0;
		}
		else if(socket == 1)
		{
			write32(base_addr + 0xc, 0x00020200);
			sock_valid[socket] = 0;
		}
		else if(socket == 2)
		{
			write32(base_addr + 0xc, 0x00040400);
			sock_valid[socket] = 0;
		}
		else if(socket == 3)
		{
			write32(base_addr + 0xc, 0x00080800);
			sock_valid[socket] = 0;
		}
		else
			sock_valid[socket] = 0; // Am i missing something?
	}
	// 1A543
	if(mpFlash[socket])
	{
		mpFlash[socket]->card_int = int_obj;
		if(num_sockets != 4) mpFlash[socket]->is_xx12 = 1;
	}

	if(r_val)
	{
		if(socket == 0)
			write32(base_addr + 0x8, 0x00010100);
		else if(socket == 1)
			write32(base_addr + 0x8, 0x00020200);
		else if(socket == 2)
			write32(base_addr + 0x8, 0x00040400);
		else if(socket == 3)
			write32(base_addr + 0x8, 0x00080800);
	}
	return r_val;
}

char 
Cxx21::sub_0_1A610(char socket)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1; // socket empty
	return 0;
}

char 
Cxx21::InitializeCard(char socket)
{
	char r_val;

	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	if(mpFlash[socket]->vara_2) return 0;
	mpFlash[socket]->SDSwitch = SDSwitch;
	mpFlash[socket]->SMEnable = SMEnable;
	mpFlash[socket]->SMCISEnable = SMCISEnable;
	mpFlash[socket]->InsDelEnable = InsDelEnable;
	
	if(0 != (r_val = mpFlash[socket]->Initialize())) return r_val;
	char t_id = mpFlash[socket]->GetMediaID();
	CFlash *t_fld;

	if(t_id == 0x12)
	{
		if(mpFlash[socket]) delete mpFlash[socket];
		// second argument: 0 == serial mode 2
		t_fld = new CMS(base_addr + ((socket + 1) << 0xa), MSPEnable ? (socket == d_socket) : 0);
	}
	else if(t_id == 0x22)
	{
		if(mpFlash[socket]) delete mpFlash[socket];
		// second argument: 0 == serial mode 3
		t_fld = new CMSPro(base_addr + ((socket + 1) << 0xa), socket == d_socket);
	}
	else return 0;
	//1A8A3
	mpFlash[socket] = t_fld;
	t_fld->card_int = int_obj;
	return mpFlash[socket]->Initialize();
}

char
Cxx21::GetMediaID(char socket)
{
	if(socket >= num_sockets) return 0xa0;

	if(sock_valid[socket] == 0) return 0xa1;
	
	return mpFlash[socket]->GetMediaID();
}

struct tigd* 
Cxx21::GetGeometry(struct tigd *arg_1, char socket)
{
	*arg_1 = {0, 0, 0, 0};
	if(socket < num_sockets && sock_valid[socket])
	{
		struct tigd lvar_x20;
		mpFlash[socket]->GetGeometry(&lvar_x20);
		*arg_1 = lvar_x20;
	}
	return arg_1;
}

char 
Cxx21::Read(char uiSocket, int uiLBA, short ReadSectorCount, int DMAPhysicalAddress, char *pDMAPageCount) // read5
{
	if(uiSocket >= num_sockets) return 0xa0;
	if(!sock_valid[uiSocket]) return 0xa1;     // socket empty
	if(uiLBA == -1) return 0x82;
	if(mpFlash[uiSocket]->vara_6) return 0xc3; // socket busy
	
	mpFlash[uiSocket]->vara_6 = 1;
	
	return mpFlash[uiSocket]->Read(uiLBA, &ReadSectorCount, DMAPhysicalAddress, pDMAPageCount);
}

char 
Cxx21::Read(char uiSocket, int DMAPhysicalAddress, char *pDMAPageCount) // read3
{
	if(uiSocket >= num_sockets) return 0xa0;
	if(!sock_valid[uiSocket]) return 0xa1;     // socket empty
	if(mpFlash[uiSocket]->vara_6) return 0xc3; // socket busy

	mpFlash[uiSocket]->vara_6 = 1;
	short ReadSectorCount = 0;
	return mpFlash[uiSocket]->Read(-1, &ReadSectorCount, DMAPhysicalAddress, pDMAPageCount);
}

char 
Cxx21::Write(char uiSocket, int uiLBA, short WriteSectorCount, int DMAPhysicalAddress, char *pDMAPageCount) // write5
{
	if(uiSocket >= num_sockets) return 0xa0;
	if(!sock_valid[uiSocket]) return 0xa1;     // socket empty
	if(uiLBA == -1) return 0x82;
	if(mpFlash[uiSocket]->vara_6) return 0xc3; // socket busy
	
	mpFlash[uiSocket]->vara_6 = 1;
	
	return mpFlash[uiSocket]->Write(uiLBA, &WriteSectorCount, DMAPhysicalAddress, pDMAPageCount);
}

char
Cxx21::Write(char uiSocket, int DMAPhysicalAddress, char *pDMAPageCount) // write 3
{
	if(uiSocket >= num_sockets) return 0xa0;
	if(!sock_valid[uiSocket]) return 0xa1;     // socket empty
	if(mpFlash[uiSocket]->vara_6) return 0xc3; // socket busy

	mpFlash[uiSocket]->vara_6 = 1;
	short WriteSectorCount = 0;
	return mpFlash[uiSocket]->Write(-1, &WriteSectorCount, DMAPhysicalAddress, pDMAPageCount);
}

char 
Cxx21::GetSMDmaParams(char socket, int arg_2, int arg_3, int arg_4)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;     // socket empty
	mpFlash[socket]->GetSMDmaParams(arg2, arg_3, arg_4);
	return 0;
}

char 
Cxx21::sub_0_1AFE0(char socket, CMMCSD::ExecParam *pParam, int uiDMAPhysicalAddress, char *uiDMAPageCount)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	if(mpFlash[socket]->GetMediaID() != 0x23) return 0xa2;
	return (CMMCSD*)mpFlash[socket]->Execute(pParam, uiDMAPhysicalAddress, uiDMAPageCount, 0);
}

char 
Cxx21::sub_0_1B0A0(char socket, int uiDMAPhysicalAddress, char *uiDMAPageCount)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	if(mpFlash[socket]->GetMediaID() != 0x23) return 0xa2;
	return (CMMCSD*)mpFlash[socket]->Execute(0, uiDMAPhysicalAddress, uiDMAPageCount, 0);
}

char 
Cxx21::sub_0_1B150(char socket, CMMCSD::ExecParam *pParam, char *pData)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	if(mpFlash[socket]->GetMediaID() != 0x23) return 0xa2;
	char uiDMAPageCount = 0;
	return (CMMCSD*)mpFlash[socket]->Execute(pParam, 0, &uiDMAPageCount, pData);
}

char
Cxx21::sub_0_1B210(char socket, char *arg_2)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	if(mpFlash[socket]->GetMediaID() != 0x23) return 0xa2;
	(CMMCSD*)mpFlash[socket]->sub_0_1EEA0(arg_2);
	return 0;
}

char*
Cxx21::sub_0_1B2E0(char socket)
{
	if(socket >= num_sockets) return "Invalid Socket";
	if(!sock_valid[socket]) return "Invalid Socket";
	if(mpFlash[socket]->GetMediaID() != 0x23) return "Not an SD/MMC card";
	return (CMMCSD*)mpFlash[socket]->mpSerialNumber;
}

char 
Cxx21::sub_0_1B3A0(char socket, int *arg_2)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	*arg_2 = mpFlash[socket]->SerialNumber;
	return 0;
}

char 
Cxx21::WriteProtected(char socket)
{
	if(socket >= num_sockets) return 1;
	if(!sock_valid[socket]) return 1;
	return mpFlash[socket]->mbWriteProtected;
}

char
Cxx21::CloseWrite(char socket)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	if(mpFlash[socket]->vara_6) return 0xc3;
	return mpFlash[socket]->CloseWrite();
}

char 
Cxx21::sub_0_1B570(char socket, char arg_2)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	mpFlash[socket]->Power(arg_2);
	return 0;
}

char 
Cxx21::sub_0_1B5E0(char socket)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	if(mpFlash[socket]->GetMediaID() != 0x22) return 0xa2;
	return (CMSPro*)mpFlash[socket]->FormatX();
}

char 
Cxx21::CISFixCheck(char socket, char arg_2)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	char t_id = mpFlash[socket]->GetMediaID();
	if(t_id != 0x1 && t_id != 0x4) return 0xa2;
	return (CSM*)mpFlash[socket]->CISFixOrCheck(arg_2);
}

char
Cxx21::RescueAction(char socket)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	return mpFlash[socket]->RescueRWFail();
}

char
Cxx21::GetIntObj(PKINTERRUPT _int_obj)
{
	int_obj = _int_obj; return 0;
}

char
Cxx21::GetSDSwitch(int reg_SDSwitch)
{
	SDSwitch = reg_SDSwitch; return 0;
}

char
Cxx21::GetSMEnable(int reg_SMEnable)
{
	SMEnable = reg_SMEnable; return 0;
}

char
Cxx21::GetSMCISEnable(int reg_SMCISEnable)
{
	SMCISEnable = reg_SMCISEnable; return 0;
}

char
Cxx21::GetInsDelEnable(int reg_InsDelEnable)
{
	InsDelEnable = reg_InsDelEnable; return 0;
}

char
Cxx21::GetMSPEnable(int reg_MSPEnable)
{
	MSPEnable = reg_MSPEnable; return 0;
}

char 
Cxx21::SocketValid(char socket)
{
	if(socket >= num_sockets) return 0;
	if(!sock_valid[socket]) return 0;
	return 1;
}

char 
Cxx21::sub_0_1B930(char socket)
{
	return (mpFlash[socket] == 0);
}

char 
Cxx21::KillEvent(char socket)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	mpFlash[socket]->sub_0_1FE70();
	return 0;
}

char 
Cxx21::sub_0_1BA00()
{
	if(var_2 != 0) return 0;

	int t_val, r_val = 0;
	
	if(mpFlash[0] != 0)
		if(0x100 & mpFlash[0]->vtbl02()) r_val = 0x100;
		
	if(mpFlash[1] != 0)
		if(0x100 & mpFlash[1]->vtbl02()) r_val |= 0x200;
	
	if(mpFlash[2] != 0)
		if(0x100 & mpFlash[2]->vtbl02()) r_val |= 0x400;
	
	if(mpFlash[3] != 0)
		if(0x100 & mpFlash[3]->vtbl02()) r_val |= 0x800;

	if(irq_status & 0x1) 
	{
		CardDetection(0); r_val |= 0x1;
	}

	if(irq_status & 0x2) 
	{
		CardDetection(1); r_val |= 0x2;
	}

	if(irq_status & 0x4) 
	{
		CardDetection(2); r_val |= 0x4;
	}

	if(irq_status & 0x8) 
	{
		CardDetection(3); r_val |= 0x8;
	}

	write32(base_addr + 0x8, 0x80000000);
	return r_val;
}

char 
Cxx21::RemoveCard(char socket)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	// turn socket power off
	write16zx(mpFlash[socket]->base_addr + 0x4, 0xfff8 & read16(mpFlash[socket]->base_addr + 0x4));
	if(mpFlash[socket]) delete mpFlash[socket];
	mpFlash[socket] = 0;
	sock_valid[socket] = 0;
	return 0;
}

char 
Cxx21::SocketPower(char socket, char arg_2)
{
	if(socket >= num_sockets) return 0xa0;
	if(!sock_valid[socket]) return 0xa1;
	if(mpFlash[socket]->vara_6) return 0xc3; // socket busy
	if(arg_2) return 0;

	mpFlash[socket]->Power(1);
	mpFlash[socket]->sub_0_1FD80(-50000);
	write16zx(mpFlash[socket]->base_addr + 0x4, read16(mpFlash[socket]->base_addr + 0x4) & 0xfff8);
	mpFlash[socket]->sub_0_1FD80(-450000);
	// I don't know what this does here.
	if(socket >= num_sockets) return 0;
	if(!sock_valid[socket]) return 0;
	mpFlash[socket]->Power(0);
	return 0;
}

char 
Cxx21::Suspend()
{
	write32(base_addr + 0xc, 0x80000000);
	char socket = 0;
	if(num_sockets > 0)
	{
		do
		{
			RemoveCard(socket);
			socket ++;
		} while(socket < num_sockets)
	}
	return 0;
}

char
Cxx21::Resume()
{
	var_2 = 0;
	write32(base_addr + 0x14, 0xffffffff);
	write32(base_addr + 0xc, 0xffffffff);
	write32(base_addr + 0x8, 0x80000000);
	char socket = 0, r_val;
	if(num_sockets > 0)
	{
		do
		{
			CardDetection(socket);
			r_val = InitializeCard(socket);
			if(r_val && r_val != 0xa1) RemoveCard(socket);
			socket ++;
		} while(socket < num_sockets)
	}
	write32(base_addr + 0x8, 0xf | read32(base_addr + 0x8));
	return 0;
}
