#ifndef _BSP_SYSTICK_H
#define _BSP_SYSTICK_H

#include <stdint.h>

extern volatile uint32_t g_sys_tick_ms;

void systick_config(void);

void delay_ms(uint32_t count);

#endif 
