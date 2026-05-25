#ifndef _BSP_SYSTICK_H
#define _BSP_SYSTICK_H

#include <stdint.h>

/* 配置 SysTick 定时器，使其每 1ms 产生一次中断 */
void systick_config(void);

/* 毫秒级精准延时函数 */
void delay_ms(uint32_t count);

#endif /* _BSP_SYSTICK_H */
