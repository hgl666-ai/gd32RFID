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

/* FM17622 SDK tag_demo 的寄存器回读表 */
static const uint8_t s_nfc_reg[8] = {0x07,0x24,0x15,0x27,0x28,0x0C,0x26,0x18};

/* 已确认可用的读卡器地址 (0=尚未找到) */
static uint8_t s_reader_addr = 0;

/* 最近一次 set_tag_reader 返回的 SW (供扫描时做"变化检测/统计", 避免刷屏) */
static uint16_t s_reader_sw = 0xFFFF;

/* 本轮启动是否已做过地址扫描 (只扫一次, 避免反复阻塞主循环) */
static uint8_t s_sweep_done = 0;

/*
 * 读卡通路测试 (2026-09-16 重做版)
 *
 * 背景(重要): 之前那批探针(明文载荷 / CLA 变体 / 故意破坏 session_key)会让 SE 回
 *   STA=0x01 的**帧级错误**, 而 SE 一旦报帧级错误就**作废当前会话**, 之后所有命令
 *   都降级成 0x6985/0x6982 —— 于是产生"读卡族整族一律 0x6985"的假象。
 *   实测证据: 干净会话下 set_tag_reader = 0x9EFA(耗时 159ms); 中间被 STA=0x01 污染后
 *   变 0x6985; **重新认证后又回到 0x9EFA**。9月8日"还没加任何探针"时的原始观测也是 0x9EFA。
 *
 * 本版做法: **每一条命令前都重新认证一次**, 保证每条结果都测自干净会话, 并打印 SW+耗时。
 *   (重新认证会打印 ATR/ONLINE, 属正常现象, 正好证明会话是新的。)
 */
static void se_probe_cmds(void)
{
    uint8_t  rbuf[64];
    uint16_t rlen;
    uint16_t sw;
    uint32_t t0, dt;

    /* ---- 1. 配置读卡器(写) ---- */
    crypto_chip_init();
    bsp_watchdog_feed();
    t0 = g_sys_tick_ms;
    rlen = 0;
    sw = set_tag_reader(P1_RESET, WRITE_NFC_REG, sizeof(s_nfc_param),
                        (uint8_t *)s_nfc_param, 0x28, SOFT_RST, rbuf, &rlen);
    dt = g_sys_tick_ms - t0;
    DBG_PRINTF("[SE] clean1: set_tag_reader(WRITE 0x28) SW=0x%04X dt=%ums\r\n", sw, (unsigned)dt);

    /* ---- 2. 读回读卡器寄存器 ---- */
    crypto_chip_init();
    bsp_watchdog_feed();
    t0 = g_sys_tick_ms;
    rlen = 0;
    sw = set_tag_reader(P1_NO_RESET, READ_NFC_REG, sizeof(s_nfc_reg),
                        (uint8_t *)s_nfc_reg, 0x28, SOFT_RST, rbuf, &rlen);
    dt = g_sys_tick_ms - t0;
    DBG_PRINTF("[SE] clean2: set_tag_reader(READ  0x28) SW=0x%04X dt=%ums len=%u:",
               sw, (unsigned)dt, rlen);
    for (uint16_t i = 0; (i < rlen) && (i < 8U); i++) DBG_PRINTF(" %02X", rbuf[i]);
    DBG_PRINTF("\r\n");

    /* ---- 3. 寻卡 ---- */
    crypto_chip_init();
    bsp_watchdog_feed();
    t0 = g_sys_tick_ms;
    rlen = 0;
    sw = get_tag_uid(Tag_SingleCh, 0x28, rbuf, &rlen);
    dt = g_sys_tick_ms - t0;
    DBG_PRINTF("[SE] clean3: get_tag_uid(0x28) SW=0x%04X dt=%ums len=%u:", sw, (unsigned)dt, rlen);
    for (uint16_t i = 0; (i < rlen) && (i < 11U); i++) DBG_PRINTF(" %02X", rbuf[i]);
    DBG_PRINTF("\r\n");

    /* ---- 4. 标签认证 ---- */
    crypto_chip_init();
    bsp_watchdog_feed();
    t0 = g_sys_tick_ms;
    rlen = 0;
    sw = tag_active(Tag_SigCh_Key, 0x28, rbuf, &rlen);
    dt = g_sys_tick_ms - t0;
    DBG_PRINTF("[SE] clean4: tag_active SW=0x%04X dt=%ums\r\n", sw, (unsigned)dt);

    /* ---- 5. 读标签数据 ---- */
    crypto_chip_init();
    bsp_watchdog_feed();
    t0 = g_sys_tick_ms;
    rlen = 0;
    sw = get_tag_data(0, rbuf, &rlen);
    dt = g_sys_tick_ms - t0;
    DBG_PRINTF("[SE] clean5: get_tag_data SW=0x%04X dt=%ums len=%u\r\n", sw, (unsigned)dt, rlen);

    /* ---- 6. 会话完好性对照: 干净会话下 get_se_uid 已知为 0x8FE2 ----
     * 若这里不是 0x8FE2 ⇒ 说明会话已被前面的命令破坏, 那前面的结果就不可信。 */
    crypto_chip_init();
    bsp_watchdog_feed();
    rlen = 0;
    sw = get_se_uid(0, rbuf, &rlen);
    DBG_PRINTF("[SE] clean6: get_se_uid(会话对照, 期望0x8FE2) SW=0x%04X\r\n", sw);

    /*
     * ---- 7/8. 寿命计数读取 (只读, 安全) ----
     * 这颗 SE 是带"次数管理"的耗材类安全芯片(SDK 有 dec/get_life_count)。
     * 若寿命计数已用尽, 应用层可能整体拒绝读卡操作 —— 这是除"读卡器不应答"之外
     * 另一个能解释"干净会话下 set_tag_reader 仍 0x9EFA"的状态原因, 故单独读出来看看。
     *   INS=0x7B get_tag_life_count (无参数, 期望返回4字节)
     *   INS=0x7A get_se_life_count  (参数 TLV: C0 02 0C 01, 期望返回4字节)
     */
    crypto_chip_init();
    bsp_watchdog_feed();
    rlen = 0;
    sw = get_tag_life_count(rbuf, &rlen);
    DBG_PRINTF("[SE] clean7: get_tag_life_count SW=0x%04X len=%u:", sw, rlen);
    for (uint16_t i = 0; (i < rlen) && (i < 8U); i++) DBG_PRINTF(" %02X", rbuf[i]);
    DBG_PRINTF("\r\n");

    crypto_chip_init();
    bsp_watchdog_feed();
    {
        uint8_t cdat[4] = {0xC0, 0x02, 0x0C, 0x01};   /* SDK se_iic_demo 的原值 */
        rlen = 0;
        sw = get_se_life_count(0, sizeof(cdat), cdat, rbuf, &rlen);
        DBG_PRINTF("[SE] clean8: get_se_life_count SW=0x%04X len=%u:", sw, rlen);
        for (uint16_t i = 0; (i < rlen) && (i < 8U); i++) DBG_PRINTF(" %02X", rbuf[i]);
        DBG_PRINTF("\r\n");
    }

    /*
     * ---- 9. RFID_IRQ(PA3) 上拉探测: 间接判断"读卡器那侧的电源(AVDD)是否在位" ----
     * 网络 RFID_IRQ = FM17622 pin23(IRQ, 开漏输出) + SE pin2(GPIO5), 经 R19(5.1k) 上拉到 AVDD。
     * 做法: 把 MCU 的 PA3 配成"输入 + 内部下拉"再读电平:
     *   读到 1 ⇒ 有外部上拉(4.7k/5.1k 到 AVDD)把线拉高 ⇒ AVDD 大概率在位;
     *   读到 0 ⇒ 没有任何外部上拉 ⇒ 高度怀疑 AVDD/读卡器电源缺失(或该线被短路到地)。
     * 注意: SE 的 GPIO5 也在这根线上, 若它在驱动, 读数会受其影响, 故只作为旁证。
     */
    gpio_mode_set(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN, GPIO_PIN_3);
    delay_ms(10);
    DBG_PRINTF("[SE] clean9: RFID_IRQ(PA3) 内部下拉后读值 = %u (1=有外部上拉/AVDD在位)\r\n",
               (unsigned)gpio_input_bit_get(GPIOA, GPIO_PIN_3));
    gpio_mode_set(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO_PIN_3);   /* 还原为浮空输入 */
}

/*
 * LED闪烁 + 周期性读卡器尝试 (仅诊断模式)
 *
 * 目的: 让"用示波器判断 SE 到底有没有驱动读卡器总线"这件事变得没有歧义 ——
 *   LED(PA5/SYS_LED) 闪 6 下 = 视觉标记, 紧接着立刻发一条 set_tag_reader;
 *   探头放在 RFID_SCL(SE pin6 / FM17622 pin31) 上, LED 闪完后的那一段波形就是它。
 *
 * 判据:
 *   - SCL 出现"成串方波"(3.3V、边沿陡) ⇒ SE 确实在驱动 SCL ⇒ SWD 没占用 GPIO0;
 *   - SCL 一直是平的(只有噪声/一条线), 而 SDA 有方波串 ⇒ **GPIO0 被 SWD 占用**
 *     (技术手册: SWD 启用时 gpio0/gpio1 强制走 SWD 通道, 与本板把读卡器 SCL
 *      接在 gpio0 上的设计冲突) ⇒ 根因即"SE 的 SWD 未关闭"。
 */
#if DEBUG_ENABLE && RFID_DIAG_BLINK_PROBE
/*
 * 诊断专用延时: 自动喂狗 + 维持串口处理。
 *
 * 教训(本项目已因此误判三次): 诊断代码里任何一个**裸 delay_ms(>=1000)**
 * 都会触发 1s 看门狗复位 —— 表现为"串口日志反复从头开始 / 引脚电压跳变 /
 * 示波器波形时有时无", 极易被误判成"SE 在抢控制线"。
 * 因此诊断路径里**禁止使用裸 delay_ms**, 一律走这个函数。
 */
static void dbg_delay_ms(uint32_t ms)
{
    while (ms > 0U) {
        uint32_t step = (ms > 100U) ? 100U : ms;
        delay_ms(step);
        ms -= step;
        bsp_watchdog_feed();
        app_uart_rx_task();
    }
}

/*
 * 采样指定引脚的翻转次数, 持续 window_ms 毫秒(内部每 100ms 喂一次狗)。
 * 相当于固件侧的"小示波器", 用来判断某条线是稳定还是被别的器件驱动到跳变。
 */
static uint32_t dbg_count_edges(uint32_t pin, uint32_t window_ms, uint8_t *last_lvl)
{
    uint32_t edges = 0;
    uint32_t t0    = g_sys_tick_ms;
    uint32_t tf    = t0;
    uint8_t  prev  = (uint8_t)gpio_input_bit_get(GPIOA, pin);

    while ((g_sys_tick_ms - t0) < window_ms) {
        uint8_t now = (uint8_t)gpio_input_bit_get(GPIOA, pin);
        if (now != prev) { edges++; prev = now; }
        if ((g_sys_tick_ms - tf) >= 100U) { tf = g_sys_tick_ms; bsp_watchdog_feed(); }
    }
    if (last_lvl != NULL) *last_lvl = prev;
    return edges;
}

static void se_reader_blink_probe(void)
{
    uint8_t  rb[64];
    uint16_t rl;
    uint16_t sw;
    uint32_t t0;

    /* PA5 = SYS_LED (LED1 + 限流电阻), 配成推挽输出 */
    rcu_periph_clock_enable(RCU_GPIOA);
    gpio_mode_set(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO_PIN_5);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, GPIO_PIN_5);
    gpio_bit_set(GPIOA, GPIO_PIN_5);

    /* PA4 = 读卡器 NPD, 固定推挽输出高, 全程不再改动 */
    gpio_mode_set(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO_PIN_4);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, GPIO_PIN_4);
    gpio_bit_set(GPIOA, GPIO_PIN_4);
    bsp_watchdog_feed();

    /*
    /*
     * 2026-09-18 新实验(基于"SE 自己会初始化读卡器"的推断):
     *   - SE 开机后前 8~10 秒一直拉着读卡器那条线(实测) ⇒ 很可能是 SE 自己在初始化读卡器;
     *   - 此前所有 get_tag_uid/tag_active 探针都在开机后 3~5 秒跑 ⇒ 正好落在它的初始化窗口里。
     * 本版: 开机后先安静等 25 秒(让 SE 完成自己的初始化), 再**不发 set_tag_reader**直接问它卡在哪,
     *       每轮依次问: get_tag_uid → tag_active → get_tag_data → (对照)set_tag_reader。
     */
    DBG_PRINTF("[QUIET] 先安静等待 25 秒, 让 SE 完成它自己的读卡器初始化...\r\n");
    for (uint8_t i = 0; i < 25U; i++) {
        gpio_bit_toggle(GPIOA, GPIO_PIN_5);
        dbg_delay_ms(500);
        gpio_bit_toggle(GPIOA, GPIO_PIN_5);
        dbg_delay_ms(500);
    }
    DBG_PRINTF("[QUIET] 等待结束, 开始不做配置直接问卡\r\n");

    for (;;) {
        uint16_t sw1, sw2, sw3, sw4;
        uint16_t l1, l2, l3, l4;
        uint32_t t1, t2, t3, t4;

        /* ---- 1) 直接寻卡(不发 set_tag_reader) ---- */
        crypto_chip_init();
        bsp_watchdog_feed();
        t1 = g_sys_tick_ms; l1 = 0;
        sw1 = get_tag_uid(Tag_SingleCh, 0x28, rb, &l1);
        t1 = g_sys_tick_ms - t1;
        DBG_PRINTF("[NO-CFG] get_tag_uid(0x28)              SW=0x%04X dt=%ums len=%u:", sw1, (unsigned)t1, l1);
        for (uint16_t k = 0; (k < l1) && (k < 11U); k++) DBG_PRINTF(" %02X", rb[k]);
        DBG_PRINTF("\r\n");
        if (sw1 == 0x9000) DBG_PRINTF("******** 寻卡成功! 请贴回本行! ********\r\n");

        /* ---- 2) 激活卡片 ---- */
        t2 = g_sys_tick_ms; l2 = 0;
        sw2 = tag_active(Tag_SigCh_Key, 0x28, rb, &l2);
        t2 = g_sys_tick_ms - t2;
        DBG_PRINTF("[NO-CFG] tag_active(Tag_SigCh_Key)     SW=0x%04X dt=%ums len=%u\r\n", sw2, (unsigned)t2, l2);
        if (sw2 == 0x9000) DBG_PRINTF("******** 激活成功! 请贴回本行! ********\r\n");

        /* ---- 3) 读标签数据 ---- */
        t3 = g_sys_tick_ms; l3 = 0;
        sw3 = get_tag_data(0, rb, &l3);
        t3 = g_sys_tick_ms - t3;
        DBG_PRINTF("[NO-CFG] get_tag_data                  SW=0x%04X dt=%ums len=%u:", sw3, (unsigned)t3, l3);
        for (uint16_t k = 0; (k < l3) && (k < 12U); k++) DBG_PRINTF(" %02X", rb[k]);
        DBG_PRINTF("\r\n");
        if (sw3 == 0x9000) DBG_PRINTF("******** 读到标签数据! 请贴回本行! ********\r\n");

        /* ---- 4) 对照: 配置读卡器(已证实恒为 0x9EFA) ---- */
        t4 = g_sys_tick_ms; l4 = 0;
        sw4 = set_tag_reader(P1_RESET, WRITE_NFC_REG, sizeof(s_nfc_param),
                             (uint8_t *)s_nfc_param, 0x28, SOFT_RST, rb, &l4);
        t4 = g_sys_tick_ms - t4;
        DBG_PRINTF("[NO-CFG] (对照)set_tag_reader(写 0x28)  SW=0x%04X dt=%ums\r\n\r\n", sw4, (unsigned)t4);

        gpio_bit_toggle(GPIOA, GPIO_PIN_5);
        dbg_delay_ms(2000);
        gpio_bit_toggle(GPIOA, GPIO_PIN_5);
        dbg_delay_ms(500);
    }
}
#endif  /* DEBUG_ENABLE && RFID_DIAG_BLINK_PROBE */

/*
 * 读卡器 I2C 地址候选 (快速路径)
 * 依据: 审核后的 FM17622 原理图 —— EA=1(上拉), ADR0=1(上拉), ADR1=0(下拉), ADR2=0(下拉)
 *       基址 0x28 (0b0101000) + ADR[2:0]=0b001 → 0x29 首选;
 *       其余为邻域候选与SDK默认值, 都找不到时再全范围扫描兜底。
 */
static const uint8_t s_reader_addr_fast[5] = {0x29, 0x2C, 0x28, 0x2F, 0x2E};

/* 尝试用指定地址配置读卡器: 成功则记录地址并做SDK式寄存器回读验证, 返回1
 * verbose: 1=打印本次SW(快速路径用), 0=静默(全范围扫描用, 避免刷屏) */
static uint8_t se_reader_try_addr(uint8_t addr, uint8_t verbose)
{
    uint8_t  rbuf[64];
    uint16_t rlen;
    uint16_t sw;
    uint32_t t0;
    uint32_t dt;

    bsp_watchdog_feed();   /* 单次约130ms, 每次尝试前喂狗(看门狗1s) */

    t0 = g_sys_tick_ms;
    sw = set_tag_reader(P1_RESET, WRITE_NFC_REG,
                        sizeof(s_nfc_param), (uint8_t *)s_nfc_param,
                        addr, SOFT_RST, rbuf, &rlen);
    dt = g_sys_tick_ms - t0;
    s_reader_sw = sw;
    if (verbose) {
        DBG_PRINTF("[RFID] set_tag_reader(0x%02X) SW=0x%04X dt=%ums\r\n", addr, sw, (unsigned)dt);
    }
    if (sw != 0x9000) return 0;

    s_reader_addr = addr;
    DBG_PRINTF("[RFID] reader ACK at 0x%02X (write 0x%02X)\r\n",
               (unsigned)addr, (unsigned)(addr << 1));

    /* SDK tag_demo 的验证步骤: 回读读卡器寄存器 */
    sw = set_tag_reader(P1_NO_RESET, READ_NFC_REG, sizeof(s_nfc_reg),
                        (uint8_t *)s_nfc_reg, addr, SOFT_RST, rbuf, &rlen);
    DBG_PRINTF("[RFID] reg readback SW=0x%04X len=%u:", sw, rlen);
    for (uint16_t i = 0; (i < rlen) && (i < 16U); i++) {
        DBG_PRINTF(" %02X", rbuf[i]);
    }
    DBG_PRINTF("\r\n");
    return 1;
}

/*
 * 通过 SE 配置读卡器: 先按原理图推算的地址快速尝试, 失败再全范围扫描兜底。
 * 每个地址约160ms: 快速路径约1秒, 兜底全扫约18秒 (仅本轮启动做一次)。
 */
static uint8_t se_reader_setup(void)
{
#if DEBUG_ENABLE && RFID_DIAG_BLINK_PROBE
    /* 闪烁/等待诊断模式: 直接进入独立循环, 不返回 (不跑 clean 探针, 保持时序干净) */
    se_reader_blink_probe();
#endif

    /* 一次性探针: 探测 SE 命令可用性 (仅诊断) */
    se_probe_cmds();

    /*
     * 读卡器测试前**重新认证一次**, 保证用的是一条全新的、干净的会话
     * (上面的探针里发过错命令, 不让"会话被搞脏"成为失败的解释)。
     */
    DBG_PRINTF("[RFID] re-auth before reader tests ...\r\n");
    crypto_chip_init();
    bsp_watchdog_feed();

    /* 1) 快速路径: 原理图推算地址优先 (打印SW, 便于观察SE的真实拒绝码) */
    for (uint8_t i = 0; i < sizeof(s_reader_addr_fast); i++) {
        if (se_reader_try_addr(s_reader_addr_fast[i], 1)) return 1;
    }

    /*
     * 注: 原先这里还有两个"我们自己的"反证实验, 结论都已拿到(2026-09-16):
     *   - PA4 切成高阻让 SE 自己驱动该线 → 仍 0x9EFA(与推挽高电平相同);
     *   - PA4 极性反证(拉低)         → 仍 0x9EFA(与拉高相同)。
     *   ⇒ 我们控制的这条使能线**不是**原因, 故从常态流程里移除, 缩短启动。
     */

#if DEBUG_ENABLE && RFID_DIAG_FULL_SWEEP
    /*
     * 2) **干净版**全范围扫描 (0x08~0x77, 约17秒)
     *
     * 为什么要重做: 2026-09-16 上午那次扫描是在"会话被污染"的状态下测的
     * (污染时所有地址都返回 0x6985) ⇒ **那次扫描根本没验证过地址**, 结论无效。
     *
     * 本版规则:
     *   - 干净会话下 set_tag_reader 的正常返回是 0x9EFA;
     *   - 若某地址返回 0x6985/0x6982/0xFFE6(脏状态特征), **重新认证后重测该地址一次**;
     *   - 只打印"非 0x9EFA"的地址(发现读卡器会打 *** reader FOUND ***), 最后给统计。
     */
    DBG_PRINTF("[RFID] CLEAN address sweep 0x08-0x77 ...\r\n");
    {
        uint16_t cnt_all = 0;
        uint16_t cnt_found = 0;
        uint16_t cnt_other = 0;
        uint16_t prev_sw = 0x9EFA;

        for (uint16_t a = 0x08U; a <= 0x77U; a++) {
            uint8_t  rb2[64];
            uint16_t rl2;
            uint16_t sw2;

            bsp_watchdog_feed();
            rl2 = 0;
            sw2 = set_tag_reader(P1_RESET, WRITE_NFC_REG, sizeof(s_nfc_param),
                                 (uint8_t *)s_nfc_param, (uint8_t)a, SOFT_RST, rb2, &rl2);

            /* 脏状态特征 → 重新认证后再测一次同一地址, 避免把污染当成结果 */
            if ((sw2 == 0x6985) || (sw2 == 0x6982) || (sw2 == 0xFFE6)) {
                crypto_chip_init();
                bsp_watchdog_feed();
                rl2 = 0;
                sw2 = set_tag_reader(P1_RESET, WRITE_NFC_REG, sizeof(s_nfc_param),
                                     (uint8_t *)s_nfc_param, (uint8_t)a, SOFT_RST, rb2, &rl2);
            }

            cnt_all++;
            if (sw2 == 0x9000) {
                cnt_found++;
                DBG_PRINTF("[RFID] *** reader FOUND at 0x%02X ***\r\n", (unsigned)a);
            } else if (sw2 != 0x9EFA) {
                cnt_other++;
                DBG_PRINTF("[RFID] scan 0x%02X -> SW=0x%04X (非0x9EFA)\r\n",
                           (unsigned)a, sw2);
                prev_sw = sw2;
            }
        }
        DBG_PRINTF("[RFID] clean sweep done: %u addr, found=%u, 0x9EFA=%u, other=%u (last other=0x%04X)\r\n",
                   cnt_all, cnt_found, (unsigned)(cnt_all - cnt_found - cnt_other), cnt_other, prev_sw);
    }
    DBG_PRINTF("[RFID] reader NOT found on any addr (0x08-0x77)\r\n");
#else
    DBG_PRINTF("[RFID] reader not found (candidates only; full sweep off)\r\n");
#endif
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

    /* 配置读卡器: 本轮启动只做一次地址扫描 (约18秒); 未找到则不再重试, 避免反复阻塞 */
    if (!s_reader_addr) {
        if (s_sweep_done) return;
        s_sweep_done = 1;
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
