# FMSE SDK 移植思考与适配分析

## 一、移植的总体思考逻辑

### 1.1 问题定义

复旦微电子提供的 SDK (`se_sdk_nfc_tag_23109`) 是为 STM32F103 编写的完整示例工程，包含 NFC Tag 操作和 SE 安全芯片通信两部分。本项目 (GD32E230 + FM17622 + FMSE) 只需要 SE 安全芯片的 I2C 通信能力，且硬件平台、I2C 实现方式、系统架构完全不同。

核心矛盾：**SDK 是独立工程，本项目是已有工程** — 不能把 SDK 当作主框架往里填代码，而是要把 SDK 中需要的部分"抠出来"，塞进已有架构里。

### 1.2 分层策略：保留上层，重写底层

SDK 的四层架构天然存在一个"断裂点" — 协议层与驱动层之间的 `StSeI2CDriver` 函数指针结构体。这个结构体是抽象接口，上层通过函数指针调用底层，不关心底层是 STM32 硬件 I2C 还是 GPIO 模拟。

```
SDK 四层架构:
┌─────────────────────────────────────────────────┐
│ se_app.c (业务层)        ← 不移植, 拆分入本项目  │
├─────────────────────────────────────────────────┤
│ se_cmd.c  (命令层)       ← 移植, 去除外部依赖    │  保留
├─────────────────────────────────────────────────┤
│ fmse_i2c.c (协议层)      ← 移植, 替换延时/超时   │  保留
├─────────────────────────────────────────────────┤
│ port_i2c.c (驱动层)      ← 重写, 桥接到 bsp_i2c │  重写
└─────────────────────────────────────────────────┘
          ↑ 断裂点: StSeI2CDriver 函数指针
```

选择在 `StSeI2CDriver` 这一层"切开"，原因是：

1. **上层代码量大且逻辑复杂** — `se_cmd.c` (1057行) 包含完整的 APDU 命令构造、3DES 加解密包装、双向认证流程，重写成本高且容易出错
2. **上层是纯逻辑层** — 不直接操作硬件，只依赖 `StSeFunc` 和 `StSeI2CDriver` 两个接口结构体
3. **底层代码量小且简单** — `port_i2c.c` 本质就是把几个 I2C 基础操作（start/stop/send/recv）映射到具体硬件，重写成本低
4. **本项目已有成熟的 I2C 驱动** — `bsp_i2c.c` 已经实现了软件 I2C 的全部操作，且与 FM17622 共用总线

### 1.3 文件取舍逻辑

| SDK 文件 | 决策 | 理由 |
|---------|------|------|
| `des.c` / `des.h` | **原样保留** | 纯 C 算法实现，零平台依赖，S-box 和密钥调度逻辑不可改动 |
| `fmse_i2c.c` / `fmse_i2c.h` | **移植+修改** | 协议层是 SE 通信的核心，帧格式/BCC 校验/轮询逻辑不可改，只替换延时和超时函数 |
| `se_cmd.c` / `se_cmd.h` | **移植+修改** | 命令层包含认证和数据操作的 APDU 构造逻辑，不可重写，只去除 STM32 外部依赖 |
| `port_i2c.c` / `port_i2c.h` | **完全重写** | SDK 用 STM32 硬件 I2C2 + GPIO 位操作，本项目用软件 I2C，底层完全不同 |
| `timer.c` / `timer.h` | **不移植** | SDK 用 TIM3 定时器做超时，本项目已有 SysTick (`g_sys_tick_ms`)，直接替代 |
| `bitband.h` | **不移植** | STM32 特有的位带操作宏，GD32 不需要，本项目用 `gpio_bit_set/reset` |
| `base64.c` | **不移植** | SDK 中未被 SE 通信流程调用 |
| `se_app.c` / `se_app.h` | **不移植** | SDK 的 demo 程序，功能已拆分入 `bsp_crypto.c` (初始化+加密) 和 `app_protocol.c` (业务流程) |

---

## 二、逐层适配详解

### 2.1 驱动层重写：port_i2c.c → fmse_port.c

这是唯一需要"从零编写"的文件。SDK 原始驱动层 (`port_i2c.c`, 377行) 包含大量 STM32 特有代码：

**SDK 原始实现的问题：**

```
port_i2c.c 的内容:
├── se_reset()              — 控制 PD0 引脚做硬件复位 (30行)
├── user_i2c_dev_power_on() — 控制 PE4 (SE_VCC_EN) 上电 (20行)
├── user_i2c_dev_power_off()— 控制 PE4 断电 (10行)
├── #ifdef USE_ST_I2C       — STM32 I2C2 硬件驱动 (150行)
│   ├── I2C2 GPIO 配置 (PB10/PB11 复用推挽)
│   ├── I2C2 寄存器操作 (SR1/SR2/DR/CR1)
│   └── 400KHz 时钟配置
├── #else                   — GPIO 软件 I2C (100行)
│   ├── 位带宏 PBout/PBin 操作
│   └── delay_us(I2C_DUTY) 延时
└── gusr_i2c_drv 实例       — 函数指针赋值
```

**本项目的实际情况：**
- GD32E230 没有独立的 SE 电源引脚 (PE4) 和复位引脚 (PD0)
- I2C 总线与 FM17622 共用 PB6/PB7，已由 `bsp_i2c_init()` 初始化
- `bsp_i2c.c` 提供了完整的软件 I2C 操作：`i2c_start()`, `i2c_stop()`, `i2c_send_byte()`, `i2c_wait_ack()`, `i2c_read_byte()`

**适配方案 — 58行完成替换：**

```c
// fmse_port.c — 完整实现只有 58 行
// 电源控制: 空实现 (硬件常供电, 无独立控制引脚)
static void fmse_power_on(void)  { delay_ms(FMSE_PWR_ON_DELAY); }
static void fmse_power_off(void) { delay_ms(FMSE_PWR_OFF_DELAY); }

// I2C 初始化: 空实现 (总线已由 bsp_i2c_init 完成)
static void fmse_i2c_init(void) { /* 共用总线, 已初始化 */ }

// I2C 操作: 一对一桥接
static void fmse_i2c_start(void)           { i2c_start(); }
static void fmse_i2c_stop(void)            { i2c_stop(); }
static uint8_t fmse_i2c_send_char(uint8_t ch) {
    i2c_send_byte(ch);
    return i2c_wait_ack();  // 0=ACK, 1=NACK — 与 SDK 语义一致
}
static uint8_t fmse_i2c_recv_char(void)    { return i2c_read_byte(1); }  // 发 ACK
static uint8_t fmse_i2c_send_addr(uint8_t ch) {
    i2c_send_byte(ch);
    return i2c_wait_ack();
}
static uint8_t fmse_i2c_recv_char_nak(void) { return i2c_read_byte(0); } // 发 NACK
```

**关键适配点 — 返回值语义对齐：**

SDK 的 `user_i2c_send_char()` 返回 `0=成功` (ACK) 或 `12=超时` (NACK)。本项目的 `i2c_wait_ack()` 返回 `0=ACK` 或 `1=NACK`。虽然错误码数值不同 (12 vs 1)，但 **非零=失败** 的语义一致，上层代码只判断 `if(ret)` 而不判断具体数值，因此可以直接桥接。

### 2.2 协议层移植：fmse_i2c.c 的四个关键修改

协议层是 SE 通信的核心 — 帧格式、BCC 校验、轮询机制全部在此。SDK 原始代码 372 行，移植后 277 行。修改集中在四个方面：

#### 修改 1：延时和超时函数替换

SDK 依赖 `timer.c` 提供的三个时间函数：

| SDK 原函数 | 实现方式 | 替换方案 |
|-----------|---------|---------|
| `delayms(N)` | TIM3 + `delayus(1000)` 循环 | `delay_ms(N)` — SysTick ISR 驱动 |
| `init_timeout_ms(N)` | 配置 TIM3 ARR 和 PSC | `fmse_init_timeout_ms(N)` — 计算 deadline |
| `check_timeout_ms()` | 读 TIM3 SR 标志位 | `fmse_check_timeout()` — 比较 `g_sys_tick_ms` |

替换实现：

```c
static uint32_t s_timeout_deadline;

static void fmse_init_timeout_ms(uint32_t timeout_ms) {
    s_timeout_deadline = g_sys_tick_ms + timeout_ms;
}

static uint8_t fmse_check_timeout(void) {
    return (g_sys_tick_ms >= s_timeout_deadline) ? 1 : 0;
}
```

**思考：** 为什么不用 SysTick 直接实现 `init_timeout_ms` / `check_timeout_ms`？因为 SDK 的超时机制基于定时器中断标志位，是"配置-检查"模式。而 SysTick 是"读取当前时间"模式。用 deadline 方式更自然，且不需要额外配置硬件定时器。

#### 修改 2：I2C 总线释放 — 修复 bus lockup bug

**SDK 原始代码 (fmse_i2c_send_frame)：**
```c
// SDK: i2c_stop() 在成功路径上，不在 END 标签后
ret = pgfm_I2CDrv->fm_i2c_send_char(bcc);
// ... (没有 goto END)
pgfm_I2CDrv->fm_i2c_stop();   // ← 只有成功才释放总线
delay_ms(FRAME_DELAY_I2C);
return (ret);

END:                            // ← 错误路径到这里, 没有 stop!
    return (ret);
```

**问题：** 如果发送过程中任何字节 NACK（`goto END`），总线不会被释放。在独立 I2C 总线上这可能不是问题（下次初始化会重置），但在 **共享总线** 上（FM17622 和 FMSE 共用 PB6/PB7），总线锁死会导致 FM17622 也无法通信。

**移植后修复：**
```c
// 修复: i2c_stop() 移到 END 标签后, 无条件执行
END:
    pgfm_I2CDrv->fm_i2c_stop();   // ← 无论成功失败都释放总线
    delay_ms(FRAME_DELAY_I2C);
    return (ret);
```

#### 修改 3：看门狗喂狗 — 防止 watchdog starvation

本项目启用了 FWDGT 硬件看门狗 (200ms 超时，一旦启用无法关闭)。SDK 的轮询循环中没有喂狗操作：

```c
// SDK 原始代码 — 轮询循环中没有喂狗
do {
    ret = fm_i2c_recv_frame(rbuf, rlen);
    if (!ret) break;
    // 没有喂狗!
} while (!check_timeout_ms());
```

SE 处理 APDU 命令需要 10~4000ms。如果轮询超过 200ms 没有喂狗，看门狗会复位 MCU。

**移植后修复：**
```c
// 修复: 每次轮询都喂狗
do {
    ret = fm_i2c_recv_frame(rbuf, rlen);
    if (!ret) break;
    bsp_watchdog_feed();     // ← 喂狗
} while (!fmse_check_timeout());
```

#### 修改 4：ret==0x04 的处理 — 修复 watchdog starvation 间接原因

SDK 原始代码中，`fm_i2c_transceive` 的 `ret==0x04` 分支使用 `continue`：

```c
// SDK 原始代码
if (ret == 0x04) {
    continue;   // ← 跳过循环体剩余部分, 包括后面的喂狗
}
bsp_watchdog_feed();   // ← 被 continue 跳过了!
```

`continue` 会跳过 `bsp_watchdog_feed()` 调用。如果 SE 持续返回 0x04，看门狗永远不会被喂。

**移植后修复：**
```c
// 修复: 空操作代替 continue, 喷狗代码不被跳过
if (ret == 0x04) {
    /* 空操作, 仅喂狗后继续轮询 */
}
bsp_watchdog_feed();   // ← 不再被跳过
```

### 2.3 命令层移植：se_cmd.c 的五个关键修改

命令层 (`se_cmd.c`, 原始 1057 行) 是 SDK 中最复杂的文件，包含 APDU 命令构造、ISO 9797-1 填充、3DES 加解密包装、双向认证流程。移植策略是"最小改动" — 只去除编译依赖和修复安全问题，不改业务逻辑。

#### 修改 1：去除 STM32 外部依赖

SDK 的 `se_cmd.c` 依赖三个外部函数：

```c
// SDK 原始依赖 — 来自 se_app.c, 本项目不需要
extern void dump_data(uint16_t len, uint8_t *buf);       // 调试打印
extern void StrToHex(BYTE *pbDest, BYTE *pbSrc, int nLen); // 十六进制转换
extern unsigned char *base64_encode(...);                  // Base64 编码
```

- `dump_data` — 仅在 `set_tag_reader()` 中用于调试输出，删除不影响功能
- `StrToHex` / `base64_encode` — 在 SE 通信流程中未被调用，是 `se_app.c` 的 demo 功能

#### 修改 2：类型定义替换

SDK 通过 `#include "stm32f10x.h"` 获得 `u8`/`u16` 类型。GD32 没有这个头文件。

```c
// SDK: u8/u16 来自 STM32 头文件
// 移植后: 本地 typedef
typedef uint8_t  u8;
typedef uint16_t u16;
```

#### 修改 3：缓冲区扩容 32→64 — 修复 buffer overflow

SDK 中 `apdu_plain[32]` 和 `apdu_rbuf[32]` 只有 32 字节。但 SE 的 APDU 响应数据域可能超过 32 字节：

- 认证流程中 `recv_buf` 最大 32 字节 (R1'[8] + R2[8] + SW[2] = 18 字节，安全)
- `response_data_unwrap` 解包时，如果响应数据超过 30 字节 (32 - 2 字节 SW)，`tempbuf` 会溢出
- 加密流程中 40 字节明文对应的密文可能超过 32 字节

**修复：**
```c
// SDK 原始
uint8_t apdu_plain[32];
uint8_t apdu_rbuf[32];

// 移植后
uint8_t apdu_plain[64];
uint8_t apdu_rbuf[64];
```

#### 修改 4：response_data_unwrap 整数下溢 — 修复 undefined behavior

SDK 原始代码：

```c
// SDK 原始 — 存在整数下溢风险
while (inlen--) {           // ← 如果 inlen=0, 先比较再减, 不会执行
    if (*p == 0x80) break;  // ← 但如果 inlen 从某值减到 0 后继续减,
    p++; inlen--;           // ← inlen 变成 0xFFFF (uint16_t 下溢)
}
```

实际上更严重的问题是：如果数据中没有 `0x80` 标记，`inlen` 会一直减到 0，然后下溢为 65535，继续循环读取缓冲区外的内存。

**修复：**
```c
// 移植后 — 添加边界检查, 防止下溢
if (inlen > sizeof(tempbuf)) {
    return 0x6700;  // 长度错误
}
while (inlen > 0) {
    inlen--;
    if (*p == 0x80) break;
    p++;
}
```

#### 修改 5：des3_ecb_decrypt 前添加长度校验

SDK 在调用 `des3_ecb_decrypt` 时没有检查输入长度是否超过目标缓冲区：

```c
// SDK 原始 — 没有长度检查
des3_ecb_decrypt(apdu_plain, rbuf+1, rlen-2, session_key, 16);
// 如果 rlen-2 > 32, 会写越界 apdu_plain[32]
```

**修复：**
```c
// 移植后 — 添加边界检查
if (apdu_rlen - 2 > sizeof(apdu_plain)) {
    return 0x6700;  // 响应数据过长
}
des3_ecb_decrypt(apdu_plain, rbuf+1, apdu_rlen-2, session_key, 16);
```

### 2.4 3DES 库：des.c 的最小改动

`des.c` (1034 行) 是纯 C 实现的 3DES 算法库，包含 S-box 表、密钥调度、DES/3DES ECB/CBC 加解密。这是整个移植中改动最小的文件 — 只删除了末尾的 `des_encDecTest()` 测试函数（55行），因为它是 demo 代码，且调用了 `printf` 打印大量调试信息。

**为什么不能简化或重写 des.c？**
- S-box 表是标准值，改一个数字整个加密就废了
- 密钥调度算法是 3DES 规范的一部分，不可修改
- 本项目 MCU 不做加密运算，但认证过程中 MCU 侧需要用 3DES 验证 SE 的响应（`des3_ecb_decrypt` 解密 R1'），以及构造 `tmpkey` 和 `session_key`

---

## 三、SDK 示例代码到本项目的核心适配

### 3.1 se_app.c 的功能拆分

SDK 的 `se_app.c` (459行) 是一个完整的 demo 程序，包含：

```
se_app.c 的功能:
├── StrToHex()          — 工具函数, 不移植
├── dump_data()         — 调试函数, 不移植
├── UpdateCrc/ComputeCrc— CRC 计算, 不移植 (本项目有自己的 CRC)
├── uart_init()         — STM32 USART1 初始化, 不移植
├── show_shell_info()   — 启动信息打印, 不移植
├── tag_demo()          — NFC Tag 操作 demo, 不移植
├── se_iic_demo()       — SE 操作 demo, 不移植
└── l013_test()         — 主测试入口
    ├── fm_se_register()    ← 移入 bsp_crypto.c
    ├── fm_open_device()    ← 移入 bsp_crypto.c
    ├── fm_device_init()    ← 移入 bsp_crypto.c
    ├── fm_dev_power_on()   ← 移入 bsp_crypto.c
    ├── mcu_l013_mutual_auth() ← 移入 bsp_crypto.c
    └── se_iic_demo()       ← 不移植 (demo 功能)
```

**拆分逻辑：** `l013_test()` 中的初始化+认证流程是本项目需要的核心功能，被提取到 `bsp_crypto.c` 的 `crypto_chip_init()` 中。`se_iic_demo()` 中的 SE 操作演示（读写数据、生命周期计数等）是 demo 功能，不需要移植。

### 3.2 timer.c 的功能替代

SDK 的 `timer.c` (57行) 提供三个函数：

| SDK timer.c | 本项目替代 | 说明 |
|-------------|----------|------|
| `delayus(N)` — 软件循环延时 | 不需要 | FMSE 通信最小延时单位是 ms |
| `delayms(N)` — 调用 delayus(1000) | `delay_ms(N)` — SysTick ISR | 更精确, 不阻塞中断 |
| `init_timeout_ms(N)` — 配置 TIM3 | `fmse_init_timeout_ms(N)` — 计算 deadline | 软件实现, 不占用硬件定时器 |
| `check_timeout_ms()` — 读 TIM3 SR | `fmse_check_timeout()` — 比较时间戳 | 语义相同 |

**为什么不用 TIM3？** GD32E230 的定时器资源有限（只有 TM1/TM2/TM5/TM13/TM14），且本项目已经用 SysTick 提供了毫秒级时间基准。用 `g_sys_tick_ms` 实现超时检测比占用一个硬件定时器更经济。

### 3.3 bitband.h 的替代方案

SDK 的 `bitband.h` 提供 STM32 位带操作宏：

```c
// SDK: STM32 位带操作
PBout(11) = 1;   // 直接操作 PB11 输出
PBin(11) = 1;    // 直接读取 PB11 输入
```

本项目的 `bsp_i2c.h` 用标准 GPIO 宏实现相同功能：

```c
// 本项目: 标准 GPIO 操作
#define I2C_SDA_H()   gpio_bit_set(GPIOB, GPIO_PIN_7)
#define I2C_SDA_L()   gpio_bit_reset(GPIOB, GPIO_PIN_7)
#define I2C_SDA_READ() gpio_input_bit_get(GPIOB, GPIO_PIN_7)
```

功能完全等价，且 GD32 的 `gpio_bit_set/reset` 内部也是寄存器直接操作，性能差异可忽略。

### 3.4 TLV 格式 — 从 SDK 示例代码中提取的参数

SDK 的 `se_app.c` 中 `se_iic_demo()` 函数演示了 SE 数据读写：

```c
// SDK se_app.c 中的示例代码
// 写入:
uint8_t write_buf[] = {0xC0,0x02,0x00,0x03, 0xC1,0x02,0x00,0x00, 0xC2,0x04, data...};
write_se_data(0x0000, sizeof(write_buf), write_buf, rbuf, &rlen);

// 读取:
uint8_t read_buf[] = {0xC0,0x02,0x00,0x03, 0xC1,0x02,0x00,0x00, 0xC2,0x01, read_len};
get_se_data(0x0000, sizeof(read_buf), read_buf, rbuf, &rlen);
```

这些参数（P1P2=0x0000, TLV 格式）不是从文档中得到的，而是从 SDK 示例代码中逆向分析得出的。`bsp_crypto.c` 中的 `crypto_chip_encrypt()` 直接复用了这个 TLV 格式。

---

## 四、架构适配的思考过程

### 4.1 I2C 总线共享问题

**问题：** FM17622 (地址 0x28) 和 FMSE (地址 0xE2) 共用 PB6/PB7 两根线。SDK 假设 SE 独占 I2C 总线，但本项目中总线是共享的。

**影响分析：**
- `fmse_i2c_send_frame` 中如果发送失败不释放总线 (`i2c_stop`)，FM17622 的后续通信也会失败
- SE 的轮询等待期间 (10~4000ms)，如果不清除总线状态，FM17622 的操作会被阻塞

**解决方案：**
1. `send_frame` 中 `i2c_stop()` 移到 `END:` 标签后，确保无条件释放
2. `recv_frame` 中长度校验失败时添加 `i2c_stop()`
3. 轮询循环中每次迭代都喂狗，防止看门狗复位导致总线状态不确定

### 4.2 栈空间约束

**问题：** GD32E230 只有 1KB 栈空间。SDK 运行在 STM32F103 上（20KB SRAM），栈空间充裕。

**分析：** 需要计算最深调用链的栈使用量：

```
app_rfid_poll_task          (~200B, 现有)
  └── crypto_chip_encrypt   (~73B: write_tlv[73] + read_tlv[11] + rbuf[64])
      └── write_se_data     (~48B: apdu 局部变量)
          └── cmd_data_wrap  (~48B: padding + cipher)
              └── des3_ecb_encrypt (~140B: des_context)
```

最深调用链约 500~664 字节，在 1KB 栈限制内。

**关键决策：** `apdu_plain[32]` 和 `apdu_rbuf[32]` 扩容到 `[64]` 是在静态区 (BSS)，不影响栈。`write_tlv[9 + CRYPTO_CIPHER_MAX_LEN]` 是栈上分配 (73字节)，需要控制 `CRYPTO_CIPHER_MAX_LEN` 不超过 64。

### 4.3 编译开关 CRYPTO_REQUIRE_SE 的设计

**思考：** 开发阶段 SE 芯片可能不在板上（样品、测试环境），但 RFID 读卡功能需要正常工作。

**方案：** 编译时宏控制，不是运行时检测。

```c
#define CRYPTO_REQUIRE_SE   0   // 开发阶段: 透传模式
#define CRYPTO_REQUIRE_SE   1   // 生产环境: 严格模式
```

**为什么不用运行时检测？** 因为 `crypto_chip_init()` 在启动时执行一次，之后通过 `s_se_authenticated` 标志位判断。如果 SE 不在线，`init` 返回 0，后续加密调用直接走透传路径。编译开关控制的是"SE 不在线时是否允许透传"这个策略，不是检测逻辑。

### 4.4 session_key 的存储位置

**问题：** `session_key[16]` 在 `se_cmd.c` 中定义为全局变量，`bsp_crypto.c` 通过 `extern` 访问。

**为什么不移到 bsp_crypto.c？** 因为 `se_cmd.c` 中的 `cmd_data_wrap()` 函数需要用 `session_key` 对 APDU 数据域做 3DES-ECB 加密。如果 `session_key` 移到 `bsp_crypto.c`，就需要在 `se_cmd.c` 中 `extern` 它 — 只是换了方向，没有本质改善。

**为什么不传参？** 因为 `cmd_data_wrap` 是被 `write_se_data` / `get_se_data` 等上层函数间接调用的，修改函数签名会影响整个调用链，改动量太大。保持 SDK 原始的全局变量方式是最小改动原则的体现。

---

## 五、移植过程中发现并修复的 SDK 缺陷

| # | 缺陷 | 位置 | 风险 | 修复方式 |
|---|------|------|------|---------|
| 1 | I2C 总线不释放 | `fmse_i2c_send_frame` 错误路径 | 共享总线锁死, FM17622 也无法通信 | `i2c_stop()` 移到 END 标签后 |
| 2 | 看门狗饿死 | `fmse_i2c_get_atr`/`transceive` 轮询循环 | MCU 被看门狗复位 | 循环内添加 `bsp_watchdog_feed()` |
| 3 | 看门狗饿死 (间接) | `fmse_i2c_transceive` ret==0x04 分支 | `continue` 跳过喂狗代码 | 改为空操作 |
| 4 | 缓冲区溢出 | `se_cmd.c` apdu_plain/rbuf [32] | 栈破坏, 硬件故障 | 扩容到 [64] |
| 5 | 整数下溢 | `response_data_unwrap` while(inlen--) | 读取缓冲区外内存 | 改为 while(inlen>0) + 边界检查 |
| 6 | 无长度校验 | `des3_ecb_decrypt` 调用前 | 写越界 | 添加 sizeof 检查 |
| 7 | recv_frame 不释放总线 | `fmse_i2c_recv_frame` 长度校验失败 | 共享总线锁死 | 失败路径添加 `i2c_stop()` |

---

## 六、总结：移植的核心原则

1. **保留不可改的，重写必须改的** — 帧协议、APDU 构造、3DES 算法不可改；I2C 驱动、延时函数、电源控制必须改
2. **最小改动原则** — 能改 include 就不改代码，能加一行检查就不重构函数
3. **适配而非替代** — 用 `bsp_i2c.c` 替代 `port_i2c.c`，用 `bsp_systick.c` 替代 `timer.c`，但上层调用方式不变
4. **共享总线思维** — 所有 I2C 操作路径都必须确保总线释放，这是 SDK 原始设计没有考虑的
5. **资源约束优先** — 1KB 栈、200ms 看门狗、8KB SRAM，每个决策都要在这些约束下验证
