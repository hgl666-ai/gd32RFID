#include "bsp_fm17622.h"
#include <string.h>

// 局部微秒级延时，匹配 I2C 时序
static void delay_i2c_bus(void) {
    volatile uint32_t i = 200;
    while(i--);
}

// === I2C 寄存器写 ===
void FM17622_WriteReg(uint8_t regAddr, uint8_t data) {
    i2c_start();
    i2c_send_byte(FM17622_I2C_WRITE);
    if(i2c_wait_ack()) return;

    i2c_send_byte(regAddr);
    i2c_wait_ack();

    i2c_send_byte(data);
    i2c_wait_ack();

    i2c_stop();
}

// === I2C 寄存器读 ===
uint8_t FM17622_ReadReg(uint8_t regAddr) {
    uint8_t data = 0;

    // 1. 发送伪写，定位寄存器
    i2c_start();
    i2c_send_byte(FM17622_I2C_WRITE);
    if(i2c_wait_ack()) return 0;
    i2c_send_byte(regAddr);
    i2c_wait_ack();

    // 2. 重新启动，进入读模式
    i2c_start();
    i2c_send_byte(FM17622_I2C_READ);
    i2c_wait_ack();

    // 3. 软件模拟读取 8 bit 数据
    I2C_SDA_H(); // 主机释放数据线
    for(int i = 0; i < 8; i++) {
        data <<= 1;
        I2C_SCL_H();
        delay_i2c_bus();
        if(I2C_SDA_READ()) {
            data |= 0x01;
        }
        I2C_SCL_L();
        delay_i2c_bus();
    }

    // 4. 发送 NACK 并停止
    I2C_SDA_H();
    I2C_SCL_H();
    delay_i2c_bus();
    I2C_SCL_L();

    i2c_stop();
    return data;
}

// 辅助位操作
static void FM17622_SetBitMask(uint8_t reg, uint8_t mask) {
    uint8_t tmp = FM17622_ReadReg(reg);
    FM17622_WriteReg(reg, tmp | mask);
}
static void FM17622_ClearBitMask(uint8_t reg, uint8_t mask) {
    uint8_t tmp = FM17622_ReadReg(reg);
    FM17622_WriteReg(reg, tmp & (~mask));
}

// === 业务逻辑：射频与寻卡 ===
void FM17622_AntennaOn(void) {
    uint8_t temp = FM17622_ReadReg(FM_TxControlReg);
    if ((temp & 0x03) != 0x03) {
        FM17622_SetBitMask(FM_TxControlReg, 0x03);
    }
}

void FM17622_Init(void) {
    // 软复位命令
    FM17622_WriteReg(FM_CommandReg, 0x0F);
    volatile uint32_t delay = 50000; while(delay--);

    // 配置定时器 (TModeReg + TPrescalerReg)
    // TAuto=1, TPrescaler = 0xD3 -> 定时器频率 = 13.56MHz/(2*211+1) ≈ 32kHz
    FM17622_WriteReg(FM_TModeReg, 0x8D);
    FM17622_WriteReg(Fm_TPrescalerReg, 0x3E);

    // 配置定时器重载值 = 0x1E0 = 480, 超时约 15ms
    FM17622_WriteReg(FM_TReloadRegH, 0x01);
    FM17622_WriteReg(FM_TReloadRegL, 0xE0);

    // 配置接收增益 (RFCfgReg): 最大增益 48dB
    FM17622_WriteReg(FM_RFCfgReg, 0x70);

    // 打开天线
    FM17622_AntennaOn();
}

uint8_t FM17622_CheckComm(void) {
    uint8_t version = FM17622_ReadReg(FM_VersionReg);
    if(version != 0x00 && version != 0xFF) {
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

/**
 * @brief  防冲突 (Anticollision)，获取卡片 UID
 * @param  pUid:    UID 输出缓冲区 (至少 CARD_UID_MAX_LEN 字节)
 * @param  pUidLen: UID 实际长度输出 (4 或 7 字节)
 * @retval 1: 成功  0: 失败
 * @note   支持 ISO14443A Type A 标准的防冲突机制
 *         目前实现 4 字节 UID 的防冲突 (MIFARE Classic/UL)
 */
uint8_t FM17622_Anticoll(uint8_t *pUid, uint8_t *pUidLen)
{
    uint8_t status = 0;

    if (pUid == NULL || pUidLen == NULL) return 0;

    FM17622_SetBitMask(FM_FIFOLevelReg, 0x80);  /* 清空 FIFO */
    FM17622_ClearBitMask(FM_CollReg, 0x80);      /* 清除防冲突位 */

    FM17622_WriteReg(FM_BitFramingReg, 0x00);
    FM17622_WriteReg(FM_FIFODataReg, 0x93);  /* SEL = 0x93 (级联等级1) */
    FM17622_WriteReg(FM_FIFODataReg, 0x20);  /* NVB = 0x20 (不发送UID位) */
    FM17622_WriteReg(FM_CommandReg, 0x0C);   /* Transceive 命令 */
    FM17622_SetBitMask(FM_BitFramingReg, 0x80);

    /* 等待命令完成 */
    uint16_t timeout = 2000;
    while (timeout--) {
        uint8_t irq = FM17622_ReadReg(FM_ComIrqReg);
        if (irq & 0x30) { break; }
    }
    FM17622_ClearBitMask(FM_BitFramingReg, 0x80);

    if (timeout > 0) {
        if ((FM17622_ReadReg(FM_ErrorReg) & 0x1B) == 0x00) {
            uint8_t rxLen = FM17622_ReadReg(FM_FIFOLevelReg);
            if (rxLen == 5) {
                /* 4 字节 UID + 1 字节 BCC 校验 */
                for (uint8_t i = 0; i < 5; i++) {
                    pUid[i] = FM17622_ReadReg(FM_FIFODataReg);
                }
                /* BCC 校验: UID[0]^UID[1]^UID[2]^UID[3] == BCC */
                if ((pUid[0] ^ pUid[1] ^ pUid[2] ^ pUid[3]) == pUid[4]) {
                    *pUidLen = 4;
                    status = 1;
                }
            } else if (rxLen > 0) {
                /*
                 * TODO: 支持 7 字节 UID 的级联防冲突
                 * 需要使用 SEL=0x95 (级联等级2) 进行二次防冲突
                 * 当前仅支持 4 字节 UID，7 字节 UID 需补充实现
                 */
            }
        }
    }

    FM17622_SetBitMask(FM_CollReg, 0x80);  /* 恢复防冲突位 */
    return status;
}

/**
 * @brief  选卡 (Select)
 * @param  pUid:   卡片 UID
 * @param  uidLen: UID 长度 (4 或 7)
 * @retval 1: 成功  0: 失败
 */
uint8_t FM17622_Select(const uint8_t *pUid, uint8_t uidLen)
{
    uint8_t status = 0;

    if (pUid == NULL || uidLen == 0) return 0;

    FM17622_SetBitMask(FM_FIFOLevelReg, 0x80);  /* 清空 FIFO */

    FM17622_WriteReg(FM_BitFramingReg, 0x00);
    FM17622_WriteReg(FM_FIFODataReg, 0x93);  /* SEL */
    FM17622_WriteReg(FM_FIFODataReg, 0x70);  /* NVB = 0x70 (发送完整UID) */

    /* 写入 UID 数据 */
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

/**
 * @brief  读取标签数据 (从指定扇区/块读取并解析为 tag_data_t 格式)
 * @param  pTagData: 标签数据输出结构体
 * @retval 1: 成功  0: 失败
 * @note   当前实现为框架代码，标签数据读取的具体实现取决于:
 *         1. 标签类型 (MIFARE Classic / MIFARE Ultralight / NTAG 等)
 *         2. 数据在标签中的存储布局 (扇区/块映射)
 *         3. 是否需要密钥认证
 *
 *         假设标签为 MIFARE Classic 1K:
 *         - 每个扇区 4 块，每块 16 字节
 *         - 40 字节数据分布在 扇区1~扇区3 的数据块中
 *         - 扇区0 块0 为厂商数据 (含UID)
 *
 *         TODO: 待补充 FM17622 芯片的具体驱动实现细节:
 *         - MIFARE 认证流程 (Authentication)
 *         - 块读取命令 (READ)
 *         - 标签数据各字段的编码格式和取值范围
 */
uint8_t FM17622_ReadTagData(tag_data_t *pTagData)
{
    if (pTagData == NULL) return 0;

    /*
     * === 框架实现 ===
     *
     * 完整流程应为:
     * 1. RequestA -> 检测到卡
     * 2. Anticoll -> 获取UID
     * 3. Select   -> 选定卡片
     * 4. Auth     -> 密钥认证 (需 KEY)
     * 5. Read     -> 读取数据块
     * 6. Parse    -> 解析为 tag_data_t 格式
     *
     * 当前仅初始化结构体为 0，返回失败
     * 待补充 FM17622 驱动细节后完善
     */
    memset(pTagData, 0, sizeof(tag_data_t));

    /* TODO: 实现以下步骤 */

    /* Step 1: 密钥认证 (使用已存储的 KEY) */
    /* fmc_status = FM17622_Auth(KEY, block_addr); */

    /* Step 2: 读取数据块 */
    /* fmc_status = FM17622_ReadBlock(block_addr, block_data); */

    /* Step 3: 解析数据到 tag_data_t 结构 */
    /* parse_block_data(block_data, pTagData); */

    return 0;  /* 框架阶段返回失败，待完善 */
}

/**
 * @brief  将 tag_data_t 结构体序列化为 40 字节原始数据
 * @param  pTagData: 标签数据结构体指针
 * @param  pOutBuf:  输出缓冲区 (至少 40 字节)
 */
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

    (void)offset; /* 避免编译器警告, offset 最终应为 40 */
}
