#ifndef __FMSE_PORT_H
#define __FMSE_PORT_H

#include <stdint.h>

/* FMSE 安全芯片 I2C 地址 (8位格式) */
#define FMSE_I2C_ADDR           0xE2
#define FMSE_I2C_ADDR_READ      0xE3

/* 超时参数 */
#define FMSE_PWR_ON_DELAY       10   /* ms */
#define FMSE_PWR_OFF_DELAY      10   /* ms */
#define FMSE_FRAME_DELAY        5    /* ms */

/* I2C 驱动函数指针结构体 (与 SDK 一致) */
typedef struct {
    uint8_t se_i2c_addr;
    void (*fm_i2c_power_on)(void);
    void (*fm_i2c_power_off)(void);
    void (*fm_i2c_init)(void);
    void (*fm_i2c_start)(void);
    void (*fm_i2c_stop)(void);
    uint8_t (*fm_i2c_send_char)(uint8_t ch);
    uint8_t (*fm_i2c_recv_char)(void);
    uint8_t (*fm_i2c_send_addr)(uint8_t ch);
    uint8_t (*fm_i2c_recv_char_nak)(void);
} StSeI2CDriver;

extern StSeI2CDriver gusr_i2c_drv;

#endif /* __FMSE_PORT_H */
