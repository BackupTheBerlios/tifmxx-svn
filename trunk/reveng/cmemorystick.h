bool ClearStatus(int *status_var);

class CMemoryStick : public CFlash
{
protected:
	int        dwMS_STATUS;         // 0x0C0;
	KEVENT     ms_event_x0c8;
	char       mbParallelInterface; // 0x0E0
	char       var_x0e1;
	char       var_x0e2;

	struct
	{
// MS status register:
		struct
		{
			char   rsv1;      // 0x0E3
			char   interrupt; // 0x0E4
			char   status0;   // 0x0E5
			char   status1;   // 0x0E6
			char   type;      // 0x0E7
			char   rsv2;      // 0x0E8
			char   category;  // 0x0E9
			char   class;     // 0x0EA
			char   rsv3[8];   // 0x0EB - 0x0F2
		} status;
// MS param register:
		struct
		{
			char   system;           // 0x0F3
			char   block_address[3]; // 0x0F4 - 0x0F6
			char   cp;               // 0x0F7
			char   page_address;     // 0x0F8
		} param;
// MS extra data register:
		struct
		{
			char   overwrite_flag;     // 0x0F9
			char   management_flag;    // 0x0FA
			char   logical_address[2]; // 0x0FB - 0x0FC
		} extra_data;

		char       var_x0fd;
		char       var_x0fe;
		char       var_x0ff;
		char       var_x100;
		char       var_x101;
		char       var_x102;
		char       var_x103;
	} ms_regs __attribute__((packed));

	int        var_x104;
	int        var_x108;        // 0x108 - read block count
	int        var_x10c;        // 0x10c - write block count

	static bool HandleError(int *status_var);
	static bool HandleMSINT(int *status_var);
	static bool HandleRDY(int *status_var);

	char WaitForRDY();
	char sub21DA0();
	char sub22370(int arg_1);

public:
	CMemoryStick(char *_base_addr, char serial_mode);
	virtual ~CMemoryStick(); //     vtable slot 0
	virtual int IsrDpc();    //                 2
	virtual int Isr(char arg_1, char arg_2); // 3

	virtual char InitializeCard();
	virtual char ReadSectors(int arg_1, short *arg_2, char *arg_3, char arg_4);
	virtual char WriteSectors(int arg_1, short *arg_2, char *arg_3, char arg_4);
	virtual char WriteProtectedWorkaround() { return 0; };
	virtual char ReadSector(int arg_1, short arg_2, char arg_3) { return 0; };
	virtual char WriteSector(int arg_1, short arg_2, char arg_3) { return 0; };

}
