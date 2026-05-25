#include "m_ams5600.h"
#include "bsp_i2c.h"     // 接入我们自己的底层引脚控制
#include "bsp_systick.h" // 接入我们自己的延时 delay_ms

#define ams5600_ADDR                0x36
#define ams5600_reg_raw_ang_hi      0x0c

#if AMS_DIRVER_DATA_TYPE_INT
    #define RAW_ANGLE_TO_DEGREE(x)      ((int32_t)(x))
    #define DEGREE_DATA(x)              ((int32_t)(x) * 4095 / 360)
#else
    #define RAW_ANGLE_TO_DEGREE(x)      ((x) * (360.0f / 4095))
    #define DEGREE_DATA(x)              (x)
#endif

// ====================================================
// 补充：本地 I2C 接收单字节函数 (因为原来的 bsp_i2c 里没有)
// ====================================================
static void local_i2c_delay(void) {
    volatile uint32_t i = 1000; 
    while(i--);
}

// ack_en = 1 发送 ACK (继续读)；ack_en = 0 发送 NACK (不读了)
static uint8_t ams5600_i2c_read_byte(uint8_t ack_en) {
    uint8_t i, receive = 0;
    I2C_SDA_H(); // 主机释放SDA线
    local_i2c_delay();
    for (i = 0; i < 8; i++) {
        receive <<= 1;
        I2C_SCL_H();
        local_i2c_delay();
        if (I2C_SDA_READ()) {
            receive++;
        }
        I2C_SCL_L();
        local_i2c_delay();
    }
    // 发送应答信号
    if (ack_en) I2C_SDA_L(); // ACK
    else        I2C_SDA_H(); // NACK
    local_i2c_delay();
    I2C_SCL_H();
    local_i2c_delay();
    I2C_SCL_L();
    return receive;
}

// ====================================================
// 核心：读取原始角度寄存器 (完全使用裸机时序重写)
// ====================================================
uint8_t ams5600_get_raw_angle(uint16_t *ret_data)
{
    uint8_t high_byte, low_byte;

    // 1. 发送写地址，定位寄存器
    i2c_start();
    i2c_send_byte(ams5600_ADDR << 1); // 0x6C
    if (i2c_wait_ack() != 0) { i2c_stop(); return AMS_FAIL; }
    
    i2c_send_byte(ams5600_reg_raw_ang_hi); // 0x0C
    if (i2c_wait_ack() != 0) { i2c_stop(); return AMS_FAIL; }

    // 2. Restart，进入读模式
    i2c_start();
    i2c_send_byte((ams5600_ADDR << 1) | 1); // 0x6D
    if (i2c_wait_ack() != 0) { i2c_stop(); return AMS_FAIL; }

    // 3. 读取高 8 位并给 ACK，读取低 8 位并给 NACK
    high_byte = ams5600_i2c_read_byte(1);
    low_byte  = ams5600_i2c_read_byte(0);
    i2c_stop();

    *ret_data = ((uint16_t)high_byte << 8) | low_byte;
    return AMS_OK;
}

// 取平均值
uint8_t ams5600_get_raw_angle_average(uint16_t *ret_data)
{
    uint16_t data_temp;
    uint32_t sum = 0;
    for (int i = 0; i < 5; i++) {
        if (AMS_OK != ams5600_get_raw_angle(&data_temp)) return AMS_FAIL;
        sum += data_temp;
        delay_ms(1); // 替换 FreeRTOS 的 vTaskDelay
    }
    *ret_data = sum / 5;
    return AMS_OK;
}

// ====================================================
// 下面是角度计算业务逻辑 (剔除 OS 依赖)
// ====================================================
void ams5600_angle_inc_init(angle_inc_t *ainc, float rotor_c) {
    ainc->started = 0;
    ainc->enable = 1;
    ainc->raw_angle = 0;
    ainc->raw_angle_last = 0;
    ainc->angle_inc = 0;
    ainc->length_inc = 0;
    ainc->rotor_c = rotor_c;
}

void ams5600_angle_set_rotor_c(angle_inc_t *ainc, float rotor_c) {
    ainc->rotor_c = rotor_c;
    ams5600_angle_inc_clear(ainc);
}

uint8_t ams5600_angle_inc_calculation(angle_inc_t *ainc) {
    if (ainc->enable == 0) return 2; // 无效状态
    
    uint16_t raw_data;
    if (AMS_OK != ams5600_get_raw_angle(&raw_data)) return AMS_FAIL;
    
    ainc->raw_angle = RAW_ANGLE_TO_DEGREE(raw_data);
    if (ainc->started == 0) {
        ainc->started = 1;
        ainc->raw_angle_last = ainc->raw_angle;
        ainc->angle_inc = 0;
        ainc->length_inc = 0;
        return AMS_OK;
    }

    #define abs2(x, y)   ((x) > (y) ? (x) - (y) : (y) - (x))

    if (abs2(ainc->raw_angle_last, ainc->raw_angle) < DEGREE_DATA(200)) {
        ainc->angle_inc += ainc->raw_angle - ainc->raw_angle_last;                 
    } else {
        if (ainc->raw_angle_last > ainc->raw_angle) {
            ainc->angle_inc += ainc->raw_angle + (DEGREE_DATA(360) - ainc->raw_angle_last);
        } else {
            ainc->angle_inc += -(ainc->raw_angle_last + (DEGREE_DATA(360) - ainc->raw_angle));
        }
    }
    
    #if AMS_DIRVER_DATA_TYPE_INT
        float len_um = (ainc->angle_inc * (float)ainc->rotor_c / DEGREE_DATA(360)) * 1000;
        ainc->length_inc = (int32_t)len_um;
    #else
        ainc->length_inc = (ainc->angle_inc/360.0f)*ainc->rotor_c;
    #endif
    
    ainc->raw_angle_last = ainc->raw_angle;
    if (ainc->angle_inc > 0x0FFFFFFF || ainc->length_inc > 0x0FFFFFFF) {
        ainc->angle_inc = 0;
        ainc->length_inc = 0;
    }
    return AMS_OK;
}

void ams5600_angle_inc_clear(angle_inc_t *ainc) {
    ainc->angle_inc = 0;
    ainc->length_inc = 0;
}

void ams5600_angle_inc_stop(angle_inc_t *ainc) {
    ainc->enable = 0;
}

void ams5600_angle_inc_restart(angle_inc_t *ainc) {
    ainc->started = 0;
    ainc->enable = 1;
}
