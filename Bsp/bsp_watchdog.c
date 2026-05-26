#include "bsp_watchdog.h"

/*
 * FWDGT (Free Watchdog Timer) 初始化
 *
 * 使用独立内部 RC 振荡器 IRC40K (~40kHz) 作为时钟源,
 * 即使系统主时钟 (HXTAL/PLL) 故障, 看门狗仍能工作。
 *
 * 配置:
 *   预分频器 = /64  →  40kHz/64 = 625Hz ≈ 1.6ms/tick
 *   重载值   = 125  →  125 × 1.6ms = 200ms 超时
 *   无窗口   (WND = 0xFFF, 允许任意时刻喂狗)
 */
void bsp_watchdog_init(void)
{
    /* 1. 使能 FWDGT 写访问 (必须, 否则无法配置寄存器) */
    fwdgt_write_enable();

    /* 2. 等待 PSC 和 RLD 寄存器可写 (上一次写操作完成) */
    while (fwdgt_flag_get(FWDGT_FLAG_PUD) == SET);
    while (fwdgt_flag_get(FWDGT_FLAG_RUD) == SET);

    /* 3. 配置: 预分频 /64, 重载值 125 → 200ms 超时 */
    fwdgt_config(125, FWDGT_PSC_DIV64);

    /* 4. 等待 RLD 更新完成, 然后重载计数器 (加载初始值) */
    while (fwdgt_flag_get(FWDGT_FLAG_RUD) == SET);
    fwdgt_counter_reload();

    /* 5. 启动看门狗 (此后必须周期性喂狗, 否则复位) */
    fwdgt_enable();
}

/*
 * 喂狗: 重载计数器, 将超时计时器重置为 200ms
 *
 * 必须在主循环中周期性调用, 间隔 < 200ms
 */
void bsp_watchdog_feed(void)
{
    fwdgt_counter_reload();
}
