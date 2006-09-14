
class CMemoryStick : public CFlash
{
protected:
	int        dwMS_STATUS;     // 0x0C0;
	KEVENT     ms_event_x0c8;
	char       serial_mode;     // 0x0E0
	char       var_x0e1;
	char       var_x0e2;
	char       var_x0e3[32];
	int        var_x104;
	int        var_x108;        // 0x108 - read block count
	int        var_x10c;        // 0x10c - write block count

	static bool ClearStatus(int *status_var);
	static bool HandleError(int *status_var);
	static bool HandleMSINT(int *status_var);
	static bool HandleRDY(int *status_var);

	char WaitForRDY();
public:
	CMemoryStick(char *_base_addr) : CFlash(_base_addr) {};
	virtual ~CMemoryStick(); //     vtable slot 0
	virtual int IsrDpc();    //                 2
	virtual int Isr(char arg_1, char arg_2); // 3

	virtual char InitializeCard();
	virtual char ReadSectors(int arg_1, short *arg_2, char *arg_3, char arg_4);
	virtual char WriteSectors(int arg_1, short *arg_2, char *arg_3, char arg_4);
	virtual char WriteProtectedWorkaround() { return 0; };
	virtual char ReadSector(int arg_1, short arg_2, char arg_3) { return 0; };
	virtual char WriteSector(int arg_1, short arg_2, char arg_3) { return 0; };

	char sub21DA0();
	char sub22370(int arg_1);
}
