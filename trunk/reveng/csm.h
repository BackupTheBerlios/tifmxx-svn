
class CSM : public CFlash
{
	char mbWriteBlockPos; // var_x0a4;

	int var_x0b0;
	int var_x0b4;

	int dwSM_STATUS;      // 0x0c0
	int var_x0c4;
	int var_x0c8;
	KEVENT sm_event_x0d0;

	char mbyZones;        // 0x0e8
	short mwPhyBlocks;    // 0x0ea
	short mwLogBlocks;    // 0x0ec
	char mbyPhyBlockSize; // 0x0ee
	char mbyLogBlockSize; // 0x0ef
	short mwPageSize;     // 0x0f0

	short *mpFirstEmptyPhyBlock; // 0x0f8
	short *var_x100;
	short var_x108;
	char var_x10a;
	short var_x10c;
	short mwHeadSrcPhyBlock;  // var_x10e;
	short var_x110;
	char var_x112;
	char var_x113;
	short var_x114;
	short mwTailSrcPhyBlock;  // var_x116;
	short var_x118;
	char var_x11a;

	int var_x11c;
	int var_x120;
	short var_x124;
	char var_x126;
	char *var_x127;

	char var_x147;
	short *var_x148;
	char *var_x150;

	int muiBadBlocks;      // var_x158;
	char var_x15c;
	char var_x15d;
	char var_x15e;
	char var_x15f;
	short var_x160;
	char var_x162;
	int mdwDMAaddressReg;  // var_x164;
	short mwDMAcontrolReg; // var_x168;
	short var_x16a;
	char var_x16c;
	char var_x16d;
	char var_x16e;

	void sub20000(short arg_1);
	void sub27910(char arg_1, short arg_2, short arg_3)
	short sub27950(char arg_1, short arg_2)
	static char CheckTimeOut(int *status_var);
	static char CheckDataErr(int *status_var);
	static char CheckLogicErr(int *status_var);
	static char CheckDSErr(int *status_var);
	static char CheckCorrEcc(int *status_var);
	static char CheckUnCorrEcc(int *status_var);
	static char CheckFlashErr(int *status_var);
	char WaitForCMD(char arg_1, char arg_2, char arg_3)
	void ResetCard();
	char ReadXDId();
	char ReadID();
	char MediaModel();
	char GetEmptyPhyBlock(char zone, short *arg_2);
	char sub28b80(char arg_1, short arg_2, char arg_3);
	char sub28cb0(short *arg_1);
	char ReadPage(char byZone, short wPhyBlock, char byPage, char arg_4);
	char MarkBadBlock(char byZone, short wPhyBlock, char arg_3);
	char FillUpBlock(char arg_1, short arg_2, char arg_3, char arg_4, char arg_5);
	short PhyBlockDefective();
	void sub29ec0();
	char sub29ff0();
	char CheckCIS();
	char EccCorrectData(int dwECCregister);
	char EraseBlock(char byZone, short wPhyBlock);
	char WritePage(char arg_1, short arg_2, short arg_3, char arg_4, char arg_5);
	char WritePhyBlock(char byZone, short wLogBlock, short *arg_3, char byPage, char bySectorCount);
	char WriteSector(int arg_1, short bySectorCount);
	char MakeLUT();
	void EraseCard();
	char WriteCIS();
	char CISFixOrCheck(char uiFixCheckCIS);
	char ProcessCorrectableError(char byZone, short wLogBlock, short *wPhyBlock, char arg_4, char arg_5);
	char ReadSector(int arg_1);
	char CopyPagesDMA(char byZone, short arg_2, short dstPhyBlock, short srcPhyBlock, char srcPage, char dstPage, char arg_7);
	char CopyPages(char byZone/*r12*/, short arg_2, short dstPhyBlock, short srcPhyBlock/*si*/, char srcPage, char dstPage, char arg_7);


	CSM(char *_base_addr);
	~CSM();
	char CloseWrite();
	char Isr(char arg_1, char arg_2);
	int IsrDpc();
	char RescueRWFail();
	char InitializeCard();
	char ReadSectors(int arg_1, short *arg_2, char *arg_3, char arg_4);
	char WriteSectors(int arg_1, short *arg_2, char *arg_3);
	char WriteProtectedWorkaround();
	
};
