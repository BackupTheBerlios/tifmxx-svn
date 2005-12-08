#include "cxx21.h"

Cxx21::Cxx21(char *_base_addr)
      :base_addr(_base_addr), var_1(0), var_2(0), num_sockets(4), d_socket(2)
{
	for(int c = 4; c>0; c--) { mpFlash[c] = 0; sock_valid[c] = 0; }
}

Cxx21::~Cxx21()
{
    	write32(iomem + 0xc, 0xffffffff);
	for(int cnt = 0; cnt < num_sockets; cnt++)
		if(mpFlash[cnt]) mpFlash[cnt]->vtable[0](1);
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
Cxx21::GetMSPEnable(int reg_MSPEnable)
{
	MSPEnable = reg_MSPEnable; return 0;
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
Cxx21::CardDetection(char socket)
{
	char *socket_ptr = base_addr + (((long)socket + 1) << 10); // == r13
	char m_id;
	CSocket *t_socket;
	char retval; // == r15
	
	if(mpFlash[socket] == 0 || 0x43 != (m_id = mpFlash[socket]->GetMediaID()))
	{ // 1A2Af
		t_socket = new CSocket(socket_ptr); //constructor0, t_socket == rbp
		if(!t_socket) return 0x88;
		if(num_sockets != 4) t_socket->is_xx12 = 1;
		t_socket->SocketPowerCtrl();
		m_id = t_socket->GetMediaID();
		t_socket->vtable[0](1);	//destructor
	}
	// 1A328
	switch(m_id)
	{
		case 1:
			// generic SmartMedia
			mpFlash[socket] = new CSM(socket_ptr);
			if(!mpFlash[socket]) return 0x88; //alloc failed
			sock_valid[socket] = 1;
			break;
		case 2:
			// generic MemoryStick
			mpFlash[socket] = (sel_3 == 0) ? new CMS(socket_ptr, 0):
				 new CMS(socket_ptr, (socket == d_socket));
			
			if(!mpFlash[socket]) return 0x88;
			sock_valid[socket] = 1;
			break;
		case 3:
			// generic MMCSD
			mpFlash[socket] = new MMCSD(socket_ptr);
			if(!mpFlash[socket]) return 0x88; 
			sock_valid[socket] = 1;
			break;
		default: // 1A344 - no valid card
			retval = 0x83; 
			if(mpFlash[socket] != 0)
			{
				//! old card was removed
				mpFlash[socket]->sub_0_1FE70(); //KeSetEvent
			}
			//1A37B
			switch(socket)
			{
	    			case 0:
					write32(iomem + 0xc, 0x00010100);
					sock_valid[socket] = 0;
					break;
	    			case 1:
					write32(iomem + 0xc, 0x00020200);
					sock_valid[socket] = 0;
					break;
	    			case 2:
					write32(iomem + 0xc, 0x00040400);
					sock_valid[socket] = 0;
					break;
	    			case 3:
					write32(iomem + 0xc, 0x00080800);
					sock_valid[socket] = 0;
					break;
	    			default:
				sock_valid[socket] = 0;
			}
	}	
//1A543
	if(!retval)
	{ 
		switch(socket)
		{
			case 0:
				write32(iomem + 0x8, 0x00010100);
				break;
			case 1:
				write32(iomem + 0x8, 0x00020200);
				break;
			case 2:
				write32(iomem + 0x8, 0x00040400);
				break;
			case 3:
				write32(iomem + 0x8, 0x00080800);
				break;
		}
	}
	return retval;		
}

char
Cxx21::sub_0_1A100()
{
	unsigned int v1;

	v1 = read32(base_addr + 0x14);
	if(v1 == 0 || v1 == 0xffffffff) return 0;
	var_1 = v1;
	char rc = v1 & 0x80000000;

	if(var_2 || !v1) goto out;
	
	write32(base_addr + 0xc, 0x80000000);
	
	if(0 != mpFlash[0])
	{
		if((var_1 & 0x00010000) || (var_1 & 0x00000100))
			mpFlash[0]->vtable[3]();
	}

	if(0 != mpFlash[1])
	{
		if((var_1 & 0x00020000) || (var_1 & 0x00000200))
			mpFlash[1]->vtable[3]();
	}

	if(0 != mpFlash[2])
	{
		if((var_1 & 0x00040000) || (var_1 & 0x00000400))
			mpFlash[2]->vtable[3]();
	}

	if(0 != mpFlash[3])
	{
		if((var_1 & 0x00080000) || (var_1 & 0x00000800))
			mpFlash[3]->vtable[3]();
	}

out:
	write32(base_addr + 0x14, var_1);
	return rc;
}

char 
Cxx21::sub_0_1BA00()
{
	if(var_2 != 0) return 0;

	int t_val, r_val = 0;
	
	if(mpFlash[0] != 0)
		if(0x100 & mpFlash[0]->vtable[2]()) r_val = 0x100;
		
	if(mpFlash[1] != 0)
		if(0x100 & mpFlash[1]->vtable[2]()) r_val |= 0x200;
	
	if(mpFlash[2] != 0)
		if(0x100 & mpFlash[2]->vtable[2]()) r_val |= 0x400;
	
	if(mpFlash[3] != 0)
		if(0x100 & mpFlash[3]->vtable[2]()) r_val |= 0x800;

	if(var_1 & 0x1) 
	{
		CardDetection(0); r_val |= 0x1;
	}

	if(var_1 & 0x2) 
	{
		CardDetection(1); r_val |= 0x2;
	}

	if(var_1 & 0x4) 
	{
		CardDetection(2); r_val |= 0x4;
	}

	if(var_1 & 0x8) 
	{
		CardDetection(3); r_val |= 0x8;
	}

	write32(base_addr + 0x8, 0x80000000);
	return r_val;
}

char
Cxx21::GetMediaID(char socket)
{
	if(socket >= num_sockets) return 0xa0;

	if(sock_valid[socket] == 0) return 0xa1;
	
	return mpFlash[socket]->GetMediaID();
}
