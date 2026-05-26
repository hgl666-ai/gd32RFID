#include "app_protocol.h"
#include "bsp_usart.h"
#include "bsp_uid.h"
#include "bsp_flash.h"
#include "bsp_fm17622.h"
#include "bsp_systick.h"
#include "bsp_crypto.h"
#include <stdio.h>
#include <string.h>

/*
 * 应用层协议处理模块
 *
 * 数据帧协议:
 *   发送帧: HEADER(A55A) + CMD + LENGTH + DATA + CRC16_H + CRC16_L
 *   接收帧: 同上格式，由 protocol.c 状态机解析
 *
 * 命令处理 (CMD 不允许变更):
 *   CMD=0x01, LEN=0x00: 查询UID → 应答: CMD=0x01, LEN=0x0C, DATA=12字节UID
 *   CMD=0x01, LEN=0x0C: UID 查询应答 (发送方向)
 *   CMD=0x01, LEN=N:    标签数据上传 (上传方向, N 为密文或明文长度)
 *   CMD=0x02, LEN=0x10: 写入KEY → 应答: CMD=0x02, LEN=0x01, DATA=00/01
 */

/* 发送缓冲区 */
static uint8_t s_tx_buf[MAX_FRAME_LEN];

/* 上次检测到的卡 UID (用于去重) */
static uint8_t  s_last_card_uid[CARD_UID_MAX_LEN];
static uint8_t  s_last_card_uid_len = 0;
static uint8_t  s_card_present = 0;

/* MIFARE 密钥长度 */
#define MIFARE_KEY_LEN  6U

/**
 * @brief  应用层协议处理初始化
 */
void app_protocol_init(void)
{
    protocol_parser_init();
    s_card_present = 0;
    s_last_card_uid_len = 0;
    memset(s_last_card_uid, 0, sizeof(s_last_card_uid));

    /* 加密芯片初始化 (如未接入则跳过) */
    if (crypto_chip_init()) {
        printf("[CRYPTO] Chip online\r\n");
    } else {
        printf("[CRYPTO] Chip offline, using passthrough mode\r\n");
    }

    printf("=== App Protocol Init OK ===\r\n");
}

/* ================================================================
 * 命令处理
 * ================================================================ */

/**
 * @brief  处理 CMD=0x01 (查询主控UID)
 */
static void app_handle_query_uid(void)
{
    uint8_t uid_buf[UID_LEN];
    uid_read(uid_buf);

    uint16_t frame_len = Pack_Data_Frame(APP_CMD_QUERY_UID, uid_buf, UID_LEN, s_tx_buf);
    uart_send_data(s_tx_buf, frame_len);

    printf("[TX] UID Query Resp: ");
    for (uint8_t i = 0; i < UID_LEN; i++) {
        printf("%02X ", uid_buf[i]);
    }
    printf("\r\n");
}

/**
 * @brief  处理 CMD=0x02 (写入16字节KEY)
 */
static void app_handle_write_key(const uint8_t *pKeyData)
{
    uint8_t result = KEY_WRITE_FAIL;

    flash_op_status status = flash_key_write(pKeyData);
    if (status == FLASH_OP_OK) {
        result = KEY_WRITE_SUCCESS;
        printf("[KEY] Write SUCCESS\r\n");
    } else {
        printf("[KEY] Write FAILED, err=%d\r\n", status);
    }

    uint16_t frame_len = Pack_Data_Frame(APP_CMD_WRITE_KEY, &result, 1, s_tx_buf);
    uart_send_data(s_tx_buf, frame_len);
}

/**
 * @brief  上传标签数据 (CMD=0x01, LEN=实际数据长度)
 */
static void app_upload_data(uint8_t cmd, const uint8_t *pData, uint8_t dataLen)
{
    uint16_t frame_len = Pack_Data_Frame(cmd, pData, dataLen, s_tx_buf);
    uart_send_data(s_tx_buf, frame_len);

    printf("[TX] Upload CMD=0x%02X LEN=%d: ", cmd, dataLen);
    for (uint8_t i = 0; i < dataLen && i < 16; i++) {
        printf("%02X ", pData[i]);
    }
    if (dataLen > 16) printf("...");
    printf("\r\n");
}

/**
 * @brief  处理已解析的协议帧
 */
void app_process_frame(const parsed_frame_t *pFrame)
{
    if (pFrame == NULL) return;

    printf("[RX] CMD=0x%02X, LEN=%d\r\n", pFrame->cmd, pFrame->length);

    switch (pFrame->cmd) {
    case APP_CMD_QUERY_UID:
        if (pFrame->length == 0x00) {
            app_handle_query_uid();
        }
        /* LEN=0x28(40) 为标签上传帧 (仅发送方向, 不应收到) */
        /* LEN=0x0C(12) 为 UID 应答帧 (仅发送方向, 不应收到) */
        break;

    case APP_CMD_WRITE_KEY:
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

/* ================================================================
 * RFID 标签轮询任务
 * ================================================================ */

/**
 * @brief  RFID 标签轮询任务
 * @note   检测流程: RequestA -> Anticoll -> Select -> Auth -> ReadBlocks -> Encrypt -> Upload
 */
void app_rfid_poll_task(void)
{
    static uint32_t last_poll = 0;
    uint16_t card_type = 0;
    uint8_t  card_uid[CARD_UID_MAX_LEN];
    uint8_t  uid_len = 0;

    /* 间隔检查: 200ms 轮询一次 */
    if ((g_sys_tick_ms - last_poll) < RFID_POLL_INTERVAL_MS) return;
    last_poll = g_sys_tick_ms;

    /* 1. 发送 REQA 寻卡指令 */
    if (!FM17622_RequestA(&card_type)) {
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

    /* 4. 卡片去重 */
    if (s_card_present && uid_len == s_last_card_uid_len) {
        uint8_t same = 1;
        for (uint8_t i = 0; i < uid_len; i++) {
            if (card_uid[i] != s_last_card_uid[i]) {
                same = 0;
                break;
            }
        }
        if (same) return;
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

    /* 6. 读取 FLASH 中存储的 KEY */
    uint8_t key_buf[FLASH_KEY_LEN];
    if (!flash_key_is_stored() || flash_key_read(key_buf) != FLASH_OP_OK) {
        printf("[RFID] KEY not stored, skip tag read\r\n");
        return;
    }

    /* 7. 认证 + 读取标签数据 */
    tag_data_t tag_data;
    if (!FM17622_ReadTagData(&tag_data, key_buf, card_uid)) {
        printf("[RFID] Read tag data failed (auth or read error)\r\n");
        return;
    }

    printf("[RFID] Tag data read OK\r\n");

    /* 8. 序列化 40 字节 */
    uint8_t tag_raw[TAG_DATA_LEN];
    tag_data_serialize(&tag_data, tag_raw);

    /* 9. 加密 (或透传) */
    uint8_t enc_buf[CRYPTO_CIPHER_MAX_LEN];
    uint8_t enc_len = 0;
    if (!crypto_chip_encrypt(tag_raw, TAG_DATA_LEN, enc_buf, &enc_len)) {
        printf("[RFID] Encrypt failed\r\n");
        return;
    }

    /* 10. 上传: CMD=0x01, LEN=密文长度 */
    app_upload_data(APP_CMD_QUERY_UID, enc_buf, enc_len);
}

/* ================================================================
 * UART 接收处理任务
 * ================================================================ */

void app_uart_rx_task(void)
{
    while (uart_rx_available() > 0) {
        uint8_t byte = uart_rx_read_byte();

        parse_result_enum result = protocol_parse_byte(byte);

        if (result == PARSE_RESULT_OK) {
            const parsed_frame_t *pFrame = protocol_get_parsed_frame();
            app_process_frame(pFrame);
        } else if (result == PARSE_RESULT_CRC_ERR) {
            printf("[ERR] Frame CRC check failed\r\n");
        } else if (result == PARSE_RESULT_LEN_ERR) {
            printf("[ERR] Frame length error\r\n");
        }
    }
}
