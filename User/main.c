#include "gd32e23x.h"
#include <stdio.h>
#include "bsp_systick.h"
#include "bsp_usart.h"
#include "bsp_i2c.h"
#include "bsp_flash.h"
#include "bsp_uid.h"
#include "bsp_fm17622.h"
#include "app_protocol.h"

/*
 * 主程序: GD32E230C8T6 + FM17622 RFID 系统
 *
 * 功能:
 *   1. 秘钥Key下载接口 (UID查询 + KEY写入)
 *   2. 标签数据上传接口 (RFID标签识别与数据上报)
 *
 * 主循环架构:
 *   - 非阻塞式轮询，无 delay 死等
 *   - UART 接收任务: 从环形缓冲区读取数据，协议解析
 *   - RFID 轮询任务: 周期性检测标签，自动上传数据
 */

int main(void)
{
    /* 1. 基础硬件初始化 */
    systick_config();       /* SysTick 1ms 中断 */
    bsp_uart_init();        /* USART0 初始化 (115200, 8N1, 中断接收) */
    bsp_i2c_init();         /* I2C 引脚初始化 (PB6/PB7, 软件模拟) */

    printf("\r\n========================================\r\n");
    printf("  GD32E230 + FM17622 RFID System\r\n");
    printf("  Build: %s %s\r\n", __DATE__, __TIME__);
    printf("========================================\r\n");

    /* 2. 读取并打印主控 UID */
    uint8_t uid_buf[UID_LEN];
    uid_read(uid_buf);
    printf("[UID] ");
    for (uint8_t i = 0; i < UID_LEN; i++) {
        printf("%02X ", uid_buf[i]);
    }
    printf("\r\n");

    /* 3. 检查 FLASH 中是否已有 KEY */
    if (flash_key_is_stored()) {
        uint8_t key_buf[FLASH_KEY_LEN];
        flash_key_read(key_buf);
        printf("[KEY] Already stored: ");
        for (uint8_t i = 0; i < FLASH_KEY_LEN; i++) {
            printf("%02X ", key_buf[i]);
        }
        printf("\r\n");
    } else {
        printf("[KEY] Not stored yet, waiting for download...\r\n");
    }

    /* 4. FM17622 RFID 芯片初始化 */
    FM17622_Init();
    uint8_t fm_ver = FM17622_CheckComm();
    if (fm_ver != 0) {
        printf("[FM17622] Version: 0x%02X, Init OK\r\n", fm_ver);
    } else {
        printf("[FM17622] Communication FAILED! Check I2C connection.\r\n");
    }

    /* 5. 应用层协议初始化 */
    app_protocol_init();

    printf("\r\nSystem ready. Waiting for commands...\r\n");

    /* 6. 主循环 */
    while (1) {
        /* 任务1: UART 接收处理 (高优先级，每次循环都执行) */
        app_uart_rx_task();

        /* 任务2: RFID 标签轮询 (周期性执行，约 200ms 一次) */
        app_rfid_poll_task();
    }
}
