#include "bsp_i2c.h"


static void i2c_delay(void) {
		volatile uint32_t i = 1000; 
    while(i--);
}

/*!
    \brief      初始化 I2C 引脚
*/
void bsp_i2c_init(void) {
    rcu_periph_clock_enable(I2C_RCU);

    gpio_mode_set(I2C_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, I2C_SCL_PIN | I2C_SDA_PIN);
    gpio_output_options_set(I2C_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_50MHZ, I2C_SCL_PIN | I2C_SDA_PIN);
    
    I2C_SCL_H();
    I2C_SDA_H();
}


void i2c_start(void) {
    I2C_SDA_H();
    I2C_SCL_H();
    i2c_delay();
    I2C_SDA_L();
    i2c_delay();
    I2C_SCL_L();
}


void i2c_stop(void) {
    I2C_SDA_L();
    I2C_SCL_H();
    i2c_delay();
    I2C_SDA_H();
    i2c_delay();
}


void i2c_send_byte(uint8_t byte) {
    uint8_t i;
    for (i = 0; i < 8; i++) {
        if (byte & 0x80) {
            I2C_SDA_H();
        } else {
            I2C_SDA_L();
        }
        byte <<= 1;
        i2c_delay();
        I2C_SCL_H();
        i2c_delay();
        I2C_SCL_L();
        i2c_delay();
    }
}


uint8_t i2c_wait_ack(void) {
    uint16_t timeout = 0;

    I2C_SDA_H(); 
    i2c_delay();
    I2C_SCL_H();
    i2c_delay();

    while (I2C_SDA_READ()) {
        timeout++;
        if (timeout > 200) {  
            i2c_stop();
            return 1; // 1 表示失败 (NACK)
        }
    }

    I2C_SCL_L();
    return 0; // 0 表示成功收到应答 (ACK)
}

// 软件模拟 I2C 读一个字节
uint8_t i2c_read_byte(uint8_t send_ack) {
    uint8_t data = 0;

    I2C_SDA_H(); // 主机释放数据线
    for (uint8_t i = 0; i < 8; i++) {
        data <<= 1;
        I2C_SCL_H();
        i2c_delay();
        if (I2C_SDA_READ()) {
            data |= 0x01;
        }
        I2C_SCL_L();
        i2c_delay();
    }

    // 主机发送 ACK(0) 或 NACK(1)
    if (send_ack) {
        I2C_SDA_L();
    } else {
        I2C_SDA_H();
    }
    i2c_delay();
    I2C_SCL_H();
    i2c_delay();
    I2C_SCL_L();

    return data;
}

void i2c_bus_recovery(void) {
    I2C_SDA_H();
    for (uint8_t i = 0; i < 9; i++) {
        I2C_SCL_H();
        i2c_delay();
        I2C_SCL_L();
        i2c_delay();
    }
    i2c_stop();
}
