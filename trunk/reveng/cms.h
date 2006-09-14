
class CMS : public CMemoryStick
{
	char WaitForMSINT();
	char sub219F0();
	char WriteRegisters();
	char Cmd(char arg_1, char arg_2, char arg_3, char arg_4);
	char sub22260();
	char ReadPage(short arg_1, char arg_2, char arg_3, int arg_4);

	CMS(char *_base_addr, char arg_2);
}
