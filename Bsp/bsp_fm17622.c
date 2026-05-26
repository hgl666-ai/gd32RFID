#include "bsp_fm17622.h"
#include "bsp_flash.h"
#include <string.h>

/* ================================================================
 * 内部辅助
 * ================================================================ */

/* 连续通信失败计数 (用于触发 I2C 总线恢复) */
static uint8_t s_i2c_err_count = 0;
#define I2C_ERR_MAX  3U  /* 连续失败 3 次触发总线恢复 */

/* 检查并处理 I2C 错误 */
static void i2c_err_track(uint8_t ack_fail) {
    if (ack_fail) {
        s_i2c_err_count++;
        if (s_i2c_err_count >= I2C_ERR_MAX) {
            i2c_bus_recovery();
            s_i2c_err_count = 0;
        }
    } else {
        s_i2c_err_count = 0;
    }
}

/* 向 FM17622 FIFO 写入数据并执行 Transceive 命令
 * 依赖 FM17622_WriteReg / FM17622_ReadReg (声明于 bsp_fm17622.h)
 * 依赖 FM17622_SetBitMask / FM17622_ClearBitMask (定义于下方) */
static uint8_t fm_transceive(const uint8_t *pTxData, uint8_t txLen,
                             uint8_t *pRxData, uint8_t *pRxLen);

/* 辅助位操作 (必须在 fm_transceive 之前定义, 因为 fm_transceive 调用它们) */
static void FM17622_SetBitMask(uint8_t reg, uint8_t mask) {
    uint8_t tmp = FM17622_ReadReg(reg);
    FM17622_WriteReg(reg, tmp | mask);
}
static void FM17622_ClearBitMask(uint8_t reg, uint8_t mask) {
    uint8_t tmp = FM17622_ReadReg(reg);
    FM17622_WriteReg(reg, tmp & (~mask));
}

static uint8_t fm_transceive(const uint8_t *pTxData, uint8_t txLen,
                             uint8_t *pRxData, uint8_t *pRxLen)
{
    /* 清空 FIFO */
    FM17622_SetBitMask(FM_FIFOLevelReg, 0x80);
    FM17622_ClearBitMask(FM_ComIrqReg, 0x80);

    /* 写入发送数据 */
    for (uint8_t i = 0; i < txLen; i++) {
        FM17622_WriteReg(FM_FIFODataReg, pTxData[i]);
    }

    /* 执行 Transceive 命令 */
    FM17622_WriteReg(FM_CommandReg, 0x0C);
    FM17622_SetBitMask(FM_BitFramingReg, 0x80);

    /* 等待命令完成 */
    uint16_t timeout = 2000;
    while (timeout--) {
        uint8_t irq = FM17622_ReadReg(FM_ComIrqReg);
        if (irq & 0x30) { break; }
    }
    FM17622_ClearBitMask(FM_BitFramingReg, 0x80);

    if (timeout == 0) return 0;

    /* 检查错误 */
    if ((FM17622_ReadReg(FM_ErrorReg) & 0x1B) != 0x00) return 0;

    /* 读取接收数据 */
    if (pRxData != NULL && pRxLen != NULL) {
        *pRxLen = FM17622_ReadReg(FM_FIFOLevelReg);
        for (uint8_t i = 0; i < *pRxLen; i++) {
            pRxData[i] = FM17622_ReadReg(FM_FIFODataReg);
        }
    }
    return 1;
}

/* ================================================================
 * I2C 寄存器读写 (统一使用 bsp_i2c.c 接口)
 * ================================================================ */

void FM17622_WriteReg(uint8_t regAddr, uint8_t data) {
    i2c_start();
    i2c_send_byte(FM17622_I2C_WRITE);
    if (i2c_wait_ack()) { i2c_err_track(1); return; }

    i2c_send_byte(regAddr);
    if (i2c_wait_ack()) { i2c_stop(); i2c_err_track(1); return; }

    i2c_send_byte(data);
    if (i2c_wait_ack()) { i2c_stop(); i2c_err_track(1); return; }

    i2c_stop();
    i2c_err_track(0);
}

uint8_t FM17622_ReadReg(uint8_t regAddr) {
    uint8_t data = 0;

    /* 1. 发送伪写，定位寄存器 */
    i2c_start();
    i2c_send_byte(FM17622_I2C_WRITE);
    if (i2c_wait_ack()) { i2c_err_track(1); return 0; }

    i2c_send_byte(regAddr);
    if (i2c_wait_ack()) { i2c_stop(); i2c_err_track(1); return 0; }

    /* 2. 重新启动，进入读模式 */
    i2c_start();
    i2c_send_byte(FM17622_I2C_READ);
    if (i2c_wait_ack()) { i2c_stop(); i2c_err_track(1); return 0; }

    /* 3. 读取 1 字节 + NACK + STOP */
    data = i2c_read_byte(0);  /* send_ack=0: 最后一个字节发 NACK */
    i2c_stop();

    i2c_err_track(0);
    return data;
}

/* ================================================================
 * 射频与寻卡
 * ================================================================ */

void FM17622_AntennaOn(void) {
    uint8_t temp = FM17622_ReadReg(FM_TxControlReg);
    if ((temp & 0x03) != 0x03) {
        FM17622_SetBitMask(FM_TxControlReg, 0x03);
    }
}

void FM17622_Init(void) {
    /* 软复位命令 */
    FM17622_WriteReg(FM_CommandReg, 0x0F);
    volatile uint32_t delay = 50000; while (delay--);

    /* 配置定时器 (TModeReg + TPrescalerReg)
     * TAuto=1, TPrescaler = 0xD3 -> 定时器频率 = 13.56MHz/(2*211+1) ≈ 32kHz */
    FM17622_WriteReg(FM_TModeReg, 0x8D);
    FM17622_WriteReg(Fm_TPrescalerReg, 0x3E);

    /* 配置定时器重载值 = 0x1E0 = 480, 超时约 15ms */
    FM17622_WriteReg(FM_TReloadRegH, 0x01);
    FM17622_WriteReg(FM_TReloadRegL, 0xE0);

    /* 配置接收增益 (RFCfgReg): 最大增益 48dB */
    FM17622_WriteReg(FM_RFCfgReg, 0x70);

    /* 打开天线 */
    FM17622_AntennaOn();

    /* 复位 I2C 错误计数 */
    s_i2c_err_count = 0;
}

uint8_t FM17622_CheckComm(void) {
    uint8_t version = FM17622_ReadReg(FM_VersionReg);
    if (version != 0x00 && version != 0xFF) {
        return version;
    }
    return 0;
}

uint8_t FM17622_RequestA(uint16_t *cardType) {
    uint8_t status = 0;

    FM17622_SetBitMask(FM_FIFOLevelReg, 0x80);
    FM17622_ClearBitMask(FM_ComIrqReg, 0x80);
    FM17622_WriteReg(FM_BitFramingReg, 0x07);
    FM17622_WriteReg(FM_FIFODataReg, 0x26);
    FM17622_WriteReg(FM_CommandReg, 0x0C);
    FM17622_SetBitMask(FM_BitFramingReg, 0x80);

    uint16_t timeout = 2000;
    while (timeout--) {
        uint8_t irq = FM17622_ReadReg(FM_ComIrqReg);
        if (irq & 0x30) { break; }
    }

    FM17622_ClearBitMask(FM_BitFramingReg, 0x80);

    if (timeout > 0) {
        if ((FM17622_ReadReg(FM_ErrorReg) & 0x1B) == 0x00) {
            uint8_t rxLen = FM17622_ReadReg(FM_FIFOLevelReg);
            if (rxLen == 2) {
                uint8_t byte0 = FM17622_ReadReg(FM_FIFODataReg);
                uint8_t byte1 = FM17622_ReadReg(FM_FIFODataReg);
                *cardType = (uint16_t)((byte0 << 8) | byte1);
                status = 1;
            }
        }
    }
    return status;
}

/* ================================================================
 * 防冲突 (Anticollision) — 支持级联等级 1/2
 * ================================================================ */

uint8_t FM17622_Anticoll(uint8_t *pUid, uint8_t *pUidLen)
{
    uint8_t status = 0;

    if (pUid == NULL || pUidLen == NULL) return 0;

    /* ── 级联等级 1: SEL=0x93 ── */
    FM17622_SetBitMask(FM_FIFOLevelReg, 0x80);
    FM17622_ClearBitMask(FM_CollReg, 0x80);

    FM17622_WriteReg(FM_BitFramingReg, 0x00);
    FM17622_WriteReg(FM_FIFODataReg, 0x93);  /* SEL = 0x93 (级联等级1) */
    FM17622_WriteReg(FM_FIFODataReg, 0x20);  /* NVB = 0x20 (不发送UID位) */
    FM17622_WriteReg(FM_CommandReg, 0x0C);
    FM17622_SetBitMask(FM_BitFramingReg, 0x80);

    uint16_t timeout = 2000;
    while (timeout--) {
        uint8_t irq = FM17622_ReadReg(FM_ComIrqReg);
        if (irq & 0x30) { break; }
    }
    FM17622_ClearBitMask(FM_BitFramingReg, 0x80);

    if (timeout == 0) goto anticoll_exit;
    if ((FM17622_ReadReg(FM_ErrorReg) & 0x1B) != 0x00) goto anticoll_exit;

    {
        uint8_t rxLen = FM17622_ReadReg(FM_FIFOLevelReg);
        if (rxLen != 5) goto anticoll_exit;

        uint8_t cl1_buf[5];
        for (uint8_t i = 0; i < 5; i++) {
            cl1_buf[i] = FM17622_ReadReg(FM_FIFODataReg);
        }
        /* BCC 校验 */
        if ((cl1_buf[0] ^ cl1_buf[1] ^ cl1_buf[2] ^ cl1_buf[3]) != cl1_buf[4])
            goto anticoll_exit;

        if (cl1_buf[0] == 0x88) {
            /* ── 级联标签: CT=0x88, 取 UID[1..3] → 进入 CL2 ── */
            pUid[0] = cl1_buf[1];
            pUid[1] = cl1_buf[2];
            pUid[2] = cl1_buf[3];

            /* 级联等级 2: SEL=0x95 */
            FM17622_SetBitMask(FM_FIFOLevelReg, 0x80);
            FM17622_ClearBitMask(FM_CollReg, 0x80);

            FM17622_WriteReg(FM_BitFramingReg, 0x00);
            FM17622_WriteReg(FM_FIFODataReg, 0x95);  /* SEL = 0x95 */
            FM17622_WriteReg(FM_FIFODataReg, 0x20);
            FM17622_WriteReg(FM_CommandReg, 0x0C);
            FM17622_SetBitMask(FM_BitFramingReg, 0x80);

            timeout = 2000;
            while (timeout--) {
                uint8_t irq = FM17622_ReadReg(FM_ComIrqReg);
                if (irq & 0x30) { break; }
            }
            FM17622_ClearBitMask(FM_BitFramingReg, 0x80);

            if (timeout == 0) goto anticoll_exit;
            if ((FM17622_ReadReg(FM_ErrorReg) & 0x1B) != 0x00) goto anticoll_exit;

            rxLen = FM17622_ReadReg(FM_FIFOLevelReg);
            if (rxLen != 5) goto anticoll_exit;

            uint8_t cl2_buf[5];
            for (uint8_t i = 0; i < 5; i++) {
                cl2_buf[i] = FM17622_ReadReg(FM_FIFODataReg);
            }
            if ((cl2_buf[0] ^ cl2_buf[1] ^ cl2_buf[2] ^ cl2_buf[3]) != cl2_buf[4])
                goto anticoll_exit;

            /* CL2 返回 UID[3..6], 加上 CL1 的 UID[0..2] = 7 字节 */
            pUid[3] = cl2_buf[0];
            pUid[4] = cl2_buf[1];
            pUid[5] = cl2_buf[2];
            pUid[6] = cl2_buf[3];
            *pUidLen = 7;
            status = 1;
        } else {
            /* ── 非级联标签: 直接得到 4 字节 UID ── */
            memcpy(pUid, cl1_buf, 4);
            *pUidLen = 4;
            status = 1;
        }
    }

anticoll_exit:
    FM17622_SetBitMask(FM_CollReg, 0x80);
    return status;
}

/* ================================================================
 * 选卡 (Select)
 * ================================================================ */

uint8_t FM17622_Select(const uint8_t *pUid, uint8_t uidLen)
{
    uint8_t status = 0;

    if (pUid == NULL || uidLen == 0) return 0;

    FM17622_SetBitMask(FM_FIFOLevelReg, 0x80);

    FM17622_WriteReg(FM_BitFramingReg, 0x00);
    FM17622_WriteReg(FM_FIFODataReg, 0x93);  /* SEL */
    FM17622_WriteReg(FM_FIFODataReg, 0x70);  /* NVB = 0x70 (发送完整UID) */

    for (uint8_t i = 0; i < uidLen; i++) {
        FM17622_WriteReg(FM_FIFODataReg, pUid[i]);
    }
    /* 写入 BCC */
    if (uidLen == 4) {
        FM17622_WriteReg(FM_FIFODataReg, pUid[0] ^ pUid[1] ^ pUid[2] ^ pUid[3]);
    }

    FM17622_WriteReg(FM_CommandReg, 0x0C);
    FM17622_SetBitMask(FM_BitFramingReg, 0x80);

    uint16_t timeout = 2000;
    while (timeout--) {
        uint8_t irq = FM17622_ReadReg(FM_ComIrqReg);
        if (irq & 0x30) { break; }
    }
    FM17622_ClearBitMask(FM_BitFramingReg, 0x80);

    if (timeout > 0) {
        if ((FM17622_ReadReg(FM_ErrorReg) & 0x1B) == 0x00) {
            uint8_t rxLen = FM17622_ReadReg(FM_FIFOLevelReg);
            if (rxLen == 1) {
                /* 选卡成功，返回 SAK */
                (void)FM17622_ReadReg(FM_FIFODataReg);
                status = 1;
            }
        }
    }
    return status;
}

/* ================================================================
 * MIFARE Classic 认证 + 块读取
 * ================================================================ */

uint8_t FM17622_MifareAuth(uint8_t authMode, uint8_t blockAddr,
                           const uint8_t *pKey, const uint8_t *pUid)
{
    if (pKey == NULL || pUid == NULL) return 0;

    uint8_t txBuf[12];
    txBuf[0] = authMode;       /* 0x60=KeyA, 0x61=KeyB */
    txBuf[1] = blockAddr;      /* 要认证的块地址 */
    /* 2~7: 6 字节密钥 */
    memcpy(&txBuf[2], pKey, 6);
    /* 8~11: 4 字节卡 UID */
    memcpy(&txBuf[8], pUid, 4);

    return fm_transceive(txBuf, 12, NULL, NULL);
}

uint8_t FM17622_MifareReadBlock(uint8_t blockAddr, uint8_t *pOutBuf)
{
    if (pOutBuf == NULL) return 0;

    uint8_t txBuf[2];
    txBuf[0] = MIFARE_READ;    /* 0x30 */
    txBuf[1] = blockAddr;

    uint8_t rxData[18];
    uint8_t rxLen = 0;

    if (!fm_transceive(txBuf, 2, rxData, &rxLen)) return 0;

    /* MIFARE READ 返回 16 字节数据 + 2 字节 CRC (共 18 字节) */
    if (rxLen != 18) return 0;

    memcpy(pOutBuf, rxData, 16);
    return 1;
}

/* ================================================================
 * 块数据 → tag_data_t 解析
 * ================================================================ */

static void block_to_tag_data(const uint8_t block_buf[48], tag_data_t *pTagData)
{
    uint16_t offset = 0;

    /* 块4: month[1] + day[2] + year[2] + vendor[4] + batch[2] + id[0..4] */
    pTagData->month = block_buf[offset];                    /* Byte 0   */
    pTagData->day[0]   = block_buf[offset + 1];             /* Byte 1   */
    pTagData->day[1]   = block_buf[offset + 2];             /* Byte 2   */
    pTagData->year[0]  = block_buf[offset + 3];             /* Byte 3   */
    pTagData->year[1]  = block_buf[offset + 4];             /* Byte 4   */
    memcpy(pTagData->vendor, &block_buf[offset + 5], 4);    /* Byte 5~8 */
    pTagData->batch[0] = block_buf[offset + 9];             /* Byte 9   */
    pTagData->batch[1] = block_buf[offset + 10];            /* Byte 10  */
    memcpy(pTagData->id,    &block_buf[offset + 11], 5);    /* Byte 11~15 (id前5字节) */

    offset = 16; /* 块5 */
    /* 块5: id[5] + color[7] + length[4] + sn[0..3] */
    pTagData->id[5] = block_buf[offset];                    /* Byte 16 (id第6字节) */
    memcpy(pTagData->color,  &block_buf[offset + 1], 7);    /* Byte 17~23 */
    memcpy(pTagData->length, &block_buf[offset + 8], 4);    /* Byte 24~27 */
    memcpy(pTagData->sn,     &block_buf[offset + 12], 4);   /* Byte 28~31 (sn前4字节) */

    offset = 32; /* 块6 */
    /* 块6: sn[4..5] + res[6] + padding[8] */
    pTagData->sn[4] = block_buf[offset];                    /* Byte 32 */
    pTagData->sn[5] = block_buf[offset + 1];                /* Byte 33 */
    memcpy(pTagData->res, &block_buf[offset + 2], 6);       /* Byte 34~39 */
}

/* ================================================================
 * 读取标签数据 (MIFARE Classic 1K)
 * ================================================================ */

uint8_t FM17622_ReadTagData(tag_data_t *pTagData,
                            const uint8_t *pKey6,
                            const uint8_t *pCardUid)
{
    uint8_t block_buf[48];

    if (pTagData == NULL || pKey6 == NULL || pCardUid == NULL) return 0;

    /* 1. 认证扇区 1 (块 4/5/6 属于扇区 1, 尾块为块 7) */
    if (!FM17622_MifareAuth(MIFARE_AUTHENT_A, TAG_BLOCK_START, pKey6, pCardUid))
        return 0;

    /* 2. 依次读 3 个数据块 (每块 16 字节) */
    for (uint8_t i = 0; i < 3; i++) {
        if (!FM17622_MifareReadBlock(TAG_BLOCK_START + i, &block_buf[i * 16]))
            return 0;
    }

    /* 3. 解析为 tag_data_t */
    block_to_tag_data(block_buf, pTagData);
    return 1;
}

/* ================================================================
 * tag_data_t 序列化为 40 字节
 * ================================================================ */

void tag_data_serialize(const tag_data_t *pTagData, uint8_t *pOutBuf)
{
    if (pTagData == NULL || pOutBuf == NULL) return;

    uint16_t offset = 0;

    /* Byte 0: month */
    pOutBuf[offset++] = pTagData->month;

    /* Byte 1~2: day */
    pOutBuf[offset++] = pTagData->day[0];
    pOutBuf[offset++] = pTagData->day[1];

    /* Byte 3~4: year */
    pOutBuf[offset++] = pTagData->year[0];
    pOutBuf[offset++] = pTagData->year[1];

    /* Byte 5~8: vendor */
    memcpy(&pOutBuf[offset], pTagData->vendor, 4);
    offset += 4;

    /* Byte 9~10: batch */
    pOutBuf[offset++] = pTagData->batch[0];
    pOutBuf[offset++] = pTagData->batch[1];

    /* Byte 11~16: id */
    memcpy(&pOutBuf[offset], pTagData->id, 6);
    offset += 6;

    /* Byte 17~23: color */
    memcpy(&pOutBuf[offset], pTagData->color, 7);
    offset += 7;

    /* Byte 24~27: length */
    memcpy(&pOutBuf[offset], pTagData->length, 4);
    offset += 4;

    /* Byte 28~33: sn */
    memcpy(&pOutBuf[offset], pTagData->sn, 6);
    offset += 6;

    /* Byte 34~39: res */
    memcpy(&pOutBuf[offset], pTagData->res, 6);
    offset += 6;

    (void)offset; /* offset 最终应为 40 */
}
