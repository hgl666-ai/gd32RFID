#include "app_protocol.h"
#include "bsp_usart.h"
#include "bsp_uid.h"
#include "bsp_flash.h"
#include "bsp_fm17622.h"
#include "bsp_systick.h"
#include "bsp_crypto.h"
#include "bsp_watchdog.h"
#include "debug_config.h"
#include <string.h>

/*
 * 应用层协议处理模块
 *
 * 数据帧协议:
 *   发送帧: HEADER(A55A) + CMD + LENGTH + DATA + CRC16_H + CRC16_L
 *   接收帧: 同上格式，由 protocol.c 状态机解析
 *
 * 命令处理 (CMD 不允许变更):
 *   CMD=0x01, LEN=0x00: 查询UID → 应答: CMD=0x01, LEN=0x08, DATA=8字节UID(异或折叠)
 *   CMD=0x01, LEN=0x08: UID 查询应答 (发送方向)
 *   CMD=0x01, LEN=N:    标签数据上传 (上传方向, N 为密文或明文长度)
 *   CMD=0x02, LEN=0x10: 写入KEY → 应答: CMD=0x02, LEN=0x01, DATA=00/01
 *
 * UID 折叠规则 (协议V1.0-2026.07.18):
 *   12字节芯片UID (3个word) 折叠为 8字节:
 *     - 前4字节: word0 原样保留
 *     - 后4字节: word1 ^ word2 异或折叠
 *
 * 调试输出策略 (DEBUG_ENABLE=1 时):
 *   只打印协议命令的收发过程和CRC校验结果, 帮助定位通信问题。
 *   RFID轮询完全静默, 不刷屏。
 *   DEBUG_ENABLE=0 时所有输出编译为空, 串口只跑协议数据。
 */

/* 发送缓冲区 */
static uint8_t s_tx_buf[MAX_FRAME_LEN];

/* 上次检测到的卡 UID (用于去重) */
static uint8_t  s_last_card_uid[CARD_UID_MAX_LEN];
static uint8_t  s_last_card_uid_len = 0;
static uint8_t  s_card_present = 0;

/* FM17622在线状态, main.c中FM17622_CheckComm后设置 (0=离线则跳过RFID轮询) */
uint8_t g_fm17622_online = 0;

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

    bsp_watchdog_feed();
    /* 加密芯片初始化 (失败则降级透传, 不阻塞, 静默不打印) */
    crypto_chip_init();
    bsp_watchdog_feed();
}


/**
 * @brief  处理 CMD=0x01 (查询主控UID)
 */
static void app_handle_query_uid(void)
{
    uint8_t uid_buf[UID_LEN];
    uid_read(uid_buf);

    uint16_t frame_len = Pack_Data_Frame(APP_CMD_QUERY_UID, uid_buf, UID_LEN, s_tx_buf);
    uart_send_data(s_tx_buf, frame_len);

    /* 打印完整应答帧, 方便对比 */
    DBG_PRINTF("[TX] UID Resp:");
    for (uint8_t i = 0; i < frame_len; i++) {
        DBG_PRINTF(" %02X", s_tx_buf[i]);
    }
    DBG_PRINTF("\r\n");
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
        DBG_PRINTF("[KEY] Write OK\r\n");
    } else {
        DBG_PRINTF("[KEY] Write FAIL err=%d\r\n", status);
    }

    uint16_t frame_len = Pack_Data_Frame(APP_CMD_WRITE_KEY, &result, 1, s_tx_buf);
    uart_send_data(s_tx_buf, frame_len);

    /* 打印完整应答帧, 方便对比 */
    DBG_PRINTF("[TX] KEY Resp:");
    for (uint8_t i = 0; i < frame_len; i++) {
        DBG_PRINTF(" %02X", s_tx_buf[i]);
    }
    DBG_PRINTF("\r\n");
}

/**
 * @brief  上传标签数据 (CMD=0x01, LEN=实际数据长度)
 */
static void app_upload_data(uint8_t cmd, const uint8_t *pData, uint8_t dataLen)
{
    uint16_t frame_len = Pack_Data_Frame(cmd, pData, dataLen, s_tx_buf);
    uart_send_data(s_tx_buf, frame_len);

    DBG_PRINTF("[TX] Tag Upload CMD:%02X LEN:%d\r\n", cmd, dataLen);
}

/**
 * @brief  处理已解析的协议帧
 */
void app_process_frame(const parsed_frame_t *pFrame)
{
    if (pFrame == NULL) return;

    switch (pFrame->cmd) {
    case APP_CMD_QUERY_UID:
        if (pFrame->length == 0x00) {
            DBG_PRINTF("[CMD] QueryUID -> replying\r\n");
            app_handle_query_uid();
        } else {
            DBG_PRINTF("[CMD] CMD=01 but LEN=%d (expected 0 for UID query)\r\n", pFrame->length);
        }
        break;

    case APP_CMD_WRITE_KEY:
        if (pFrame->length == 0x10) {
            DBG_PRINTF("[CMD] WriteKey -> writing FLASH\r\n");
            app_handle_write_key(pFrame->data);
        } else {
            DBG_PRINTF("[CMD] CMD=02 but LEN=%d (expected 16 for KEY write)\r\n", pFrame->length);
        }
        break;

    default:
        DBG_PRINTF("[CMD] Unknown CMD=0x%02X (only 01/02 supported)\r\n", pFrame->cmd);
        break;
    }
}


/**
 * @brief  RFID 标签轮询任务
 * @note   检测流程: RequestA -> Anticoll -> Select -> Auth -> ReadBlocks -> Encrypt -> Upload
 *         调试版本: 完全静默, 不打印任何信息, 避免刷屏干扰协议命令调试
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

    /* FM17622离线时跳过RFID轮询, 避免I2C超时阻塞串口命令响应 */
    if (!g_fm17622_online) return;

    /*  发送 REQA 寻卡指令 */
    if (!FM17622_RequestA(&card_type)) {
        if (s_card_present) {
            s_card_present = 0;
            s_last_card_uid_len = 0;
        }
        return;
    }
    bsp_watchdog_feed();

    /*  防冲突，获取 UID */
    if (!FM17622_Anticoll(card_uid, &uid_len)) {
        return;
    }

    /*  选卡 */
    if (!FM17622_Select(card_uid, uid_len)) {
        return;
    }

    /*卡片去重 */
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

    /*  更新卡片状态 */
    s_card_present = 1;
    s_last_card_uid_len = uid_len;
    memcpy(s_last_card_uid, card_uid, uid_len);

    /*  读取 FLASH 中存储的 KEY */
    uint8_t key_buf[FLASH_KEY_LEN];
    if (!flash_key_is_stored() || flash_key_read(key_buf) != FLASH_OP_OK) {
        return;
    }

    /*  认证 + 读取标签数据 */
    tag_data_t tag_data;
    if (!FM17622_ReadTagData(&tag_data, key_buf, card_uid)) {
        return;
    }
    bsp_watchdog_feed();

    /*  序列化 40 字节 */
    uint8_t tag_raw[TAG_DATA_LEN];
    tag_data_serialize(&tag_data, tag_raw);

    /*  加密 (或透传) */
    uint8_t enc_buf[CRYPTO_CIPHER_MAX_LEN];
    uint8_t enc_len = 0;
    if (!crypto_chip_encrypt(tag_raw, TAG_DATA_LEN, enc_buf, &enc_len)) {
        return;
    }

    /*  上传: CMD=0x01(单天线=左侧), LEN=密文长度 */
    app_upload_data(APP_CMD_UPLOAD_TAG, enc_buf, enc_len);
}


/**
 * @brief  UART 接收处理任务
 * @note   调试版本: 打印收到的完整帧内容和CRC校验结果, 帮助定位通信问题
 */
void app_uart_rx_task(void)
{
    while (uart_rx_available() > 0) {
        uint8_t byte = uart_rx_read_byte();

        parse_result_enum result = protocol_parse_byte(byte);

        if (result == PARSE_RESULT_OK) {
            const parsed_frame_t *pFrame = protocol_get_parsed_frame();

            /* 打印收到的完整帧: CMD + LEN + DATA + CRC */
            DBG_PRINTF("[RX] OK CMD:%02X LEN:%02X", pFrame->cmd, pFrame->length);
            if (pFrame->length > 0) {
                DBG_PRINTF(" DATA:");
                for (uint8_t i = 0; i < pFrame->length; i++) {
                    DBG_PRINTF("%02X", pFrame->data[i]);
                }
            }
            DBG_PRINTF(" CRC:%04X\r\n", pFrame->crc16);

            app_process_frame(pFrame);

        } else if (result == PARSE_RESULT_CRC_ERR) {
            /*
             * CRC校验失败: 打印收到的CRC和正确CRC的对比
             * 这是甲方最可能出错的地方 (CRC算法或字节序不对)
             */
            const parsed_frame_t *pFrame = protocol_get_parsed_frame();

            /* 重新计算正确的CRC */
            static uint8_t crc_buf[MAX_FRAME_LEN];
            uint16_t off = 0;
            crc_buf[off++] = FRAME_HEADER_0;
            crc_buf[off++] = FRAME_HEADER_1;
            crc_buf[off++] = pFrame->cmd;
            crc_buf[off++] = pFrame->length;
            for (uint8_t i = 0; i < pFrame->length; i++) {
                crc_buf[off++] = pFrame->data[i];
            }
            uint16_t calc_crc = Calculate_CRC16_Modbus(crc_buf, off);

            DBG_PRINTF("[RX] CRC ERR! CMD:%02X LEN:%02X RECV_CRC:%04X CALC_CRC:%04X\r\n",
                       pFrame->cmd, pFrame->length, pFrame->crc16, calc_crc);
            DBG_PRINTF("    Correct frame should be: A5 5A %02X %02X",
                       pFrame->cmd, pFrame->length);
            for (uint8_t i = 0; i < pFrame->length; i++) {
                DBG_PRINTF(" %02X", pFrame->data[i]);
            }
            DBG_PRINTF(" %02X %02X\r\n", (uint8_t)(calc_crc >> 8), (uint8_t)(calc_crc & 0xFF));

        } else if (result == PARSE_RESULT_LEN_ERR) {
            DBG_PRINTF("[RX] LEN ERR (LENGTH > 250, frame rejected)\r\n");
        } else if (result == PARSE_RESULT_FRAME_ERR) {
            DBG_PRINTF("[RX] Invalid byte 0x%02X (ignored, waiting for A5 5A)\r\n", byte);
        }
    }
}
