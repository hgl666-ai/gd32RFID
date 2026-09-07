# GD32E230C8T6 + FM17622 + FMSE 安全芯片 项目文档

> 最后更新: 2026-05-28
> 基线: PROJECT_OVERVIEW.md (2026-05-25, 移植前)
> 目标: 记录 FMSE 安全芯片 SDK 移植后的完整架构变化，便于后续开发和维护

---

## 一、变更总览

本次移植将复旦微电子 FMSE 安全芯片 SDK（`se_sdk_nfc_tag_23109`）集成到现有 GD32E230 RFID 项目中。移植遵循**"保留上层、重写底层"**策略：SDK 的协议层和命令层原样保留，驱动层重写为桥接本项目 `bsp_i2c.c` 的实现。

**重要说明:** SDK 及本项目移植代码仅负责 MCU 与 FMSE 芯片之间的 I2C 通信，即构造 APDU 命令、发送、接收结果。真正的加密运算（3DES/AES/RSA/SM2 等）全部在 FMSE 芯片内部完成，芯片固件由复旦微电子官方预烧录，MCU 侧无法也不需要干预加密细节。MCU 侧的 `des.c` 仅用于双向认证报文的构造和 session_key 生成，数据加密本身完全由 SE 芯片完成。

### 移植前 vs 移植后对比

```
移植前 (透传模式)                        移植后 (FMSE SDK 集成)
────────────────                        ────────────────────────
bsp_crypto.c: 框架代码, 明文直出         bsp_crypto.c: 调用 FMSE SDK 完成认证+加密
加密芯片: 未接入, 地址待定                FMSE: I2C 地址 0xE2, 已对接
I2C 总线: 仅 FM17622 (0x28)              I2C 总线: FM17622 (0x28) + FMSE (0xE2)
数据流: 标签→序列化→透传→上传             数据流: 标签→序列化→SE加密→上传
新增文件: 0                              新增文件: 8 个 (4 .c + 4 .h)
Keil 编译单元: 不含加密模块               Keil 编译单元: +des.c +fmse_port.c +fmse_i2c.c +se_cmd.c
```

---

## 二、硬件总览 (更新)

```
                    ┌──────────────────────────────────────────┐
                    │          PB6/PB7 软件 I2C 总线            │
                    │                                          │
                    │    ┌──────────┐      ┌──────────────┐    │
                    │    │ FM17622  │      │   FMSE 安全    │    │
                    │    │ 0x28     │      │   芯片 0xE2    │    │
                    │    │ RFID读卡  │      │  3DES 认证加密  │    │
                    │    └────┬─────┘      └──────┬───────┘    │
                    │         │                   │            │
                    │         └───────┬───────────┘            │
                    │                 ↓                        │
                    │          GD32E230C8T6                    │
                    │          Cortex-M23 72MHz                │
                    │          64KB FLASH / 8KB SRAM           │
                    │                 ↓                        │
                    │          USART0 PA9(TX) PA10(RX)         │
                    └─────────────────┬───────────────────────┘
                                      ↓
                               上位机 (师父编写)
```

| 芯片 | 接口 | 总线 | 地址 | 说明 |
|------|------|------|------|------|
| GD32E230C8T6 | — | — | — | 主控, Cortex-M23, 72MHz |
| FM17622 | I2C | PB6(SCL)/PB7(SDA) | 0x28 (写=0x50, 读=0x51) | RFID 读卡 |
| **FMSE** | **I2C** | **PB6(SCL)/PB7(SDA)** | **0xE2 (写), 0xE3 (读)** | **安全芯片, 3DES-ECB** |

---

## 三、目录结构 (更新)

```
project/
├── Bsp/                          ← 用户层驱动
│   ├── bsp_i2c.c/h               ← 软件模拟 I2C (未变)
│   ├── bsp_usart.c/h             ← USART0 收发 (未变)
│   ├── bsp_systick.c/h           ← SysTick 1ms 定时 (未变)
│   ├── bsp_fm17622.c/h           ← FM17622 RFID 驱动 (未变)
│   ├── bsp_flash.c/h             ← FLASH KEY 存储 (未变)
│   ├── bsp_uid.c/h               ← 芯片唯一 ID 读取 (未变)
│   ├── bsp_watchdog.c/h          ← FWDGT 看门狗 200ms (未变)
│   ├── bsp_crypto.c/h            ← [修改] 加密芯片接口层 → 调用 FMSE SDK
│   ├── protocol.c/h              ← 通信协议 (未变)
│   ├── app_protocol.c/h          ← 应用层 (未变, 接口兼容)
│   ├── des.c                     ← [新增] 3DES-ECB 纯 C 实现
│   ├── fmse_port.c               ← [新增] FMSE I2C 驱动桥接层
│   ├── fmse_i2c.c                ← [新增] FMSE I2C 帧协议层
│   └── se_cmd.c                  ← [新增] FMSE APDU 命令层
├── Head/                         ← 头文件
│   ├── (原有头文件不变)
│   ├── des.h                     ← [新增] 3DES 接口
│   ├── fmse_port.h               ← [新增] StSeI2CDriver 结构体 + FMSE 地址宏
│   ├── fmse_i2c.h                ← [新增] FM_I2C_HEAD 结构体 + 帧协议常量
│   └── se_cmd.h                  ← [新增] StApduPack + StSeFunc + APDU 函数声明
├── Library/                      ← GD32 官方外设库 (未变)
├── User/                         ← 未变
├── Start/                        ← 未变
├── Objects/                      ← 未变
├── project.uvprojx               ← [修改] 新增 4 个编译文件
└── PROJECT_OVERVIEW_SE.md        ← [新增] 本文档
```

---

## 四、新增模块详解

### 4.1 3DES 库 (`Bsp/des.c` + `Head/des.h`)

**来源:** SDK `app/des.c`，原样移植，无任何修改。

**功能:** 3DES-ECB 模式的加密/解密，纯 C 实现，无硬件依赖。

**核心函数:**

| 函数 | 功能 | 参数说明 |
|------|------|----------|
| `des3_ecb_encrypt(pout, pdata, nlen, pkey, klen)` | 3DES-ECB 加密 | nlen 必须是 8 的倍数, klen=16 |
| `des3_ecb_decrypt(pout, pdata, nlen, pkey, klen)` | 3DES-ECB 解密 | 同上 |

**内存使用:** 无静态变量。栈上使用 `des3_context` (96×4=384 字节) 仅在函数调用期间临时存在。

**移植说明:** 纯 C 代码，可直接在任何平台编译。`des_free` / `des3_free` 函数仅做内存清零，不涉及堆操作。

### 4.2 FMSE 端口层 (`Bsp/fmse_port.c` + `Head/fmse_port.h`)

**来源:** SDK `app/port_i2c.c`，**完全重写**。

**职责:** 将 SDK 的 `StSeI2CDriver` 函数指针结构体桥接到本项目的 `bsp_i2c.c` 软件 I2C 函数。

**桥接映射:**

| StSeI2CDriver 函数指针 | 桥接目标 | 说明 |
|----------------------|----------|------|
| `fm_i2c_power_on()` | 空实现 | GD32 无独立 SE 电源引脚, 硬件常供电 |
| `fm_i2c_power_off()` | 空实现 | 同上 |
| `fm_i2c_init()` | 空实现 | I2C 总线已由 `bsp_i2c_init()` 初始化 |
| `fm_i2c_start()` | `i2c_start()` | 直接映射 |
| `fm_i2c_stop()` | `i2c_stop()` | 直接映射 |
| `fm_i2c_send_char(ch)` | `i2c_send_byte(ch)` + `i2c_wait_ack()` | 0=ACK/1=NACK, 与 SDK 语义一致 |
| `fm_i2c_recv_char()` | `i2c_read_byte(1)` | 发 ACK (非末字节) |
| `fm_i2c_send_addr(ch)` | `i2c_send_byte(ch)` + `i2c_wait_ack()` | 与 send_char 相同逻辑 |
| `fm_i2c_recv_char_nak()` | `i2c_read_byte(0)` | 发 NACK (末字节) |

**驱动实例:** `gusr_i2c_drv` 全局变量，I2C 地址 = `0xE2`。

**关键适配说明:**

SDK 原始实现使用 STM32 硬件 I2C2，本项目使用 PB6/PB7 软件模拟 I2C。通过函数指针桥接，上层协议完全不感知底层差异。SDK 的 `se_reset()` (PD0 硬复位) 在本项目中无对应硬件，已删除。

### 4.3 FMSE 协议层 (`Bsp/fmse_i2c.c` + `Head/fmse_i2c.h`)

**来源:** SDK `app/fmse_i2c.c`，**保留核心逻辑，修改 include 和 timer 适配**。

**职责:** 封装 FMSE 自定义 I2C 帧协议，提供帧发送/接收/轮询/ATR 获取。

**修改点:**

| 项目 | SDK 原始 | 移植后 |
|------|---------|--------|
| include | `port_i2c.h`, `timer.h` | `fmse_port.h`, `bsp_systick.h`, `bsp_watchdog.h` |
| 延时函数 | `delayms(N)` | `delay_ms(N)` |
| 超时函数 | `init_timeout_ms()` / `check_timeout_ms()` | 自定义 `fmse_init_timeout_ms()` / `fmse_check_timeout()`，基于 `g_sys_tick_ms` |
| I2C 分支 | `#ifdef USE_ST_I2C` (硬件/软件双分支) | 仅保留 GPIO I2C 分支 |
| recv_frame 长度校验失败 | 直接 return 13 | **修复:** 先调 `i2c_stop()` 释放总线再 return |
| 轮询循环 | 无喂狗 | **新增:** `bsp_watchdog_feed()` 防止看门狗复位 |

**FMSE I2C 帧协议:**

```
发送帧 (MCU → SE):
[lenlo][lenhi][nad=0x00][cmd=0x02][data[0..N]][bcc]
  len = N + 3
  bcc = lenlo ^ lenhi ^ nad ^ cmd ^ data[0] ^ ... ^ data[N]

接收帧 (SE → MCU):
[lenlo][lenhi][nad=0x00][sta][data[0..N]][bcc]
  len 由 SE 返回, sta 为状态字节
  bcc 校验 (含末尾字节, 接收时用 NAK 模式读取)
```

**通信流程:**

```
MCU                           SE (FMSE)
 |                              |
 |--- I2C Start + Addr(W) ---->|
 |--- Send Frame (APDU) ------>|
 |--- I2C Stop ---------------->|
 |                              | (处理中, 10~4000ms)
 |--- I2C Start + Addr(R) ---->|
 |<-- Recv Frame (Response) ---|
 |--- NACK + Stop ------------>|
 |                              |
 |--- delay_ms(5) ------------>|  (帧间延时)
```

**全局实例:** `StSeFunc gfm_se_i2c` — 协议层向命令层暴露的接口表。

### 4.4 FMSE 命令层 (`Bsp/se_cmd.c` + `Head/se_cmd.h`)

**来源:** SDK `app/se_cmd.c`，**保留核心逻辑，去除外部依赖**。

**职责:** 构造 APDU 命令、调用协议层收发、3DES 加解密数据域。

**修改点:**

| 项目 | SDK 原始 | 移植后 |
|------|---------|--------|
| include | `port_i2c.h` | `fmse_port.h` |
| 外部依赖 | `dump_data()`, `StrToHex()`, `base64_encode()` | **删除**, 不再引用 |
| 调试输出 | `set_tag_reader` 中调用 `dump_data()` | **删除** |
| 类型定义 | 依赖 `stm32f10x.h` 提供 u8/u16 | **新增** `typedef uint8_t u8; typedef uint16_t u16;` |

**核心数据结构:**

```c
/* APDU 命令包 (261 字节) */
typedef struct {
    uint8_t cla;        // 命令类别
    uint8_t ins;        // 指令码
    uint8_t p1, p2;     // 参数
    union { uint8_t lc; uint8_t le; } p3;  // 数据长度
    uint8_t capdu[256]; // 命令数据域
} StApduPack;

/* SE 接口函数表 */
typedef struct {
    uint8_t se_name;
    void (*fm_driver_register)(void *user_drv);
    void (*fm_device_init)(void);
    void (*fm_open_device)(void);
    void (*fm_close_device)(void);
    uint8_t (*fm_dev_power_on)(uint8_t *rbuf, uint16_t *rlen);
    uint8_t (*fm_apdu_transceive)(...);
    void (*fm_driver_unregister)(void);
} StSeFunc;
```

**认证凭证 (硬编码):**

```c
mcu_uid[8]      = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
mcu_com_key[16] = {0xCC,0x5B,0x14,0xB6,0xE2,0xF2,0x59,0x29,
                   0x64,0x9F,0xA5,0x5C,0x72,0x6B,0xAE,0xD5};
session_key[16] = {认证后由 mcu_l013_mutual_auth 生成};
```

**关键函数一览:**

| 函数 | APDU | 功能 |
|------|------|------|
| `mcu_l013_mutual_auth()` | se_auth1 + se_auth2 | 双向认证, 生成 session_key |
| `write_se_data(para, inlen, inbuf, ...)` | CLA=80 INS=76 | 发送数据到 SE, SE 内部执行加密 |
| `get_se_data(para, inlen, inbuf, ...)` | CLA=80 INS=78 | 从 SE 读取加密结果 (密文) |
| `get_se_uid(para, ...)` | CLA=80 INS=72 | 获取 SE UID |
| `se_active(para, ...)` | CLA=80 INS=74 | SE 激活 |
| `cmd_data_wrap(...)` | — | APDU 命令传输保护: 填充 + 3DES 包装 (非数据加密) |
| `response_data_unwrap(...)` | — | APDU 响应传输保护: 3DES 解包 + 去填充 |

### 4.5 加密接口层 (`Bsp/bsp_crypto.c` + `Head/bsp_crypto.h`)

**来源:** 本项目原有文件，**重写内部实现，保持接口不变**。

**移植前:**
```c
// 空框架, 透传模式
uint8_t crypto_chip_encrypt(...) {
    memcpy(pCipher, pPlain, plain_len);  // 明文直出
    *pCipher_len = plain_len;
    return 1;
}
```

**移植后:**
```c
// 完整 FMSE 对接
uint8_t crypto_chip_init(void) {
    fm_se_register(&gfm_se_i2c);         // 注册 I2C 驱动
    pfm->fm_open_device();               // SE 上电
    pfm->fm_device_init();               // I2C 初始化
    pfm->fm_dev_power_on(atr, &len);     // 获取 ATR, 验证通信
    mcu_l013_mutual_auth(rbuf, &rlen);   // 双向认证
    memcpy(session_key, rbuf, rlen);     // 保存 session_key
}

uint8_t crypto_chip_encrypt(...) {
    if (!s_se_authenticated) → 透传或拒绝 (编译开关控制)

    // 写入: TLV 格式
    write_tlv = [C0 02 00 03] [C1 02 00 00] [C2 plain_len plain_data]
    write_se_data(0x0000, 9+plain_len, write_tlv, rbuf, &rlen);

    // 读取: TLV 格式
    read_tlv = [C0 02 00 03] [C1 02 00 00] [C2 01 read_len]
    get_se_data(0x0000, 11, read_tlv, pCipher, &cipher_len16);
}
```

**接口不变:** `crypto_chip_init()`, `crypto_chip_ping()`, `crypto_chip_encrypt()` 签名与移植前完全一致, `app_protocol.c` 无需任何修改。

---

## 五、系统启动流程 (更新)

```
上电复位
  ↓
Reset_Handler → SystemInit → main()
  ├── systick_config()
  ├── bsp_uart_init()
  ├── bsp_i2c_init()                    ← 未变
  ├── 打印芯片 UID
  ├── 检查 FLASH KEY
  ├── FM17622_Init() + CheckComm()
  ├── app_protocol_init()
  │     ├── crypto_chip_init()           ← [变化] 原来仅检测, 现在执行完整认证流程
  │     │     ├── fm_se_register()       ← 注册 FMSE I2C 驱动
  │     │     ├── fm_open_device()       ← SE 上电
  │     │     ├── fm_device_init()       ← I2C 控制器初始化
  │     │     ├── fm_dev_power_on()      ← 发送 ATR 命令, 验证 SE 通信
  │     │     │     └── fm_i2c_get_atr() ← I2C 帧发送+轮询接收 (内含 watchdog feed)
  │     │     └── mcu_l013_mutual_auth() ← 3DES 双向认证, 生成 session_key
  │     │           ├── se_auth1()       ← 发送随机数挑战
  │     │           ├── 3DES 解密验证    ← 验证 SE 响应
  │     │           ├── se_auth2()       ← 发送密钥确认
  │     │           └── 3DES 生成 key    ← session_key = 3DES_Enc(key, tmpkey)
  │     └── (认证成功/失败不影响后续流程)
  └── while(1) 主循环 (未变)
        ├── app_uart_rx_task()
        └── app_rfid_poll_task()
              ├── RFID 读卡 (未变)
              ├── tag_data_serialize() (未变)
              ├── crypto_chip_encrypt() ← [变化] 认证成功后调用 SE 加密
              │     ├── write_se_data(TLV) ← APDU: TLV 格式发送明文到 SE, SE 内部执行加密
              │     └── get_se_data(TLV)   ← APDU: TLV 格式从 SE 读取加密结果 (密文)
              └── Pack_Data_Frame() + uart_send_data() (未变)
```

---

## 六、数据流对比

### 移植前: 透传模式

```
RFID 标签
  → FM17622 读卡 (MIFARE Classic 1K)
  → 48 字节原始数据
  → tag_data_serialize() → 40 字节
  → crypto_chip_encrypt() → 40 字节明文 (透传, 不变)
  → Pack_Data_Frame() → 组帧
  → USART0 → 上位机
```

### 移植后: SE 加密模式

```
RFID 标签
  → FM17622 读卡 (MIFARE Classic 1K)
  → 48 字节原始数据
  → tag_data_serialize() → 40 字节明文
  → crypto_chip_encrypt()
  │   ├── [认证态] write_se_data(0x0000, TLV写入)  → SE 内部 3DES 加密
  │   │   └── I2C 帧: [lenlo][lenhi][nad][cmd=0x02][APDU][bcc]
  │   │       → SE 处理 10~4000ms
  │   │       → I2C 帧: [lenlo][lenhi][nad][sta][response][bcc]
  │   ├── [认证态] get_se_data(0x0000, TLV读取) → SE 返回密文
  │   │   └── 同上 I2C 帧收发流程
  │   └── [未认证态] 明文直出 (CRYPTO_REQUIRE_SE=0) 或拒绝 (CRYPTO_REQUIRE_SE=1)
  → N 字节密文 (长度由 SE 返回)
  → Pack_Data_Frame() → 组帧
  → USART0 → 上位机
```

---

## 七、安全机制

### 7.1 双向认证流程

```
MCU                                        SE (FMSE)
 |                                          |
 | 1. 生成 mcu_rnd[16] = uid[8] + rand[8]  |
 |                                          |
 |--- se_auth1(mcu_rnd[16]) ------------->|  (明文发送)
 |<-- R1'[8] + R2[8] --------------------|  (SE 响应)
 |                                          |
 | 2. 验证: 3DES_Dec(key, R1') == rnd[8:]  |
 |    若不等 → 返回 0xFFCE (认证失败)        |
 |                                          |
 | 3. 构造: tmpkey[0..7]  = R1'            |
 |         tmpkey[8..15] = 3DES_Enc(key,R2)|
 |                                          |
 |--- se_auth2(tmpkey[8:15]) ------------>|  (明文发送)
 |<-- OK (0x9000) -----------------------|
 |                                          |
 | 4. session_key = 3DES_Enc(key, tmpkey)  |
 |                                          |
 | 后续所有 APDU 数据域用 session_key       |
 | 进行 3DES-ECB 加密 + ISO 9797-1 填充    |
```

### 7.2 数据加密

```
数据加密由 FMSE 芯片内部完成, MCU 不参与加密运算:

  MCU 发送明文 → SE 内部加密 → SE 返回密文

MCU 侧的 3DES 仅用于 APDU 命令传输保护 (不是数据加密):

  发送方向: APDU 数据域 → ISO 9797-1 填充 → 3DES-ECB(session_key) 包装 → 发送
  接收方向: SE 响应 → 3DES-ECB(session_key) 解包 → 去填充 → 取出 SE 返回的结果

两者的区别:
  - 命令传输保护: MCU 侧用 session_key 对 APDU 命令做 3DES 包装, 防止通信被窃听/篡改
  - 数据加密: SE 芯片内部对业务数据执行加密运算 (如标签数据加密), MCU 不接触密钥
```

---

## 八、内存与栈分析

### 8.1 新增静态/全局变量

| 文件 | 变量 | 大小 | 说明 |
|------|------|------|------|
| `se_cmd.c` | `gfm_SeCmdHand` (StApduPack) | 261 B | APDU 命令包 |
| `se_cmd.c` | `apdu_cipher[32]` | 32 B | 加密缓冲区 |
| `se_cmd.c` | `apdu_plain[64]` | 64 B | 解密缓冲区 (审查后扩大) |
| `se_cmd.c` | `apdu_rbuf[64]` | 64 B | 接收缓冲区 (审查后扩大) |
| `se_cmd.c` | `session_key[16]` | 16 B | 会话密钥 |
| `se_cmd.c` | `apdu_padding[8]` | 8 B | 填充模板 |
| `se_cmd.c` | `mcu_uid[8]` + `mcu_com_key[16]` | 24 B | 认证凭证 |
| `fmse_i2c.c` | `s_timeout_deadline` + 其他 | 8 B | 超时辅助 |
| `bsp_crypto.c` | `s_se_authenticated` | 1 B | 认证状态标志 |
| **合计** | | **~478 B** | |

**SRAM 使用:**

| 项目 | 移植前 | 移植后 | 变化 |
|------|--------|--------|------|
| 已用 SRAM | ~3 KB | ~3.5 KB | +478 B |
| 总 SRAM | 8 KB | 8 KB | — |
| 剩余 | ~5 KB | ~4.5 KB | 安全 |

注: 代码审查后 `apdu_plain`/`apdu_rbuf` 从 32B 扩大到 64B, 新增 +64B 静态变量。

### 8.2 栈深度分析

| 调用链 | 局部变量 | 栈深 |
|--------|---------|------|
| `mcu_l013_mutual_auth` | `mcu_rnd[16]` + `recv_buf[32]` + `tmpkey[16]` | ~80 B |
| `fm_i2c_send_frame` | 帧头 + bcc | ~16 B |
| `fm_i2c_recv_frame` | 帧头 + bcc | ~16 B |
| `response_data_unwrap` | `tempbuf[64]` | ~64 B |
| `cmd_data_wrap` | `apdu_padding[8]` + `apdu_cipher[32]` | ~48 B |
| `des3_ecb_encrypt` | `des3_context` (96×4) | ~384 B |
| **最深调用链** | `encrypt → wrap → des3` | **~464 B** |

加上现有 `app_rfid_poll_task` 的 ~200 B，总计 ~664 B。1KB 栈限制内，**安全**。

---

## 九、看门狗适配

### 问题

FWDGT (自由看门狗) 超时 200ms，SE 通信轮询最长可达 4000ms (POLL_TIMEOUT)。认证和加密期间会触发看门狗复位。

### 解决方案

在 `fmse_i2c.c` 的两个轮询循环中添加 `bsp_watchdog_feed()`:

```c
// fm_i2c_get_atr() 中
do {
    ret = fm_i2c_recv_frame(rbuf, rlen);
    if (!ret) break;
    bsp_watchdog_feed();          // ← 每次轮询喂狗
} while (!fmse_check_timeout());

// fm_i2c_transceive() 中
do {
    ret = fm_i2c_recv_frame(rbuf, rlen);
    if (ret == 12) delay_ms(poll_inv);
    else if (ret == 0x04) continue;
    else break;
    bsp_watchdog_feed();          // ← 每次轮询喂狗
} while (!fmse_check_timeout());
```

SE 单次轮询间隔 = POLL_INTERVAL (10ms) << 看门狗超时 (200ms)，不会复位。

---

## 十、编译开关

| 宏 | 默认值 | 位置 | 作用 |
|----|--------|------|------|
| `CRYPTO_REQUIRE_SE` | `0` | `Bsp/bsp_crypto.c` | `0`=SE 离线时透传 (开发调试), `1`=SE 离线时拒绝加密 (生产) |

切换方式: Keil 工程 → Options → C/C++ → Preprocessor Symbols → Define 中添加 `CRYPTO_REQUIRE_SE=1`。

---

## 十一、I2C 总线共用说明

FM17622 (0x28) 和 FMSE (0xE2) 共用 PB6/PB7 软件 I2C 总线。

```
时序保证:
  ├── 每次 I2C 操作 (start→数据→stop) 是原子的
  ├── FMSE 帧间延时 5ms (FRAME_DELAY_I2C)
  ├── FM17622 操作无帧间延时要求
  └── 主循环中 RFID 轮询 200ms 间隔, 不会与 FMSE 操作冲突

总线恢复:
  ├── FM17622: 连续 ACK 失败 3 次 → i2c_bus_recovery()
  └── FMSE: i2c_wait_ack() 超时 → 自动 i2c_stop()
```

---

## 十二、错误处理 (更新)

| 场景 | 检测机制 | 处理 |
|------|----------|------|
| SE ATR 失败 | `fm_dev_power_on()` 返回非 0 | `crypto_chip_init()` 返回 0, 串口打印错误 |
| SE 认证失败 | `mcu_l013_mutual_auth()` 返回非 0x9000 | 同上 |
| SE 认证验证失败 | `memcmp(rbuf, mcu_rnd+8, 8)` 不等 | 返回 0xFFCE |
| SE APDU 超时 | `fm_i2c_transceive()` 轮询 POLL_TIMEOUT (4s) | 返回超时错误码 |
| SE 帧 BCC 校验失败 | `fm_i2c_recv_frame()` 计算 bcc != 0 | 返回 14, `*rlen=0` |
| SE 帧长度异常 | `recvLen < 3 \|\| recvLen > 1024` | 返回 13, 先 `i2c_stop()` 再返回 |
| write_se_data 失败 | SW != 0x9000 | `crypto_chip_encrypt()` 返回 0 |
| get_se_data 失败 | SW != 0x9000 | 同上 |
| SE 离线 + CRYPTO_REQUIRE_SE=0 | `s_se_authenticated == 0` | 透传模式, 明文直出 |
| SE 离线 + CRYPTO_REQUIRE_SE=1 | `s_se_authenticated == 0` | 返回 0, 拒绝加密 |

---

## 十三、模块依赖关系 (更新)

```
main.c
  ├── bsp_systick.h
  ├── bsp_usart.h
  ├── bsp_i2c.h
  ├── bsp_flash.h
  ├── bsp_uid.h
  ├── bsp_watchdog.h
  ├── bsp_fm17622.h
  └── app_protocol.h
        ├── protocol.h
        ├── bsp_fm17622.h
        │     └── bsp_i2c.h
        ├── bsp_flash.h
        ├── bsp_systick.h
        └── bsp_crypto.h              ← 接口不变
              ├── se_cmd.h             ← [新增] APDU 命令层
              │     ├── fmse_i2c.h     ← [新增] I2C 帧协议层
              │     │     ├── fmse_port.h  ← [新增] StSeI2CDriver + FMSE 地址
              │     │     └── se_cmd.h     (StSeFunc 类型定义)
              │     └── des.h          ← [新增] 3DES 接口
              ├── fmse_i2c.h
              └── fmse_port.h
```

**新增编译依赖:**
- `des.c` → 无依赖
- `fmse_port.c` → 依赖 `bsp_i2c.h`, `bsp_systick.h`
- `fmse_i2c.c` → 依赖 `fmse_port.h`, `bsp_systick.h`, `bsp_watchdog.h`
- `se_cmd.c` → 依赖 `fmse_i2c.h`, `fmse_port.h`, `des.h`
- `bsp_crypto.c` → 依赖 `se_cmd.h`, `fmse_i2c.h`, `fmse_port.h`

---

## 十四、已确认 & 待确认事项

### 已确认

| 项目 | 结论 |
|------|------|
| FMSE I2C 地址 | 0xE2 (写) / 0xE3 (读), 8 位格式 |
| mcu_uid[8] | `{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88}` |
| mcu_com_key[16] | `{0xCC,0x5B,0x14,0xB6,0xE2,0xF2,0x59,0x29, 0x64,0x9F,0xA5,0x5C,0x72,0x6B,0xAE,0xD5}` |
| 认证算法 | 3DES-ECB 双向认证 |
| 加密填充 | ISO 9797-1 Method 2 (0x80 + 0x00...) |
| 看门狗适配 | 轮询循环中喂狗, 已解决 |
| SE 离线行为 | 编译开关 `CRYPTO_REQUIRE_SE` 控制, 已解决 |
| P1P2 参数 | I2C 接口下填 0x0000, 与 SDK 示例一致 |
| write_se_data inbuf | TLV 格式: `C0 02 00 03 C1 02 00 00 C2 len data` |
| get_se_data inbuf | TLV 格式: `C0 02 00 03 C1 02 00 00 C2 01 read_len` |

### 待确认 (见 question.md)

| # | 问题 | 代码位置 |
|---|------|----------|
| 4 | SE 内部加密行为: write 后自动加密? 还是需要 DataEnDecrypt? | `bsp_crypto.c:104` |
| 5 | 密文长度与缓冲区 | `bsp_crypto.c:146` |

---

## 十五、移植经验与注意事项

1. **SDK 指令码不可变:** `cmd=0x01` (ATR) 和 `cmd=0x02` (IBLOCK) 是 FMSE 硬件协议规定, 不能修改。

2. **帧间延时 5ms:** FMSE 要求帧与帧之间至少间隔 5ms, 已在 `fmse_i2c.c` 中实现 (`delay_ms(FRAME_DELAY_I2C)`)。

3. **SE 响应时间不确定:** APDU 响应可能 10ms~4000ms, 必须轮询。不能用固定延时。

4. **栈深度控制:** `des3_ecb_encrypt` 内部 `des3_context` 占 384 字节, 是最大栈消耗者。调用链需控制在 1KB 以内。

5. **I2C 总线释放:** FMSE recv_frame 长度校验失败时必须先 `i2c_stop()` 再返回, 否则总线卡死 (已修复)。

6. **session_key 生命周期:** 每次上电认证后生成, 存储在 RAM 中, 掉电丢失。每次重启需重新认证。

7. **MCU 不做数据加密:** `des.c` 的 3DES 仅用于双向认证报文构造和 APDU 命令传输保护 (session_key 包装)。业务数据的加密运算完全由 FMSE 芯片内部完成, 芯片固件由复旦微电子官方预烧录, MCU 侧无法也不需要干预。

---

## 十六、代码审查修复记录

以下问题在移植完成后整体审查中发现并修复:

### 已修复问题

| # | 严重度 | 问题 | 文件 | 修复方式 |
|---|--------|------|------|----------|
| 1 | **严重** | `fm_i2c_send_frame` 错误路径未调 `i2c_stop()`, 共享 I2C 总线锁死 | `fmse_i2c.c:131` | 将 `i2c_stop()` 移到 `END:` 标签之后, 无论成功失败都释放总线 |
| 2 | **严重** | `fm_i2c_transceive` 中 SE 返回 0x04 时 `continue` 跳过 `bsp_watchdog_feed()`, 看门狗饿死复位 | `fmse_i2c.c:253` | 将 `continue` 改为空操作, 使 `bsp_watchdog_feed()` 始终执行 |
| 3 | **严重** | `response_data_unwrap` 的 `tempbuf[32]` 不够大, SE 返回 >34 字节时栈溢出 | `se_cmd.c:122` | `tempbuf` 扩大到 64 字节, 添加 `inlen > sizeof(tempbuf)` 边界检查 |
| 4 | **严重** | `response_data_unwrap` 中 0x80 填充查找 `while(inlen--)` 未找到时整数下溢, `fm_memmove` 复制巨量数据 | `se_cmd.c:132` | 改为 `while(inlen > 0) { inlen--; if(...) break; }` 防止下溢 |
| 5 | **高** | `apdu_plain[32]`/`apdu_rbuf[32]` 缓冲区太小, SE 密文超过 32 字节时溢出 | `se_cmd.c:49-50` | 扩大到 64 字节, 所有 5 处 `des3_ecb_decrypt` 调用前添加 `apdu_rlen-2 > sizeof(apdu_plain)` 边界检查 |

### 问题 1 详解: I2C 总线锁死

```
修复前:                              修复后:
send_char(bcc) → 失败               send_char(bcc) → 失败
  → goto END (无 i2c_stop)            → goto END
  → 返回错误码                         → i2c_stop()  ← 释放 SDA/SCL
  → I2C 总线锁死!                      → 返回错误码
  → FM17622 和 FMSE 都无法通信          → 总线正常
```

### 问题 2 详解: 看门狗饿死

```
修复前:                              修复后:
ret == 0x04 → continue               ret == 0x04 → (空操作)
  → 跳到 while 判断                     → bsp_watchdog_feed()  ← 喂狗
  → 无喂狗, 无延时                      → while 判断
  → 快速死循环 → 200ms 看门狗复位!       → 继续轮询
```
