
class CMS : public CMemoryStick
{
	char  bySegments;        // 0x110;
	short mwPhyBlocks;       // 0x112
	short mwUserBlocks;      // 0x114
	short mwEffectiveBlocks; // 0x116
	char  byBlockSize;       // 0x118
	short boot_blocks[2];    // 0x11A

	char WaitForMSINT();
	char sub219F0();
	char WriteRegisters();
	char Cmd(char opcode, char arg_2, char arg_3, char arg_4);
	char GetInt(); //sub22260
	char ReadPage(short wPhyBlock, char byPage, char arg_3, int arg_4);
	char FindBootBlocks();
	char MediaModel();

	CMS(char *_base_addr, char arg_2);
}
