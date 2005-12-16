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
	void GetNumSockets(int _num_sockets);
	void Initialize();
	char sub_0_1A100();
	char CardDetection(char socket);	
	char sub_0_1A610(char socket);
	char InitializeCard(char socket);
	char GetMediaID(char socket);
	struct tigd* GetGeometry(struct tigd *arg_1, char socket);
	char Read(char uiSocket, int uiLBA, short ReadSectorCount, int DMAPhysicalAddress, char *pDMAPageCount); // read5
	char Read(char uiSocket, int DMAPhysicalAddress, char *pDMAPageCount); // read3
	// write 5
	char Write(char uiSocket, int uiLBA, short WriteSectorCount, int DMAPhysicalAddress, char *pDMAPageCount);
	char Write(char uiSocket, int DMAPhysicalAddress, char *pDMAPageCount); // write3
	char GetSMDmaParams(char socket, int arg_2, int arg_3, int arg_4);
	char sub_0_1AFE0(char socket, CMMCSD::ExecParam *pParam, int uiDMAPhysicalAddress, char *uiDMAPageCount);
	char sub_0_1B0A0(char socket, int uiDMAPhysicalAddress, char *uiDMAPageCount);
	char sub_0_1B150(char socket, CMMCSD::ExecParam *pParam, char *pData);
	char sub_0_1B210(char socket, char *arg_2);
	char* sub_0_1B2E0(char socket);
	char sub_0_1B3A0(char socket, int *arg_2);
	char WriteProtected(char socket);
	char CloseWrite(char socket);
	char sub_0_1B570(char socket, char arg_2);
	char sub_0_1B5E0(char socket);
	char CISFixCheck(char socket, char arg_2);
	char RescueAction(char socket);
	char GetIntObj(void *_int_obj);
	char GetSDSwitch(int reg_SDSwitch);
	char GetSMEnable(int reg_SMEnable);
	char GetSMCISEnable(int reg_SMCISEnable);
	char GetInsDelEnable(int reg_InsDelEnable);
	char GetMSPEnable(int reg_MSPEnable);
	char SocketValid(char socket);
	char sub_0_1B930(char socket);
	char KillEvent(char socket);
	char sub_0_1BA00();	
	char RemoveCard(char socket);
	char SocketPower(char socket, char arg_2);
	char Suspend();
	char Resume();
};

