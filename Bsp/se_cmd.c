#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "se_cmd.h"
#include "fmse_i2c.h"
#include "fmse_port.h"
#include "des.h"

/* SDK 类型别名 (原 se_app.h / stm32f10x.h 提供) */
typedef uint8_t  u8;
typedef uint16_t u16;

/*
 * FMSE 安全芯片 APDU 命令层 (移植自复旦微电子 SDK)
 *
 * 架构:
 * +--------------+
 * |  APP Layer   |  ← bsp_crypto.c (本项目业务层)
 * +--------------+
 * | CA Cmd Layer |  ← se_cmd.c (本文件, APDU 命令构造)
 * +--------------+
 * |Protocol Layer|  ← fmse_i2c.c (I2C 帧封装/轮询)
 * +--------------+
 * | Driver Layer |  ← fmse_port.c → bsp_i2c.c (软件 I2C)
 * +--------------+
 *
 * 重要说明:
 *   本层仅负责构造 APDU 命令并通过协议层发送给 FMSE 芯片。
 *   真正的加密运算 (3DES/AES/RSA/SM2 等) 由 FMSE 芯片内部完成,
 *   芯片固件由复旦微电子官方预烧录, MCU 侧仅发送指令和接收结果。
 *   其中 des.c 的 3DES 仅用于 MCU 侧的认证报文构造和 session_key 生成,
 *   数据加密本身完全由 SE 芯片完成。
 */

/* 全局变量 */
static StApduPack  gfm_SeCmdHand;
static StSeFunc   *pgfm_SeFunc = NULL;
static uint32_t auth_cnt = 0;

/*
* MCUUID=1122334455667788
* MCU KEY:CC5B14B6E2F25929649FA55C726BAED5
*/
static uint8_t mcu_uid[8]={0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
static uint8_t mcu_com_key[16]={0xCC,0x5B,0x14,0xB6,0xE2,0xF2,0x59,0x29,
                                0x64,0x9F,0xA5,0x5C,0x72,0x6B,0xAE,0xD5};
static uint8_t apdu_padding[8]={0x80,0,0,0,0,0,0,0};
static uint8_t apdu_cipher[32];
static uint8_t apdu_plain[64];  /* SE 返回密文可能超过 32 字节, 扩大到 64 */
static uint8_t apdu_rbuf[64];   /* SE 响应缓冲区, 同上 */
static uint16_t apdu_rlen;
uint8_t session_key[16];

/* 内存设置 */
void* fm_memset( void* dst, int val, size_t count )
{
    char * tmpdst = (char *) dst;
    char tmpval   = (char) val;

    if ( dst == NULL || !count )
        return (0);

    while ( count-- )
    {
        *tmpdst++ = tmpval;
    }

    return (dst);
}

/* 内存拷贝 (支持重叠区域) */
void* fm_memmove( void* dst, const void* src, size_t count )
{
    char* tmpdst = (char *) dst;
    char* tmpsrc = (char *) src;

    if ( dst == NULL || src == NULL || !count )
        return (0);

    if ( tmpdst <= tmpsrc || tmpdst >= tmpsrc + count )
    {
        while ( count-- )
        {
            *tmpdst++ = *tmpsrc++;
        }
    }
    else
    {
        tmpdst = tmpdst + count - 1;
        tmpsrc = tmpsrc + count - 1;
        while ( count-- )
        {
            *tmpdst-- = *tmpsrc--;
        }
    }

    return (dst);
}

/* 填充并加密 (命令传输保护) */
void cmd_data_wrap(u8 *inbuf, u8 inlen, u8 *keybuf, u8 keylen, u8 *outbuf, u8 *outlen)
{
    u8 padding_num;
    u8 apdu_padding[8]={0x80,0,0,0,0,0,0,0};
    u8 apdu_cipher[32];

    /* ISO 9797-1 Method 2 填充 */
    padding_num = 8 - inlen%8;
    if(padding_num){
        fm_memmove( outbuf, inbuf, inlen);
        fm_memmove( outbuf + inlen, apdu_padding, padding_num );
    }
    *outlen = inlen+padding_num;

    des3_ecb_encrypt( apdu_cipher, outbuf, *outlen, keybuf, keylen );

    fm_memmove( outbuf, apdu_cipher, *outlen );
}

void response_data_unwrap(u8 *inbuf, u8 inlen, u8 *keybuf, u8 keylen, u8 *outbuf, u16 *outlen)
{
    u8 tempbuf[64];

    if ( inlen == 0 || inlen > sizeof(tempbuf) )
    {
        *outlen = 0;
        return;
    }

    des3_ecb_decrypt( tempbuf, inbuf, inlen, keybuf, 16 );
    /* 从后向前查找 0x80 填充字节, 确定明文边界 */
    while(inlen > 0)
    {
        inlen --;
        if ( tempbuf[inlen] == 0x80)
        {
            break;
        }
    }

    fm_memmove( outbuf, tempbuf, inlen );
    *outlen = inlen;
}

/* 注册 SE 接口和驱动 */
StSeFunc *fm_se_register( StSeFunc *fm_se )
{
    pgfm_SeFunc = fm_se;
    if ( pgfm_SeFunc )
    {
        if ( pgfm_SeFunc->se_name == SE_IF_I2C )
        {
            pgfm_SeFunc->fm_driver_register( &gusr_i2c_drv );
        }
    }

    return (pgfm_SeFunc);
}

/* 注销 SE 接口和驱动 */
void fm_se_unregister( void )
{
    if ( pgfm_SeFunc )
        pgfm_SeFunc->fm_driver_unregister();
    pgfm_SeFunc = NULL;
}

/* 检查 I2C 传输结果, 转换为 SE 错误码 */
uint16_t fm_chk_result( uint8_t result )
{
    uint16_t SW;

    switch ( result )
    {
    case 1:
        SW = SE_ERR_CRC;
        break;
    case 2:
        SW = SE_ERR_INS;
        break;
    case 11:
        SW = IF_ERR_NULL_POINT;
        break;
    case 12:
        SW = IF_ERR_RECV_ACK;
        break;
    case 13:
        SW = IF_ERR_LENGTH;
        break;
    case 14:
        SW = IF_ERR_LRC;
        break;
    default:
        SW = IF_ERR_OTHER;
        break;
    }

    return (SW);
}

/* 获取标签 UID */
uint16_t get_tag_uid( TAG_CHN para, uint8_t i2cAddr, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x77;
    gfm_SeCmdHand.p1    = para>>8;
    gfm_SeCmdHand.p2    = para;
    gfm_SeCmdHand.p3.lc = 0x01;
    gfm_SeCmdHand.capdu[0] = i2cAddr;

    cmd_data_wrap( gfm_SeCmdHand.capdu, gfm_SeCmdHand.p3.lc, session_key, 16, gfm_SeCmdHand.capdu, & gfm_SeCmdHand.p3.lc);
    slen = 5 + gfm_SeCmdHand.p3.lc;

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    if(SW != 0x9000)
        return SW;

    response_data_unwrap(apdu_rbuf, apdu_rlen - 2, session_key, 16, rbuf, rlen);

    return (SW);
}

/* 设置标签读卡器 */
uint16_t set_tag_reader( uint8_t rst, uint8_t wrmode, uint16_t inlen, uint8_t *inbuf, uint8_t i2cAddr, uint8_t rstmode, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x70;
    gfm_SeCmdHand.p1    = rst;
    gfm_SeCmdHand.p2    = wrmode;
    gfm_SeCmdHand.p3.lc = inlen + 2;
    fm_memmove( gfm_SeCmdHand.capdu, inbuf, inlen );
    gfm_SeCmdHand.capdu[inlen] = i2cAddr;
    gfm_SeCmdHand.capdu[inlen + 1] = rstmode;

    cmd_data_wrap( gfm_SeCmdHand.capdu, gfm_SeCmdHand.p3.lc, session_key, 16, gfm_SeCmdHand.capdu, & gfm_SeCmdHand.p3.lc);
    slen = 5 + gfm_SeCmdHand.p3.lc;

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    if(SW != 0x9000)
        return SW;

    response_data_unwrap(apdu_rbuf, apdu_rlen - 2, session_key, 16, rbuf, rlen);

    return (SW);
}

/* 标签激活 */
uint16_t tag_active( TAG_ROOT_KEY para, uint8_t i2cAddr, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x75;
    gfm_SeCmdHand.p1    = para>>8;
    gfm_SeCmdHand.p2    = para;
    gfm_SeCmdHand.p3.lc = 0x01;
    gfm_SeCmdHand.capdu[0] = i2cAddr;

    cmd_data_wrap( gfm_SeCmdHand.capdu, gfm_SeCmdHand.p3.lc, session_key, 16, gfm_SeCmdHand.capdu, & gfm_SeCmdHand.p3.lc);
    slen = 5 + gfm_SeCmdHand.p3.lc;

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    if(SW != 0x9000)
        return SW;

    response_data_unwrap(apdu_rbuf, apdu_rlen - 2, session_key, 16, rbuf, rlen);

    return (SW);
}

/* 写入标签数据 */
uint16_t write_tag_data( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x7F;
    gfm_SeCmdHand.p1    = para>>8;
    gfm_SeCmdHand.p2    = para;
    gfm_SeCmdHand.p3.lc = inlen;
    fm_memmove( gfm_SeCmdHand.capdu, inbuf, inlen );

    cmd_data_wrap( gfm_SeCmdHand.capdu, gfm_SeCmdHand.p3.lc, session_key, 16, gfm_SeCmdHand.capdu, & gfm_SeCmdHand.p3.lc);
    slen = 5 + gfm_SeCmdHand.p3.lc;

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *)&gfm_SeCmdHand, slen, rbuf, rlen, interval, timeout );
        if ( !result )
            SW = rbuf[*rlen - 2] << 8 | rbuf[*rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    return (SW);
}

/* 读取标签数据 */
uint16_t get_tag_data( uint16_t para, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x79;
    gfm_SeCmdHand.p1    = para>>8;
    gfm_SeCmdHand.p2    = para;
    gfm_SeCmdHand.p3.lc = 0;

    cmd_data_wrap( gfm_SeCmdHand.capdu, gfm_SeCmdHand.p3.lc, session_key, 16, gfm_SeCmdHand.capdu, & gfm_SeCmdHand.p3.lc);
    slen = 5 + gfm_SeCmdHand.p3.lc;

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    if(SW != 0x9000)
        return SW;

    response_data_unwrap(apdu_rbuf, apdu_rlen - 2, session_key, 16, rbuf, rlen);

    return (SW);
}

/* 递减标签生命周期 */
uint16_t dec_tag_life_count( uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x7D;
    gfm_SeCmdHand.p1    = 0;
    gfm_SeCmdHand.p2    = 0;
    gfm_SeCmdHand.p3.lc = inlen;
    fm_memmove( gfm_SeCmdHand.capdu, inbuf, inlen );

    cmd_data_wrap( gfm_SeCmdHand.capdu, gfm_SeCmdHand.p3.lc, session_key, 16, gfm_SeCmdHand.capdu, & gfm_SeCmdHand.p3.lc);
    slen = 5 + gfm_SeCmdHand.p3.lc;

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    if(SW != 0x9000)
        return SW;

    response_data_unwrap(apdu_rbuf, apdu_rlen - 2, session_key, 16, rbuf, rlen);

    return (SW);
}

/* 获取标签生命周期 */
uint16_t get_tag_life_count( uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x7B;
    gfm_SeCmdHand.p1    = 0;
    gfm_SeCmdHand.p2    = 0;
    gfm_SeCmdHand.p3.lc = 0;

    cmd_data_wrap( gfm_SeCmdHand.capdu, gfm_SeCmdHand.p3.lc, session_key, 16, gfm_SeCmdHand.capdu, & gfm_SeCmdHand.p3.lc);
    slen = 5 + gfm_SeCmdHand.p3.lc;

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    if(SW != 0x9000)
        return SW;

    response_data_unwrap(apdu_rbuf, apdu_rlen - 2, session_key, 16, rbuf, rlen);

    return (SW);
}

/* SE 认证第一步: 发送随机数挑战 */
uint16_t se_auth1( uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x71;
    gfm_SeCmdHand.p1    = 0;
    gfm_SeCmdHand.p2    = 0;
    gfm_SeCmdHand.p3.lc = 0x10;
    fm_memmove( gfm_SeCmdHand.capdu, inbuf, inlen );
    slen = 5 + inlen;

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, rbuf, rlen, interval, timeout );
        if ( !result )
            SW = rbuf[*rlen - 2] << 8 | rbuf[*rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    return (SW);
}

/* SE 认证第二步: 发送密钥确认 */
uint16_t se_auth2( uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;

    gfm_SeCmdHand.cla   = 0x84;
    gfm_SeCmdHand.ins   = 0x73;
    gfm_SeCmdHand.p1    = 0x01;
    gfm_SeCmdHand.p2    = 0;
    gfm_SeCmdHand.p3.lc = 0x08;
    fm_memmove( gfm_SeCmdHand.capdu, inbuf, inlen );
    slen = 5 + inlen;

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, rbuf, rlen, interval, timeout );
        if ( !result )
            SW = rbuf[*rlen - 2] << 8 | rbuf[*rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    return (SW);
}

/* MCU 与 SE 双向认证, 生成 session_key */
uint16_t mcu_l013_mutual_auth( uint8_t *rbuf, uint16_t *rlen )
{
    uint16_t slen = 0;
    uint16_t sw;
    uint8_t mcu_rnd[16];
    uint8_t i;
    uint8_t recv_buf[32];
    uint16_t recv_len;
    uint8_t tmpkey[16];

    *rlen = 0;
    slen = 16;

    srand(auth_cnt);
    if(auth_cnt++ > RAND_MAX)
        auth_cnt = 0;
    for( i = 0 ; i < slen ; i++ ) {
        mcu_rnd[i] = rand();
    }

    memcpy(mcu_rnd, mcu_uid, 8);
    sw = se_auth1(slen, mcu_rnd, recv_buf, &recv_len);
    if(sw != 0x9000)
        return sw;

    /* 3DES 解密 R1', 用于验证 SE 响应 */
    des3_ecb_decrypt(rbuf, recv_buf, 8, mcu_com_key, 16);
    if(memcmp(rbuf, mcu_rnd+8, 8))
        return 0xffce;

    /* 保存 R1' 到 tmpkey 前半部分 */
    fm_memmove(tmpkey, recv_buf, 8);

    /* 3DES 加密 R2, 生成 tmpkey 后半部分 R2' */
    des3_ecb_encrypt(tmpkey+8, recv_buf+8, 8, mcu_com_key, 16);

    slen = 8;
    sw = se_auth2(slen, tmpkey+8, recv_buf, &recv_len);
    if(sw != 0x9000)
        return sw;

    /* 3DES 加密 tmpkey, 生成最终 session_key */
    des3_ecb_encrypt(rbuf, tmpkey, 16, mcu_com_key, 16);
    *rlen = 16;

    return (sw);
}

/* 获取 SE 芯片 UID */
uint16_t get_se_uid( uint16_t para, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x72;
    gfm_SeCmdHand.p1    = para>>8;
    gfm_SeCmdHand.p2    = para;
    gfm_SeCmdHand.p3.lc = 8;
    slen = 5;

    des3_ecb_encrypt(apdu_cipher, apdu_padding, 8, session_key, 16);
    fm_memmove( gfm_SeCmdHand.capdu, apdu_cipher, 8 );
    slen = 5 + 8;

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    if(SW != 0x9000)
        return SW;

    if(apdu_rlen-2 > sizeof(apdu_plain))
        return IF_ERR_LENGTH;

    des3_ecb_decrypt(apdu_plain, apdu_rbuf, apdu_rlen-2, session_key, 16);
    fm_memmove(rbuf, apdu_plain, apdu_rlen-2-1);
    *rlen = apdu_rlen-2-1;

    return (SW);
}

/* SE 激活 */
uint16_t se_active( uint16_t para, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x74;
    gfm_SeCmdHand.p1    = para>>8;
    gfm_SeCmdHand.p2    = para;
    gfm_SeCmdHand.p3.lc = 8;
    slen = 5;

    des3_ecb_encrypt(apdu_cipher, apdu_padding, 8, session_key, 16);
    fm_memmove( gfm_SeCmdHand.capdu, apdu_cipher, 8 );
    slen = 5 + 8;

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    if(SW != 0x9000)
        return SW;

    if(apdu_rlen-2 > sizeof(apdu_plain))
        return IF_ERR_LENGTH;

    des3_ecb_decrypt(apdu_plain, apdu_rbuf, apdu_rlen-2, session_key, 16);
    fm_memmove(rbuf, apdu_plain, apdu_rlen-2-1);
    *rlen = apdu_rlen-2-1;

    return (SW);
}

/*
 * 向 SE 发送数据 (SE 内部执行加密运算)
 *
 * 流程: MCU 侧用 session_key 加密 APDU 命令 → 发送给 SE → SE 解密并执行
 * APDU: CLA=80 INS=76 P1P2=para Lc=inlen Data=明文(已用 session_key 包装)
 * 注: capdu 中的 des3_ecb_encrypt 是命令传输保护, 不是数据加密, 数据加密由 SE 完成
 */
uint16_t write_se_data( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;
    uint8_t padding_num;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x76;
    gfm_SeCmdHand.p1    = para>>8;
    gfm_SeCmdHand.p2    = para;
    gfm_SeCmdHand.p3.lc = inlen;
    fm_memmove( gfm_SeCmdHand.capdu, inbuf, inlen );
    slen = 5 + inlen;

    /* ISO 9797-1 Method 2 填充 */
    padding_num = 8 - inlen%8;
    if(padding_num){
        fm_memmove(gfm_SeCmdHand.capdu+inlen, apdu_padding, padding_num);
        gfm_SeCmdHand.p3.lc = inlen+padding_num;
        slen = 5+inlen+padding_num;
    }

    des3_ecb_encrypt(apdu_cipher, (uint8_t *)&gfm_SeCmdHand.capdu, inlen+padding_num, session_key, 16);
    fm_memmove( (uint8_t *)&gfm_SeCmdHand.capdu, apdu_cipher, inlen+padding_num );

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *)&gfm_SeCmdHand, slen, rbuf, rlen, interval, timeout );
        if ( !result )
            SW = rbuf[*rlen - 2] << 8 | rbuf[*rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    return (SW);
}

/*
 * 从 SE 读取数据 (SE 返回加密运算结果)
 *
 * 流程: MCU 侧用 session_key 加密 APDU 命令 → 发送给 SE → SE 返回结果 → MCU 用 session_key 解密
 * APDU: CLA=80 INS=78 P1P2=para Lc=inlen Data=请求参数(已用 session_key 包装)
 * 注: capdu 中的 des3_ecb_encrypt 是命令传输保护, 不是数据加密
 *     SE 返回的密文在 rbuf 中, des3_ecb_decrypt 解密的是传输保护层, 取出的是 SE 的加密结果
 */
uint16_t get_se_data( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;
    uint8_t padding_num;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x78;
    gfm_SeCmdHand.p1    = para>>8;
    gfm_SeCmdHand.p2    = para;
    gfm_SeCmdHand.p3.lc = inlen;
    slen = 5;
    memcpy((uint8_t *)&gfm_SeCmdHand.capdu, inbuf, inlen);
    slen += inlen;

    /* ISO 9797-1 Method 2 填充 */
    padding_num = 8 - inlen%8;
    if(padding_num){
        fm_memmove(gfm_SeCmdHand.capdu+inlen, apdu_padding, padding_num);
        gfm_SeCmdHand.p3.lc = inlen+padding_num;
        slen = 5+inlen+padding_num;
    }

    des3_ecb_encrypt(apdu_cipher, (uint8_t *)&gfm_SeCmdHand.capdu, inlen+padding_num, session_key, 16);
    fm_memmove( (uint8_t *)&gfm_SeCmdHand.capdu, apdu_cipher, inlen+padding_num );

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    if(SW != 0x9000)
        return SW;

    if(apdu_rlen-2 > sizeof(apdu_plain))
        return IF_ERR_LENGTH;

    des3_ecb_decrypt(apdu_plain, apdu_rbuf, apdu_rlen-2, session_key, 16);
    fm_memmove(rbuf, apdu_plain, apdu_rlen-2);
    *rlen = apdu_rlen-2;

    return (SW);
}

/* 递减 SE 生命周期 */
uint16_t dec_se_life_count( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;
    uint8_t padding_num;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x7C;
    gfm_SeCmdHand.p1    = para>>8;
    gfm_SeCmdHand.p2    = para;
    gfm_SeCmdHand.p3.lc = inlen;
    fm_memmove( gfm_SeCmdHand.capdu, inbuf, inlen );
    slen = 5 + inlen;

    /* ISO 9797-1 Method 2 填充 */
    padding_num = 8 - inlen%8;
    if(padding_num){
        fm_memmove(gfm_SeCmdHand.capdu+inlen, apdu_padding, padding_num);
        gfm_SeCmdHand.p3.lc = inlen+padding_num;
        slen = 5+inlen+padding_num;
    }

    des3_ecb_encrypt(apdu_cipher, (uint8_t *)&gfm_SeCmdHand.capdu, inlen+padding_num, session_key, 16);
    fm_memmove( (uint8_t *)&gfm_SeCmdHand.capdu, apdu_cipher, inlen+padding_num );

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    if(SW != 0x9000)
        return SW;

    *rlen = apdu_rlen-2;

    return (SW);
}

/* 获取 SE 生命周期 */
uint16_t get_se_life_count( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;
    uint8_t padding_num;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x7A;
    gfm_SeCmdHand.p1    = para>>8;
    gfm_SeCmdHand.p2    = para;
    gfm_SeCmdHand.p3.lc = inlen;
    slen = 5;
    memcpy((uint8_t *)&gfm_SeCmdHand.capdu, inbuf, inlen);
    slen += inlen;

    /* ISO 9797-1 Method 2 填充 */
    padding_num = 8 - inlen%8;
    if(padding_num){
        fm_memmove(gfm_SeCmdHand.capdu+inlen, apdu_padding, padding_num);
        gfm_SeCmdHand.p3.lc = inlen+padding_num;
        slen = 5+inlen+padding_num;
    }

    des3_ecb_encrypt(apdu_cipher, (uint8_t *)&gfm_SeCmdHand.capdu, inlen+padding_num, session_key, 16);
    fm_memmove( (uint8_t *)&gfm_SeCmdHand.capdu, apdu_cipher, inlen+padding_num );

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    if(SW != 0x9000)
        return SW;

    if(apdu_rlen-2 > sizeof(apdu_plain))
        return IF_ERR_LENGTH;

    des3_ecb_decrypt(apdu_plain, apdu_rbuf, apdu_rlen-2, session_key, 16);
    fm_memmove(rbuf, apdu_plain, apdu_rlen-2-4);
    *rlen = apdu_rlen-2-4;

    return (SW);
}

/* 设置 SE OTP 状态 */
uint16_t set_se_otp_status( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t     result      = 0;
    uint16_t    slen        = 0;
    uint16_t    SW          = IF_ERR_NULL_POINT;
    uint16_t    interval    = POLL_INTERVAL;
    uint32_t    timeout     = POLL_TIMEOUT;
    uint8_t padding_num;

    gfm_SeCmdHand.cla   = 0x80;
    gfm_SeCmdHand.ins   = 0x7E;
    gfm_SeCmdHand.p1    = para>>8;
    gfm_SeCmdHand.p2    = para;
    gfm_SeCmdHand.p3.lc = inlen;
    slen = 5;
    memcpy((uint8_t *)&gfm_SeCmdHand.capdu, inbuf, inlen);
    slen += inlen;

    /* ISO 9797-1 Method 2 填充 */
    padding_num = 8 - inlen%8;
    if(padding_num){
        fm_memmove(gfm_SeCmdHand.capdu+inlen, apdu_padding, padding_num);
        gfm_SeCmdHand.p3.lc = inlen+padding_num;
        slen = 5+inlen+padding_num;
    }

    des3_ecb_encrypt(apdu_cipher, (uint8_t *)&gfm_SeCmdHand.capdu, inlen+padding_num, session_key, 16);
    fm_memmove( (uint8_t *)&gfm_SeCmdHand.capdu, apdu_cipher, inlen+padding_num );

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    if(SW != 0x9000)
        return SW;

    if(apdu_rlen-2 > sizeof(apdu_plain))
        return IF_ERR_LENGTH;

    des3_ecb_decrypt(apdu_plain, apdu_rbuf, apdu_rlen-2, session_key, 16);
    fm_memmove(rbuf, apdu_plain, apdu_rlen-2-1);
    *rlen = apdu_rlen-2-1;

    return (SW);
}

/***********************EOF********************************/
