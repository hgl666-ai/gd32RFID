#include "bsp_crypto.h"
#include "bsp_usart.h"
#include "se_cmd.h"
#include "fmse_i2c.h"
#include "fmse_port.h"
#include <string.h>
#include "debug_config.h"

/*
 * 加密模块说明
 *
 * 初始化流程 (生产模式):
 *   注册驱动 → 上电 → 获取 ATR → 双向认证 → 建立 session_key
 *
 * 加密流程 (生产模式):
 *   向 SE 发送明文 → SE 内部自动加密 → 从 SE 读取密文
 *
 * 前提假设 (待 SE 侧最终确认, 见 bsp_crypto.h):
 *   1. SE 收到 write_se_data 写入的明文后会自动执行加密
 *   2. 加密后密文长度 == 明文长度 (无 padding/IV 附加)
 *
 * 模式切换 (CRYPTO_PASSTHROUGH):
 *   1 = 透传模式 (调试): 跳过 SE 加密, 明文直出
 *   0 = 加密模式 (生产): 必须通过 SE 认证并加密
 */

/* 认证状态: 1 = SE 已在线并通过双向认证, 0 = 未认证 */
static uint8_t s_se_authenticated = 0;

uint8_t crypto_chip_init(void)
{
    uint8_t atr_buf[32];
    uint16_t atr_len = 0;
    uint8_t rbuf[32];
    uint16_t rlen = 0;
    StSeFunc *pfm;

    /* 透传模式下依然尝试初始化 SE, 仅用于状态探测, 失败不阻塞 */
    pfm = fm_se_register(&gfm_se_i2c);
    if (pfm == NULL) {
        DBG_PRINTF("[SE] OFFLINE (driver register failed)\r\n");
        return 0;
    }

    pfm->fm_open_device();
    pfm->fm_device_init();

    /* 获取 ATR (验证通信, 最长 4s, 内部有喂狗) */
    uint8_t ret = pfm->fm_dev_power_on(atr_buf, &atr_len);
    if (ret != 0) {
        DBG_PRINTF("[SE] OFFLINE (ATR timeout, check wiring/address)\r\n");
        return 0;
    }

    /* 双向认证 */
    uint16_t sw = mcu_l013_mutual_auth(rbuf, &rlen);
    if (sw != 0x9000) {
        DBG_PRINTF("[SE] OFFLINE (auth failed, SW=0x%04X)\r\n", sw);
        return 0;
    }

    /* 保存 session_key */
    if (rlen > 16) rlen = 16;
    memcpy(session_key, rbuf, rlen);
    s_se_authenticated = 1;

    DBG_PRINTF("[SE] ONLINE (auth success, session_key ready)\r\n");
    return 1;
}

uint8_t crypto_chip_ping(void)
{
    return s_se_authenticated;
}

uint8_t crypto_chip_encrypt(const uint8_t *pPlain,  uint8_t  plain_len,
                            uint8_t       *pCipher, uint8_t *pCipher_len)
{
    if (pPlain == NULL || pCipher == NULL || pCipher_len == NULL)
        return 0;

    /*==========================================================
     * 透传模式 (调试): 明文直出, 不调用 SE
     * 刷卡场景不打印 (避免刷屏), 仅启动时打印模式摘要
     *========================================================*/
#if CRYPTO_PASSTHROUGH
    memcpy(pCipher, pPlain, plain_len);
    *pCipher_len = plain_len;
    return 1;
#else

    /*==========================================================
     * 加密模式 (生产): 必须通过 SE 认证才能加密
     * 仅在失败时打印一行原因, 成功不打印 (避免刷卡刷屏)
     *========================================================*/
    if (!s_se_authenticated) {
        DBG_PRINTF("[SE] encrypt FAIL: SE not authenticated\r\n");
        return 0;
    }

    if (plain_len > CRYPTO_CIPHER_MAX_LEN) {
        DBG_PRINTF("[SE] encrypt FAIL: plain too long (%u>%u)\r\n", plain_len, CRYPTO_CIPHER_MAX_LEN);
        return 0;
    }

    /*
     * TLV 格式 (与 SDK 示例代码一致):
     *   C0 02 00 03 — 模式标识
     *   C1 02 00 00 — 参数
     *   C2 XX data  — 实际数据 (写入) 或 读取长度 (读取)
     */
    uint8_t rbuf[64];
    uint16_t rlen = 0;
    uint16_t sw;

    /* 第1步: 构造写入 TLV, 把明文送给 SE */
    uint8_t write_tlv[9 + CRYPTO_CIPHER_MAX_LEN];
    write_tlv[0] = 0xC0; write_tlv[1] = 0x02; write_tlv[2] = 0x00; write_tlv[3] = 0x03;
    write_tlv[4] = 0xC1; write_tlv[5] = 0x02; write_tlv[6] = 0x00; write_tlv[7] = 0x00;
    write_tlv[8] = 0xC2; write_tlv[9] = plain_len;
    memcpy(&write_tlv[10], pPlain, plain_len);

    sw = write_se_data(0x0000, 9 + plain_len, write_tlv, rbuf, &rlen);
    if (sw != 0x9000) {
        DBG_PRINTF("[SE] encrypt FAIL: write_se_data SW=0x%04X\r\n", sw);
        return 0;
    }

    /* 第2步: 构造读取 TLV, 读取相同长度 (假设密文长度 == 明文长度) */
    uint8_t read_tlv[11];
    read_tlv[0] = 0xC0; read_tlv[1] = 0x02; read_tlv[2] = 0x00; read_tlv[3] = 0x03;
    read_tlv[4] = 0xC1; read_tlv[5] = 0x02; read_tlv[6] = 0x00; read_tlv[7] = 0x00;
    read_tlv[8] = 0xC2; read_tlv[9] = 0x01; read_tlv[10] = plain_len;

    uint16_t cipher_len16 = 0;
    sw = get_se_data(0x0000, 11, read_tlv, pCipher, &cipher_len16);
    if (sw != 0x9000) {
        DBG_PRINTF("[SE] encrypt FAIL: get_se_data SW=0x%04X\r\n", sw);
        return 0;
    }

    /* 长度校验: 假设密文长度 == 明文长度 */
    if (cipher_len16 != plain_len) {
        DBG_PRINTF("[SE] encrypt FAIL: cipher len %u != %u\r\n", cipher_len16, plain_len);
        return 0;
    }

    *pCipher_len = (uint8_t)cipher_len16;
    /* 成功不打印, 避免刷卡时刷屏 */
    return 1;
#endif
}
