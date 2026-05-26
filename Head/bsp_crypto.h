#ifndef _BSP_CRYPTO_H
#define _BSP_CRYPTO_H

#include <stdint.h>

/*
 * 加密芯片 I2C 通信封装
 *
 * 加密芯片挂在 PB6/PB7 I2C 总线上，与 FM17622(0x28) 共用。
 * 本模块仅负责 I2C 数据传输层，加密算法本身由官方库提供。
 *
 * 待确认: 加密芯片型号和 I2C 地址后填充具体实现。
 */

/* 加密结果最大长度 (预留, 根据实际加密芯片调整) */
#define CRYPTO_CIPHER_MAX_LEN  64U

/**
 * @brief  初始化加密芯片 (上电自检)
 * @retval 1: 在线  0: 离线
 */
uint8_t crypto_chip_init(void);

/**
 * @brief  检测加密芯片是否在线
 * @retval 1: 在线  0: 离线
 */
uint8_t crypto_chip_ping(void);

/**
 * @brief  使用加密芯片加密数据
 * @param  pPlain:     明文数据
 * @param  plain_len:  明文长度
 * @param  pCipher:    密文输出缓冲区
 * @param  pCipher_len: 密文实际长度 (输出)
 * @retval 1: 成功  0: 失败
 * @note   当加密芯片未就绪时，可配置为透传模式 (明文直出)
 */
uint8_t crypto_chip_encrypt(const uint8_t *pPlain,  uint8_t  plain_len,
                            uint8_t       *pCipher, uint8_t *pCipher_len);

#endif /* _BSP_CRYPTO_H */
