
class CSM : public CFlash
{
	char var_x0a4;
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

	char var_x0f8[16];
	char var_x10a;
	short var_x10c;
	short var_x10e;
	short var_x110;
	char var_x112;
	char var_x113;
	short var_x114;
	short var_x116;
	short var_x118;
	char var_x11a;

	short var_x124;
	char var_x126;

	short var_x148[8];

	char var_x15d;
	char var_x15e;
	char var_x15f;
	short var_x160;
	char var_x162;
	int var_x164;
	short var_x168;
	short var_x16a;
	char var_x16c;
	char var_x16d;
	char var_x16e;

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

	CSM(char *_base_addr);
	char Isr(char arg_1, char arg_2);
	int IsrDpc();
};
