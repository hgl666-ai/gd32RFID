#ifndef _BSP_CRYPTO_H
#define _BSP_CRYPTO_H

#include <stdint.h>


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

#endif 
