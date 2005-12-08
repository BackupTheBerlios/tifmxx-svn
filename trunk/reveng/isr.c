BOOLEAN
InterruptServiceRoutine(struct _KINTERRUPT *i_obj, PVOID data)
{
    Cxx21 *d_data = data->off_x268;
    BOOLEAN rc;
    
    if(d_data->sub_0_1A100())
    {
	KeInsertQueueDpc(data->off_x0C8, 0, data); //DefaultDPC
	return true;
    }
    
    return false;
}

void
DefaultDPC(struct _KDPC *i_dpc, void *def_cont, void *arg1, void *arg2)
{
	KeAcquireSpinLockAtDpcLevel(arg2->off_x1C8);
	Cxx21 *d_data = arg2->off_x268;

	char m_sock = d_data->sub_0_1BA00();
	if(m_sock & 0x1)
	{
		if(0xa1 == d_data->GetMediaID(0))
		{
			//! socket 0 empty
		}
		else
		{
			//! socket 0 card insertion
		}
		//! IoInvalidateDeviceRelations(...);
	}
	
	if(m_sock & 0x2)
	{
		if(0xa1 == d_data->GetMediaID(1))		
		{
			//! socket 1 empty
		}
		else
		{
			//! socket 1 card insertion
		}
		//! IoInvalidateDeviceRelations(...);
	}
	
	if(m_sock & 0x4)
	{
		if(0xa1 == d_data->GetMediaID(2))		
		{
			//! socket 2 empty
		}
		else
		{
			//! socket 2 card insertion
		}
		//! IoInvalidateDeviceRelations(...);
	}
	
	if(m_sock & 0x8)
	{
		if(0xa1 == d_data->GetMediaID(3))		
		{
			//! socket 3 empty
		}
		else
		{
			//! socket 3 card insertion
		}
		//! IoInvalidateDeviceRelations(...);
	}
	KeReleaseSpinLockFromDpcLevel(arg2->off_x1C8);
}
 