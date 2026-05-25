#ifndef _BSP_USART_H
#define _BSP_USART_H

#include "gd32e23x.h"
#include <stdio.h>
#include <stdint.h>

/*
 * UART 配置参数:
 *   - 波特率: 115200 (默认，可根据需要修改)
 *   - 数据位: 8
 *   - 停止位: 1
 *   - 校验位: 无
 *   - USART0: TX=PA9, RX=PA10
 */

/* 环形缓冲区大小 (2的幂次方，便于取模运算优化) */
#define UART_RX_BUF_SIZE    256U

/* 串口初始化函数声明 */
void bsp_uart_init(void);

/* 串口发送单字节 */
void uart_send_byte(uint8_t data);

/* 串口发送多字节 */
void uart_send_data(const uint8_t *pData, uint16_t len);

/* 获取环形缓冲区中已接收的数据长度 */
uint16_t uart_rx_available(void);

/* 从环形缓冲区读取 1 字节 (非阻塞) */
uint8_t uart_rx_read_byte(void);

/* USART0 中断服务函数 (需在 gd32e23x_it.c 中调用) */
void usart0_isr(void);

#endif /* _BSP_USART_H */
