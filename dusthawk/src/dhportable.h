#ifndef __DHPORTABLE_H__
#define __DHPORTABLE_H__

#ifdef __cplusplus
extern "C"
{
#endif

void port_delay_init(void);
void port_delay_ms(uint32_t ms);

void port_pump_init(void);
void port_pump_set(uint8_t state);
void port_pump_get(uint8_t *state);

void port_valve_init(void);
void port_valve_open(uint32_t duty);
void port_valve_close(void);

void port_rtc_init(void);
void port_rtc_set(uint32_t epoch);
void port_rtc_get(uint32_t *epoch);

void port_nvm_init(void);
void port_nvm_get(void *buff, uint32_t ui32Address, uint32_t ui32Count);
void port_nvm_set(void *buff, uint32_t ui32Address, uint32_t ui32Count);

void port_lcd_init(void);
void port_lcd_clear(void);
void port_lcd_cursor(uint8_t type);
void port_lcd_goto(uint8_t row, uint8_t col);
void port_lcd_print(const char *str, ...);

void port_dbg_init(void);
void port_dbg_print(const char *str, ...);




#ifdef __cplusplus
}
#endif

#endif
