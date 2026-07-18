#include "fmse_port.h"
#include "bsp_i2c.h"
#include "bsp_systick.h"

/*
 * FMSE 端口层 — 桥接 SDK StSeI2CDriver 到 bsp_i2c.c 软件 I2C
 * 返回值约定: 0=成功(ACK), 非0=失败(NACK)
 */

static void fmse_power_on(void)  { delay_ms(FMSE_PWR_ON_DELAY); }
static void fmse_power_off(void) { delay_ms(FMSE_PWR_OFF_DELAY); }

/* I2C 初始化 — 总线已由 bsp_i2c_init() 完成 */
static void fmse_i2c_init(void) { /* 共用总线, 已初始化 */ }

/* I2C 基本操作桥接 */
static void fmse_i2c_start(void) { i2c_start(); }
static void fmse_i2c_stop(void)  { i2c_stop(); }

static uint8_t fmse_i2c_send_char(uint8_t ch)
{
    i2c_send_byte(ch);
    return i2c_wait_ack();  /* 0=ACK, 1=NACK */
}

static uint8_t fmse_i2c_recv_char(void)
{
    return i2c_read_byte(1);  /* 发 ACK, 继续读 */
}

static uint8_t fmse_i2c_send_addr(uint8_t ch)
{
    i2c_send_byte(ch);
    return i2c_wait_ack();
}

static uint8_t fmse_i2c_recv_char_nak(void)
{
    return i2c_read_byte(0);  /* 发 NACK, 停止读 */
}

/* 驱动实例 */
StSeI2CDriver gusr_i2c_drv = {
    .se_i2c_addr        = FMSE_I2C_ADDR,
    .fm_i2c_power_on    = fmse_power_on,
    .fm_i2c_power_off   = fmse_power_off,
    .fm_i2c_init        = fmse_i2c_init,
    .fm_i2c_start       = fmse_i2c_start,
    .fm_i2c_stop        = fmse_i2c_stop,
    .fm_i2c_send_char   = fmse_i2c_send_char,
    .fm_i2c_recv_char   = fmse_i2c_recv_char,
    .fm_i2c_send_addr   = fmse_i2c_send_addr,
    .fm_i2c_recv_char_nak = fmse_i2c_recv_char_nak,
};
