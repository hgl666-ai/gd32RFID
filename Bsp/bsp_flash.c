#include "bsp_flash.h"
#include "bsp_usart.h"
#include <string.h>

/*
 * GD32E230 FLASH:
 *   1. 编程前必须调用 fmc_unlock() 解锁
 *   2. 编程完成后必须调用 fmc_lock() 上锁
 *   3. 写入前必须先擦除 (整页擦除)
 *   4. 编程粒度为 4 字节 (32位), 使用 fmc_word_program()
 *   5. 擦除后 FLASH 内容全为 0xFF
 *
 * FLASH 操作期间 UART 保护:
 *   FLASH 擦写时 CPU 暂停, USART 硬件无法响应, 上位机可能发来数据
 *   导致 overrun 或收到残缺帧。操作前关闭接收中断, 操作后清除错误
 *   标志并清空接收缓冲区, 确保协议状态机不会解析到垃圾帧。
 */

/* FLASH 操作前: 关闭 UART 接收, 防止操作期间收到残缺帧 */
static void flash_uart_protect_enter(void)
{
    usart_interrupt_disable(USART0, USART_INT_RBNE);
}

/* FLASH 操作后: 排空硬件 RDR, 清除错误, 清空缓冲区, 重开接收 */
static void flash_uart_protect_exit(void)
{
    /*
     * 排空 RDR:
     * FLASH 操作期间 CPU 暂停, USART 硬件仍在接收。
     * 第 1 个字节会正常填入 RDR, 第 2 个字节起触发 OVERRUN。
     * 操作完成后 RDR 中可能残留那第 1 个字节, 必须先读走,
     * 否则重新使能中断后 ISR 会立即把它存入环形缓冲区。
     */
    if (usart_flag_get(USART0, USART_FLAG_RBNE) == SET) {
        (void)usart_data_receive(USART0);  /* 读取并丢弃 RDR 残留字节 */
    }

    /* 清除 USART 错误标志 */
    usart_flag_clear(USART0, USART_FLAG_ORERR);
    usart_flag_clear(USART0, USART_FLAG_NERR);
    usart_flag_clear(USART0, USART_FLAG_FERR);
    usart_flag_clear(USART0, USART_FLAG_PERR);

    /* 清空环形缓冲区 (丢弃已存入的垃圾字节) */
    uart_rx_flush();

    /* 重新使能接收中断 (此时 RDR 已空, 不会立即触发 ISR) */
    usart_interrupt_enable(USART0, USART_INT_RBNE);
}

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

    /* 直接从 FLASH 读取 KEY 数据  */
    volatile uint32_t *pAddr = (volatile uint32_t *)(FLASH_KEY_PAGE_ADDR + FLASH_KEY_DATA_OFFSET);

    
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

    /* FLASH 操作期间关闭 UART 接收, 防止收到残缺帧 */
    flash_uart_protect_enter();

    /* 解锁 FLASH */
    fmc_unlock();

    /* 清除所有 FMC 挂起标志 */
    fmc_flag_clear(FMC_FLAG_END);
    fmc_flag_clear(FMC_FLAG_WPERR);
    fmc_flag_clear(FMC_FLAG_PGERR);

    /*  擦除 KEY 存储页 */
    fmc_status = fmc_page_erase(FLASH_KEY_PAGE_ADDR);
    if (fmc_status != FMC_READY) {
        fmc_lock();
        flash_uart_protect_exit();
        return FLASH_OP_ERR;
    }

    /*  写入标记值 "KEY\0" (大端格式存储) */
    fmc_status = fmc_word_program(FLASH_KEY_PAGE_ADDR + FLASH_KEY_MARK_OFFSET, FLASH_KEY_MARK_VALUE);
    if (fmc_status != FMC_READY) {
        fmc_lock();
        flash_uart_protect_exit();
        return FLASH_OP_ERR;
    }

    /*  写入 16 字节 KEY 数据 (按 4 字节 word 编程) */
    for (uint8_t i = 0; i < FLASH_KEY_LEN / 4; i++) {
        /* 将 4 个字节组合为 1 个 32 位 word (大端格式) */
        uint32_t word = ((uint32_t)pKeyData[i * 4 + 0] << 24) |
                        ((uint32_t)pKeyData[i * 4 + 1] << 16) |
                        ((uint32_t)pKeyData[i * 4 + 2] << 8)  |
                        ((uint32_t)pKeyData[i * 4 + 3] << 0);

        fmc_status = fmc_word_program(FLASH_KEY_PAGE_ADDR + FLASH_KEY_DATA_OFFSET + i * 4, word);
        if (fmc_status != FMC_READY) {
            fmc_lock();
            flash_uart_protect_exit();
            return FLASH_OP_ERR;
        }
    }

    /*  上锁 FLASH */
    fmc_lock();

    /*  恢复 UART 接收 */
    flash_uart_protect_exit();

    /* 读回校验 */
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

    flash_uart_protect_enter();

    fmc_unlock();

    fmc_flag_clear(FMC_FLAG_END);
    fmc_flag_clear(FMC_FLAG_WPERR);
    fmc_flag_clear(FMC_FLAG_PGERR);

    fmc_status = fmc_page_erase(FLASH_KEY_PAGE_ADDR);

    fmc_lock();

    flash_uart_protect_exit();

    return (fmc_status == FMC_READY) ? FLASH_OP_OK : FLASH_OP_ERR;
}
