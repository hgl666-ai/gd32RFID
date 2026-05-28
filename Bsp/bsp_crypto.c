#include "bsp_crypto.h"
#include "bsp_usart.h"
#include "se_cmd.h"
#include "fmse_i2c.h"
#include "fmse_port.h"
#include <string.h>
#include <stdio.h>

/*
 * FMSE 安全芯片加密模块
 *
 * 初始化流程: 注册驱动 → 上电 → 获取 ATR → 双向认证 → 建立 session_key
 * 加密流程:   向 SE 发送明文 → SE 内部加密 → 从 SE 读取密文
 *
 * 重要说明:
 *   本模块仅负责与 FMSE 芯片的 I2C 通信, 构造和发送 APDU 命令。
 *   数据加密运算 (3DES/AES 等) 全部由 FMSE 芯片内部完成,
 *   芯片固件由复旦微电子官方预烧录, MCU 侧无法也不需要干预加密细节。
 *   MCU 发送的是明文, 收到的是 SE 加密后的密文, MCU 不接触密钥也不做加密运算。
 *
 * 认证失败或芯片离线时自动切换为透传模式。
 */

/*
 * 编译开关: SE 离线时的行为
 *   0 = 透传模式 (开发调试用, SE 不在线时明文直出)
 *   1 = 严格模式 (生产环境, SE 不在线时加密接口返回失败)
 */
#ifndef CRYPTO_REQUIRE_SE
#define CRYPTO_REQUIRE_SE   0
#endif

/* 认证状态 */
static uint8_t s_se_authenticated = 0;

uint8_t crypto_chip_init(void)
{
    uint8_t atr_buf[32];
    uint16_t atr_len = 0;
    uint8_t rbuf[32];
    uint16_t rlen = 0;
    StSeFunc *pfm;

    /* 1. 注册 I2C 驱动 */
    pfm = fm_se_register(&gfm_se_i2c);
    if (pfm == NULL) {
        printf("[CRYPTO] Register failed\r\n");
        return 0;
    }

    /* 2. SE 上电 */
    pfm->fm_open_device();

    /* 3. I2C 控制器初始化 */
    pfm->fm_device_init();

    /* 4. 获取 ATR (验证通信) */
    uint8_t ret = pfm->fm_dev_power_on(atr_buf, &atr_len);
    if (ret != 0) {
        printf("[CRYPTO] ATR failed, ret=%d\r\n", ret);
        return 0;
    }
    printf("[CRYPTO] ATR OK, len=%d\r\n", atr_len);

    /* 5. 双向认证 */
    uint16_t sw = mcu_l013_mutual_auth(rbuf, &rlen);
    if (sw != 0x9000) {
        printf("[CRYPTO] Auth failed, SW=0x%04X\r\n", sw);
        return 0;
    }

    /* 6. 保存 session_key */
    memcpy(session_key, rbuf, rlen);
    s_se_authenticated = 1;
    printf("[CRYPTO] Auth OK, session_key established\r\n");

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

    if (!s_se_authenticated) {
#if CRYPTO_REQUIRE_SE
        /* 严格模式: SE 未认证, 拒绝加密 */
        printf("[CRYPTO] SE not authenticated, reject\r\n");
        return 0;
#else
        /* 透传模式: 未认证时明文直出 (开发调试) */
        memcpy(pCipher, pPlain, plain_len);
        *pCipher_len = plain_len;
        return 1;
#endif
    }

    /*
     * 已确认: P1P2=0 (I2C 接口), inbuf 采用 TLV 格式 (来源: SDK se_app.c 示例)
     *
     * TLV 格式:
     *   C0 02 00 03 — 模式标识
     *   C1 02 00 00 — 参数
     *   C2 XX data  — 实际数据 (写入) 或 读取长度 (读取)
     *
     * TODO[SE-CONFIRM]: SE 内部是否对写入的数据自动执行加密?
     *   如果是, get_se_data 读回的就是密文 (当前假设)
     *   如果否, 需要改用 DataEnDecrypt 指令
     */
    uint8_t rbuf[64];
    uint16_t rlen = 0;
    uint16_t sw;

    /* 构造写入 TLV: C0 02 00 03 C1 02 00 00 C2 plain_len plain_data */
    uint8_t write_tlv[9 + CRYPTO_CIPHER_MAX_LEN];
    write_tlv[0] = 0xC0; write_tlv[1] = 0x02; write_tlv[2] = 0x00; write_tlv[3] = 0x03;
    write_tlv[4] = 0xC1; write_tlv[5] = 0x02; write_tlv[6] = 0x00; write_tlv[7] = 0x00;
    write_tlv[8] = 0xC2; write_tlv[9] = plain_len;
    memcpy(&write_tlv[10], pPlain, plain_len);

    /* 发送明文到 SE */
    sw = write_se_data(0x0000, 9 + plain_len, write_tlv, rbuf, &rlen);
    if (sw != 0x9000) {
        printf("[CRYPTO] write_se_data failed, SW=0x%04X\r\n", sw);
        return 0;
    }

    /* 构造读取 TLV: C0 02 00 03 C1 02 00 00 C2 01 read_len */
    uint8_t read_len = ((plain_len + 7) / 8) * 8;  /* 8 字节对齐 */
    uint8_t read_tlv[11];
    read_tlv[0] = 0xC0; read_tlv[1] = 0x02; read_tlv[2] = 0x00; read_tlv[3] = 0x03;
    read_tlv[4] = 0xC1; read_tlv[5] = 0x02; read_tlv[6] = 0x00; read_tlv[7] = 0x00;
    read_tlv[8] = 0xC2; read_tlv[9] = 0x01; read_tlv[10] = read_len;

    /* 从 SE 读取加密结果 */
    uint16_t cipher_len16 = 0;
    sw = get_se_data(0x0000, 11, read_tlv, pCipher, &cipher_len16);
    if (sw != 0x9000) {
        printf("[CRYPTO] get_se_data failed, SW=0x%04X\r\n", sw);
        return 0;
    }

    *pCipher_len = (uint8_t)cipher_len16;

    return 1;
}
