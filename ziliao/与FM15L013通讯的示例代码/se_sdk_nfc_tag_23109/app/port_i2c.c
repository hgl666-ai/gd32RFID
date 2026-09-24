
#include "stm32f10x.h"
#include "port_i2c.h"
#include "timer.h"

void se_reset(void)
{
    PDout(0) = 0;
    delayms(10);
    PDout(0) = 1;
    delayms(10);
}
/********************************************************************
* Function: I2C SE power on
* Input Parameter:None
* Output Parameter:None
* Return: None
*********************************************************************/
void user_i2c_dev_power_on(void)
{
    //SE_VCC_EN--PE4,SE_IRQ--PD9
    GPIOE->CRL	&= 0xFFF0FFFF;
    GPIOE->CRL	|= 0x00030000;

    //SE RST--PD0
    GPIOD->CRL	&= 0xFFFFFFF0;
    GPIOD->CRL	|= 0x00000003;

    PDout(0) = 1;

    //power on
    PEout( 4 ) = 1;
    delayms( I2C_PWR_ON_DELAY );

}

/********************************************************************
* Function: I2C SE power off
* Input Parameter:None
* Output Parameter:None
* Return: None
*********************************************************************/
void user_i2c_dev_power_off(void)
{
    //SE_VCC_EN--PE4,SE_IRQ--PD9
    GPIOE->CRL	&= 0xFFF0FFFF;
    GPIOE->CRL	|= 0x00030000;

    //power off
    PEout( 4 ) = 0;
    delayms( I2C_PWR_OFF_DELAY );

}

/********************************************************************
* Function: I2C SE interface and IO init
* Input Parameter:None
* Output Parameter:None
* Return: None
*********************************************************************/
#ifdef USE_ST_I2C
void user_i2c_init(void)
{
	//enable I2C2 clk
	RCC->APB1ENR |= 1<<22;
	
	//hard_i2c io init
	GPIOB->CRH &= 0xFFFF00FF;
	GPIOB->CRH |= 0x0000EE00;

	//i2c reg init
	I2C2->CR1 |= 1<<15;
	I2C2->CR1 &= ~(1<<15);
	I2C2->CR2 |= 30;
#ifdef I2C_CLK_400K
	//400K
	I2C2->CCR |= 1<<15 | 30;
	I2C2->TRISE = 11;
#else
	//100Kbps
	I2C2->CCR |= 180;
	I2C2->TRISE = 37;
#endif
	I2C2->OAR1 |= 1<<14;
	//enable ack,enable i2c module
	I2C2->CR1 |= 1<<10 | 1;
}
#else
void user_i2c_init(void)
{
    //gpio i2c init
	GPIOB->CRH &= 0xFFFF00FF;
	GPIOB->CRH |= 0x00002200;
	IIC_SCL = 1;
	IIC_SDA = 1;
}
#endif

/********************************************************************
* Function: generate a I2C start signal
* Input Parameter:None
* Output Parameter:None
* Return: None
*********************************************************************/
#ifdef USE_ST_I2C
void user_i2c_start(void)
{
	uint32_t count;

	//enable ack
	I2C2->CR1 |= 1<<10;
	//start
	I2C2->CR1 |= 1<<8;
	
	count = CHECK_TIMES_I2C;
	do{
		count--;
		if((I2C2->SR1&1)&&(I2C2->SR2&3))
		{
			break;
		}
	}while(count);
}
#else
void user_i2c_start(void)
{
	IIC_SDA=1;
	IIC_SCL=1;
	delay_us(I2C_DUTY);
 	IIC_SDA=0;
	delay_us(I2C_DUTY);
}
#endif
/********************************************************************
* Function: generate a I2C stop signal
* Input Parameter:None
* Output Parameter:None
* Return: None
*********************************************************************/
#ifdef USE_ST_I2C
void user_i2c_stop(void)
{
	//disable ack
	I2C2->CR1 &= ~(1<<10);
	//stop
	I2C2->CR1 |= 1<<9;
}
#else
void user_i2c_stop(void)
{
	IIC_SDA=0;
	IIC_SCL=1;
 	delay_us(I2C_DUTY); 
	IIC_SDA=1;
	delay_us(I2C_DUTY);
}
#endif
/********************************************************************
* Function: Master send a I2C Byte to slaver,and receive slaver ACK
* Input Parameter:
* ch: the send byte
* Output Parameter:None
* Return: result
* 0-success
* 12-wait ack timeout
*********************************************************************/
#ifdef USE_ST_I2C
uint8_t user_i2c_send_char(uint8_t ch)
{
	uint32_t count;

	I2C2->DR = ch;
	count = CHECK_TIMES_I2C;
	do{
		count--;
		if((I2C2->SR1&0x84)&&(I2C2->SR2&7))
		{
			break;
		}
	}while(count);

	delayus(BYTE_DELAY_I2C);

	if(count)
		return 0;
	else 
		return 12;	//timeout err
}
#else
uint8_t user_i2c_send_char(uint8_t ch)
{
    uint32_t i,cnt;
	uint8_t ack;
	
    IIC_SCL=0;
    for(i=0; i<8; i++)
    {   
   		IIC_SCL=0;
		delay_us(I2C_DUTY);
        IIC_SDA =(ch & 0x80) >> 7;
        ch <<= 1;
		IIC_SCL=1;
		delay_us(I2C_DUTY);
    }

	//recv ack
	IIC_SCL=0;
	SDA_IN();
	delay_us(I2C_DUTY);
	IIC_SCL=1;
    cnt=CHECK_TIMES_I2C;
    do{
        ack = READ_SDA;
		if(!ack)break;
    }while(cnt--);
	delay_us(I2C_DUTY); 
	IIC_SCL=0;
	SDA_OUT();	
	delay_us(I2C_DUTY);

	if(ack)
		return 12;
	else 
		return 0;
}
#endif
/********************************************************************
* Function: Master receive a I2C Byte from slaver
* Input Parameter:None
* Output Parameter:None
* Return: master received byte from slaver
*********************************************************************/
#ifdef USE_ST_I2C
uint8_t user_i2c_recv_char(void)
{
	uint32_t count;

	count = CHECK_TIMES_I2C;
	do{
		count--;
		if((I2C2->SR1&0x40)&&(I2C2->SR2&3))
		{
			break;
		}
	}while(count);
	
	delayus(BYTE_DELAY_I2C);
        
    return I2C2->DR;
}
#else
uint8_t user_i2c_recv_char(void)
{
	uint32_t i;
	uint8_t rch = 0;
	
    IIC_SCL=0;
	SDA_IN();
    for(i=0; i<8; i++)
	{
        IIC_SCL=0; 
        delay_us(I2C_DUTY);
		IIC_SCL=1;
        rch <<= 1;
        if(READ_SDA) rch++;
		delay_us(I2C_DUTY);
    }

	//send ack
	IIC_SCL=0;
	SDA_OUT();
	IIC_SDA=0;//ack
	delay_us(I2C_DUTY);
	IIC_SCL=1;
	delay_us(I2C_DUTY);
	IIC_SCL=0;
	delay_us(I2C_DUTY);
	return rch;
}
#endif

#ifndef USE_ST_I2C
uint8_t user_i2c_recv_char_nak(void)
{
    uint8_t i;
    uint8_t rch = 0;
    
    IIC_SCL=0;
	SDA_IN();
    for(i=0; i<8; i++)
	{
        IIC_SCL=0; 
        delay_us(I2C_DUTY);
		IIC_SCL=1;
        rch <<= 1;
        if(READ_SDA) rch++;
		delay_us(I2C_DUTY);
    }

	//send ack
	IIC_SCL=0;
	SDA_OUT();
    IIC_SDA=1;//nak
	delay_us(I2C_DUTY);
	IIC_SCL=1;
	delay_us(I2C_DUTY);
	IIC_SCL=0;
	delay_us(I2C_DUTY);
	return rch;
}
#endif

/********************************************************************
* Function: Master send a I2C Addr to slaver,and receive slaver ACK
* Input Parameter:
* ch: the send byte
* Output Parameter:None
* Return: result
* 0-success
* 12-wait ack timeout
*********************************************************************/
#ifdef USE_ST_I2C
uint8_t user_i2c_send_addr(uint8_t ch)
{
	uint32_t count;

	I2C2->DR = ch;
	count = CHECK_TIMES_I2C;
	do{
		count--;
		if((I2C2->SR1&2)&&(I2C2->SR2&3))
		{
			//ack
			break;
		}else if((I2C2->SR1&0x400)&&(I2C2->SR2&3))
		{
			//nak
			count=0;
			I2C2->SR1 &= ~0x0400;
			break;
		}
	}while(count);

	delayus(BYTE_DELAY_I2C);

	if(count){
		PCout(9) = 0;
		return 0;
	}else{
		PCout(9) = 1;
		return 12;	//timeout err
	}
}
#else
#define user_i2c_send_addr user_i2c_send_char
#endif

//global variable of I2C interface driver
StSeI2CDriver gusr_i2c_drv = 
{
	SE_ADDR,
	user_i2c_dev_power_on,	//se power on
	user_i2c_dev_power_off,	//se power off
	user_i2c_init,
	user_i2c_start,
	user_i2c_stop,
	user_i2c_send_char,
	user_i2c_recv_char,
	user_i2c_send_addr,
#ifndef USE_ST_I2C
    user_i2c_recv_char_nak
#endif
};



