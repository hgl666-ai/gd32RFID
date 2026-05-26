#ifndef _BSP_WATCHDOG_H
#define _BSP_WATCHDOG_H

#include "gd32e23x.h"

/*
 * FWDGT (自由运行看门狗) 配置:
 *   时钟源: IRC40K (40kHz 内部 RC, 独立于系统时钟)
 *   预分频: /64 → 40kHz/64 = 625Hz
 *   重载值: 125  → 125/625Hz = 200ms 超时
 *
 * 喂狗要求: 主循环每轮迭代调用一次 bsp_watchdog_feed()
 *           若主循环卡死超过 200ms, 系统自动复位
 */

void bsp_watchdog_init(void);
void bsp_watchdog_feed(void);

#endif /* _BSP_WATCHDOG_H */
