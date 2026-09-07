#include <stdint.h>
#include "fmse_i2c.h"
#include "fmse_port.h"
#include "bsp_systick.h"
#include "bsp_watchdog.h"


/* 帧间延时 */
#ifndef FRAME_DELAY_I2C
#define FRAME_DELAY_I2C     5   /* ms */
#endif

/*
 * 防御加固: 接收缓冲区容量上限。
 * 本项目的调用方缓冲最大为 64 字节 (apdu_rbuf[64]/apdu_plain[64]/atr_buf[64]),
 * 而 SDK 帧协议的 I2C_MAX_LEN=1024 是为大缓冲(300B)宿主设计的。
 * 若 SE 返回超长帧, 直接判长度错误并释放总线, 防止栈溢出。
 */
#define FMSE_RECV_BUF_MAX   64U

static uint8_t gfm_I2CAddr;
static StSeI2CDriver *pgfm_I2CDrv = NULL;

/* 超时辅助函数  */
static uint32_t s_timeout_deadline;

static void fmse_init_timeout_ms(uint32_t timeout_ms)
{
    s_timeout_deadline = g_sys_tick_ms + timeout_ms;
}

static uint8_t fmse_check_timeout(void)
{
    return (g_sys_tick_ms >= s_timeout_deadline) ? 1 : 0;
}

/* 注册 I2C 驱动 */
void fm_i2c_drv_reg(void *fm_i2c_drv)
{
    pgfm_I2CDrv = (StSeI2CDriver *)fm_i2c_drv;
}

/* 注销 I2C 驱动 */
void fm_i2c_drv_unreg(void)
{
    pgfm_I2CDrv = NULL;
}

/* I2C 设备初始化 */
void fm_i2c_dev_init(void)
{
    if (pgfm_I2CDrv)
        pgfm_I2CDrv->fm_i2c_init();
}

/* 打开 I2C 设备 (上电) */
void fm_i2c_open_device(void)
{
    if (pgfm_I2CDrv)
    {
        gfm_I2CAddr = pgfm_I2CDrv->se_i2c_addr;
        pgfm_I2CDrv->fm_i2c_power_on();
    }
}

/* 关闭 I2C 设备 (断电) */
void fm_i2c_close_device(void)
{
    if (pgfm_I2CDrv)
    {
        gfm_I2CAddr = 0;
        pgfm_I2CDrv->fm_i2c_power_off();
    }
}

/*
 * 发送一帧数据到 I2C SE
 * 返回: 0-成功, 11-空指针, 12-等待 ACK 超时
 */
uint8_t fm_i2c_send_frame(uint8_t cmd, uint8_t *sbuf, uint16_t slen)
{
    FM_I2C_HEAD fm_i2c_hd;
    uint16_t i;
    uint8_t bcc;
    uint8_t ret;

    fm_i2c_hd.lenlo     = slen + 3;
    fm_i2c_hd.lenhi     = (slen + 3) >> 8;
    fm_i2c_hd.nad       = 0;
    fm_i2c_hd.flag.cmd  = cmd;

    if (!pgfm_I2CDrv)
        return (11);

    pgfm_I2CDrv->fm_i2c_start();

    /* 发送 SE 地址 (写) */
    ret = pgfm_I2CDrv->fm_i2c_send_addr(gfm_I2CAddr);
    if (ret)
        goto END;

    /* 发送帧头 (4字节: lenlo, lenhi, nad, cmd) */
    for (i = 0; i < 4; i++)
    {
        ret = pgfm_I2CDrv->fm_i2c_send_char(*(&fm_i2c_hd.lenlo + i));
        if (ret)
            goto END;
    }

    /* 计算 BCC 校验 */
    bcc = fm_i2c_hd.lenlo ^ fm_i2c_hd.lenhi ^ fm_i2c_hd.nad ^ fm_i2c_hd.flag.cmd;

    /* 发送数据域 */
    for (i = 0; i < slen; i++)
    {
        ret = pgfm_I2CDrv->fm_i2c_send_char(*(sbuf + i));
        bcc ^= sbuf[i];
        if (ret)
            goto END;
    }

    /* 发送 BCC 校验字节 */
    ret = pgfm_I2CDrv->fm_i2c_send_char(bcc);

END:
    /* 无论成功失败都释放总线, 防止共享 I2C 总线锁死 */
    pgfm_I2CDrv->fm_i2c_stop();
    delay_ms(FRAME_DELAY_I2C);
    return (ret);
}

/*
 * 从 I2C SE 接收一帧数据
 * 返回: 0-成功, 1-SE CRC 错误, 2-SE INS 错误, 0xF2-SE WTX 错误,
 *       11-空指针, 12-接收 ACK 错误, 13-长度异常, 14-BCC 校验错误
 */
uint8_t fm_i2c_recv_frame(uint8_t *rbuf, uint16_t *rlen)
{
    uint16_t i;
    uint16_t recvLen;
    uint8_t bcc;
    uint8_t ret;
    FM_I2C_HEAD fm_i2c_hd;

    *rlen = 0;

    if (!pgfm_I2CDrv)
        return (11);

    pgfm_I2CDrv->fm_i2c_start();

    /* 发送 SE 地址 (读) */
    ret = pgfm_I2CDrv->fm_i2c_send_addr(gfm_I2CAddr + 1);
    if (ret)
    {
        pgfm_I2CDrv->fm_i2c_stop();
        return (ret);
    }

    /* 接收长度字段 (2字节, 小端) */
    fm_i2c_hd.lenlo = pgfm_I2CDrv->fm_i2c_recv_char();
    fm_i2c_hd.lenhi = pgfm_I2CDrv->fm_i2c_recv_char();

    recvLen = (fm_i2c_hd.lenhi << 8) + fm_i2c_hd.lenlo;

    if (recvLen < I2C_MIN_LEN || recvLen > I2C_MAX_LEN)
    {
        *rlen = 0;
        pgfm_I2CDrv->fm_i2c_stop();
        return (13);
    }

    /* 接收 NAD 和状态字节 */
    fm_i2c_hd.nad       = pgfm_I2CDrv->fm_i2c_recv_char();
    fm_i2c_hd.flag.sta  = pgfm_I2CDrv->fm_i2c_recv_char();

    bcc = fm_i2c_hd.lenlo ^ fm_i2c_hd.lenhi ^ fm_i2c_hd.nad ^ fm_i2c_hd.flag.sta;

    /* 计算数据域长度 (总长度减去 nad + sta + bcc) */
    *rlen = recvLen - 3;

    /* 防御加固: 数据域长度超过调用方缓冲区上限时拒绝, 防止栈溢出 */
    if (*rlen > FMSE_RECV_BUF_MAX)
    {
        *rlen = 0;
        pgfm_I2CDrv->fm_i2c_stop();
        return (13);
    }

    /* 接收数据域 */
    for (i = 0; i < *rlen; i++)
    {
        rbuf[i] = pgfm_I2CDrv->fm_i2c_recv_char();
        bcc ^= rbuf[i];
    }

    /* 接收末尾 BCC 字节 (NAK 模式), 然后发 STOP */
    bcc ^= pgfm_I2CDrv->fm_i2c_recv_char_nak();
    pgfm_I2CDrv->fm_i2c_stop();

    if (bcc)
    {
        *rlen = 0;
        return (14);
    }

    delay_ms(FRAME_DELAY_I2C);

    return (fm_i2c_hd.flag.sta);
}

//从 I2C SE 获取 ATR
uint8_t fm_i2c_get_atr(uint8_t *rbuf, uint16_t *rlen)
{
    uint8_t ret;

    ret = fm_i2c_send_frame(I2C_CMD_GET_ATR, 0, 0);
    if (ret)
        return (ret);

    fmse_init_timeout_ms(POLL_TIMEOUT);
    do
    {
        ret = fm_i2c_recv_frame(rbuf, rlen);
        if (!ret)
            break;
        bsp_watchdog_feed();
    }
    while (!fmse_check_timeout());

    return (ret);
}
// 向 I2C SE 收发一帧数据
uint8_t fm_i2c_transceive(uint8_t *sbuf, uint16_t slen, uint8_t *rbuf, uint16_t *rlen,
                          uint16_t poll_inv, uint32_t poll_timeout)
{
    uint8_t ret;

    *rlen = 0;
    ret = fm_i2c_send_frame(I2C_CMD_IBLOCK, sbuf, slen);
    if (ret)
        return (ret);

    fmse_init_timeout_ms(poll_timeout);
    do
    {
        ret = fm_i2c_recv_frame(rbuf, rlen);

        if (ret == 12)          /* SE 未就绪, 等待后再轮询 */
        {
            delay_ms(poll_inv);
        }
        else if (ret == 0x04)   /* SE 内部错误, 跳过本轮 */
        {
            /* 空操作, 仅喂狗后继续轮询 */
        }
        else
            break;
        bsp_watchdog_feed();
    }
    while (!fmse_check_timeout());

    return (ret);
}

/* I2C SE 全局接口实例 (供上层 se_cmd.c 调用) */
StSeFunc gfm_se_i2c =
{
    SE_IF_I2C,
    fm_i2c_drv_reg,
    fm_i2c_dev_init,
    fm_i2c_open_device,
    fm_i2c_close_device,
    fm_i2c_get_atr,
    fm_i2c_transceive,
    fm_i2c_drv_unreg
};
