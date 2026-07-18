#ifndef __APP_PROTOCOL_H
#define __APP_PROTOCOL_H

#include <stdint.h>
#include "protocol.h"
#include "bsp_fm17622.h"


/* 命令码定义 (与上位机协议一致) */
#define APP_CMD_QUERY_UID       0x01U   /* 查询主控UID */
#define APP_CMD_WRITE_KEY       0x02U   /* 写入16字节KEY */

/* KEY 写入结果 */
#define KEY_WRITE_FAIL         0x00U
#define KEY_WRITE_SUCCESS      0x01U


#define APP_CMD_UPLOAD_TAG     0x01U   /* 上传标签数据 (单天线=左侧, 与UID查询共用CMD, 靠LEN区分) */

/* RFID 轮询间隔 (ms) */
#define RFID_POLL_INTERVAL_MS  200U

/* FM17622在线状态: 1=在线, 0=离线(离线时跳过RFID轮询, 避免I2C超时阻塞串口) */
extern uint8_t g_fm17622_online;

/**
 * @brief  应用层协议处理初始化
 * @note   初始化协议解析器、FLASH、FM17622 等
 */
void app_protocol_init(void);

/**
 * @brief  处理已解析的协议帧
 * @param  pFrame: 指向已解析的帧数据
 * @note   根据 CMD 字段分发到对应的处理函数
 */
void app_process_frame(const parsed_frame_t *pFrame);

/**
 * @brief  RFID 标签轮询任务
 * @note   在主循环中周期性调用，检测标签并上传数据
 *         当检测到有效标签时，自动采集数据并通过 UART 上传
 */
void app_rfid_poll_task(void);

/**
 * @brief  UART 接收处理任务
 * @note   在主循环中调用，从环形缓冲区读取数据并推入协议解析器
 *         当解析完成一帧后，调用 app_process_frame 处理
 */
void app_uart_rx_task(void);

#endif
