#ifndef _BSP_FLASH_H
#define _BSP_FLASH_H

#include "gd32e23x.h"
#include <stdint.h>

/*
 * GD32E230C8T6 FLASH 规格:
 *   - 总容量: 64KB (0x08000000 ~ 0x0800FFFF)
 *   - 页大小: 1KB (1024 字节)
 *   - 共 64 页 (Page 0 ~ Page 63)
 *
 * KEY 存储区域划分:
 *   - 使用最后一页 (Page 63) 存储 KEY 数据
 *   - 起始地址: 0x0800FC00
 *   - 存储内容: 16 字节 KEY + 4 字节 标记(0x4B455900, 即 "KEY\0")
 *   - 总占用: 20 字节 (远小于 1KB 页大小)
 */

/* KEY 存储 FLASH 页地址 (最后一页) */
#define FLASH_KEY_PAGE_ADDR     ((uint32_t)0x0800FC00U)

/* KEY 数据在页内的偏移 (标记之后为 KEY 数据) */
#define FLASH_KEY_MARK_OFFSET   0U
#define FLASH_KEY_DATA_OFFSET   4U

/* KEY 标记值: "KEY\0" = 0x4B455900，用于判断 KEY 是否已写入 */
#define FLASH_KEY_MARK_VALUE    ((uint32_t)0x4B455900U)

/* KEY 长度 */
#define FLASH_KEY_LEN           16U

/* 操作结果定义 */
typedef enum {
    FLASH_OP_OK       = 0,  /* 操作成功 */
    FLASH_OP_ERR      = 1,  /* 操作失败 */
    FLASH_OP_TIMEOUT  = 2,  /* 操作超时 */
    FLASH_OP_VERIFY   = 3   /* 校验失败 */
} flash_op_status;

/**
 * @brief  检查 FLASH 中是否已写入 KEY
 * @retval 1: KEY 已存在  0: KEY 不存在
 */
uint8_t flash_key_is_stored(void);

/**
 * @brief  从 FLASH 读取已存储的 KEY
 * @param  pKeyBuf: KEY 数据输出缓冲区 (至少 16 字节)
 * @retval FLASH_OP_OK: 读取成功  FLASH_OP_ERR: KEY 不存在
 */
flash_op_status flash_key_read(uint8_t *pKeyBuf);

/**
 * @brief  将 KEY 写入 FLASH
 * @param  pKeyData: 待写入的 KEY 数据 (16 字节)
 * @retval FLASH_OP_OK: 写入成功  其他: 写入失败
 * @note   写入前会擦除整页，请确保该页无其他关键数据
 */
flash_op_status flash_key_write(const uint8_t *pKeyData);

/**
 * @brief  擦除 KEY 存储 FLASH 页
 * @retval FLASH_OP_OK: 擦除成功  其他: 擦除失败
 */
flash_op_status flash_key_erase(void);

#endif /* _BSP_FLASH_H */
