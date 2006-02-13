struct CFlash
{
	void *vtable;            // 0x000 : 11 methods, 7 empty	
	char *base_addr;         // 0x008 - iomem base address for socket
	PKINTERRUPT card_int;    // 0x010
	int SDSwitch;            // 0x018 (need_sw, 0x01A -> var_9 -> MSW)
	int SMEnable;            // 0x01C
	int SMCISEnable;         // 0x020
	int InsDelEnable;        // 0x024
	char muiMediaID;         // 0x028
	int ClkFreq;             // 0x02C (var_1)
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
	KEVENT event_1;          // 0x050 // correct KeEventSet/Clear to use references
	KEVENT event_2;          // 0x068
	short var_10;            // 0x080	
	short var_x82;           // 0x082
	short var_x84;           // 0x084
	short muiReadSectorCountStart;  // 0x086 (var_3)
	int muiSMReadSector;     // 0x088 (var_4)
	int var_5;               // 0x08C
	int var_x90;             // 0x090
	char var_x94[16];        // 0x094
	char var_xa4;	         // 0x0A4
	int var_xa8;             // 0x0A8
	int var_xb0;             // 0x0B0
	int var_xb4;             // 0x0B4
	int var_7;               // 0x0B8
	
	CFlash(char *_base_addr);
	virtual ~CFlash();

	virtual char CloseWrite();                   // vtable + 0x08
	virtual int vtbl02();                        // vtable + 0x10
	virtual char vtbl03(char arg_1, char arg_2); // vtable + 0x18
	virtual char RescueRWFail();	             // vtable + 0x30

	char GetMediaID();

	void sub_0_1FE70();
	char Read(int uiLBAStart, short *uiReadSectorCountStart, int uiDMAPhysicalAddress, char *uiDMAPageCount);
	char Write(int uiLBAStart, short *uiWriteSectorCountStart, int uiDMAPhysicalAddress, char *uiDMAPageCount);
	void InitializeWriteBlocks();
	struct tigd* GetGeometry(struct tigd *arg_1);
	void GetSMDmaParams(int arg_1, int arg_2, int arg_3);
	void Power(char arg_1);
	char sub_0_1FEE0(PRKEVENT c_event, int arg_2);
	void sub_0_1FD80(long interval);

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






