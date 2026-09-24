
#ifndef __PORT_I2C_H__
#define __PORT_I2C_H__

#include "stm32f10x.h"
#include "bitband.h"


#define SE_ADDR					0xE2

#define I2C_PWR_ON_DELAY		10
#define I2C_PWR_OFF_DELAY		10

//check I2C reg timeout
#define CHECK_TIMES_I2C			0xFFFF
#define BYTE_DELAY_I2C			1
#define	FRAME_DELAY_I2C			5

/* user to define use ST hardware I2C or GPIO I2C! */
//use stm32 hard i2c
#define USE_ST_I2C

#ifdef USE_ST_I2C
//user config use i2c fast mode
#define I2C_CLK_400K
#else
//use gpio i2c
#define delay_us	delayus
#define I2C_DUTY	5
#define SDA_IN()	do{GPIOB->CRH&=0xFFFF0FFF;GPIOB->CRH|=0x00008000;GPIOB->ODR|=1<<11;}while(0)
#define SDA_OUT()	do{GPIOB->CRH&=0xFFFF0FFF;GPIOB->CRH|=0x00002000;}while(0)
#define IIC_SCL		PBout(10)
#define IIC_SDA		PBout(11)
#define READ_SDA	PBin(11)
#endif

typedef struct{
	uint8_t se_i2c_addr;
	void (*fm_i2c_power_on)(void);
	void (*fm_i2c_power_off)(void);
	void (*fm_i2c_init)(void);
	void (*fm_i2c_start)(void);
	void (*fm_i2c_stop)(void);
	uint8_t (*fm_i2c_send_char)(uint8_t ch);
	uint8_t (*fm_i2c_recv_char)(void);
	uint8_t (*fm_i2c_send_addr)(uint8_t ch);
    uint8_t (*fm_i2c_recv_char_nak)(void);
} StSeI2CDriver;


extern StSeI2CDriver gusr_i2c_drv;

void se_reset(void);



#endif


