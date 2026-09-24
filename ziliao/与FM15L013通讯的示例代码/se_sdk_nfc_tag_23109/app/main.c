
/* Includes ------------------------------------------------------------------*/
#include "stm32f10x.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bitband.h"
#include "timer.h"
#include "se_app.h"
  
/****************************************************/
void GPIO_init(void)
{
	//clk GPIOA,GPIOB,GPIOC,UART1 en!
	RCC->APB2ENR |= 0x1D|1<<14;
	//clk I2C2,SPI2,UART2,TIM2,TIM3 en!
	RCC->APB1ENR |= 3<<22|1<<17|1<<14|3;

	//usb,PB0
	GPIOB->CRL &= 0xFFFFFFF0;
	GPIOB->CRL |= 0x00000003;
	GPIOB->BRR |= 1;
		
	//beep,PB5
	GPIOB->CRL &= 0xFF0FFFFF;
	GPIOB->CRL |= 0x00300000;

	//led,PB8,PB9
	GPIOB->CRH &= 0xFFFFFF00;
	GPIOB->CRH |= 0x00000033;
	GPIOB->BRR |= 0x0300;

	//DIS JTAG,EN SWD
    AFIO->MAPR &= ~(2<<24);
    AFIO->MAPR |= (2<<24);
	
	//pwr-PA8,RFU-PA15
	GPIOA->CRH &= 0x0FFFFFF0;
	GPIOA->CRH |= 0x20000002;
	GPIOA->BRR |= 0x8100;

	//LDO PWR,PB2
	GPIOB->CRL &= 0xFFFFF0FF;
	GPIOB->CRL |= 0x00000200;
	GPIOB->BRR |= 0x0004;

	//LDO_1V8 PWR,PC14
	GPIOC->CRH &= 0xF0FFFFFF;
	GPIOC->CRH |= 0x02000000;
	GPIOC->BSRR |= 0x4000;

	//USER BTN,PB1
	GPIOB->CRL &= 0xFFFFFF0F;
	GPIOB->CRL |= 0x00000040;

	GPIOB->BSRR |= 0x0020;
	delayms(100);
	GPIOB->BRR |= 0x0020;
}

void board_init(void)
{	
	GPIO_init();

	uart_init();

	printf("Board Init OK!\r\n");
}


int main(void)
{
	board_init();

	show_shell_info();

	delayms(1000);

	l013_test(0);
	
	while(1)
	{
        ;
	}
}



