#ifndef _BSP_I2C_H
#define _BSP_I2C_H

#include "gd32e23x.h"

/*
 * 软件I2C引脚定义
 *
 * F8P6 板 (GD32E230F8P6TR, TSSOP20) 原理图映射:
 *   I2C1_SCL = PA0 (芯片脚6)
 *   I2C1_SDA = PA1 (芯片脚7)
 * FM17622 与 FMSE 共用该软件I2C总线 (开漏输出+上拉, ~100kHz)
 */

#define I2C_RCU         RCU_GPIOA
#define I2C_PORT        GPIOA
#define I2C_SCL_PIN     GPIO_PIN_0
#define I2C_SDA_PIN     GPIO_PIN_1


#define I2C_SCL_H()     gpio_bit_set(I2C_PORT, I2C_SCL_PIN)
#define I2C_SCL_L()     gpio_bit_reset(I2C_PORT, I2C_SCL_PIN)

#define I2C_SDA_H()     gpio_bit_set(I2C_PORT, I2C_SDA_PIN)
#define I2C_SDA_L()     gpio_bit_reset(I2C_PORT, I2C_SDA_PIN)

#define I2C_SDA_READ()  gpio_input_bit_get(I2C_PORT, I2C_SDA_PIN)


void bsp_i2c_init(void);
void i2c_start(void);
void i2c_stop(void);
void i2c_send_byte(uint8_t byte);
uint8_t i2c_wait_ack(void);

uint8_t i2c_read_byte(uint8_t send_ack);

void i2c_bus_recovery(void);

void i2c_bus_scan(void);

#endif 
