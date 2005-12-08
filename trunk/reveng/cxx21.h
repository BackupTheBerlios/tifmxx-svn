class Cxx21
{
	// alloc: 0x70 bytes
	char *base_addr;     // 0x000 - iomem base address for device
	PKINTERRUPT int_obj; // 0x008 - windows interrupt object

	int SDSwitch;        // 0x010
	int SMEnable;        // 0x014
	int MSPEnable;       // 0x018
	int SMCISEnable;     // 0x01C
	int InsDelEnable     // 0x020

	int var_1;           // 0x024
	char var_2;          // 0x028
	
	CSocket *mpFlash[4]; // 0x030
	char sock_valid[4];  // 0x050
	int num_sockets;     // 0x054
	char d_socket;       // 0x058


public:
	Cxx21(char *_base_addr);
	~Cxx21();
	
	char GetIntObj(void *_int_obj);
	char GetSDSwitch(int reg_SDSwitch);
	char GetSMEnable(int reg_SMEnable);
	char GetMSPEnable(int reg_MSPEnable);
	char GetSMCISEnable(int reg_SMCISEnable);
	char GetInsDelEnable(int reg_InsDelEnable);
	void GetNumSockets(int _num_sockets);
	
	void Initialize();

	char CardDetection(char socket);	
	char sub_0_1A670(char socket);
	char sub_0_1A8F0(char socket);
	char sub_0_1A100();
	char sub_0_1BA00();
	char GetMediaID(char socket);
};

