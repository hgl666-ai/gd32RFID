#ifndef _BSP_UID_H
#define _BSP_UID_H

#include <stdint.h>

/*
 * GD32E230C8T6 芯片 UID (Unique Device ID) 说明:
 *
 *   GD32E230 系列芯片的 UID 存储在系统存储区:
 *     - UID[0]: 0x1FFFF7AC (字0, 32位)
 *     - UID[1]: 0x1FFFF7B0 (字1, 32位)
 *     - UID[2]: 0x1FFFF7B4 (字2, 32位)
 *
 *   共 96 位 (12 字节), 全球唯一
 *
 *   注意: GD32E230 的 UID 地址与 STM32F103 不同!
 *         STM32F103 为 0x1FFFF7E8/EC/F0
 *         GD32E230 为 0x1FFFF7AC/B0/B4
 *         如地址不正确，请查阅 GD32E230 Datasheet 确认
 */

/* UID 寄存器地址定义 */
#define UID_REG_ADDR_0  ((uint32_t)0x1FFFF7ACU)
#define UID_REG_ADDR_1  ((uint32_t)0x1FFFF7B0U)
#define UID_REG_ADDR_2  ((uint32_t)0x1FFFF7B4U)

/* UID 总字节数 */
#define UID_LEN         12U

/**
 * @brief  读取主控芯片 UID (12字节)
 * @param  pUidBuf: UID 输出缓冲区 (至少 12 字节)
 * @note   输出格式: 大端序，即 UID[0] 的高字节在前
 *         pUidBuf[0~3]  = UID_REG_0 (大端序)
 *         pUidBuf[4~7]  = UID_REG_1 (大端序)
 *         pUidBuf[8~11] = UID_REG_2 (大端序)
 */
void uid_read(uint8_t *pUidBuf);

#endif /* _BSP_UID_H */
