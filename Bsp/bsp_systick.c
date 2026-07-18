#include "gd32e23x.h"
#include "bsp_systick.h"


volatile uint32_t g_sys_tick_ms = 0;

static volatile uint32_t delay_count;

/*!
    \brief      配置 SysTick 定时器
    \param[in]  none
    \param[out] none
    \retval     none
*/
void systick_config(void)
{
    if (SysTick_Config(SystemCoreClock / 1000U)){
        while (1){
        }
    }
    
    NVIC_SetPriority(SysTick_IRQn, 0x00U);
}

/*!
    \brief      毫秒级精准延时函数
    \param[in]  count: 需要延时的毫秒数
    \param[out] none
    \retval     none
*/
void delay_ms(uint32_t count)
{
    delay_count = count;
    // 等待中断服务函数将 delay_count 递减到 0
    while(0U != delay_count){
    }
}

/*!
    \brief      SysTick 中断服务函数 (每 1ms 自动进入一次)
    \param[in]  none
    \param[out] none
    \retval     none
*/
/*!
    \brief      SysTick 中断服务函数 (每 1ms 自动进入一次)
    \param[in]  none
    \param[out] none
    \retval     none
*/
void SysTick_Handler(void)
{
    g_sys_tick_ms++;
    if (0U != delay_count){
        delay_count--;
    }
}
