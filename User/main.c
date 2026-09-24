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
     * 复位原因取证 (排查"串口反复刷屏"式重启循环):
     *   FWDGT = 看门狗超时复位 (说明有代码路径超过1s未喂狗)
     *   PIN   = 外部复位引脚;  SW = 软件复位;  POR = 上电复位 (正常)
     * 打印后清标志, 使下一次复位原因可分辨。
     */
    DBG_PRINTF("[SYS] reset cause:%s%s%s%s%s\r\n",
               (rcu_flag_get(RCU_FLAG_FWDGTRST) != RESET) ? " FWDGT" : "",
               (rcu_flag_get(RCU_FLAG_SWRST)    != RESET) ? " SW"    : "",
               (rcu_flag_get(RCU_FLAG_EPRST)    != RESET) ? " PIN"   : "",
               (rcu_flag_get(RCU_FLAG_PORRST)   != RESET) ? " POR"   : "",
               (rcu_flag_get(RCU_FLAG_LPRST)    != RESET) ? " LP"    : "");
    rcu_all_reset_flag_clear();

    /*
     * 初始化期间持续喂狗!
     * 看门狗200ms超时, FM17622_Init + crypto_chip_init 的初始化总耗时
     * 可能超过200ms, 不喂狗会导致MCU反复复位重启(表现为串口刷屏)。
     * 每个耗时步骤后都喂一次, 确保不超时。
     */
    bsp_watchdog_feed();

    /* FM17622 初始化 (含 RFID_NPD/PA4 使能时序) */
    FM17622_Init();
    bsp_watchdog_feed();

    /*
     * 取证: RFID_NPD 双状态诊断 (PA4高/低各做一次总线扫描+版本读取)
     * 仅"MCU直连读卡器"的板子有意义。
     * ★ SE读卡板(F8P6)必须跳过: PA4 同时接着 FM17622 的 NPD,
     *   该诊断会把读卡器按住复位约 0.5 秒, 而 SE 正在运行 ——
     *   已在 2026-09-16 实测该板上读卡器不在 MCU 总线上(全地址仅 SE 0x71 应答),
     *   故毫无收益却引入"读卡器被复位"的干扰, 直接关掉。
     */
#if DEBUG_ENABLE && !RFID_READ_VIA_SE
    FM17622_NpdDiag();
    bsp_watchdog_feed();
#endif

#if !RFID_READ_VIA_SE
    /* 直连模式: 校验FM17622在线 (SE读卡模式下读卡器不在MCU总线上, 跳过) */
    g_fm17622_online = FM17622_CheckComm();
    bsp_watchdog_feed();
#endif

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
