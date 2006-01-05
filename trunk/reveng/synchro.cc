//KeSynchronizeExecution routines

BOOLEAN
sub_0_27DE0(int *arg_1)
{
    *arg_1 = 0;
    return true;
}

BOOLEAN
sub_0_1C050(int *arg_1)
{
    return *arg_1 & 0x1;
}

BOOLEAN
sub_0_1C060(int *arg_1)
{
    if(*arg_1 & 0x4000) return true; // cardstatus error
    if(*arg_1 & 0x0080) return true; // timeout error
    if(*arg_1 & 0x0100) return true; // crc error
    return false;
}
