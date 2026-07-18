#include "bsp_uid.h"
#include <string.h>

/**
 * @brief  读取主控芯片 UID (8字节, 异或折叠)
 * @param  pUidBuf: UID 输出缓冲区 (至少 8 字节)
 * @note   协议V1.0-2026.07.18更新: 12字节UID折叠为8字节
 *         - 前4字节: word0 (UID_REG_ADDR_0) 原样保留
 *         - 后4字节: word1 ^ word2 异或折叠, 最大程度保留唯一特征
 *         - 字节序: 与uint32_t内存布局一致 (ARM Cortex-M 小端序, memcpy直拷)
 *
 * 实现思路 (来自甲方):
 *   uint32_t uid_low  = UID_BASE_ADDR[0];
 *   uint32_t uid_mid  = UID_BASE_ADDR[1];
 *   uint32_t uid_high = UID_BASE_ADDR[2];
 *   memcpy(&buf[0], &uid_low, 4);                   // 拷贝低32位
 *   uint32_t folded_high = uid_mid ^ uid_high;      // 中32位^高32位
 *   memcpy(&buf[4], &folded_high, 4);               // 拷贝折叠后的高32位
 */
void uid_read(uint8_t *pUidBuf)
{
    if (pUidBuf == NULL) return;

    /* 使用普通变量接收 volatile 指针的值, 保证真实的硬件读操作并消除编译警告 */
    uint32_t uid_low  = *(volatile uint32_t *)UID_REG_ADDR_0;
    uint32_t uid_mid  = *(volatile uint32_t *)UID_REG_ADDR_1;
    uint32_t uid_high = *(volatile uint32_t *)UID_REG_ADDR_2;

    /* 拷贝低32位 (word0) 原样保留 */
    memcpy(&pUidBuf[0], &uid_low, 4);

    /* 将中32位(word1)和高32位(word2)进行异或折叠, 最大程度保留唯一特征 */
    uint32_t folded_high = uid_mid ^ uid_high;
    memcpy(&pUidBuf[4], &folded_high, 4);
}
