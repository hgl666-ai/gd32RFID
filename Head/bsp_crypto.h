#ifndef _BSP_CRYPTO_H
#define _BSP_CRYPTO_H

#include <stdint.h>


/*
 * 加密模式开关 CRYPTO_PASSTHROUGH 统一定义在 Head/debug_config.h 中
 * (本文件不再自带默认值, 避免宏双重定义冲突)。
 *
 *   CRYPTO_PASSTHROUGH = 1  → 透传调试: 跳过 SE 加密, 明文直出
 *   CRYPTO_PASSTHROUGH = 0  → 生产加密: 必须通过 SE 认证并加密, 失败则返回错误
 *
 * 使用本头文件前请先包含 debug_config.h 获取宏定义。
 *
 * 前提假设 (待 SE 侧最终确认):
 *   1. SE 收到 write_se_data 写入的明文后会自动执行加密
 *   2. 加密后密文长度 == 明文长度 (无 padding / 无 IV 附加)
 *   若上述假设不成立, 需要改用 DataEnDecrypt 指令并调整长度处理
 */

/* 加密结果最大长度 (预留, 根据实际加密芯片调整) */
#define CRYPTO_CIPHER_MAX_LEN  64U

/**
 * @brief  初始化加密芯片 (上电自检)
 * @retval 1: 在线  0: 离线
 * @note   透传模式下也会尝试初始化 SE, 用于状态检测, 但加密不依赖此结果
 */
uint8_t crypto_chip_init(void);

/**
 * @brief  检测加密芯片是否在线 (且已完成双向认证)
 * @retval 1: 在线已认证  0: 离线或未认证
 */
uint8_t crypto_chip_ping(void);

/**
 * @brief  使用加密芯片加密数据
 * @param  pPlain:      明文数据
 * @param  plain_len:   明文长度
 * @param  pCipher:     密文输出缓冲区
 * @param  pCipher_len: 密文实际长度 (输出)
 * @retval 1: 成功  0: 失败
 *
 * @note   行为由 CRYPTO_PASSTHROUGH 宏控制:
 *           1 = 透传模式 (调试): 直接 memcpy 明文, 不调用 SE
 *           0 = 加密模式 (生产): 必须通过 SE 认证, write→get 两步法取密文
 *
 *         假设 (待 SE 侧确认):
 *           - SE 写入明文后自动加密
 *           - 密文长度 == 明文长度
 */
uint8_t crypto_chip_encrypt(const uint8_t *pPlain,  uint8_t  plain_len,
                            uint8_t       *pCipher, uint8_t *pCipher_len);

#endif
