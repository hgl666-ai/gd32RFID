#ifndef _BSP_UID_H
#define _BSP_UID_H

#include <stdint.h>

/* UID 寄存器地址定义 (GD32E230 共12字节, 3个32位word) */
#define UID_REG_ADDR_0  ((uint32_t)0x1FFFF7ACU)
#define UID_REG_ADDR_1  ((uint32_t)0x1FFFF7B0U)
#define UID_REG_ADDR_2  ((uint32_t)0x1FFFF7B4U)

/* UID 返回字节数 (协议V1.0-2026.07.18更新: 12字节折叠为8字节) */
#define UID_LEN         8U

/**
 * @brief  读取主控芯片 UID (8字节, 异或折叠)
 * @param  pUidBuf: UID 输出缓冲区 (至少 8 字节)
 * @note   芯片UID共12字节(3个word), 协议V1.0-2026.07.18要求折叠为8字节返回
 *
 *         折叠规则 (最大程度保留唯一特征):
 *           - 前4字节: word0 (UID_REG_ADDR_0) 原样保留
 *           - 后4字节: word1 ^ word2 异或折叠
 *
 *         字节序: 与uint32_t内存布局一致 (ARM Cortex-M 小端序, memcpy直拷)
 *           pUidBuf[0..3] = word0 的 4 字节 (内存顺序)
 *           pUidBuf[4..7] = (word1 ^ word2) 的 4 字节 (内存顺序)
 */
void uid_read(uint8_t *pUidBuf);

#endif
