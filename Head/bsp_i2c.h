#ifndef _BSP_I2C_H
#define _BSP_I2C_H

#include "gd32e23x.h"

/* --- I2C 引脚配置 (选定 PB6, PB7) --- */
#define I2C_RCU         RCU_GPIOB
#define I2C_PORT        GPIOB
#define I2C_SCL_PIN     GPIO_PIN_6
#define I2C_SDA_PIN     GPIO_PIN_7

/* --- 标准库 GPIO 作宏 --- */
#define I2C_SCL_H()     gpio_bit_set(I2C_PORT, I2C_SCL_PIN)
#define I2C_SCL_L()     gpio_bit_reset(I2C_PORT, I2C_SCL_PIN)

#define I2C_SDA_H()     gpio_bit_set(I2C_PORT, I2C_SDA_PIN)
#define I2C_SDA_L()     gpio_bit_reset(I2C_PORT, I2C_SDA_PIN)
// 读取 SDA 线的电平
#define I2C_SDA_READ()  gpio_input_bit_get(I2C_PORT, I2C_SDA_PIN)

/* --- 接口函数声明 --- */
void bsp_i2c_init(void);
void i2c_start(void);
void i2c_stop(void);
void i2c_send_byte(uint8_t byte);
uint8_t i2c_wait_ack(void);

#endif /* _BSP_I2C_H */
