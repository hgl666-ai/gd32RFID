#ifndef _BSP_USART_H
#define _BSP_USART_H

#include "gd32e23x.h"
#include <stdio.h>
#include <stdint.h>


#define UART_RX_BUF_SIZE    256U

void bsp_uart_init(void);

void uart_send_byte(uint8_t data);

void uart_send_data(const uint8_t *pData, uint16_t len);

uint16_t uart_rx_available(void);

uint8_t uart_rx_read_byte(void);

void uart_rx_flush(void);

void usart0_isr(void);

#endif 