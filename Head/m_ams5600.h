#ifndef _M_AMS5600_H_
#define _M_AMS5600_H_

#include <stdint.h>

// 定义状态码，替换掉原来的 ESP_OK 和 ESP_FAIL
#define AMS_OK    0
#define AMS_FAIL  1

#define AMS_DIRVER_DATA_TYPE_INT  1

typedef struct {
    uint8_t started;
    uint8_t enable;
    float rotor_c;
    #if AMS_DIRVER_DATA_TYPE_INT
        int32_t raw_angle_last;
        int32_t raw_angle;
        volatile int32_t angle_inc;
        volatile int32_t length_inc;  
    #else
        float raw_angle_last;
        float raw_angle;
        volatile float angle_inc;
        volatile float length_inc;  
    #endif
} angle_inc_t;

// 函数声明，把原来的 esp_err_t 替换为 uint8_t
uint8_t ams5600_get_raw_angle(uint16_t *ret_data);
uint8_t ams5600_get_raw_angle_average(uint16_t *ret_data);

void ams5600_angle_inc_init(angle_inc_t *ainc, float rotor_c); 
void ams5600_angle_set_rotor_c(angle_inc_t *ainc, float rotor_c); 
uint8_t ams5600_angle_inc_calculation(angle_inc_t *ainc);
void ams5600_angle_inc_clear(angle_inc_t *ainc);
void ams5600_angle_inc_stop(angle_inc_t *ainc);
void ams5600_angle_inc_restart(angle_inc_t *ainc);

#endif
