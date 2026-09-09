#include "debug_config.h"
#include "app_protocol.h"
#include "bsp_usart.h"
#include "bsp_uid.h"
#include "bsp_flash.h"
#include "bsp_fm17622.h"
#include "bsp_systick.h"
#include "bsp_crypto.h"
#include "bsp_watchdog.h"
#include "se_cmd.h"
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

/* 取证用: 每个"有卡周期"内早期阶段(Anticoll/Select)失败最多打印次数, 防止刷屏 */
#define DBG_FAIL_PRINT_MAX  3U

/* 取证用: 连续非法字节(UART噪声)最多打印次数, 收到合法帧后复位, 防止刷屏阻塞收发 */
#define DBG_INVALID_PRINT_MAX  3U

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
    uint8_t se_online = crypto_chip_init();
    (void)se_online;   /* DEBUG_ENABLE=0 时避免 unused 警告 */
    bsp_watchdog_feed();

    /* 启动摘要: 一行式, 不刷屏
     * 示例:
     *   [SYS] Ready | Crypto=PASSTHROUGH | SE=OFFLINE | KEY=EMPTY
     *   [SYS] Ready | Crypto=ENCRYPT     | SE=ONLINE  | KEY=STORED
     */
    DBG_PRINTF("[SYS] Ready | Crypto=%s | SE=%s | KEY=%s\r\n",
               CRYPTO_PASSTHROUGH ? "PASSTHROUGH" : "ENCRYPT",
               se_online ? "ONLINE" : "OFFLINE",
               flash_key_is_stored() ? "STORED" : "EMPTY");
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
 * @note   读卡路径由 debug_config.h 的 RFID_READ_VIA_SE 决定:
 *           =1 SE读卡 (F8P6板): set_tag_reader->get_tag_uid->tag_active->get_tag_data
 *           =0 直连读卡 (旧C8板): RequestA->Anticoll->Select->Auth->ReadBlocks
 *         取证版: 各失败点打印诊断信息; 生产版(DEBUG_ENABLE=0)完全静默
 */
#if RFID_READ_VIA_SE
/* ============================================================
 * SE 读卡模式: FM17622 挂在 FM15L013(SE) 的 I2C 主机口上,
 * 由 SE 完成 配置读卡器/寻卡/认证/读数据 (复旦微SDK原生流程)。
 * ============================================================ */

/* FM17622 寄存器配置参数 (SDK tag_demo 原值) */
static const uint8_t s_nfc_param[15] = {
    0x07,0x24,0x26,0x15,0x40,0x27,0xf0,0x28,0x1f,0x0C,0x10,0x26,0x40,0x18,0x54
};

/* FM17622 I2C 地址候选 (SDK默认0x28; ADR2:0全上拉时可能为0x2E/0x2F, 按序尝试) */
static const uint8_t s_reader_addr_try[3] = {0x28, 0x2F, 0x2E};

/* 已配置成功的读卡器地址 (0=尚未配置成功, 每次SE认证恢复后重新配置) */
static uint8_t s_reader_addr = 0;

/* 读卡器配置重试限频: 每5秒最多尝试一轮, 防止失败时刷屏 */
static uint32_t s_last_setup_ms = 0;

/*
 * 取证探针: 在配置读卡器前依次探测 SE 的命令可用性
 *   1. get_se_uid        — SE 基本可命令性
 *   2. se_active(0x0100) — I2C 通道激活 (SDK I2C demo 原值)
 *   3. write_se_data(KEY)— 把协议02的KEY以TLV格式载入SE
 *      (验证"主板主控通过KEY进行认证即可识别三密标签"的KEY解锁假设)
 */
static void se_probe_cmds(void)
{
    uint8_t  rbuf[64];
    uint16_t rlen;
    uint16_t sw;

    sw = get_se_uid(0, rbuf, &rlen);
    DBG_PRINTF("[SE] probe: get_se_uid SW=0x%04X\r\n", sw);

    sw = se_active(0x0100, rbuf, &rlen);
    DBG_PRINTF("[SE] probe: se_active(0x0100) SW=0x%04X\r\n", sw);

    uint8_t key_buf[FLASH_KEY_LEN];
    if (flash_key_is_stored() && flash_key_read(key_buf) == FLASH_OP_OK) {
        /* TLV: C0 02 00 03 | C1 02 00 00 | C2 10 | KEY[16] = 10+16=26字节 */
        uint8_t tlv[10 + FLASH_KEY_LEN];
        tlv[0] = 0xC0; tlv[1] = 0x02; tlv[2] = 0x00; tlv[3] = 0x03;
        tlv[4] = 0xC1; tlv[5] = 0x02; tlv[6] = 0x00; tlv[7] = 0x00;
        tlv[8] = 0xC2; tlv[9] = FLASH_KEY_LEN;
        memcpy(&tlv[10], key_buf, FLASH_KEY_LEN);
        sw = write_se_data(0x0000, 10 + FLASH_KEY_LEN, tlv, rbuf, &rlen);
        DBG_PRINTF("[SE] probe: write_se_data(KEY) SW=0x%04X\r\n", sw);
    } else {
        DBG_PRINTF("[SE] probe: KEY absent, skip key-load test\r\n");
    }
}

/* 通过 SE 配置读卡器: 先跑探针, 再逐个候选地址尝试, 成功返回 1 */
static uint8_t se_reader_setup(void)
{
    uint8_t  rbuf[64];
    uint16_t rlen;
    uint16_t sw;

    /* 每轮重试前探测 SE 命令可用性 (仅诊断) */
    se_probe_cmds();

    for (uint8_t i = 0; i < sizeof(s_reader_addr_try); i++) {
        uint8_t addr = s_reader_addr_try[i];
        sw = set_tag_reader(P1_RESET, WRITE_NFC_REG,
                            sizeof(s_nfc_param), (uint8_t *)s_nfc_param,
                            addr, SOFT_RST, rbuf, &rlen);
        DBG_PRINTF("[RFID] set_tag_reader(0x%02X) SW=0x%04X\r\n", addr, sw);
        if (sw == 0x9000) {
            s_reader_addr = addr;
            DBG_PRINTF("[RFID] reader configured at 0x%02X\r\n", addr);
            return 1;
        }
    }
    return 0;
}
#endif /* RFID_READ_VIA_SE */

void app_rfid_poll_task(void)
{
    static uint32_t last_poll = 0;
    static uint32_t last_reauth = 0;

    /* 间隔检查: 200ms 轮询一次 */
    if ((g_sys_tick_ms - last_poll) < RFID_POLL_INTERVAL_MS) return;
    last_poll = g_sys_tick_ms;

    /*
     * 生产加密模式: SE 未认证时周期性重试认证 (每 5s 一次)
     * 原实现只在开机认证一次, 失败后永远拒绝上传;
     * SE 上电慢/瞬时错误时系统可自恢复。内部轮询有喂狗。
     */
#if !CRYPTO_PASSTHROUGH
    if (!crypto_chip_ping() && (g_sys_tick_ms - last_reauth) >= 5000U) {
        last_reauth = g_sys_tick_ms;
        DBG_PRINTF("[SE] retry auth...\r\n");
        crypto_chip_init();
    }
#endif

#if RFID_READ_VIA_SE
    /* ============ SE 读卡路径 (F8P6板) ============ */
    uint8_t  se_buf[64];
    uint16_t se_len;
    uint16_t sw;

    /* SE 未认证: 无法驱动读卡器; 认证恢复后重新配置读卡器 */
    if (!crypto_chip_ping()) {
        s_reader_addr = 0;
        return;
    }

    /* 配置读卡器 (每次SE认证后一次; 失败每5秒重试一轮, 防刷屏) */
    if (!s_reader_addr) {
        if (s_last_setup_ms && (g_sys_tick_ms - s_last_setup_ms) < 5000U) return;
        s_last_setup_ms = g_sys_tick_ms;
        if (!se_reader_setup()) return;
        bsp_watchdog_feed();
    }

    /* 寻卡 (SE 驱动读卡器轮询标签) */
    sw = get_tag_uid(Tag_SingleCh, s_reader_addr, se_buf, &se_len);
    if (sw != 0x9000) {
        if (s_card_present) {
            s_card_present = 0;
            s_last_card_uid_len = 0;
            DBG_PRINTF("[RFID] card removed\r\n");
        }
        return;
    }

    /* 响应格式 (SDK): ATQA(2) + UID(7) + SAK(2) */
    if (se_len < 11) {
        DBG_PRINTF("[RFID] get_tag_uid short resp len=%u\r\n", se_len);
        return;
    }

    /* 卡片去重 (7字节UID) */
    if (s_card_present && s_last_card_uid_len == 7 &&
        memcmp(s_last_card_uid, &se_buf[2], 7) == 0) {
        return;
    }
    s_card_present = 1;
    s_last_card_uid_len = 7;
    memcpy(s_last_card_uid, &se_buf[2], 7);
    DBG_PRINTF("[RFID] new card UID:");
    for (uint8_t i = 0; i < 7; i++) DBG_PRINTF(" %02X", se_buf[2 + i]);
    DBG_PRINTF("\r\n");
    bsp_watchdog_feed();

    /* KEY 门控 (与甲方产线顺序一致: 先下发KEY再读卡) */
    uint8_t key_buf[FLASH_KEY_LEN];
    if (!flash_key_is_stored() || flash_key_read(key_buf) != FLASH_OP_OK) {
        DBG_PRINTF("[RFID] KEY not stored, skip (need protocol-02 write)\r\n");
        return;
    }

    /* 标签认证 (SE 内部根密钥, 单天线 Tag_SigCh_Key) */
    sw = tag_active(Tag_SigCh_Key, s_reader_addr, se_buf, &se_len);
    DBG_PRINTF("[RFID] tag_active SW=0x%04X\r\n", sw);
    if (sw != 0x9000) return;
    bsp_watchdog_feed();

    /* 读标签数据 (假设返回40字节业务结构) */
    sw = get_tag_data(0, se_buf, &se_len);
    DBG_PRINTF("[RFID] get_tag_data SW=0x%04X len=%u\r\n", sw, se_len);
    if (sw != 0x9000) return;
    if (se_len < TAG_DATA_LEN) {
        DBG_PRINTF("[RFID] tag data too short: %u < %u\r\n", se_len, TAG_DATA_LEN);
        return;
    }
    bsp_watchdog_feed();

    /*  加密 (或透传) */
    uint8_t enc_buf[CRYPTO_CIPHER_MAX_LEN];
    uint8_t enc_len = 0;
    if (!crypto_chip_encrypt(se_buf, TAG_DATA_LEN, enc_buf, &enc_len)) {
        /* 加密失败: 打印一行告警 (不刷屏, 仅在刷卡瞬间触发一次) */
        DBG_PRINTF("[RFID] encrypt failed, upload skipped (SE=%s)\r\n",
                   crypto_chip_ping() ? "AUTH" : "NOAUTH");
        return;
    }

    /*  上传: CMD=0x01(单天线=左侧), LEN=密文长度 */
    app_upload_data(APP_CMD_UPLOAD_TAG, enc_buf, enc_len);

#else
    /* ============ 直连读卡路径 (旧C8板) ============ */
    static uint8_t  s_dbg_fail_count = 0;   /* 本"有卡周期"内 Anticoll/Select 失败打印计数 */
    uint16_t card_type = 0;
    uint8_t  card_uid[CARD_UID_MAX_LEN];
    uint8_t  uid_len = 0;

    /* FM17622离线时跳过RFID轮询 (仅打印一次, 避免刷屏) */
    if (!g_fm17622_online) {
        static uint8_t s_dbg_fm_offline_printed = 0;
        if (!s_dbg_fm_offline_printed) {
            s_dbg_fm_offline_printed = 1;
            DBG_PRINTF("[RFID] FM17622 OFFLINE, poll skipped\r\n");
        }
        return;
    }

    /*  发送 REQA 寻卡指令 */
    if (!FM17622_RequestA(&card_type)) {
        if (s_card_present) {
            s_card_present = 0;
            s_last_card_uid_len = 0;
            DBG_PRINTF("[RFID] card removed\r\n");
        }
        s_dbg_fail_count = 0;   /* 无卡: 重置失败打印计数 */
        return;
    }
    bsp_watchdog_feed();

    /*  防冲突，获取 UID */
    if (!FM17622_Anticoll(card_uid, &uid_len)) {
        if (s_dbg_fail_count++ < DBG_FAIL_PRINT_MAX)
            DBG_PRINTF("[RFID] Anticoll FAIL\r\n");
        return;
    }

    /*  选卡 */
    if (!FM17622_Select(card_uid, uid_len)) {
        if (s_dbg_fail_count++ < DBG_FAIL_PRINT_MAX)
            DBG_PRINTF("[RFID] Select FAIL\r\n");
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

    /*  更新卡片状态 (新卡: 打印一次 UID, 重置失败计数) */
    s_card_present = 1;
    s_last_card_uid_len = uid_len;
    memcpy(s_last_card_uid, card_uid, uid_len);
    s_dbg_fail_count = 0;
    DBG_PRINTF("[RFID] new card UID(%u):", uid_len);
    for (uint8_t i = 0; i < uid_len; i++) DBG_PRINTF(" %02X", card_uid[i]);
    DBG_PRINTF("\r\n");

    /*  读取 FLASH 中存储的 KEY */
    uint8_t key_buf[FLASH_KEY_LEN];
    if (!flash_key_is_stored() || flash_key_read(key_buf) != FLASH_OP_OK) {
        DBG_PRINTF("[RFID] KEY not stored, skip (need protocol-02 write)\r\n");
        return;
    }

    /*  认证 + 读取标签数据 */
    tag_data_t tag_data;
    if (!FM17622_ReadTagData(&tag_data, key_buf, card_uid)) {
        DBG_PRINTF("[RFID] ReadTagData FAIL (MIFARE auth/read error, KEY mismatch?)\r\n");
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
        /* 加密失败: 打印一行告警 (不刷屏, 仅在刷卡瞬间触发一次) */
        DBG_PRINTF("[RFID] encrypt failed, upload skipped (SE=%s)\r\n",
                   crypto_chip_ping() ? "AUTH" : "NOAUTH");
        return;
    }

    /*  上传: CMD=0x01(单天线=左侧), LEN=密文长度 */
    app_upload_data(APP_CMD_UPLOAD_TAG, enc_buf, enc_len);
#endif /* RFID_READ_VIA_SE */
}


/**
 * @brief  UART 接收处理任务
 * @note   调试版本: 打印收到的完整帧内容和CRC校验结果, 帮助定位通信问题
 */
void app_uart_rx_task(void)
{
    /* 限频计数: 连续非法字节最多打印 DBG_INVALID_PRINT_MAX 条, 收到合法帧后复位。
     * 防止RX线上有噪声/垃圾数据时逐字节打印刷屏, 阻塞命令收发 (历史教训)。 */
    static uint8_t s_dbg_invalid_prints = 0;

    while (uart_rx_available() > 0) {
        uint8_t byte = uart_rx_read_byte();

        parse_result_enum result = protocol_parse_byte(byte);

        if (result == PARSE_RESULT_OK) {
            s_dbg_invalid_prints = 0;   /* 合法帧到达, 复位限频计数 */
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
#if DEBUG_ENABLE
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
#endif /* DEBUG_ENABLE */
        } else if (result == PARSE_RESULT_LEN_ERR) {
            DBG_PRINTF("[RX] LEN ERR (LENGTH > 250, frame rejected)\r\n");
        } else if (result == PARSE_RESULT_FRAME_ERR) {
            /* 限频: 每收到一帧合法帧之前最多打印 3 条非法字节提示, 防刷屏 */
            if (s_dbg_invalid_prints++ < DBG_INVALID_PRINT_MAX)
                DBG_PRINTF("[RX] Invalid byte 0x%02X (ignored, waiting for A5 5A)\r\n", byte);
        }
    }
}
