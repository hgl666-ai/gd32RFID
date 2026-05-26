#include "bsp_usart.h"

/*
 * 环形缓冲区 (Ring Buffer) 实现:
 *   - 使用 head (写入位置) 和 tail (读取位置) 两个指针
 *   - head 由中断写入，tail 由主循环读取
 *   - 缓冲区满条件: (head + 1) % SIZE == tail
 *   - 缓冲区空条件: head == tail
 *   - 实际可用空间 = SIZE - 1
 */
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
    /* 1. 使能时钟 */
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_USART0);

    /* 2. 配置 GPIO (PA9 TX, PA10 RX) */
    gpio_af_set(GPIOA, GPIO_AF_1, GPIO_PIN_9);
    gpio_af_set(GPIOA, GPIO_AF_1, GPIO_PIN_10);

    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_9);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_9);

    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_10);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_10);

    /* 3. 配置 USART0 参数 */
    usart_deinit(USART0);
    usart_baudrate_set(USART0, 115200U);
    usart_transmit_config(USART0, USART_TRANSMIT_ENABLE);
    usart_receive_config(USART0, USART_RECEIVE_ENABLE);

    /* 4. 使能接收中断 (RBNE: Read Buffer Not Empty) */
    usart_interrupt_enable(USART0, USART_INT_RBNE);

    /* 5. 配置 NVIC */
    nvic_irq_enable(USART0_IRQn, 2U);

    /* 6. 使能 USART0 */
    usart_enable(USART0);

    /* 7. 初始化环形缓冲区 */
    s_rx_head = 0;
    s_rx_tail = 0;
}

/*!
    \brief      重定向 printf 到底层串口发送
*/
int fputc(int ch, FILE *f)
{
    (void)f;
    usart_data_transmit(USART0, (uint8_t)ch);
    while (RESET == usart_flag_get(USART0, USART_FLAG_TBE));
    return ch;
}

/*!
    \brief      串口发送单字节 (阻塞等待发送完成)
*/
void uart_send_byte(uint8_t data)
{
    usart_data_transmit(USART0, data);
    while (RESET == usart_flag_get(USART0, USART_FLAG_TBE));
}

/*!
    \brief      串口发送多字节
    \param[in]  pData: 数据指针
    \param[in]  len:   数据长度
*/
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
        /* 缓冲区满时丢弃新数据 (可根据需求改为覆盖最旧数据) */
    }
}
