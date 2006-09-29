
class CMS : public CMemoryStick
{
	char  bySegments;        // 0x110;
	short mwPhyBlocks;       // 0x112
	short mwUserBlocks;      // 0x114
	short mwEffectiveBlocks; // 0x116
	char  byBlockSize;       // 0x118
	short boot_blocks[2];    // 0x11A

	short var_x11e[];        // 0x11E

	short FFPhyBlock;         // 0x13E
	short var_x140;
	short var_x142;
	short var_x144;
	char var_x148[0x20];     // 0x148
	short var_x168;
	short var_x16a;
	short var_x16e;
	char var_x170;
	short var_x172[0x2000];  // 0x172
	short var_x2172[0x2000]; // 0x2172
	char bad_blocks[0x400];  // 0x4172

	char WaitForMSINT();
	char sub219F0();
	char WriteRegisters();
	char Cmd(char opcode, char arg_2, char arg_3, char arg_4);
	char GetInt(); //sub22260
	char ReadPage(short wPhyBlock, char byPage, char arg_3, int arg_4);
	char FindBootBlocks();
	char MediaModel();
	char EraseBlock(short block_address);
	char sub23BA0(short *arg_1, char arg_2);
	char WritePage(short arg_1, short block_address, char arg_3, char arg_4, char arg_5);
	char CopyPages(short arg_1, short *arg_2, char arg_3, char arg_4, short arg_5);
	char MakeLUT();
	char GetCHS();
	char InitializeLUT();
	char SwitchToParallelIF();

	CMS(char *_base_addr, char arg_2);
	char CloseWrite();
	char RescueRWFail();
	char InitializeCard();
}
