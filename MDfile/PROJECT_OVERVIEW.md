# GD32E230C8T6 + FM17622 RFID 项目文档

> 最后更新: 2026-05-25
> 目标: 帮助理解项目整体架构、数据流和关键实现细节，便于后续开发任务

---

## 一、硬件总览

```
                    ┌─────────────────────────────────────┐
                    │         PB6/PB7 I2C 总线             │
                    │                                     │
                    │    ┌──────────┐   ┌──────────┐      │
                    │    │ FM17622  │   │  加密芯片  │      │
                    │    │ 0x28     │   │  (待定)   │      │
                    │    │ RFID读卡  │   │          │      │
                    │    └────┬─────┘   └────┬─────┘      │
                    │         │              │             │
                    │         └──────┬───────┘             │
                    │                ↓                     │
                    │         GD32E230C8T6                 │
                    │         Cortex-M23 72MHz             │
                    │         64KB FLASH / 8KB SRAM        │
                    │                ↓                     │
                    │         USART0 PA9(TX) PA10(RX)      │
                    └────────────────┬────────────────────┘
                                     ↓
                              上位机 (师父编写)
```

| 芯片 | 接口 | 总线 | 地址/引脚 |
|------|------|------|-----------|
| GD32E230C8T6 | — | — | 主控, Cortex-M23, 72MHz |
| FM17622 | I2C | PB6(SCL)/PB7(SDA) | 0x28 (写=0x50, 读=0x51) |
| 加密芯片 | I2C | PB6(SCL)/PB7(SDA) | 待确认 |

---

## 二、目录结构

```
project/
├── Bsp/                    ← 用户层驱动 (核心代码集中于此)
│   ├── bsp_i2c.c/h         ← 软件模拟 I2C (PB6/PB7, 开漏)
│   ├── bsp_usart.c/h       ← USART0 收发 (115200, PA9/PA10)
│   ├── bsp_systick.c/h     ← SysTick 1ms 定时 + 全局计数器
│   ├── bsp_fm17622.c/h     ← FM17622 RFID 驱动 + MIFARE 读写
│   ├── bsp_flash.c/h       ← FLASH KEY 存储 (第63页)
│   ├── bsp_uid.c/h         ← 芯片唯一 ID 读取
│   ├── bsp_crypto.c/h      ← 加密芯片框架 (新建, 待填充)
│   ├── bsp_watchdog.c/h    ← FWDGT 看门狗 (200ms 超时)
│   ├── protocol.c/h        ← 通信协议: CRC16 + 组帧 + 状态机解析
│   └── app_protocol.c/h    ← 应用层: 命令分发 + RFID 轮询 + 上传
├── Head/                   ← 头文件 (与 Bsp/ 一一对应)
├── Library/                ← GD32 官方外设库 (19个外设, 只用了 GPIO/USART/TIMER/FMC)
├── User/
│   ├── main.c              ← 主入口
│   └── gd32e23x_it.c      ← 中断向量: NMI/HardFault/USART0
├── Start/
│   └── startup_gd32e23x.s  ← 启动汇编 (栈1KB/堆1KB)
├── Objects/
│   └── project.sct         ← 链接脚本 (FLASH 0x08000000, SRAM 0x20000000)
└── project.uvprojx         ← Keil MDK 工程文件
```

---

## 三、引脚分配

| 功能 | 引脚 | 模式 | 备注 |
|------|------|------|------|
| I2C SCL | PB6 | 开漏输出, 上拉 | 软件模拟, ~100kHz |
| I2C SDA | PB7 | 开漏输出, 上拉 | 软件模拟, ~100kHz |
| USART0 TX | PA9 | AF1, 推挽 | 115200 8N1 |
| USART0 RX | PA10 | AF1, 上拉 | 中断接收 (RBNE) |
| LED | PA1 | 推挽输出 | 未在主线使用 |

---

## 四、系统启动流程

```
上电复位
  ↓
Reset_Handler (startup_gd32e23x.s)
  ├── 初始化 SRAM (从 0x1FFFF7E0 读取 SRAM 大小并清零)
  ├── SystemInit() → system_clock_72m_hxtal()
  │     ├── 启用 HXTAL (8MHz 外部晶振)
  │     ├── 配置 PLL: 8MHz × 9 = 72MHz
  │     ├── FLASH 等待周期 = 2
  │     └── 切换系统时钟到 PLL
  └── __main (C 运行时初始化: 初始化全局/静态变量)
        ↓
main()
  ├── systick_config()         → SysTick 每 1ms 中断一次
  ├── bsp_uart_init()          → USART0 115200, 中断接收, 256 字节环形缓冲区
  ├── bsp_i2c_init()           → PB6/PB7 开漏输出, 上拉
  ├── 打印芯片 UID (12 字节)
  ├── 检查 FLASH 是否有 KEY
  ├── FM17622_Init()           → 软复位 + 定时器配置 + 开天线
  ├── FM17622_CheckComm()      → 读版本寄存器, 验证 I2C 通信
  ├── app_protocol_init()      → 初始化协议状态机 + 加密芯片检测
  └── while(1) 主循环
        ├── app_uart_rx_task()     ← 每次循环都执行
        └── app_rfid_poll_task()   ← 每 200ms 执行一次
```

---

## 五、模块详解

### 5.1 I2C 总线层 (`Bsp/bsp_i2c.c`)

**实现方式:** 软件模拟 (bit-bang)，未使用 GD32 硬件 I2C 外设。

**原因:** 硬件 I2C 外设在 GD32E230 上可能存在某些使用限制，软件模拟更灵活且易于调试。

**核心函数:**

| 函数 | 功能 | 特殊处理 |
|------|------|----------|
| `bsp_i2c_init()` | 初始化 PB6/PB7 为开漏+上拉 | — |
| `i2c_start()` | 发送 START 条件: SDA↓ 然后 SCL↓ | — |
| `i2c_stop()` | 发送 STOP 条件: SCL↑ 然后 SDA↑ | — |
| `i2c_send_byte(uint8_t)` | 发送 8 位, MSB 先出 | 每 bit 3 次 delay (设数据→SCL↑→SCL↓) |
| `i2c_read_byte(send_ack)` | 读取 8 位, MSB 先收 | send_ack=0: 发 NACK(最后一个字节) / =1: 发 ACK |
| `i2c_wait_ack()` | 等待从机拉低 SDA | **超时 200 次后自动发 STOP**, 返回 0=ACK/1=NACK |
| `i2c_bus_recovery()` | 发 9 个 SCL 脉冲 + STOP | 恢复卡死的从机 |

**时序基准:** `i2c_delay()` = 1000 次空循环 @ 72MHz ≈ 17μs, 对应 I2C 约 100kHz。

**重入安全:** 所有 I2C 函数在裸机环境运行，无中断嵌套 I2C 操作的风险。`i2c_wait_ack()` 内部有超时保护。

### 5.2 时钟与定时 (`Bsp/bsp_systick.c`)

```
SysTick_Handler() 每 1ms 调用一次:
  ├── g_sys_tick_ms++          ← 全局毫秒计数器, 永不归零, 供所有模块非阻塞计时
  └── if (delay_count > 0) delay_count--  ← 供 delay_ms() 阻塞延时使用
```

**关键变量:**
- `g_sys_tick_ms` — **全局公开** (extern), 32 位无符号, 每毫秒自增。49.7 天回绕, 使用差值比较 (t2 - t1 < interval) 可正确处理回绕。
- `delay_count` — 静态私有, 供 `delay_ms()` 使用。

**使用方式:**
```c
// 非阻塞延时 (推荐, 用于主循环中的周期性任务)
static uint32_t last = 0;
if ((g_sys_tick_ms - last) >= 200) {  // 每 200ms 执行
    last = g_sys_tick_ms;
    // ... 任务代码
}

// 阻塞延时 (仅用于初始化阶段, 禁止在主循环使用)
delay_ms(50);
```

### 5.3 UART 通信 (`Bsp/bsp_usart.c`)

**配置:** USART0, PA9(TX), PA10(RX), 115200bps, 8N1, NVIC 优先级 2

**接收机制:** 中断驱动的环形缓冲区

```
USART0_IRQHandler()                (gd32e23x_it.c)
  └── usart0_isr()                 (bsp_usart.c)
        ├── 读取 RBNE 标志 → 读 1 字节
        ├── head = (head+1) & 255  ← 256 字节环形缓冲区, 2 的幂高效取模
        ├── 如果缓冲区未满: 存入 s_rx_buf[head]
        └── 如果缓冲区满:  丢弃新数据 (防止覆盖未读数据)

主循环:
  app_uart_rx_task()
    └── while (uart_rx_available() > 0)
          ├── byte = uart_rx_read_byte()   ← tail 递增
          └── protocol_parse_byte(byte)    ← 推送字节到协议状态机
```

**发送机制:** 轮询 (阻塞等待 TBE 标志), `fputc()` 被重定向到 USART0, 因此 `printf()` 可直接使用。

**缓冲区容量:** 256 字节, 可用 255 字节 (1 个槽位用于区分空/满)。环形结构保证 ISR 执行极短 (读寄存器 + 存数组 + 指针运算)。

### 5.4 通信协议层 (`Bsp/protocol.c`)

#### 帧格式 (严禁修改)

```
| HEADER_0 | HEADER_1 | CMD  | LENGTH | DATA (N字节) | CRC16_H | CRC16_L |
|  0xA5    |  0x5A    | 1 字节 | 1 字节 |    变长      | CRC 高字节 | CRC 低字节 |
```

- **帧头:** `0xA5 0x5A` (固定)
- **CMD:** 命令码 (0x01 或 0x02)
- **LENGTH:** 数据域长度 (0~250)
- **DATA:** 有效载荷 (最大 250 字节)
- **CRC16:** Modbus CRC16 (多项式 0x8005 反射 = 0xA001), 校验范围覆盖 HEADER+CMD+LENGTH+DATA

**最小帧长:** 6 字节 (LENGTH=0 无数据时)
**最大帧长:** 256 字节

#### 命令码定义 (严禁修改)

| CMD | LEN | 方向 | 含义 |
|-----|-----|------|------|
| 0x01 | 0x00 | 上位机 → GD32 | 查询 MCU UID |
| 0x01 | 0x0C | GD32 → 上位机 | UID 应答 (12 字节) |
| 0x01 | N | GD32 → 上位机 | 标签数据上传 (N = 密文或明文长度) |
| 0x02 | 0x10 | 上位机 → GD32 | 写入 KEY (16 字节) |
| 0x02 | 0x01 | GD32 → 上位机 | KEY 写入应答 (0x00=失败, 0x01=成功) |

**CMD=0x01 通过 LENGTH 字段区分三种用途** — 加密数据上传复用 CMD=0x01, 仅 LEN 变化。

#### 状态机解析

逐字节推入, 7 个状态自动流转:

```
WAIT_HEADER_0 → WAIT_HEADER_1 → WAIT_CMD → WAIT_LENGTH
                                              ↓
                                    ┌─ LEN=0: → WAIT_CRC_H
                                    └─ LEN>0: → WAIT_DATA → WAIT_CRC_H
                                                               ↓
                                                          WAIT_CRC_L
                                                               ↓
                                           CRC 校验 → PARSE_RESULT_OK
                                                    → PARSE_RESULT_CRC_ERR
```

**容错设计:**
- 任何状态收到非预期字节时回到 WAIT_HEADER_0, 自动重同步
- LENGTH 超过最大允许值 (250) 时立即报 PARSE_RESULT_LEN_ERR 并复位状态机
- 连续 0xA5 不会误判 (WAIT_HEADER_1 状态收到 0xA5 保持等待)

#### CRC16-Modbus 计算

**多*项*式:** 0x8005, 反序为 0xA001
**初始值:** 0xFFFF
**计算范围:** HEADER_0 + HEADER_1 + CMD + LENGTH + DATA (不含 CRC 字段本身)
**字节序:** 大端 (CRC16 高字节在前)

`protocol_verify_crc()` 中的重组缓冲区 `temp_buf[256]` 为 **static**, 避免在 1KB 栈上分配大数组。

### 5.5 应用协议层 (`Bsp/app_protocol.c`)

#### 命令分发

```c
app_uart_rx_task()
  → protocol_parse_byte()  // 逐字节推入状态机
  → 当 result == PARSE_RESULT_OK:
      → protocol_get_parsed_frame()
      → app_process_frame(pFrame)
          switch (pFrame->cmd):
            case 0x01, LEN=0: → app_handle_query_uid()    // 返回 12 字节 UID
            case 0x02, LEN=16: → app_handle_write_key()    // FLASH 存储 KEY
```

#### RFID 标签轮询 (核心数据流)

```c
app_rfid_poll_task()  ← 主循环中每 200ms 调用一次 (g_sys_tick_ms 控制)
  │
  ├── 1. RequestA()         → 寻卡, 获取卡类型 (ATQA)
  │      └── 失败 → 清除卡片状态 → 返回
  ├── 2. Anticoll()         → 防冲突, 获取 4 字节 UID + BCC 校验
  │      └── 失败 → 返回
  ├── 3. Select()           → 选卡, 激活卡片
  │      └── 失败 → 返回
  ├── 4. 卡片去重检查        → 与上次 UID 比较, 同一张卡不重复处理
  │
  ├── 5. 从 FLASH 读取 KEY   → flash_key_read(key_buf)
  │      └── KEY 未存储 → 返回 (等待上位机下载)
  │
  ├── 6. 认证 + 读块 + 解析  → FM17622_ReadTagData(&tag_data, key_buf, card_uid)
  │      └── 内部流程:
  │            ├── FM17622_MifareAuth(KeyA, 块4, key6, uid)  // 认证扇区1
  │            ├── FM17622_MifareReadBlock(块4)  → 16 字节
  │            ├── FM17622_MifareReadBlock(块5)  → 16 字节
  │            ├── FM17622_MifareReadBlock(块6)  → 16 字节
  │            └── block_to_tag_data(48字节 → tag_data_t)
  │
  ├── 7. 序列化              → tag_data_serialize(&tag_data, tag_raw[40])
  │
  ├── 8. 加密                → crypto_chip_encrypt(tag_raw, 40, enc_buf, &enc_len)
  │      └── 当前为透传模式: enc_buf = tag_raw, enc_len = 40
  │
  └── 9. 组帧上传            → Pack_Data_Frame(CMD=0x01, enc_buf, enc_len, s_tx_buf)
         └── uart_send_data(s_tx_buf, frame_len)  → 发送到上位机
```

### 5.6 FM17622 RFID 驱动 (`Bsp/bsp_fm17622.c`)

#### I2C 通信层 (重构后)

**写寄存器:** `FM17622_WriteReg(reg, data)`
```
START → 从机地址(写0x50) → [ACK] → 寄存器地址 → [ACK] → 数据 → [ACK] → STOP
                                   ↓ ACK失败时          ↓ ACK失败时
                              i2c_stop() + 返回      i2c_stop() + 返回
```

**读寄存器:** `FM17622_ReadReg(reg)`
```
START → 从机地址(写0x50) → [ACK] → 寄存器地址 → [ACK]
          → START(重启动) → 从机地址(读0x51) → [ACK]
          → i2c_read_byte(NACK) → STOP → 返回 data
```

**关键改进:** 所有 ACK 失败路径都调用 `i2c_stop()` 释放总线; 使用统一的 `i2c_read_byte()` 替代原来内联的 bit-bang 实现。

#### 错误跟踪与恢复

```c
i2c_err_track(ack_fail):
  每次写/读后调用:
    ACK 成功 → s_i2c_err_count = 0          (复位计数)
    ACK 失败 → s_i2c_err_count++
                ≥ 3 次 → i2c_bus_recovery()  (发 9 个 SCL 脉冲释放卡死从机)
```

#### 新增 MIFARE 功能

**`FM17622_MifareAuth(authMode, blockAddr, pKey, pUid)`**
- 发送 12 字节: `[authMode(0x60/0x61)] [blockAddr] [Key 6字节] [UID 4字节]`
- 通过 `fm_transceive()` 发送到 FM17622 并等待完成
- 认证成功后才能对该扇区的块进行读/写操作

**`FM17622_MifareReadBlock(blockAddr, pOutBuf)`**
- 发送 2 字节: `[0x30] [blockAddr]`
- 接收 18 字节 (16 字节数据 + 2 字节 CRC), 取前 16 字节

**`fm_transceive(pTxData, txLen, pRxData, pRxLen)`**
- 内部辅助函数: 清 FIFO → 写发送数据 → 发 Transceive 命令(0x0C) → 等待完成 → 检查错误 → 读 FIFO
- 统一了 RequestA / Anticoll / Select / Auth / ReadBlock 的底层模式

#### RFID 标签数据布局 (MIFARE Classic 1K)

```
MIFARE Classic 1K = 16 扇区 × 4 块 × 16 字节/块
扇区 0: 块 0=厂商块(UID), 块1/2=数据, 块3=尾块(KeyA+Access+KeyB)
扇区 1: 块 4/5/6=用户数据, 块7=尾块

用户数据布局 (扇区1, 共 48 字节, 40 有效 + 8 填充):

块4 (16B): month[1] + day[2] + year[2] + vendor[4] + batch[2] + id[0..4]
块5 (16B): id[5] + color[7] + length[4] + sn[0..3]
块6 (16B): sn[4..5] + res[6] + padding[8]
```

**`tag_data_t` 结构 (40 字节):**

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | month | 月份 |
| 1 | 2 | day | 日期 |
| 3 | 2 | year | 年份 |
| 5 | 4 | vendor | 厂商信息 |
| 9 | 2 | batch | 批次号 |
| 11 | 6 | id | 标签 ID |
| 17 | 7 | color | 颜色信息 |
| 24 | 4 | length | 长度信息 |
| 28 | 6 | sn | 序列号 |
| 34 | 6 | res | 保留 |

### 5.7 FLASH 存储 (`Bsp/bsp_flash.c`)

**存储位置:** FLASH 第 63 页 (最后一页, 地址 0x0800FC00)

**存储格式:**
```
0x0800FC00: [4 字节标记] [16 字节 KEY]
              0x4B455900     KEY 数据
              ("KEY\0")
```

**标记值:** 0x4B455900 = 大端 "KEY\0", 用于判断 KEY 是否已写入 (擦除后为 0xFFFFFFFF)

**读写流程:**
- **写入:** `fmc_unlock()` → `fmc_page_erase()` → 写标记 → 写 16 字节 KEY (4 word) → `fmc_lock()` → 读回校验
- **读取:** 直接 FLASH 地址寻址, 无需 `fmc_unlock()`
- **擦除:** 整页擦除 (GD32 的 FLASH 不支持字擦除, 必须整页)

**⚠ 注意:** FLASH 写入期间 (约 20ms) CPU 暂停, 中断被延迟。UART 在这期间可能丢数据。上位机应在发送 KEY 写入命令后等待 ACK 再发下一帧。

### 5.8 加密芯片框架 (`Bsp/bsp_crypto.c`)

**当前状态:** 框架代码, 使用 **透传模式** (明文直出) 用于调试。

```
CRYPTO_PASSTHROUGH = 1  → 密文 = 明文 (不变)
CRYPTO_PASSTHROUGH = 0  → 调用加密芯片 (TODO: 等芯片接入后实现)
```

**待填充内容 (等拿到加密芯片型号和 I2C 地址后):**
1. 定义 `CRYPTO_I2C_ADDR` (7 位 I2C 地址)
2. 实现 I2C 读写加密芯片寄存器
3. 实现加密命令: 发送明文 → 等待 Busy → 读密文
4. 可能需要 KEY 的后 10 字节 (KEY[6..15]) 作为加密芯片的配置参数

---

## 六、KEY 拆分约定

上位机下载的 16 字节 KEY 存储于 FLASH 第 63 页，按以下方式拆分使用：

| KEY 字节范围 | 长度 | 用途 |
|-------------|------|------|
| KEY[0..5] | 6 字节 | MIFARE Classic KeyA 认证密钥 |
| KEY[6..15] | 10 字节 | 加密芯片密钥/参数 (预留) |

读卡时 `app_rfid_poll_task()` 中:
```c
flash_key_read(key_buf);                                          // 读 16 字节
FM17622_ReadTagData(&tag_data, key_buf, card_uid);               // key_buf 前 6 字节用作 MIFARE 密钥
```

---

## 七、中断向量表

| 中断 | 处理函数 | 位置 | 功能 |
|------|----------|------|------|
| NMI | `NMI_Handler()` | gd32e23x_it.c | SRAM 奇偶校验错误 |
| HardFault | `HardFault_Handler()` | gd32e23x_it.c | 死循环 (调试用) |
| SVCall | `SVC_Handler()` | gd32e23x_it.c | 死循环 |
| PendSV | `PendSV_Handler()` | gd32e23x_it.c | 死循环 |
| **SysTick** | `SysTick_Handler()` | bsp_systick.c | `g_sys_tick_ms++` + `delay_count--` |
| **USART0** | `USART0_IRQHandler()` | gd32e23x_it.c → bsp_usart.c | 接收字节存入环形缓冲区 |

**未使能的外设中断:** I2C (软件模拟, 不需要), Timer, EXTI 等。

---

## 八、主循环架构

```
while(1) {
    app_uart_rx_task()       ← 每次循环: 读取 UART 环形缓冲区 → 推送协议状态机
    app_rfid_poll_task()     ← 每 200ms: 寻卡→防冲突→选卡→读数据→加密→上传
}
```

**设计原则:**
- **非阻塞:** 无 `while(delay--)` 死等, 所有超时都有上限 (I2C 200次, RFID 操作 2000次)
- **优先级:** UART 接收优先 (每次循环处理), RFID 轮询次之 (200ms 间隔)
- **去重:** RFID 检测到同一张卡时不重复上传

---

## 九、错误处理与恢复

| 场景 | 检测机制 | 处理 |
|------|----------|------|
| I2C 从机无响应 | `i2c_wait_ack()` 超时 200 次 | 自动 `i2c_stop()` 释放总线 |
| I2C 从机卡死 SDA 拉低 | 连续 ACK 失败 3 次 | `i2c_bus_recovery()`: 9 个 SCL 脉冲 + STOP |
| 协议帧 CRC 校验失败 | 状态机 `protocol_verify_crc()` | 报 PARSE_RESULT_CRC_ERR, 状态机自动重置, 不阻塞 |
| 协议帧 LENGTH 超标 | 状态机 LENGTH > 250 | 报 PARSE_RESULT_LEN_ERR, 状态机自动重置 |
| FLASH 写入失败 | `fmc_word_program()` 返回非 READY | 立即 `fmc_lock()` 上锁, 返回 FLASH_OP_ERR |
| FLASH 校验失败 | `memcmp()` 读回比较 | 返回 FLASH_OP_VERIFY |
| 栈溢出风险 | `protocol_verify_crc` 的 `temp_buf[256]` 改为 static | 不再占用 1KB 栈空间 |
| USART 缓冲区满 | 环形缓冲区 `head+1 == tail` | 丢弃新字节 (保持旧数据完整) |

**注意:** 看门狗 (WWDGT/FWDGT) 尚未启用, 若主循环因未预见的路径卡死, 系统无法自动复位。

---

## 十、已知限制与 TODO

| 限制 | 位置 | 影响 |
|------|------|------|
| 加密芯片未接入 | `bsp_crypto.c` | 当前透传模式, 明文上传 |
| FLASH 写期间 UART 停止接收 | `bsp_flash.c` | FLASH 操作前关闭 UART 接收中断, 操作后恢复 (上位机等待 ACK) |

---

## 十一、编译与调试

**IDE:** Keil MDK (uVision)
**编译器:** ARMCLANG V6.16, 优化级别 -O2
**预定义宏:** `GD32E230`, `USE_STDPERIPH_DRIVER`
**输出:** `.\Objects\project.hex` (HEX 格式, 供烧录)

**串口调试连接:**
- 波特率: 115200
- 数据位: 8
- 停止位: 1
- 校验: 无
- TX: PA9, RX: PA10

**启动时的串口输出示例:**
```
========================================
  GD32E230 + FM17622 RFID System
  Build: May 25 2026 10:00:00
========================================
[UID] 01 02 03 04 05 06 07 08 09 0A 0B 0C
[KEY] Not stored yet, waiting for download...
[FM17622] Version: 0x92, Init OK
[CRYPTO] Chip offline, using passthrough mode
=== App Protocol Init OK ===

System ready. Waiting for commands...
```

---

## 十二、模块依赖关系

```
main.c
  ├── bsp_systick.h    (systick_config, g_sys_tick_ms)
  ├── bsp_usart.h      (bsp_uart_init, uart_send_data)
  ├── bsp_i2c.h        (bsp_i2c_init)
  ├── bsp_flash.h      (flash_key_is_stored, flash_key_read)
  ├── bsp_uid.h        (uid_read)
  ├── bsp_fm17622.h    (FM17622_Init, FM17622_CheckComm)
  └── app_protocol.h   (app_protocol_init, app_uart_rx_task, app_rfid_poll_task)
        ├── protocol.h         (Pack_Data_Frame, protocol_parse_byte, ...)
        ├── bsp_fm17622.h      (FM17622_RequestA, _Anticoll, _Select, _ReadTagData, ...)
        │     └── bsp_i2c.h    (i2c_start, i2c_send_byte, i2c_read_byte, ...)
        ├── bsp_flash.h        (flash_key_is_stored, flash_key_read)
        ├── bsp_systick.h      (g_sys_tick_ms)
        └── bsp_crypto.h       (crypto_chip_init, crypto_chip_encrypt)
              └── bsp_i2c.h    (已预留, 待加密芯片接入后使用)
```
