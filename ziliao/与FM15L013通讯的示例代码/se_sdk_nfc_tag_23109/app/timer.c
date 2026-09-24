
#include "stm32f10x.h"
#include "bitband.h"
#include "timer.h"

/******************************************************/
/*soft delayus when system clk @72Mhz */
void delayus( uint32_t cnt )
{
    uint32_t i, j;

    for ( i = cnt; i > 0; i-- )
    {
        for ( j = 14; j > 0; j-- )
            ;
    }
}


void delayms( uint32_t cnt )
{
    uint32_t i;
    for ( i = cnt; i > 0; i-- )
    {
        delayus( 1000 );
    }
}


void init_timeout_ms( uint32_t timeout_ms )
{
    /*enable tmr3 clk */
    RCC->APB1ENR	|= 2;
    TIM3->CR1		= 0;
    /*TIM_CLK=72MHz,72000/72M=1ms */
    TIM3->ARR = timeout_ms + timeout_ms / 5 - 1;
    TIM3->PSC	= 60000 - 1;
    TIM3->EGR	|= 1;
    TIM3->SR	= 0;
    /*enable timer3 */
    TIM3->CR1 |= 1 << 7 | 1;
}


uint8_t check_timeout_ms( void )
{
    if ( TIM3->SR & 1 )
    {
        TIM3->CR1 = 0;
        return (1);
    }
    else
    {
        return (0);
    }
}

