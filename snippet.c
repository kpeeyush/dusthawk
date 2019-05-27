
osi_EnterCritical();

for (i = 0; i < len; i++)
{
	if (i & 1)
	{
		ir_modulation_disable_2();
		osi_Busywait(irdata[i]);
	} 
	else 
	{
		ir_modulation_enable_2();
		osi_Busywait(irdata[i]);
	}
}

osi_ExitCritical(0);
