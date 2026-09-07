# 甲方反馈问题修复报告

> 日期: 2026-07-17
> 背景: 项目交付甲方后，甲方反馈 4 个问题，重点解决协议02(密钥录入)

---

## 一、甲方问题与根因分析

| # | 甲方反馈 | 根因 | 严重度 |
|---|---------|------|--------|
| ① | 收不到UUID | `printf` 重定向到 USART0(协议串口)，ASCII调试文本污染了二进制协议帧 | 致命 |
| ② | 协议和表格发的不一样 | 同①：上位机收到的字节流夹杂 `[RFID]`/`[TX]` 等文字，无法解析出干净帧 | 致命 |
| ③ | 调试信息一直打印影响数据交互 | `app_rfid_poll_task` 每200ms无卡就打印 `[RFID] Card removed` | 致命 |
| ④ | 协议没法连通 | 同①：串口数据流被污染，协议帧无法被上位机识别 | 致命 |

**核心结论：①②④三个问题同源——`printf` 与协议数据共用 USART0(PA9/PA10)。**

`bsp_usart.c` 的 `fputc` 把 `printf` 重定向到 USART0，而该串口正是与上位机通信的唯一协议串口。所有 ASCII 调试文本（Banner、UID、`[RFID]`、`[RX]`、`[TX]` 等）和二进制协议帧混在同一根线上，上位机收到的实际是：

```
[RFID] Card removed\r\n[RFID] Card removed\r\n...A5 5A 01 0C 39 53...
```

无法从中分离出有效的 `A5 5A...` 帧。

---

## 二、02协议(密钥录入)帧格式验证

**结论：02协议的代码实现完全正确，帧格式/CMD/LEN/CRC 全部与表格一致。** 它"连不通"的元凶纯粹是 printf 污染。

### 指令1：查询主控UID

| 方向 | 帧格式 | 代码实现 | 协议示例 | 验证 |
|------|--------|---------|---------|------|
| 上位机→MCU | `A5 5A 01 00 [CRC]` | `protocol_parse_byte` 解析 CMD=01,LEN=0 | `A5 5A 01 00 6B 03` | ✅ CRC=0x6B03 |
| MCU→上位机 | `A5 5A 01 0C [12B UID] [CRC]` | `Pack_Data_Frame(0x01, uid, 12)` | `A5 5A 01 0C UID CRC` | ✅ |

### 指令2：写入16字节KEY

| 方向 | 帧格式 | 代码实现 | 协议示例 | 验证 |
|------|--------|---------|---------|------|
| 上位机→MCU | `A5 5A 02 10 [16B KEY] [CRC]` | `protocol_parse_byte` 解析 CMD=02,LEN=16 | — | ✅ |
| MCU→上位机(失败) | `A5 5A 02 01 00 [CRC]` | `Pack_Data_Frame(0x02, &0x00, 1)` | `A5 5A 02 01 00 91 DA` | ✅ CRC=0x91DA |
| MCU→上位机(成功) | `A5 5A 02 01 01 [CRC]` | `Pack_Data_Frame(0x02, &0x01, 1)` | `A5 5A 02 01 01 51 1B` | ✅ CRC=0x511B |

**CRC算法**：CRC16-Modbus（多项式0xA001，初值0xFFFF），校验范围 HEADER+CMD+LEN+DATA，大端存储（CRC_H在前）。与协议"大端格式"要求一致。

---

## 三、修改文件清单

### 1. 新建 `Head/debug_config.h` — 调试输出总开关
- `DEBUG_ENABLE = 0`（发布给甲方）：`DBG_PRINTF` 编译为 `((void)0)`，0开销0污染
- `DEBUG_ENABLE = 1`（本地调试）：`DBG_PRINTF` 展开为 `printf`
- 所有源文件的 `printf` 统一替换为 `DBG_PRINTF`

### 2. 修改 `Bsp/bsp_usart.c` — fputc 双保险
- `fputc` 在 `DEBUG_ENABLE=0` 时直接 `return ch`，不向 USART0 发送任何字节
- 防止任何遗漏的 `printf` 调用污染协议串口

### 3. 修改 `Bsp/app_protocol.c` — 27处 printf→DBG_PRINTF + 命令码修正
- 所有 `printf` 替换为 `DBG_PRINTF`
- 第233行标签上传：`APP_CMD_QUERY_UID` → `APP_CMD_UPLOAD_TAG`（语义清晰）

### 4. 修改 `Bsp/bsp_crypto.c` — 8处 printf→DBG_PRINTF

### 5. 修改 `User/main.c` — printf替换 + 看门狗初始化时序修正
- 15处 `printf` → `DBG_PRINTF`
- **`bsp_watchdog_init()` 从 `app_protocol_init()` 之后移到 `bsp_i2c_init()` 之后**
  - 原顺序：UART → I2C → FM17622 → app_protocol_init(含SE认证) → 看门狗 ← 太晚！
  - 新顺序：UART → I2C → **看门狗** → FM17622 → app_protocol_init(含SE认证)
  - 原因：甲方连了加密芯片，`crypto_chip_init()` 内部 SE 双向认证（ATR轮询最长4s + 多次APDU）若卡死，看门狗未启动则无法复位。提前启动后，SE认证的 I2C 轮询循环（fmse_i2c.c）内有 `bsp_watchdog_feed()`，既能防死锁又不影响认证。

### 6. 修改 `Head/app_protocol.h` — 修正误导性宏定义
- `APP_CMD_UPLOAD_TAG` 从 `0x28`（误把LEN值当CMD）改为 `0x01`（单天线=左侧，符合协议01）
- 补充注释说明 CMD=0x01 在协议01/02中的语义区分（靠LEN区分）

---

## 四、CMD码空间说明（协议01与协议02的关系）

两个协议共用同一套 CMD 码空间，通过 LENGTH 字段区分语义：

| CMD | LEN | 含义 | 协议来源 | 方向 |
|-----|-----|------|---------|------|
| 0x01 | 0 | 查询主控UID | 协议02 | 上位机→MCU |
| 0x01 | 12 | UID应答 | 协议02 | MCU→上位机 |
| 0x01 | 40 | 上传左侧天线标签 | 协议01 | MCU→上位机 |
| 0x02 | 16 | 写入KEY | 协议02 | 上位机→MCU |
| 0x02 | 1 | KEY写入应答 | 协议02 | MCU→上位机 |
| 0x02 | 40 | 上传右侧天线标签 | 协议01 | MCU→上位机 |

本项目为**单天线**，标签上传使用 CMD=0x01（左侧），符合协议01。

---

## 五、遗留事项（需向甲方/SE侧确认）

### 1. SE内部加密行为未确认（question.md 问题4）
`bsp_crypto.c` 当前采用两步法：`write_se_data` 写入明文 → `get_se_data` 读回结果。
**需确认**：SE收到写入的明文后是否自动加密？`get_se_data` 读回的是密文还是原样明文？
- 若SE不自动加密，需改用 `DataEnDecrypt` 指令（se_cmd.h 已有声明）。

### 2. SE认证凭证与上位机下发KEY的关系
- 上位机通过协议02查询到的是**真实12字节芯片UID**（0x1FFFF7AC）
- 上位机据此生成KEY下发，存入FLASH，前6字节用作 MIFARE KeyA
- 但 SE 双向认证用的是**硬编码的** `mcu_uid[8]` 和 `mcu_com_key[16]`（与下发的KEY无关）
- 这是设计上的分离（MIFARE密钥 vs SE认证密钥），需确认是否符合甲方预期。

### 3. 加密后密文长度（question.md 问题5）
协议01要求标签上传 LEN=40。若启用SE加密，密文长度可能不是40。当前透传模式下 enc_len=40 符合协议，但启用加密后需确认密文长度并调整。

---

## 六、给甲方的测试建议

1. **重新编译烧录**后，上位机应能收到纯净的二进制协议帧（无ASCII文本干扰）。
2. **测试02协议**：
   - 发送 `A5 5A 01 00 6B 03` → 应收到 `A5 5A 01 0C [12B UID] [CRC]`
   - 发送 `A5 5A 02 10 [16B KEY] [CRC]` → 应收到 `A5 5A 02 01 01 [CRC]`（成功）或 `A5 5A 02 01 00 [CRC]`（失败）
3. **如需本地调试**：在 `Head/debug_config.h` 中将 `DEBUG_ENABLE` 改为 `1`，重新编译即可恢复 printf 调试输出。
