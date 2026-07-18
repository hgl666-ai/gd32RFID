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

#endif /* __DEBUG_CONFIG_H */
