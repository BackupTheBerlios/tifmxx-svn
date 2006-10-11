
void
CSM::sub27910(char arg_1, short arg_2, short arg_3)
{
	if(arg_1 >=  var_x0e8 || arg_2 >= var_x0ec) return;
	var_x148[var_x0ec * arg_1 + arg_2] = arg_3;
}

short
CSM::sub27950(char arg_1, short arg_2)
{
	if(arg_1 >=  var_x0e8 || arg_2 >= var_x0ec) return -1;
	return var_x148[var_x0ec * arg_1 + arg_2];
}

CSM::CSM(char *_base_addr)
    :CFlash(_base_addr), dwSM_STATUS(0), var_x0c4(0), var_x0c8(0), var_x112(0), var_x16c(0), var_x164(0),
     var_x168(0), var_x148({0}), var_x0f8({0}), var_x15d(0), var_x15e(0), var_x0a4(0), var_x162(0), var_x16e(0),
     var_x10a(-1), var_x10c(-1), var_x10e(-1), var_x110(-1), var_x113(-1), var_x114(-1), var_x116(-1), var_x118(-1),
     var_x11a(32), var_x124(-1), var_x126(-1), var_x16a(-1), var_x16d(-1), var_x160(-1), var_x15f(-1)
{
	KeInitializeEvent(&sm_event_x0d0, 0, 0);
	write32(base_addr + 0x90, 0xffffffff);

}

char
CSM::Isr(char arg_1, char arg_2)
{
	char rc;

	rc = CFlash::Isr(arg_1, arg_2);
	if(arg_2)
	{
		var_x0c4 = dwSM_STATUS | read32(base_addr + 0x98);
		var_x0c8 |= read32(base_addr + 0x90);
		write32(base_addr + 0x90, var_x0c8);
	}
	return rc | arg_2;
}

int
CMS::IsrDpc()
{
	int rc = CFlash::IsrDpc();
	if(vara_0)
	{
		dwSM_STATUS = var_x0c4;
		KeSetEvent(&sm_event_x0d0, 0, 0);
		vara_0 = 0;
	}
	return rc;
}

char
CMS::CheckTimeOut(int *status_var)
{
	return *status_var & 4 ? 1 : 0;
}

char
CMS::CheckDataErr(int *status_var)
{
	return *status_var & 2 ? 1 : 0;
}

char
CMS::CheckLogicErr(int *status_var)
{
	return *status_var & 0x03800000 ? 1 : 0;
}

char
CMS::CheckDSErr(int *status_var)
{
	return *status_var & 0x00400000 ? 1 : 0;
}

char
CMS::CheckCorrEcc(int *status_var)
{
	return *status_var & 0x00100000 ? 1 : 0;
}

char
CMS::CheckUnCorrEcc(int *status_var)
{
	return *status_var & 0x00080000 ? 1 : 0;
}

char
CMS::CheckFlashErr(int *status_var)
{
	return *status_var & 0x00010000 ? 1 : 0;
}
