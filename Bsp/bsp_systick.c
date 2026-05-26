#include "gd32e23x.h"
#include "bsp_systick.h"

/* 全局毫秒计数器，供所有模块非阻塞计时使用 */
volatile uint32_t g_sys_tick_ms = 0;

/* 静态递减变量，供 delay_ms() 阻塞延时使用 */
static volatile uint32_t delay_count;

/*!
    \brief      配置 SysTick 定时器
    \param[in]  none
    \param[out] none
    \retval     none
*/
void systick_config(void)
{
    /* 
     * SystemCoreClock 是系统在启动时配置好的主频（通常是 72000000 即 72MHz）
     * SystemCoreClock / 1000 代表 1ms 的时钟周期数 (72000)
     * SysTick_Config() 会自动开启定时器和对应的中断
     */
    if (SysTick_Config(SystemCoreClock / 1000U)){
        /* 如果配置失败，进入死循环 */
        while (1){
        }
    }
    
    /* 配置 SysTick 中断优先级为最低 (0 是最高优先级，3 是最低) */
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
