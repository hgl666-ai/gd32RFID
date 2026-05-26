#ifndef _BSP_FM17622_H
#define _BSP_FM17622_H

#include "bsp_i2c.h"
#include <stdint.h>

// FM17622 I2C 地址 (0x28左移一位)
#define FM17622_I2C_WRITE   0x50
#define FM17622_I2C_READ    0x51

// 核心寄存器地址
#define FM_CommandReg       0x01
#define FM_ComIrqReg        0x04
#define FM_ErrorReg         0x06
#define FM_FIFODataReg      0x09
#define FM_FIFOLevelReg     0x0A
#define FM_BitFramingReg    0x0D
#define FM_TxControlReg     0x14
#define FM_VersionReg       0x37
#define FM_CollReg          0x0E
#define FM_TModeReg         0x2A
#define Fm_TPrescalerReg    0x2B
#define FM_TReloadRegH      0x2C
#define FM_TReloadRegL      0x2D
#define FM_RFCfgReg         0x26
#define FM_ControlReg       0x0C

// MIFARE 命令码
#define MIFARE_AUTHENT_A    0x60
#define MIFARE_AUTHENT_B    0x61
#define MIFARE_READ         0x30
#define MIFARE_WRITE        0xA0

/* 标签数据在 MIFARE Classic 1K 中的存储布局:
 * 扇区 1 (块 4/5/6), 块 7 为扇区尾块 (KeyA+AccessBits+KeyB)
 * 块4 (16B): month[1] + day[2] + year[2] + vendor[4] + batch[2] + id[0..4]
 * 块5 (16B): id[5] + color[7] + length[4] + sn[0..3]
 * 块6 (16B): sn[4..5] + res[6] + padding[8]
 */
#define TAG_SECTOR_START    1U
#define TAG_BLOCK_START     4U     /* 扇区1 的数据起始块 */
#define TAG_BLOCK_COUNT     3U     /* 占 3 个数据块 (48 字节, 40 有效 + 8 填充) */

/* 标签数据结构 (40字节) */
#define TAG_DATA_LEN        40U

typedef struct {
    uint8_t  month;           /* Byte 0:    月份 */
    uint8_t  day[2];          /* Byte 1~2:  日期 */
    uint8_t  year[2];         /* Byte 3~4:  年份 */
    uint8_t  vendor[4];       /* Byte 5~8:  厂商信息 */
    uint8_t  batch[2];        /* Byte 9~10: 批次号 */
    uint8_t  id[6];           /* Byte 11~16:标签ID */
    uint8_t  color[7];        /* Byte 17~23:颜色信息 */
    uint8_t  length[4];       /* Byte 24~27:长度信息 */
    uint8_t  sn[6];           /* Byte 28~33:序列号 */
    uint8_t  res[6];          /* Byte 34~39:保留字段 */
} tag_data_t;

// 卡片 UID 最大长度
#define CARD_UID_MAX_LEN   10U

// 函数声明
void FM17622_Init(void);
void FM17622_WriteReg(uint8_t regAddr, uint8_t data);
uint8_t FM17622_ReadReg(uint8_t regAddr);
uint8_t FM17622_CheckComm(void);
void FM17622_AntennaOn(void);
uint8_t FM17622_RequestA(uint16_t *cardType);

/* ---- 新增接口 ---- */

/* 防冲突 (Anticollision)，获取卡片序列号 */
uint8_t FM17622_Anticoll(uint8_t *pUid, uint8_t *pUidLen);

/* 选卡 (Select) */
uint8_t FM17622_Select(const uint8_t *pUid, uint8_t uidLen);

/* MIFARE Classic 认证 (KeyA=0x60 / KeyB=0x61) */
uint8_t FM17622_MifareAuth(uint8_t authMode, uint8_t blockAddr,
                           const uint8_t *pKey, const uint8_t *pUid);

/* MIFARE Classic 读块 (16 字节) */
uint8_t FM17622_MifareReadBlock(uint8_t blockAddr, uint8_t *pOutBuf);

/* 读取标签数据 (认证+读块+解析) */
uint8_t FM17622_ReadTagData(tag_data_t *pTagData,
                            const uint8_t *pKey6,
                            const uint8_t *pCardUid);

/* 将 tag_data_t 结构体序列化为 40 字节原始数据 */
void tag_data_serialize(const tag_data_t *pTagData, uint8_t *pOutBuf);

#endif /* _BSP_FM17622_H */
