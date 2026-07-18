#ifndef _BSP_I2C_H
#define _BSP_I2C_H

#include "gd32e23x.h"


#define I2C_RCU         RCU_GPIOB
#define I2C_PORT        GPIOB
#define I2C_SCL_PIN     GPIO_PIN_6
#define I2C_SDA_PIN     GPIO_PIN_7


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

#endif 
