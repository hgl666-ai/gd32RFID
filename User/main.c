#include "gd32e23x.h"
#include "debug_config.h"
#include "bsp_systick.h"
#include "bsp_usart.h"
#include "bsp_i2c.h"
#include "bsp_flash.h"
#include "bsp_uid.h"
#include "bsp_fm17622.h"
#include "bsp_watchdog.h"
#include "app_protocol.h"

/*
 * 功能:
 *   1. 秘钥Key下载接口 (UID查询 + KEY写入)  — 协议02
 *   2. 标签数据上传接口 (RFID标签识别与数据上报) — 协议01
 *
 * 调试输出由 debug_config.h 的 DEBUG_ENABLE 控制:
 *   DEBUG_ENABLE=0 (发布): 串口只跑协议数据, 0调试输出
 *   DEBUG_ENABLE=1 (调试): 只打印协议命令收发+CRC校验结果, 不刷屏
 */

int main(void)
{

    systick_config();       /* SysTick 1ms 中断 */
    bsp_uart_init();        /* USART0 初始化 (115200, 8N1, 中断接收) */
    bsp_i2c_init();         /* I2C 引脚初始化 (PB6/PB7, 软件模拟) */

    bsp_watchdog_init();

    /*
     * 初始化期间持续喂狗!
     * 看门狗200ms超时, FM17622_Init + crypto_chip_init 的初始化总耗时
     * 可能超过200ms, 不喂狗会导致MCU反复复位重启(表现为串口刷屏)。
     * 每个耗时步骤后都喂一次, 确保不超时。
     */
    bsp_watchdog_feed();

    /* FM17622 初始化 */
    FM17622_Init();
    bsp_watchdog_feed();

    g_fm17622_online = FM17622_CheckComm();
    bsp_watchdog_feed();

    /* 应用协议初始化 (内部含 FMSE SE 认证, 打印 [SE] 和 [SYS] Ready) */
    app_protocol_init();

    while (1) {
        bsp_watchdog_feed();

        /* UART 接收处理 (高优先级，每次循环都执行) */
        app_uart_rx_task();

        /* RFID 标签轮询 (周期性执行，约 200ms 一次, 完全静默) */
        app_rfid_poll_task();
    }
}
