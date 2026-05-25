#include "bsp_led.h"

/*!
    \brief      初始化 LED 的 GPIO
*/
void bsp_led_init(void)
{
    // 1. 使能 GPIO 时钟
    rcu_periph_clock_enable(LED_CLOCK);

    // 2. 配置引脚为推挽输出模式，无上下拉，速度 50MHz
    gpio_mode_set(LED_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, LED_PIN);
    gpio_output_options_set(LED_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, LED_PIN);
    
    // 3. 默认让灯熄灭 (注意：有些板子是低电平点亮，有些是高电平，这里暂且默认复位熄灭)
    gpio_bit_reset(LED_PORT, LED_PIN);
}

/*!
    \brief      点亮 LED
*/
void bsp_led_on(void)
{
    // 如果你的板子是低电平点亮，就把这里的 set 换成 reset
    gpio_bit_set(LED_PORT, LED_PIN);
}

/*!
    \brief      熄灭 LED
*/
void bsp_led_off(void)
{
    // 如果你的板子是低电平点亮，就把这里的 reset 换成 set
    gpio_bit_reset(LED_PORT, LED_PIN);
}

/*!
    \brief      翻转 LED 状态 (亮变灭，灭变亮)
*/
void bsp_led_toggle(void)
{
    // 固件库提供的翻转函数
    gpio_bit_toggle(LED_PORT, LED_PIN);
}
