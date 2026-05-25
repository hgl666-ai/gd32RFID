#ifndef _BSP_LED_H
#define _BSP_LED_H

#include "gd32e23x.h"

/* --- 硬件引脚宏定义 (方便后期修改) --- */
#define LED_PORT         GPIOA
#define LED_PIN          GPIO_PIN_1
#define LED_CLOCK        RCU_GPIOA

/* --- 函数声明 --- */
void bsp_led_init(void);    // 初始化 LED
void bsp_led_on(void);      // 点亮 LED
void bsp_led_off(void);     // 熄灭 LED
void bsp_led_toggle(void);  // 翻转 LED 状态

#endif /* _BSP_LED_H */
