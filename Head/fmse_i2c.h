#ifndef __FMSE_I2C_H
#define __FMSE_I2C_H

#include <stdint.h>
#include "se_cmd.h"

#define I2C_NAD             0x00
#define I2C_CMD_IBLOCK      0x02
#define I2C_CMD_GET_ATR     0x30
#define I2C_CMD_NAK         0xBA

#define I2C_MIN_LEN         3
#define I2C_MAX_LEN         1024

typedef struct {
    uint8_t lenlo;
    uint8_t lenhi;
    uint8_t nad;
    union {
        uint8_t cmd;
        uint8_t sta;
    } flag;
} FM_I2C_HEAD;

extern StSeFunc gfm_se_i2c;

#endif /* __FMSE_I2C_H */
