#include "bsp_flash.h"
#include <string.h>

/*
 * GD32E230 FLASH 编程要点:
 *   1. 编程前必须调用 fmc_unlock() 解锁
 *   2. 编程完成后必须调用 fmc_lock() 上锁
 *   3. 写入前必须先擦除 (整页擦除)
 *   4. 编程粒度为 4 字节 (32位), 使用 fmc_word_program()
 *   5. 擦除后 FLASH 内容全为 0xFF
 */

/**
 * @brief  检查 FLASH 中是否已写入 KEY
 *         通过读取标记值判断 (0x4B455900 = "KEY\0")
 */
uint8_t flash_key_is_stored(void)
{
    uint32_t mark = *(volatile uint32_t *)(FLASH_KEY_PAGE_ADDR + FLASH_KEY_MARK_OFFSET);
    return (mark == FLASH_KEY_MARK_VALUE) ? 1 : 0;
}

/**
 * @brief  从 FLASH 读取已存储的 KEY
 */
flash_op_status flash_key_read(uint8_t *pKeyBuf)
{
    if (pKeyBuf == NULL) {
        return FLASH_OP_ERR;
    }

    /* 先检查标记，确认 KEY 已写入 */
    if (!flash_key_is_stored()) {
        return FLASH_OP_ERR;
    }

    /* 直接从 FLASH 读取 KEY 数据 (FLASH 可直接寻址读取) */
    volatile uint32_t *pAddr = (volatile uint32_t *)(FLASH_KEY_PAGE_ADDR + FLASH_KEY_DATA_OFFSET);

    /* 按 4 字节 (word) 读取，共 16 字节 = 4 个 word */
    for (uint8_t i = 0; i < FLASH_KEY_LEN / 4; i++) {
        uint32_t word = pAddr[i];
        pKeyBuf[i * 4 + 0] = (uint8_t)(word >> 24);
        pKeyBuf[i * 4 + 1] = (uint8_t)(word >> 16);
        pKeyBuf[i * 4 + 2] = (uint8_t)(word >> 8);
        pKeyBuf[i * 4 + 3] = (uint8_t)(word >> 0);
    }

    return FLASH_OP_OK;
}

/**
 * @brief  将 KEY 写入 FLASH
 * @note   流程: 解锁 -> 擦除页 -> 写标记 -> 写KEY -> 读回校验 -> 上锁
 */
flash_op_status flash_key_write(const uint8_t *pKeyData)
{
    if (pKeyData == NULL) {
        return FLASH_OP_ERR;
    }

    fmc_state_enum fmc_status;

    /* 1. 解锁 FLASH */
    fmc_unlock();

    /* 2. 清除所有 FMC 挂起标志 */
    fmc_flag_clear(FMC_FLAG_END);
    fmc_flag_clear(FMC_FLAG_WPERR);
    fmc_flag_clear(FMC_FLAG_PGERR);

    /* 3. 擦除 KEY 存储页 */
    fmc_status = fmc_page_erase(FLASH_KEY_PAGE_ADDR);
    if (fmc_status != FMC_READY) {
        fmc_lock();
        return FLASH_OP_ERR;
    }

    /* 4. 写入标记值 "KEY\0" (大端格式存储) */
    fmc_status = fmc_word_program(FLASH_KEY_PAGE_ADDR + FLASH_KEY_MARK_OFFSET, FLASH_KEY_MARK_VALUE);
    if (fmc_status != FMC_READY) {
        fmc_lock();
        return FLASH_OP_ERR;
    }

    /* 5. 写入 16 字节 KEY 数据 (按 4 字节 word 编程) */
    for (uint8_t i = 0; i < FLASH_KEY_LEN / 4; i++) {
        /* 将 4 个字节组合为 1 个 32 位 word (大端格式) */
        uint32_t word = ((uint32_t)pKeyData[i * 4 + 0] << 24) |
                        ((uint32_t)pKeyData[i * 4 + 1] << 16) |
                        ((uint32_t)pKeyData[i * 4 + 2] << 8)  |
                        ((uint32_t)pKeyData[i * 4 + 3] << 0);

        fmc_status = fmc_word_program(FLASH_KEY_PAGE_ADDR + FLASH_KEY_DATA_OFFSET + i * 4, word);
        if (fmc_status != FMC_READY) {
            fmc_lock();
            return FLASH_OP_ERR;
        }
    }

    /* 6. 上锁 FLASH */
    fmc_lock();

    /* 7. 读回校验 */
    uint8_t readBack[FLASH_KEY_LEN];
    if (flash_key_read(readBack) != FLASH_OP_OK) {
        return FLASH_OP_VERIFY;
    }
    if (memcmp(pKeyData, readBack, FLASH_KEY_LEN) != 0) {
        return FLASH_OP_VERIFY;
    }

    return FLASH_OP_OK;
}

/**
 * @brief  擦除 KEY 存储 FLASH 页
 */
flash_op_status flash_key_erase(void)
{
    fmc_state_enum fmc_status;

    fmc_unlock();

    fmc_flag_clear(FMC_FLAG_END);
    fmc_flag_clear(FMC_FLAG_WPERR);
    fmc_flag_clear(FMC_FLAG_PGERR);

    fmc_status = fmc_page_erase(FLASH_KEY_PAGE_ADDR);

    fmc_lock();

    return (fmc_status == FMC_READY) ? FLASH_OP_OK : FLASH_OP_ERR;
}
