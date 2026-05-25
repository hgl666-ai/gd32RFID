#include "app_protocol.h"
#include "bsp_usart.h"
#include "bsp_uid.h"
#include "bsp_flash.h"
#include "bsp_fm17622.h"
#include "bsp_systick.h"
#include <stdio.h>
#include <string.h>

/*
 * 应用层协议处理模块
 *
 * 数据帧协议:
 *   发送帧: HEADER(A55A) + CMD + LENGTH + DATA + CRC16_H + CRC16_L
 *   接收帧: 同上格式，由 protocol.c 状态机解析
 *
 * 命令处理:
 *   CMD=0x01, LEN=0x00: 查询UID -> 应答: CMD=0x01, LEN=0x0C, DATA=12字节UID
 *   CMD=0x02, LEN=0x10: 写入KEY -> 应答: CMD=0x02, LEN=0x01, DATA=00/01
 *   CMD=0x28:           上传标签 -> 主动上报: CMD=0x01, LEN=0x28, DATA=40字节标签数据
 */

/* 发送缓冲区 */
static uint8_t s_tx_buf[MAX_FRAME_LEN];

/* RFID 轮询计时 */
static uint32_t s_rfid_last_poll = 0;

/* 上次检测到的卡 UID (用于去重，避免同一张卡重复上报) */
static uint8_t  s_last_card_uid[CARD_UID_MAX_LEN];
static uint8_t  s_last_card_uid_len = 0;
static uint8_t  s_card_present = 0;  /* 0: 无卡  1: 有卡 */

/**
 * @brief  应用层协议处理初始化
 */
void app_protocol_init(void)
{
    /* 初始化协议解析状态机 */
    protocol_parser_init();

    /* 初始化 RFID 轮询计时 */
    s_rfid_last_poll = 0;
    s_card_present = 0;
    s_last_card_uid_len = 0;
    memset(s_last_card_uid, 0, sizeof(s_last_card_uid));

    printf("\r\n=== App Protocol Init OK ===\r\n");
}

/**
 * @brief  处理 CMD=0x01 (查询主控UID)
 * @note   接收: A5 5A 01 00 [CRC16]
 *         应答: A5 5A 01 0C [12字节UID] [CRC16]
 */
static void app_handle_query_uid(void)
{
    uint8_t uid_buf[UID_LEN];

    /* 读取主控芯片 UID */
    uid_read(uid_buf);

    /* 打包应答帧: CMD=0x01, LEN=0x0C(12), DATA=12字节UID */
    uint16_t frame_len = Pack_Data_Frame(APP_CMD_QUERY_UID, uid_buf, UID_LEN, s_tx_buf);

    /* 通过 UART 发送 */
    uart_send_data(s_tx_buf, frame_len);

    /* 调试输出 */
    printf("[TX] UID Query Resp: ");
    for (uint8_t i = 0; i < UID_LEN; i++) {
        printf("%02X ", uid_buf[i]);
    }
    printf("\r\n");
}

/**
 * @brief  处理 CMD=0x02 (写入16字节KEY)
 * @param  pKeyData: 16 字节 KEY 数据指针
 * @note   接收: A5 5A 02 10 [16字节KEY] [CRC16]
 *         应答: A5 5A 02 01 [00/01] [CRC16]
 *           00 = 写入失败
 *           01 = 写入成功
 */
static void app_handle_write_key(const uint8_t *pKeyData)
{
    uint8_t result = KEY_WRITE_FAIL;

    /* 将 KEY 写入 FLASH */
    flash_op_status status = flash_key_write(pKeyData);
    if (status == FLASH_OP_OK) {
        result = KEY_WRITE_SUCCESS;
        printf("[KEY] Write SUCCESS\r\n");
    } else {
        printf("[KEY] Write FAILED, err=%d\r\n", status);
    }

    /* 打包应答帧: CMD=0x02, LEN=0x01, DATA=00/01 */
    uint16_t frame_len = Pack_Data_Frame(APP_CMD_WRITE_KEY, &result, 1, s_tx_buf);

    /* 通过 UART 发送 */
    uart_send_data(s_tx_buf, frame_len);
}

/**
 * @brief  上传标签数据
 * @param  pTagData: 标签数据结构体指针
 * @note   上传帧: A5 5A 01 28 [40字节标签数据] [CRC16]
 *         CMD=0x01, LEN=0x28(40)
 */
static void app_upload_tag_data(const tag_data_t *pTagData)
{
    uint8_t tag_raw[TAG_DATA_LEN];

    /* 将标签数据结构体序列化为 40 字节原始数据 */
    tag_data_serialize(pTagData, tag_raw);

    /* 打包上传帧: CMD=0x01, LEN=0x28(40), DATA=40字节标签数据 */
    uint16_t frame_len = Pack_Data_Frame(APP_CMD_QUERY_UID, tag_raw, TAG_DATA_LEN, s_tx_buf);

    /* 通过 UART 发送 */
    uart_send_data(s_tx_buf, frame_len);

    /* 调试输出 */
    printf("[TX] Tag Upload: ");
    for (uint8_t i = 0; i < TAG_DATA_LEN; i++) {
        printf("%02X ", tag_raw[i]);
    }
    printf("\r\n");
}

/**
 * @brief  处理已解析的协议帧
 * @param  pFrame: 指向已解析的帧数据
 */
void app_process_frame(const parsed_frame_t *pFrame)
{
    if (pFrame == NULL) return;

    printf("[RX] CMD=0x%02X, LEN=%d\r\n", pFrame->cmd, pFrame->length);

    switch (pFrame->cmd) {
    /* ---- 查询主控UID ---- */
    case APP_CMD_QUERY_UID:
        /* CMD=0x01, LEN=0x00 表示查询UID指令 */
        if (pFrame->length == 0x00) {
            app_handle_query_uid();
        }
        /* CMD=0x01, LEN=0x28(40) 为标签上传帧 (仅发送方向，不应收到) */
        break;

    /* ---- 写入16字节KEY ---- */
    case APP_CMD_WRITE_KEY:
        /* CMD=0x02, LEN=0x10(16) 表示写入KEY */
        if (pFrame->length == 0x10) {
            app_handle_write_key(pFrame->data);
        } else {
            printf("[ERR] KEY length mismatch: expected 16, got %d\r\n", pFrame->length);
        }
        break;

    default:
        printf("[WARN] Unknown CMD: 0x%02X\r\n", pFrame->cmd);
        break;
    }
}

/**
 * @brief  RFID 标签轮询任务
 * @note   在主循环中周期性调用
 *         检测流程: RequestA -> Anticoll -> Select -> ReadTagData -> Upload
 */
void app_rfid_poll_task(void)
{
    uint16_t card_type = 0;
    uint8_t  card_uid[CARD_UID_MAX_LEN];
    uint8_t  uid_len = 0;

    /* 1. 发送 REQA 寻卡指令 */
    if (!FM17622_RequestA(&card_type)) {
        /* 无卡，清除状态 */
        if (s_card_present) {
            s_card_present = 0;
            s_last_card_uid_len = 0;
            printf("[RFID] Card removed\r\n");
        }
        return;
    }

    /* 2. 防冲突，获取 UID */
    if (!FM17622_Anticoll(card_uid, &uid_len)) {
        printf("[RFID] Anticoll failed\r\n");
        return;
    }

    /* 3. 选卡 */
    if (!FM17622_Select(card_uid, uid_len)) {
        printf("[RFID] Select failed\r\n");
        return;
    }

    /* 4. 卡片去重: 检查是否与上次相同的卡 */
    if (s_card_present && uid_len == s_last_card_uid_len) {
        uint8_t same = 1;
        for (uint8_t i = 0; i < uid_len; i++) {
            if (card_uid[i] != s_last_card_uid[i]) {
                same = 0;
                break;
            }
        }
        if (same) {
            /* 同一张卡，不重复上报 */
            return;
        }
    }

    /* 5. 更新卡片状态 */
    s_card_present = 1;
    s_last_card_uid_len = uid_len;
    memcpy(s_last_card_uid, card_uid, uid_len);

    printf("[RFID] Card detected, UID: ");
    for (uint8_t i = 0; i < uid_len; i++) {
        printf("%02X ", card_uid[i]);
    }
    printf(", Type: 0x%04X\r\n", card_type);

    /* 6. 读取标签数据 */
    tag_data_t tag_data;
    if (FM17622_ReadTagData(&tag_data)) {
        /* 7. 上传标签数据 */
        app_upload_tag_data(&tag_data);
    } else {
        printf("[RFID] Read tag data failed (KEY auth or driver not implemented)\r\n");

        /*
         * TODO: 当 FM17622_ReadTagData 未实现时，可使用以下方式测试上传帧格式:
         * 填充模拟数据到 tag_data 并调用 app_upload_tag_data
         * 取消下方注释即可启用模拟数据上传
         */
#if 0  /* 测试开关: 设为 1 启用模拟数据上传 */
        memset(&tag_data, 0, sizeof(tag_data));
        tag_data.month = 5;
        tag_data.day[0] = 0; tag_data.day[1] = 20;
        tag_data.year[0] = 0x20; tag_data.year[1] = 0x26;
        memcpy(tag_data.vendor, "TEST", 4);
        memcpy(tag_data.id, card_uid, (uid_len > 6) ? 6 : uid_len);
        app_upload_tag_data(&tag_data);
#endif
    }
}

/**
 * @brief  UART 接收处理任务
 * @note   在主循环中调用，从环形缓冲区读取数据并推入协议解析器
 */
void app_uart_rx_task(void)
{
    /* 从环形缓冲区逐字节读取并推入协议解析状态机 */
    while (uart_rx_available() > 0) {
        uint8_t byte = uart_rx_read_byte();

 

        parse_result_enum result = protocol_parse_byte(byte);

        if (result == PARSE_RESULT_OK) {
            /* 一帧解析成功且 CRC 校验通过，处理该帧 */
            const parsed_frame_t *pFrame = protocol_get_parsed_frame();
            app_process_frame(pFrame);
        } else if (result == PARSE_RESULT_CRC_ERR) {
            printf("[ERR] Frame CRC check failed\r\n");
        } else if (result == PARSE_RESULT_LEN_ERR) {
            printf("[ERR] Frame length error\r\n");
        }
        /* PARSE_RESULT_WAITING: 正在接收中，继续 */
    }
}
