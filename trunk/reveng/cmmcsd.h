#include "cflash.h"

struct CMMCSD : public CFlash // mmc, sd
{
	//alloc: 0x130 bytes
	void *vtable; // 0x000 : 11 methods

	int mdwBlocks;           // 0x0C0 (dwBlocks)
	char byReadBlockLen;     // 0x0C4
	short wReadBlockLen;     // 0x0C6 (cmmcsd_var_3, wBlockLen)
	char byWriteBlockLen;    // 0x0C8
	short wWriteBlockLen;    // 0x0CA
	int mdwSize;             // 0x0CC (dwSize)
	char mbySizeMult;        // 0x0D0 (bySizeMult)
	short mwClkSpeed;        // 0x0D2 (cmmcsd_var_12)
	int dwRCA;               // 0x0D4 (cmmcsd_var_13)

	char var_xd6;            // 0x0D6
	char var_xd7;            // 0x0D7
	char cmmcsd_var_1;       // 0x0D8
	int byStatus;            // 0x0DC (cmmcsd_var_5)
	int cmmcsd_var_11;       // 0x0E0
	int cmmcsd_var_10;       // 0x0E4
	KEVENT cmmcsd_event_1;   // 0x0E8 // correct KeEventSet/Clear to use references
	char mbCardBusy;         // 0x100 (cmmcsd_var_2)
	char cmmcsd_var_4;       // 0x101	
	char mpSerialNumber[21]; // 0x102
	char TAAC;               // 0x117
	char NSAC;               // 0x118
	char R2W_FACTOR;         // 0x119
	short ReadTimeOut;       // 0x11A
	short WriteTimeOut;      // 0x11C
	long cmmcsd_var_6;       // 0x120
	short muiExecute_DMAPagesProcessed;    // 0x128 (cmmcsd_var_7)
	short cmmcsd_var_8;	 // 0x12A
	char cmmcsd_var_14;      // 0x12C
	char mbCRSImplemented;   // 0x12D
	char cmmcsd_var_9;	 // 0x12E

	struct ExecParam
	{
		int uiCommandIndex;       // 0x000
		int uiCommandArgument;    // 0x004
		int uiDataTransferLength; // 0x008
		int uiResponseType;       // 0x00c
		char bDIR;                // 0x00d
		char bApp;                // 0x00e
                char RESP;                // 0x00f
		char bBLKM;               // 0x010
	};


	CMMSD(char *_base_addr);
	~CMMSD();                                 // vtable + 0x00
	//! vtable + 0x08 -> CFlash::CloseWrite();
	int vtbl02();                             // vtable + 0x10
	char vtbl03(char arg_1, char arg_2);      // vtable + 0x18
	char* LongName();                         // vtable + 0x20
	char* Name();                             // vtable + 0x28
	char RescueRWFail();                      // vtable + 0x30
	char InitializeCard();                    // vtable + 0x38
	// seems like: arg_1 == start_offset, arg_2 == count, arg_3 == some_other_count
	char ReadSectors(int arg_1, short *arg_2, short *arg_3, char arg_4);  // vtable + 0x40
	char WriteSectors(int arg_1, short *arg_2, short *arg_3); // vtable + 0x48
	char WriteProtectedWorkaround()           // vtable + 0x50

	//CMMCSD specific functions:
	char DetectCardType();
	char Standby();
	char ReadCSDInformation();
	char GetCHS();
	char ReadSerialNumber();
	void ReportMediaModel(); // print some debug info
	char sub_0_1CE40(char arg_1, int arg_2, short arg_3);
	char Execute(ExecParam *pParam, int uiDMAPhysicalAddress, char *uiDMAPageCount, char *pData);
	char GetState(char *byCardState, char arg_2);
	char WaitForEOC();
	char WaitForBRS();
	int sub_0_1C1F0(char bf_end, char bf_start, char op); //access CSD/CID bit-fields
	short sub_0_1E1B0(char arg_1, char arg_2);
	char WaitForCard();
	char WaitForAF();
	char WaitForAE();
	void sub_0_1EEA0(char *arg_1);
};
