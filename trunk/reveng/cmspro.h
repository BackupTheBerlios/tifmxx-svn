class CMSPro : public CMS
{
	static char HandleDATA(char *arg_1);
	static char sub217b0(char *arg_1);

	char GetInt();
	char ExCmd(char arg_1, short arg_2, int arg_3);
	char GetCHS(arg_1);
	char SwitchToParallelIF();
	char ConfirmCPUStartup();
	char Format(char arg_1);
	char MediaModel();

	CMSPro(char *_base_addr, _serial_mode);
	char ReadSector(int arg_1, short arg_2, char arg_3);
	char WriteSector(int arg_1, short arg_2, char arg_3)
	char CloseWrite();
	char RescueRWFail();
	~CMSPro();
	char InitializeCard();
};
