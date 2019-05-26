
typedef struct {
	/*Pointer function for LED*/
	void (*pfnLedNetwork)(int state);
	void (*pfnLedUser)(int state);
	
	/*Pointer function for Switch*/
	void (*pfnSwitchConfigGet)(int32_t *mode);
	
	/*Pointer function for Delay*/
	void (*pfnDelayMs)(unsigned int ms);
	void (*pfnReboot)(void);
	
	/*Pointer function for NVM*/
	void (*pfnMemRead)(uint8_t *data, unsigned int ui32Address,
			unsigned int ui32Count);
	void (*pfnMemWrite)(uint8_t *data, unsigned int ui32Address,
			unsigned int ui32Count);

	//Clock
	int (*pfnGetEpoch)(uint32_t *epoch);
	int (*pfnSetEpoch)(uint32_t *epoch);

	//Debug
	long (*pfnDebugPrint)(const char *pcString, ...);
  .
  .

} hb_t;
