# app_protocol 模块详解 — 项目核心枢纽

> `Bsp/app_protocol.c` + `Head/app_protocol.h`
> 这是整个项目最重要的模块，所有业务逻辑的编排中心

---

## 一、为什么它是核心

```
                         app_protocol.c
                    ┌──────────────────────┐
   上位机 ←─UART─→  │  app_uart_rx_task()  │  ← 接收命令, 解析帧
                    │  app_process_frame()  │  ← 根据 CMD 分发处理
                    │                      │
                    │  app_rfid_poll_task() │  ← 周期性轮询 RFID
                    │                      │
                    │       ↓ 依赖 ↓        │
                    │  protocol.c   (帧协议) │
                    │  bsp_fm17622  (RFID)  │
                    │  bsp_flash    (KEY)   │
                    │  bsp_crypto   (加密)  │
                    │  bsp_usart    (发送)  │
                    └──────────────────────┘
```

app_protocol 是唯一一个与**所有其他模块**都打交道的文件：

| 依赖模块 | 调用方式 |
|----------|----------|
| `protocol.c` | `protocol_parser_init()`, `protocol_parse_byte()`, `protocol_get_parsed_frame()`, `Pack_Data_Frame()` |
| `bsp_fm17622.c` | `FM17622_RequestA()`, `_Anticoll()`, `_Select()`, `_ReadTagData()`, `tag_data_serialize()` |
| `bsp_flash.c` | `flash_key_is_stored()`, `flash_key_read()` |
| `bsp_crypto.c` | `crypto_chip_init()`, `crypto_chip_encrypt()` |
| `bsp_usart.c` | `uart_rx_available()`, `uart_rx_read_byte()`, `uart_send_data()` |
| `bsp_uid.c` | `uid_read()` |
| `bsp_systick.c` | `g_sys_tick_ms` (全局计数器) |

主循环 `while(1)` 中只调用了两个函数，都在 app_protocol.c 中：`app_uart_rx_task()` 和 `app_rfid_poll_task()`。

---

## 二、文件结构速览

```
app_protocol.h   →  常量定义 + 4 个公开函数声明
app_protocol.c   →  3 个静态变量
                    1 个公开函数: app_protocol_init()
                    3 个静态函数: app_handle_query_uid / app_handle_write_key / app_upload_data
                    1 个公开函数: app_process_frame()
                    1 个公开函数: app_rfid_poll_task()
                    1 个公开函数: app_uart_rx_task()
```

---

## 三、静态变量 — 模块的私有状态

```c
/* 发送缓冲区 (256 字节, MAX_FRAME_LEN) */
static uint8_t s_tx_buf[MAX_FRAME_LEN];
```
所有发送帧都通过这个缓冲区组装。`static` 意味着它不占用栈（与 1KB 总栈的约束吻合），且整个模块共享。

```c
/* 卡片去重状态 */
static uint8_t  s_last_card_uid[CARD_UID_MAX_LEN]; // 上次检测到的卡 UID
static uint8_t  s_last_card_uid_len = 0;           // 上次 UID 长度 (4/7 字节)
static uint8_t  s_card_present = 0;                // 0=无卡  1=有卡
```
防止同一张卡反复触发上传。当卡片离开后 (`RequestA` 失败) 会清零这三个变量。

---

## 四、函数逐一解读

### 4.1 `app_protocol_init()` — 初始化

```c
void app_protocol_init(void)
{
    protocol_parser_init();    // ① 重置协议解析状态机到 WAIT_HEADER_0
    s_card_present = 0;        // ② 清除卡片状态
    s_last_card_uid_len = 0;
    memset(s_last_card_uid, 0, sizeof(s_last_card_uid));

    // ③ 尝试检测加密芯片
    if (crypto_chip_init()) {
        printf("[CRYPTO] Chip online\r\n");
    } else {
        printf("[CRYPTO] Chip offline, using passthrough mode\r\n");
    }

    printf("=== App Protocol Init OK ===\r\n");
}
```

**调用时机:** `main()` 中，FM17622 初始化完成后执行一次。

**关键点:** `crypto_chip_init()` 目前始终返回 0（加密芯片未接入），但框架已预留——拿到加密芯片后在 `bsp_crypto.c` 中填充初始化逻辑即可自动生效。

---

### 4.2 `app_uart_rx_task()` — UART 接收 → 协议解析

```c
void app_uart_rx_task(void)
{
    while (uart_rx_available() > 0) {     // ① 环形缓冲区有数据就循环读
        uint8_t byte = uart_rx_read_byte(); // ② 取一字节 (tail 后移)

        parse_result_enum result = protocol_parse_byte(byte); // ③ 推入状态机

        if (result == PARSE_RESULT_OK) {        // ④ 完整帧接收 + CRC 通过
            const parsed_frame_t *pFrame = protocol_get_parsed_frame();
            app_process_frame(pFrame);          // → 命令分发
        }
        // ⑤ 异常不阻塞: CRC_ERR / LEN_ERR 仅打印日志, 状态机已自动重置
        else if (result == PARSE_RESULT_CRC_ERR) {
            printf("[ERR] Frame CRC check failed\r\n");
        } else if (result == PARSE_RESULT_LEN_ERR) {
            printf("[ERR] Frame length error\r\n");
        }
        // PARSE_RESULT_WAITING: 帧接收中, 继续取下一字节
    }
}
```

**调用时机:** 主循环每轮迭代都执行（最高优先级任务）。

**为什么用 `while` 而不是 `if`:**
主循环可能因 RFID 轮询间隔而暂停 200ms。如果 UART 环形缓冲区在此期间积累了多个字节，`while` 循环保证一次性全部处理完，不会残留数据。

**状态机交互图示:**

```
环形缓冲区 (ISR 写入)          状态机 (主循环读取)
     ↓                              ↓
uart_rx_read_byte()  →  protocol_parse_byte(byte)
                              ↓
                    ┌── PARSE_RESULT_WAITING → 继续读下一字节
                    ├── PARSE_RESULT_OK       → 帧就绪 → app_process_frame()
                    ├── PARSE_RESULT_CRC_ERR  → 打印日志 → 状态机已重置
                    └── PARSE_RESULT_LEN_ERR  → 打印日志 → 状态机已重置
```

---

### 4.3 `app_process_frame()` — 命令分发中心

```c
void app_process_frame(const parsed_frame_t *pFrame)
{
    if (pFrame == NULL) return;       // 防御性检查

    printf("[RX] CMD=0x%02X, LEN=%d\r\n", pFrame->cmd, pFrame->length);

    switch (pFrame->cmd) {

    case APP_CMD_QUERY_UID:           // CMD = 0x01
        if (pFrame->length == 0x00) { // LEN=0 → 上位机查询 UID
            app_handle_query_uid();   // 应答: 12 字节 MCU UID
        }
        // LEN=0x28(40) 是标签上传帧, 仅发送方向
        // LEN=0x0C(12) 是 UID 应答帧, 仅发送方向
        break;

    case APP_CMD_WRITE_KEY:           // CMD = 0x02
        if (pFrame->length == 0x10) { // LEN=16 → 上位机写入 KEY
            app_handle_write_key(pFrame->data);
        } else {
            printf("[ERR] KEY length mismatch: expected 16, got %d\r\n",
                   pFrame->length);
        }
        break;

    default:
        printf("[WARN] Unknown CMD: 0x%02X\r\n", pFrame->cmd);
        break;
    }
}
```

**入参来源:** `protocol_get_parsed_frame()` 返回的指针指向 `protocol.c` 内部的**静态** `s_parsed_frame`。下一帧解析完成后会覆盖。因此 `app_process_frame` 必须在此函数返回前完成所有处理（当前代码确实如此——命令处理函数内部就完成了应答发送）。

**CMD=0x01 为何只处理 LEN=0:**
因为 CMD=0x01 被三个场景共用（查询 UID 请求 LEN=0、UID 应答 LEN=12、标签上传 LEN=40）。MCU 只需处理收到的"查询请求"（LEN=0），其他两种是 MCU 自己**发出**的帧。

---

### 4.4 `app_handle_query_uid()` — 应答 UID 查询

```c
static void app_handle_query_uid(void)
{
    uint8_t uid_buf[UID_LEN];            // 12 字节
    uid_read(uid_buf);                   // 从 0x1FFFF7AC 起读芯片唯一 ID

    // 组帧: CMD=0x01, LEN=12, DATA=12字节UID, 自动计算 CRC16
    uint16_t frame_len = Pack_Data_Frame(APP_CMD_QUERY_UID, uid_buf,
                                          UID_LEN, s_tx_buf);
    // 发送
    uart_send_data(s_tx_buf, frame_len);

    printf("[TX] UID Query Resp: ");
    for (uint8_t i = 0; i < UID_LEN; i++) printf("%02X ", uid_buf[i]);
    printf("\r\n");
}
```

**产生的空中帧:**
```
A5 5A 01 0C [12字节UID] [CRC16_H] [CRC16_L]
```

---

### 4.5 `app_handle_write_key()` — 处理 KEY 写入

```c
static void app_handle_write_key(const uint8_t *pKeyData)
{
    uint8_t result = KEY_WRITE_FAIL;     // 默认失败

    flash_op_status status = flash_key_write(pKeyData); // ★ 阻塞操作!
    if (status == FLASH_OP_OK) {
        result = KEY_WRITE_SUCCESS;
        printf("[KEY] Write SUCCESS\r\n");
    } else {
        printf("[KEY] Write FAILED, err=%d\r\n", status);
    }

    // 组帧: CMD=0x02, LEN=1, DATA=00(失败)/01(成功)
    uint16_t frame_len = Pack_Data_Frame(APP_CMD_WRITE_KEY, &result, 1, s_tx_buf);
    uart_send_data(s_tx_buf, frame_len);
}
```

**⚠ 阻塞点说明:** `flash_key_write()` 内部会先擦除整页（~20ms CPU 暂停），再逐字编程。在此期间：
- `bsp_flash.c` 已调用 `flash_uart_protect_enter()` 关闭 UART 接收中断
- 操作完成后调用 `flash_uart_protect_exit()` 清除错误标志 + 清空环形缓冲区 + 重开中断
- 上位机应等待 KEY 写入应答（CMD=0x02, LEN=1）后再发下一帧

---

### 4.6 `app_upload_data()` — 统一的帧上传入口

```c
static void app_upload_data(uint8_t cmd, const uint8_t *pData, uint8_t dataLen)
{
    uint16_t frame_len = Pack_Data_Frame(cmd, pData, dataLen, s_tx_buf);
    uart_send_data(s_tx_buf, frame_len);

    // 调试输出前 16 字节
    printf("[TX] Upload CMD=0x%02X LEN=%d: ", cmd, dataLen);
    for (uint8_t i = 0; i < dataLen && i < 16; i++) printf("%02X ", pData[i]);
    if (dataLen > 16) printf("...");
    printf("\r\n");
}
```

**设计意图:** 将"组帧 + 发送 + 日志"封装为一步。无论是明文标签上传还是密文上传，都调用同一个函数。当前 CMD 始终为 `0x01`，LEN 由调用者传入（明文=40，密文长度取决于加密芯片输出）。

---

### 4.7 `app_rfid_poll_task()` — 核心业务流水线（最重要）

这是整个项目最长的函数，实现了从"刷卡"到"上传"的 10 步流水线。

#### 步骤拆解

```
┌─────────────────────────────────────────────────────────────────┐
│ 步骤 0: 间隔检查 (200ms)                                        │
│   if ((g_sys_tick_ms - last_poll) < 200) return;              │
│   last_poll = g_sys_tick_ms;                                  │
└─────────────────────────────────────────────────────────────────┘
  ↓
┌─────────────────────────────────────────────────────────────────┐
│ 步骤 1: RequestA — 寻卡                                         │
│   FM17622_RequestA(&card_type)                                 │
│   ├── 成功 → card_type = ATQA 值, 继续                          │
│   └── 失败 → 清除卡片状态(s_card_present=0), return            │
└─────────────────────────────────────────────────────────────────┘
  ↓
┌─────────────────────────────────────────────────────────────────┐
│ 步骤 2: Anticoll — 防冲突获取 UID                               │
│   FM17622_Anticoll(card_uid, &uid_len)                         │
│   ├── 成功 → uid_len=4(或7), card_uid=UID 值                    │
│   └── 失败 → 打印日志, return                                  │
│                                                                 │
│   ★ 已支持 7 字节 UID: 若 CL1 返回 0x88, 自动进入 CL2 级联     │
└─────────────────────────────────────────────────────────────────┘
  ↓
┌─────────────────────────────────────────────────────────────────┐
│ 步骤 3: Select — 选卡 (激活卡片)                                │
│   FM17622_Select(card_uid, uid_len)                            │
│   └── 失败 → 打印日志, return                                  │
└─────────────────────────────────────────────────────────────────┘
  ↓
┌─────────────────────────────────────────────────────────────────┐
│ 步骤 4: 卡片去重                                                │
│   比较 s_last_card_uid 和 card_uid:                             │
│   ├── 相同 → return (同一张卡, 不上报)                          │
│   └── 不同 → 继续                                               │
└─────────────────────────────────────────────────────────────────┘
  ↓
┌─────────────────────────────────────────────────────────────────┐
│ 步骤 5: 更新卡片状态                                            │
│   s_card_present = 1;                                          │
│   s_last_card_uid_len = uid_len;                               │
│   memcpy(s_last_card_uid, card_uid, uid_len);                 │
└─────────────────────────────────────────────────────────────────┘
  ↓
┌─────────────────────────────────────────────────────────────────┐
│ 步骤 6: 读取 FLASH 中的 KEY                                    │
│   flash_key_read(key_buf) → 16 字节                             │
│   ├── KEY 未存储 → return (等待上位机下载)                       │
│   └── KEY 已存储 → 继续                                         │
└─────────────────────────────────────────────────────────────────┘
  ↓
┌─────────────────────────────────────────────────────────────────┐
│ 步骤 7: MIFARE 认证 + 读取 3 个数据块 + 解析为 tag_data_t      │
│   FM17622_ReadTagData(&tag_data, key_buf, card_uid)            │
│   内部流程:                                                     │
│     ├── FM17622_MifareAuth(KeyA, 块4, key_buf前6字节, uid)    │
│     ├── FM17622_MifareReadBlock(块4) → 16 字节                 │
│     ├── FM17622_MifareReadBlock(块5) → 16 字节                 │
│     ├── FM17622_MifareReadBlock(块6) → 16 字节                 │
│     └── block_to_tag_data(48字节 → tag_data_t 结构体)          │
│   └── 失败 → 打印日志, return                                  │
└─────────────────────────────────────────────────────────────────┘
  ↓
┌─────────────────────────────────────────────────────────────────┐
│ 步骤 8: 序列化 — 结构体 → 40 字节数组                           │
│   tag_data_serialize(&tag_data, tag_raw)                       │
└─────────────────────────────────────────────────────────────────┘
  ↓
┌─────────────────────────────────────────────────────────────────┐
│ 步骤 9: 加密 — 明文 → 密文 (或透传)                            │
│   crypto_chip_encrypt(tag_raw, 40, enc_buf, &enc_len)          │
│   ├── 透传模式 (当前): enc_buf = tag_raw, enc_len = 40         │
│   └── 加密模式 (未来): enc_buf = 密文, enc_len = 密文长度      │
└─────────────────────────────────────────────────────────────────┘
  ↓
┌─────────────────────────────────────────────────────────────────┐
│ 步骤 10: 组帧上传 — CMD=0x01, LEN=密文长度                     │
│   app_upload_data(APP_CMD_QUERY_UID, enc_buf, enc_len)         │
│   → Pack_Data_Frame → uart_send_data → 上位机收到              │
└─────────────────────────────────────────────────────────────────┘
```

#### 关键设计细节

**间隔控制 (步骤 0):**
```c
static uint32_t last_poll = 0;                          // 函数内 static, 仅本函数可见
if ((g_sys_tick_ms - last_poll) < RFID_POLL_INTERVAL_MS) return;
last_poll = g_sys_tick_ms;
```
- `g_sys_tick_ms` 是 `bsp_systick.c` 中的全局毫秒计数器，每 1ms SysTick 中断自增
- 用差值比较而非绝对值，天然处理 32 位溢出回绕
- 但注意：首次调用时 `last_poll=0`，`g_sys_tick_ms - 0` 已远超 200，所以第一次调用立即执行

**卡片去重 (步骤 4):**
```c
if (s_card_present && uid_len == s_last_card_uid_len) {
    uint8_t same = 1;
    for (uint8_t i = 0; i < uid_len; i++) {
        if (card_uid[i] != s_last_card_uid[i]) { same = 0; break; }
    }
    if (same) return;  // 同一张卡, 跳过
}
```
- 只在"有卡状态 + 长度相同"时才做逐字节比较
- 即使同一张卡反复进入射频场，也只上传一次

**KEY 依赖检查 (步骤 6):**
```c
if (!flash_key_is_stored() || flash_key_read(key_buf) != FLASH_OP_OK) {
    printf("[RFID] KEY not stored, skip tag read\r\n");
    return;
}
```
- 如果没有 KEY（上位机还没下载），优雅跳过标签数据读取
- 这保证了即使没有加密场景，寻卡/选卡流程仍然正常工作

---

## 五、完整数据流跟踪

以一个具体的场景走一遍完整的数据流转：

### 场景 A: 上位机查询 MCU UID

```
上位机发送: A5 5A 01 00 [CRC16]
    ↓
USART0 ISR → 环形缓冲区
    ↓
main loop → app_uart_rx_task()
    ↓ while (uart_rx_available())
protocol_parse_byte('A5') → WAIT_HEADER_1
protocol_parse_byte('5A') → WAIT_CMD
protocol_parse_byte('01') → WAIT_LENGTH
protocol_parse_byte('00') → LEN=0 → WAIT_CRC_H (跳过数据阶段)
protocol_parse_byte(CRC_H) → WAIT_CRC_L
protocol_parse_byte(CRC_L) → CRC 校验 → PARSE_RESULT_OK
    ↓
protocol_get_parsed_frame() → {.cmd=0x01, .length=0x00}
    ↓
app_process_frame(pFrame)
    switch (0x01), LEN==0 → app_handle_query_uid()
        uid_read(uid_buf)                     ← 读芯片唯一 ID
        Pack_Data_Frame(0x01, uid_buf, 12, s_tx_buf)
            → s_tx_buf = [A5][5A][01][0C][12字节UID][CRC_H][CRC_L]
        uart_send_data(s_tx_buf, frame_len)   ← PA9 发出
    ↓
上位机收到: A5 5A 01 0C [12字节UID] [CRC16]
```

### 场景 B: 刷卡 → 加密 → 上传 (当前透传模式)

```
卡片进入射频场
    ↓
main loop → app_rfid_poll_task() (每 200ms)
    ↓
步骤 1: FM17622_RequestA(&card_type)  → 成功, card_type=0x0004
步骤 2: FM17622_Anticoll(uid, &len)   → 成功, uid=0x12345678, len=4
步骤 3: FM17622_Select(uid, 4)        → 成功
步骤 4: 去重检查 (新卡)               → 通过
步骤 6: flash_key_read(key_buf)       → key_buf = [6字节KEY_A][10字节保留]
步骤 7: FM17622_ReadTagData(&tag, key_buf, uid)
    ├── FM17622_MifareAuth(0x60, 4, key_buf, uid)  → 认证扇区1成功
    ├── FM17622_MifareReadBlock(4, buf+0)           → 16 字节
    ├── FM17622_MifareReadBlock(5, buf+16)          → 16 字节
    ├── FM17622_MifareReadBlock(6, buf+32)          → 16 字节
    └── block_to_tag_data(buf, &tag)                → tag.month=05, tag.day=0520...
步骤 8: tag_data_serialize(&tag, tag_raw)           → tag_raw[40]
步骤 9: crypto_chip_encrypt(tag_raw, 40, enc, &len)
        透传模式 → enc = tag_raw, len = 40
步骤 10: app_upload_data(0x01, enc, 40)
    Pack_Data_Frame(0x01, enc, 40, s_tx_buf)
        → s_tx_buf = [A5][5A][01][28][40字节tag_raw][CRC_H][CRC_L]
    uart_send_data → 上位机收到
```

---

## 六、错误处理模式

### 模式 1: 快速失败 (Fail Fast)

每一步失败立即 `return`，不继续后续步骤：

```c
if (!FM17622_RequestA(&card_type)) {    // 失败
    if (s_card_present) {                // 之前有卡则通知移除
        s_card_present = 0;
        printf("[RFID] Card removed\r\n");
    }
    return;  // ← 立即退出, 不执行 Anticoll/Select/ReadTagData
}
```

### 模式 2: 日志 + 继续

对于不影响主流程的异常，仅记录日志，状态机自动恢复：

```c
if (result == PARSE_RESULT_CRC_ERR) {
    printf("[ERR] Frame CRC check failed\r\n");  // ← 仅打印
    // 不 return, 继续读下一字节 (状态机已自行复位到 WAIT_HEADER_0)
}
```

### 模式 3: 防御性 NULL 检查

```c
void app_process_frame(const parsed_frame_t *pFrame) {
    if (pFrame == NULL) return;  // ← 即使调用者传了 NULL 也不会崩溃
    // ...
}
```

---

## 七、与其他模块的接口契约

### 对 protocol.c 的依赖

```
调用侧 (app_protocol.c)              实现侧 (protocol.c)
─────────────────────────          ─────────────────────────
protocol_parser_init()          →  重置状态机
protocol_parse_byte(byte)       →  返回 WAITING / OK / CRC_ERR / LEN_ERR
protocol_get_parsed_frame()     →  返回 s_parsed_frame 指针 (static)
Pack_Data_Frame(cmd,data,len,buf) → 返回帧总长度, 填充 buf
```

**契约约束:** `protocol_get_parsed_frame()` 返回的是 static 指针，每次解析成功后会覆盖。`app_process_frame` 必须在下次 `protocol_parse_byte` 调用前完成处理。当前代码满足此约束。

### 对 bsp_fm17622.c 的依赖

```
app_rfid_poll_task 调用:
  FM17622_RequestA()      → 卡类型 (ATQA)
  FM17622_Anticoll()      → 卡 UID (4或7字节)
  FM17622_Select()        → 激活卡片
  FM17622_ReadTagData()   → 认证 + 读块 + 解析 (全部内部完成)
  tag_data_serialize()    → 结构体 → 40 字节数组
```

**契约约束:** `FM17622_ReadTagData` 的 `pKey6` 参数期望 6 字节 MIFARE 密钥（取自上位机下载的 16 字节 KEY 的前 6 字节）。

### 对 bsp_flash.c 的依赖

```
flash_key_is_stored()     → 1: KEY 已写入  0: 未写入
flash_key_read(key_buf)   → 读 16 字节 KEY (需先确认已写入)
```

**契约约束:** `flash_key_read` 返回 `FLASH_OP_OK` 才说明读取有效。写入操作通过 `app_process_frame → app_handle_write_key` 路径触发，不在轮询任务中。

### 对 bsp_crypto.c 的依赖

```
crypto_chip_init()        → 1: 加密芯片在线  0: 离线
crypto_chip_encrypt()     → 1: 加密/透传成功  0: 失败
```

**当前行为:** 透传模式 (`CRYPTO_PASSTHROUGH=1`)，密文 = 明文，长度不变。加密失败时轮询任务会 return（不上传），避免静默丢失数据。

---

## 八、内存使用分析

| 变量 | 位置 | 大小 | 说明 |
|------|------|------|------|
| `s_tx_buf` | 静态区 (BSS) | 256 字节 | 发送缓冲区，全局唯一 |
| `s_last_card_uid` | 静态区 (BSS) | 10 字节 | 去重用的 UID 缓存 |
| `tag_data` | 栈 (app_rfid_poll_task 内) | 40 字节 | 局部变量 |
| `tag_raw` | 栈 (app_rfid_poll_task 内) | 40 字节 | 局部变量 |
| `enc_buf` | 栈 (app_rfid_poll_task 内) | 64 字节 | 局部变量 |
| `card_uid` | 栈 (app_rfid_poll_task 内) | 10 字节 | 局部变量 |
| `key_buf` | 栈 (app_rfid_poll_task 内) | 16 字节 | 局部变量 |
| **app_rfid_poll_task 栈合计** | | **~200 字节** | 安全（总栈 1KB） |
| `uid_buf` | 栈 (app_handle_query_uid) | 12 字节 | 仅调用时存在 |

**app_uart_rx_task 栈占用:** 仅有 `uint8_t byte` 和 `parse_result_enum result`，约 10 字节。调用 `protocol_parse_byte` 深入 `protocol_verify_crc` 时，`temp_buf[256]` 已改为 static 不占栈。

**结论:** 总栈使用量远低于 1KB 限制，无溢出风险。

---

## 九、扩展指南: 加密芯片接入后

当加密芯片型号和 I2C 地址确定后，只需要修改两个地方：

### 1. `bsp_crypto.c` — 填充实现

```c
// 将 CRYPTO_PASSTHROUGH 从 1 改为 0

uint8_t crypto_chip_init(void) {
    // 读加密芯片的 WHO_AM_I / 版本寄存器
    // 验证 I2C 通信正常
    return 1;  // 在线
}

uint8_t crypto_chip_encrypt(...) {
    // 1. 通过 I2C 发送明文到加密芯片
    // 2. 等待芯片处理 (轮询 Busy 标志)
    // 3. 读取密文
    // 4. 填充 pCipher 和 pCipher_len
    return 1;
}
```

### 2. app_protocol.c — 无需修改

`app_rfid_poll_task()` 中的步骤 9 已经调用了 `crypto_chip_encrypt()`。透传模式切到真实加密后，整个流水线无需改动——enc_buf 中放入的自动变为密文，LEN 自动跟随密文长度，CMD=0x01 不变。

---

## 十、总结

`app_protocol` 是整个项目的业务逻辑编排器：

- **下行链路:** `app_uart_rx_task → protocol_parse_byte → app_process_frame → 命令处理`
- **上行链路:** `app_rfid_poll_task → RFID流水线 → 加密 → Pack_Data_Frame → uart_send_data`
- **状态管理:** 3 个 static 变量维护卡片去重状态
- **错误处理:** 失败即停 + 日志记录 + 自动恢复
- **扩展性:** 加密芯片接入后只需改 `bsp_crypto.c`，app_protocol 零改动
