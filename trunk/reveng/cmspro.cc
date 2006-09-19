
char
CMSPro::sub22150()
{
	write32(base_addr + 0x190, 0x2607 | var_x104);
	int t_val = read32(base_addr + 0x190);
	KeClearEvent(&ms_event_x0c8);
	KeSynchronizeExecution(&CMS::ClearStatus, &dwMS_STATUS, card_int);
	write32(base_addr + 0x190, (t_val & 0xfffeffff) | 0x0800);
	write32(base_addr + 0x184, 0x7001);
	char rc = WaitForRDY();
	write32(base_addr + 0x190, t_val & 0xfffeffff);
	ms_regs.status.interrupt = read32(base_addr + 0x188);
	read32(base_addr + 0x188);
	return rc;
}
