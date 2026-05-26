#include "bsp_crypto.h"
#include <string.h>

/*
 * 加密芯片通信框架 (I2C 共用 PB6/PB7)
 *
 * 当前为框架代码，待拿到加密芯片型号和 I2C 地址后填充:
 *   1. 定义 I2C 地址宏 (如 CRYPTO_I2C_ADDR)
 *   2. 实现 I2C 读写寄存器
 *   3. 实现加密命令的发送与应答解析
 *
 * 在加密芯片未就绪时，crypto_chip_encrypt() 可配置为透传模式。
 */

/* 透传模式开关: 设为 1 则加密芯片未就绪时明文直出 */
#define CRYPTO_PASSTHROUGH  1U

uint8_t crypto_chip_init(void)
{
    /* TODO: 加密芯片初始化
     * 1. 检查 I2C 通信 (读版本寄存器 / WHO_AM_I 等)
     * 2. 配置芯片工作模式
     * 3. 加载密钥 (如有需要)
     */
    return 0;  /* 加密芯片暂未接入 */
}

uint8_t crypto_chip_ping(void)
{
    /* TODO: 发送探测命令，检查芯片是否应答 */
    return 0;
}

uint8_t crypto_chip_encrypt(const uint8_t *pPlain,  uint8_t  plain_len,
                            uint8_t       *pCipher, uint8_t *pCipher_len)
{
    if (pPlain == NULL || pCipher == NULL || pCipher_len == NULL)
        return 0;

#if CRYPTO_PASSTHROUGH
    /* 透传模式: 加密芯片未就绪时，明文直出 (便于调试) */
    memcpy(pCipher, pPlain, plain_len);
    *pCipher_len = plain_len;
    return 1;
#else
    /* TODO: 调用加密芯片进行实际加密
     * 1. 发送明文到加密芯片
     * 2. 等待芯片处理完成
     * 3. 读取密文
     * 4. 填充 pCipher 和 pCipher_len
     */
    (void)plain_len;
    return 0;
#endif
}
