struct CFlash
{
	void *vtable;            // 0x000 : 11 methods, 7 empty	
	char *base_addr;         // 0x008 - iomem base address for socket

	char need_sw;            // 0x018 

	char muiMediaID;         // 0x028
	int var_1;               // 0x02C
	char is_xx12;            // 0x030

	short var_2;             // 0x038
//!	char arr_1[8];           // 0x03A	
	char vara_0;             // 0x03A
	char vara_1;             // 0x03B
	char vara_2;             // 0x03C
	char WriteProtected;     // 0x03D
	char vara_4;             // 0x03E
	char vara_5;             // 0x03F
	char vara_6;             // 0x041
	char vara_7;             // 0x042
	int var_8;               // 0x044
	short Size;              // 0x048 - card size in MB (var_6)
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

struct CMMCSD : public CFlash // mmc, sd
{
	//alloc: 0x130 bytes
	void *vtable; // 0x000 : 11 methods

	int dwBlocks;            // 0x0C0
	char byReadBlockLen;     // 0x0C4
	short wBlockLen;         // 0x0C6 (cmmcsd_var_3)

	int dwSize;              // 0x0CC

	int cmmcsd_var_11;       // 0x0E0
	int cmmcsd_var_10;       // 0x0E4
	PRKEVENT cmmcsd_event_1; // 0x0E8

	char bySizeMult;         // 0x0D0
	short cmmcsd_var_12:     // 0x0D2
	int dwRCA;               // 0x0D4 (cmmcsd_var_13)

	char cmmcsd_var_1;       // 0x0D8
	int cmmcsd_var_5;        // 0x0DC

	char cmmcsd_var_2;       // 0x100
	char cmmcsd_var_4;       // 0x101

	long cmmcsd_var_6;       // 0x120
	short cmmcsd_var_7;	 // 0x128
	short cmmcsd_var_8;	 // 0x12A
	char cmmcsd_var_14;      // 0x12C
	char cmmcsd_var_9;	 // 0x12E

	CMMSD(char *_base_addr);
	~CMMSD();                                 // vtable + 0x00
	//! vtable + 0x08 -> CFlash::CloseWrite();
	int vtbl02();                             // vtable + 0x10
	char vtbl03(char arg_1, char arg_2);      // vtable + 0x18
	char* LongName();                         // vtable + 0x20
	char* Name();                             // vtable + 0x28
	char RescueRWFail();                      // vtable + 0x30
	char InitializeCard();                    // vtable + 0x38
	ReadSectors();                            // vtable + 0x40
	WriteSectors();                           // vtable + 0x48
	char WriteProtectedWorkaround()           // vtable + 0x50

	//CMMCSD specific functions:
	void sub_0_1FEE0(PRKEVENT c_event, int arg_2);
	char DetectCardType();
	char Standby();
	char ReadCSDInformation();
	char GetCHS();
	void ReportMediaModel(); // print some debug info
	char sub_0_1CE40(char arg_1, int arg_2, short arg_3);
	char Execute(char *pParam, int uiDMAPhysicalAddress, char *uiDMAPageCount, void *pData);
	char GetState(char *arg_1, char arg_2);
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






