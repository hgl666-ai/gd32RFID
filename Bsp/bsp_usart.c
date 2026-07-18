#include "bsp_usart.h"
#include "debug_config.h"


static volatile uint8_t  s_rx_buf[UART_RX_BUF_SIZE];
static volatile uint16_t s_rx_head = 0;  /* 中断写入位置 */
static volatile uint16_t s_rx_tail = 0;  /* 主循环读取位置 */

/*!
    \brief      初始化 USART0 (TX=PA9, RX=PA10)
    \param[in]  none
    \param[out] none
    \retval     none
*/
void bsp_uart_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_USART0);

    gpio_af_set(GPIOA, GPIO_AF_1, GPIO_PIN_9);
    gpio_af_set(GPIOA, GPIO_AF_1, GPIO_PIN_10);

    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_9);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_9);

    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_10);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_10);

    usart_deinit(USART0);
    usart_baudrate_set(USART0, 115200U);
    usart_transmit_config(USART0, USART_TRANSMIT_ENABLE);
    usart_receive_config(USART0, USART_RECEIVE_ENABLE);

    usart_interrupt_enable(USART0, USART_INT_RBNE);

    nvic_irq_enable(USART0_IRQn, 2U);

    usart_enable(USART0);

    s_rx_head = 0;
    s_rx_tail = 0;
}


/*
 * fputc — printf 重定向钩子
 *
 * 重要: printf 与协议数据共用 USART0, DEBUG_ENABLE=0 时此处直接返回,
 *       不向协议串口发送任何字节, 确保二进制协议帧不被 ASCII 调试文本污染。
 *       这是 DBG_PRINTF 宏之外的第二道保险, 防止遗漏的 printf 调用。
 */
int fputc(int ch, FILE *f)
{
    (void)f;
#if DEBUG_ENABLE
    usart_data_transmit(USART0, (uint8_t)ch);
    while (RESET == usart_flag_get(USART0, USART_FLAG_TBE));
#endif
    return ch;
}


void uart_send_byte(uint8_t data)
{
    usart_data_transmit(USART0, data);
    while (RESET == usart_flag_get(USART0, USART_FLAG_TBE));
}

void uart_send_data(const uint8_t *pData, uint16_t len)
{
    if (pData == NULL || len == 0) return;

    for (uint16_t i = 0; i < len; i++) {
        uart_send_byte(pData[i]);
    }
}

/*!
    \brief      获取环形缓冲区中已接收的数据长度
    \retval     可读数据字节数
*/
uint16_t uart_rx_available(void)
{
    return (s_rx_head - s_rx_tail) & (UART_RX_BUF_SIZE - 1);
}

/*!
    \brief      从环形缓冲区读取 1 字节 (非阻塞)
    \retval     读取到的字节
    \note       调用前应先通过 uart_rx_available() 确认有数据可读
*/
uint8_t uart_rx_read_byte(void)
{
    uint8_t data = s_rx_buf[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1) & (UART_RX_BUF_SIZE - 1);
    return data;
}

/*!
    \brief      清空环形缓冲区 (丢弃所有已接收但未处理的字节)
*/
void uart_rx_flush(void)
{
    s_rx_tail = s_rx_head;
}

/*!
    \brief      USART0 中断服务函数
    \note       在 gd32e23x_it.c 的 USART0_IRQHandler 中调用此函数
*/
void usart0_isr(void)
{
    if (RESET != usart_interrupt_flag_get(USART0, USART_INT_FLAG_RBNE)) {
        /* 读取接收到的字节 (同时清除 RBNE 标志) */
        uint8_t data = (uint8_t)usart_data_receive(USART0);

        /* 计算下一个 head 位置 */
        uint16_t next_head = (s_rx_head + 1) & (UART_RX_BUF_SIZE - 1);

        /* 检查缓冲区是否已满 */
        if (next_head != s_rx_tail) {
            s_rx_buf[s_rx_head] = data;
            s_rx_head = next_head;
        }
        /* 缓冲区满时丢弃新数据 */
    }
}
