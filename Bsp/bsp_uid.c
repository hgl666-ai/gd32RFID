#include "bsp_uid.h"
#include <stddef.h>

/**
 * @brief  读取主控芯片 UID (12字节)
 * @param  pUidBuf: UID 输出缓冲区 (至少 12 字节)
 * @note   将 3 个 32 位寄存器值按大端序展开为 12 字节
 *         大端序: 高字节存储在低地址
 */
void uid_read(uint8_t *pUidBuf)
{
    if (pUidBuf == NULL) return;

    /* 读取 3 个 32 位 UID 寄存器 */
    uint32_t uid0 = *(volatile uint32_t *)UID_REG_ADDR_0;
    uint32_t uid1 = *(volatile uint32_t *)UID_REG_ADDR_1;
    uint32_t uid2 = *(volatile uint32_t *)UID_REG_ADDR_2;

    /* UID[0]: 大端序展开 (Byte0 = 最高字节) */
    pUidBuf[0]  = (uint8_t)(uid0 >> 24);
    pUidBuf[1]  = (uint8_t)(uid0 >> 16);
    pUidBuf[2]  = (uint8_t)(uid0 >> 8);
    pUidBuf[3]  = (uint8_t)(uid0 >> 0);

    /* UID[1]: 大端序展开 */
    pUidBuf[4]  = (uint8_t)(uid1 >> 24);
    pUidBuf[5]  = (uint8_t)(uid1 >> 16);
    pUidBuf[6]  = (uint8_t)(uid1 >> 8);
    pUidBuf[7]  = (uint8_t)(uid1 >> 0);

    /* UID[2]: 大端序展开 */
    pUidBuf[8]  = (uint8_t)(uid2 >> 24);
    pUidBuf[9]  = (uint8_t)(uid2 >> 16);
    pUidBuf[10] = (uint8_t)(uid2 >> 8);
    pUidBuf[11] = (uint8_t)(uid2 >> 0);
}
