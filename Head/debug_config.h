#ifndef __DEBUG_CONFIG_H
#define __DEBUG_CONFIG_H

/*
 * 调试输出总开关
 *
 * 使用方法:
 *   - 调试版本(给甲方排查问题): DEBUG_ENABLE = 1  (打印协议命令收发+CRC对比)
 *   - 正式发布版本:             DEBUG_ENABLE = 0  (彻底关闭, 串口只跑协议数据)
 *
 * 调试版本输出说明 (DEBUG_ENABLE=1):
 *   [SYS]  系统就绪
 *   [SE]   加密芯片认证状态
 *   [RX] OK CMD:XX LEN:XX DATA:XX.. CRC:XXXX      收到正确帧
 *   [RX] CRC ERR! ...RECV_CRC:XXXX CALC_CRC:XXXX   CRC错误(对比两个CRC值)
 *   [RX] LEN ERR                                   长度异常
 *   [CMD]  命令分发(QueryUID/WriteKey/Unknown)
 *   [KEY]  KEY写入结果
 *   [TX]   发送的应答帧完整HEX内容
 *   RFID轮询完全静默, 不刷屏
 */
#define DEBUG_ENABLE    0

#if DEBUG_ENABLE
    #include <stdio.h>
    #define DBG_PRINTF(fmt, ...)   printf(fmt, ##__VA_ARGS__)
#else
    #define DBG_PRINTF(fmt, ...)   ((void)0)
#endif

/*
 * 加密模式开关
 *
 *   CRYPTO_PASSTHROUGH = 1  → 调试模式 (默认): 跳过 SE 加密, 明文直出
 *   CRYPTO_PASSTHROUGH = 0  → 生产模式: 必须通过 SE 认证并加密
 *
 * 切换步骤:
 *   1. 修改下面这个宏值
 *   2. 重新编译烧录
 *
 * 调试时建议保持 CRYPTO_PASSTHROUGH=1, 避免因 SE 离线导致整条链路不通;
 * 正式交付前改为 0 并测试 SE 加密链路是否正常。
 */
#define CRYPTO_PASSTHROUGH   0

#endif /* __DEBUG_CONFIG_H */
