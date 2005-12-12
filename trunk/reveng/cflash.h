struct CFlash
{
	void *vtable;            // 0x000 : 11 methods, 7 empty	
	char *base_addr;         // 0x008 - iomem base address for socket
	PKINTERRUPT card_int;    // 0x010
	char need_sw;            // 0x018
	short var_9;             // 0x01A 

	char muiMediaID;         // 0x028
	int var_1;               // 0x02C
	char is_xx12;            // 0x030

	short var_2;             // 0x038
	char vara_0;             // 0x03A // (arr_1)
	char vara_1;             // 0x03B
	char vara_2;             // 0x03C
	char mbWriteProtected;   // 0x03D (WriteProtected)
	char vara_4;             // 0x03E
	char vara_5;             // 0x03F
	char vara_6;             // 0x040
	char vara_7;             // 0x041

	int SerialNumber;        // 0x044 (var_8)
	short mwSize;            // 0x048 (Size) - card size in MB (var_6)
	short mwCylinders;       // 0x04A
	short mwHeadCount;       // 0x04C
	short mwSectorsPerTrack; // 0x04E
	PRKEVENT event_1;        // 0x050

	PRKEVENT event_2;        // 0x068

	short var_3;             // 0x086
	int var_4;               // 0x088
	int var_5;               // 0x08C

	int var_7;               // 0x0B8
	
	CFlash(char *_base_addr); 	
	virtual ~CFlash();

	virtual char CloseWrite();                   // vtable + 0x08
	virtual int vtbl02();                        // vtable + 0x10
	virtual char vtbl03(char arg_1, char arg_2); // vtable + 0x18
	virtual char RescueRWFail();	             // vtable + 0x30

	char GetMediaID();
};

struct CSocket : public CFlash
{
	//alloc: 0x38 bytes
	void *vtable;  // 0x000 : 1 method

	~CSocket();	
	void SocketPowerCtrl();
};

struct CSM : public CFlash // smartmedia, xD
{
};

struct CMSBase : public CFlash // memorystick common
{
};

struct CMS : public CMSBase // memorystick
{
};

struct CMSPro : public CMSBase // memorystick pro
{
};






