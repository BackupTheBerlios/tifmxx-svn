struct CFlash
{
	//alloc: 0x38 bytes?
	void *vtable; // 0x000 : 11 methods, 7 empty
	
	char *base_addr;  // 0x008 - iomem base address for socket

	char muiMediaID;  // 0x028

	int var_1;        // 0x02C
	char is_xx12;     // 0x030

	//PRKEVENT c_event; // 0x050
	
	CFlash(void *_base_addr); 	
	virtual ~CSocketBase() {};

	virtual CloseWrite(); // vtable + 0x8
	virtual vtbl02(); //vtable + 0x10
	virtual vtbl03(); //vtable + 0x18
	virtual RescueRWFail();	//vtable + 0x30

	char GetMediaID();
};

struct CSocket : public CFlash
{
	void *vtable;  // 0x000 : 1 method

	~CSocket();	
	void SocketPowerCtrl();
};

struct CMMCSD : public CFlash
{
	void *vtable; // 0x000 : 11 methods

	CMMSD(void *_base_addr);
	~CMMSD(); //vtable + 0x0
	//! vtable + 0x10 -> CFlash::CloseWrite();
	vtbl02(); //vtable + 0x10
	vtbl03(); //vtable + 0x18
	char* LongName(); //vtable + 0x20
	char* Name(); //vtable + 0x28
	RescueRWFail(); //vtable + 0x30
	InitializeCard(); //vtable + 0x38
	ReadSectors(); //vtable + 0x40
	WriteSectors(); //vtable + 0x48
	WriteProtectedWorkaround(); //vtable + 0x50
};

struct CSM : public CFlash
{
};

struct CMSBase : public CFlash
{
};

struct CMS : public CMSBase
{
};

struct CMSPro : public CMSBase
{
};






