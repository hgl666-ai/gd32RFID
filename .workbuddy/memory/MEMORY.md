# RFID 项目长期记忆

## 项目概况
- 平台: GD32E230C8T6 + FM17622(RFID) + FMSE(加密芯片)
- 三颗芯片: GD32主控 / 加密芯片FMSE(I2C 0xE2) / RFID FM17622(I2C 0x28)，共PB6/PB7软件I2C
- 两个UART协议: 协议01(标签上传,CMD=01左/02右,LEN=40) + 协议02(密钥录入:UID查询CMD=01/LEN=0,KEY写入CMD=02/LEN=16)
- 两协议共用CMD空间，靠LEN区分语义
- 上位机由用户师父编写

## 关键技术约定
- **串口**: USART0 PA9/PA10 115200 8N1，既跑协议又(曾)跑printf — 已通过debug_config.h的DEBUG_ENABLE开关隔离
- **CRC**: CRC16-Modbus(多项式0xA001,初值0xFFFF)，大端存储(CRC_H在前)，校验范围HEADER+CMD+LEN+DATA
- **UID**: 12字节，从0x1FFFF7AC读3个word，大端序展开
- **KEY**: 16字节存FLASH第63页(0x0800FC00)，前6字节=MF KeyA，后10字节预留
- **看门狗**: FWDGT 200ms超时，必须在SE认证前初始化(SE认证ATR轮询最长4s)
- **SE认证**: 硬编码mcu_uid[8]={11,22,33,44,55,66,77,88} + mcu_com_key[16]，与上位机下发KEY无关

## 单天线
- 硬件为单RFID天线，标签上传用CMD=0x01(协议01左侧)

## 待确认事项
- SE内部是否自动加密(write后get读回密文还是明文) — question.md问题4
- 启用SE加密后密文长度≠40与协议01冲突

## 文档索引
- PROJECT_OVERVIEW.md — 移植前架构(2026-05-25)
- PROJECT_OVERVIEW_SE.md — SE移植后架构(2026-05-28)
- PORTING_ANALYSIS.md — FMSE SDK移植分析
- APP_PROTOCOL_详解.md — app_protocol模块详解
- question.md — 待确认事项清单
- PROTOCOL_FIX_REPORT.md — 2026-07-17甲方问题修复报告
