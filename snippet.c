for (i = 0; i < len; i++)
{
	if (i & 1)
	{
		ir_modulation_disable_2();
		bsp_delay_us(irdata[i]);
	} 
	else 
	{
		ir_modulation_enable_2();
		bsp_delay_us(irdata[i]);
	}
}
