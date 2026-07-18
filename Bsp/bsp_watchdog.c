#include "bsp_watchdog.h"


void bsp_watchdog_init(void)
{

    fwdgt_write_enable();

    while (fwdgt_flag_get(FWDGT_FLAG_PUD) == SET);
    while (fwdgt_flag_get(FWDGT_FLAG_RUD) == SET);

    /*
     * 看门狗超时: 1秒 (625 / 625Hz = 1000ms)
     * 原200ms对软件I2C+多设备(FM17622+FMSE)太短, 初始化和RFID轮询
     * 期间的I2C超时累计容易超过200ms导致反复复位重启。
     * 1秒足够防护程序卡死, 又不会误触发正常初始化。
     * SE认证的I2C轮询循环(fmse_i2c.c)内有喂狗, 不会超时。
     */
    fwdgt_config(625, FWDGT_PSC_DIV64);

    while (fwdgt_flag_get(FWDGT_FLAG_RUD) == SET);
    fwdgt_counter_reload();

    fwdgt_enable();
}

void bsp_watchdog_feed(void)
{
    fwdgt_counter_reload();
}
