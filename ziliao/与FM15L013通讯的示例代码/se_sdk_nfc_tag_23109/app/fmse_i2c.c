
#include <stdint.h>
#include "fmse_i2c.h"
#include "port_i2c.h"
#include "timer.h"


/*variable of I2C SE addr */
static uint8_t gfm_I2CAddr;
/*struct point of I2C SE driver */
static StSeI2CDriver *pgfm_I2CDrv = NULL;


/********************************************************************
 * Function: register I2C driver
 * Input Parameter:
 * fm_i2c_drv: the struct point of I2C driver
 * Output Parameter:None
 * Return: None
 *********************************************************************/
void fm_i2c_drv_reg( void *fm_i2c_drv )
{
    pgfm_I2CDrv = (StSeI2CDriver *)fm_i2c_drv;
}


/********************************************************************
 * Function: unregister I2C driver
 * Input Parameter:None
 * Output Parameter:None
 * Return: None
 *********************************************************************/
void fm_i2c_drv_unreg( void )
{
    pgfm_I2CDrv = NULL;
}


/********************************************************************
 * Function: I2C device init
 * Input Parameter:None
 * Output Parameter:None
 * Return: None
 *********************************************************************/
void fm_i2c_dev_init( void )
{
    if ( pgfm_I2CDrv )
        pgfm_I2CDrv->fm_i2c_init();
}


/********************************************************************
 * Function: open I2C device
 * Input Parameter:None
 * Output Parameter:None
 * Return: None
 *********************************************************************/
void fm_i2c_open_device( void )
{
    if ( pgfm_I2CDrv )
    {
        gfm_I2CAddr = pgfm_I2CDrv->se_i2c_addr;
        pgfm_I2CDrv->fm_i2c_power_on();
    }
}


/********************************************************************
 * Function: close I2C device
 * Input Parameter:None
 * Output Parameter:None
 * Return: None
 *********************************************************************/
void fm_i2c_close_device( void )
{
    if ( pgfm_I2CDrv )
    {
        gfm_I2CAddr = 0;
        pgfm_I2CDrv->fm_i2c_power_off();
    }
}


/********************************************************************
 * Function: send a frame to I2C SE
 * Input Parameter:
 * fm_i2c_drv: the struct point of I2C driver
 * sbuf: send frame data
 * slen: send frame length
 * Output Parameter:None
 * Return: result
 * 0-success
 * 11-null point err
 * 12-wait ack timeout
 * other-RFU
 *********************************************************************/
uint8_t fm_i2c_send_frame( uint8_t cmd, uint8_t *sbuf, uint16_t slen )
{
    FM_I2C_HEAD fm_i2c_hd;
    uint16_t	i;
    uint8_t		bcc;
    uint8_t		ret;

    fm_i2c_hd.lenlo		= slen + 3;
    fm_i2c_hd.lenhi		= (slen + 3) >> 8;
    fm_i2c_hd.nad		= 0;
    fm_i2c_hd.flag.cmd	= cmd;

    if ( !pgfm_I2CDrv )
        return (11);

    pgfm_I2CDrv->fm_i2c_start();

    /*send se addr,write slaver */
    ret = pgfm_I2CDrv->fm_i2c_send_addr( gfm_I2CAddr );
    if ( ret )
        goto END;

    /*send pack head */
    for ( i = 0; i < 4; i++ )
    {
        ret = pgfm_I2CDrv->fm_i2c_send_char( *(&fm_i2c_hd.lenlo + i) );
        if ( ret )
            goto END;
    }

    /*calc bcc */
    bcc = fm_i2c_hd.lenlo ^ fm_i2c_hd.lenhi ^ fm_i2c_hd.nad ^ fm_i2c_hd.flag.cmd;

    /*send data */
    for ( i = 0; i < slen; i++ )
    {
        ret = pgfm_I2CDrv->fm_i2c_send_char( *(sbuf + i) );
        bcc ^= sbuf[i];
        if ( ret )
            goto END;
    }

    /*send bcc */
    ret = pgfm_I2CDrv->fm_i2c_send_char( bcc );

    pgfm_I2CDrv->fm_i2c_stop();

    /*debug */
#ifdef DEBUG_I2C
    printf("HEAD:%02x,%02x,LRC:%02x\r\n",fm_i2c_hd.lenhi,fm_i2c_hd.lenlo,bcc); 
#endif

    delayms( FRAME_DELAY_I2C );

END:
    return (ret);
}


/********************************************************************
 * Function: receive a frame from I2C SE
 * Input Parameter:None
 * Output Parameter:
 * rbuf: received frame data
 * rlen: received frame length
 * Return: result
 * 0-success
 * 1-SE CRC err
 * 2-SE INS err
 * 0xF2-SE WTX err
 * 11-null point err
 * 12-receive ack err
 * 13-received length err
 * 14-received bcc err
 * other-RFU
 *********************************************************************/
uint8_t fm_i2c_recv_frame( uint8_t *rbuf, uint16_t *rlen )
{
    uint16_t	i;
    uint16_t	recvLen;
    uint8_t		bcc;
    uint8_t		ret;
    FM_I2C_HEAD fm_i2c_hd;

    *rlen = 0;

    if ( !pgfm_I2CDrv )
        return (11);

    pgfm_I2CDrv->fm_i2c_start();

    /*send se addr,read slaver */
    ret = pgfm_I2CDrv->fm_i2c_send_addr( gfm_I2CAddr + 1 );
    if ( ret )
    {
        /*stop */
        pgfm_I2CDrv->fm_i2c_stop();
        return (ret);
    }

    /*recv length */
    fm_i2c_hd.lenlo = pgfm_I2CDrv->fm_i2c_recv_char();
    fm_i2c_hd.lenhi = pgfm_I2CDrv->fm_i2c_recv_char();

    recvLen = (fm_i2c_hd.lenhi << 8) + fm_i2c_hd.lenlo;

    if ( recvLen < I2C_MIN_LEN || recvLen > I2C_MAX_LEN )
    {
        *rlen = 0;
        return (13);
    }

    /*recv nad and sta */
    fm_i2c_hd.nad		= pgfm_I2CDrv->fm_i2c_recv_char();
    fm_i2c_hd.flag.sta	= pgfm_I2CDrv->fm_i2c_recv_char();

    bcc = fm_i2c_hd.lenlo ^ fm_i2c_hd.lenhi ^ fm_i2c_hd.nad ^ fm_i2c_hd.flag.sta;

    /*recv data length */
    *rlen = recvLen - 3;

    /*recv data */
    for ( i = 0; i < *rlen; i++ )
    {
        rbuf[i] = pgfm_I2CDrv->fm_i2c_recv_char();
        bcc		^= rbuf[i];
    }

#ifdef USE_ST_I2C


    /*
     * @Note:
     * The STM32 hardware I2C special process!
     * Must to Send stop first,then receive a byte!
     */
    /*stop */
    pgfm_I2CDrv->fm_i2c_stop();

    /*recv and check bcc */
    bcc ^= pgfm_I2CDrv->fm_i2c_recv_char();
#else
    /*recv and send NAK */
    bcc ^= pgfm_I2CDrv->fm_i2c_recv_char_nak();
    /*stop */
    pgfm_I2CDrv->fm_i2c_stop();
#endif

    if ( bcc )
    {
        *rlen = 0;
        return (14);
    }

    delayms( FRAME_DELAY_I2C );

    return (fm_i2c_hd.flag.sta);
}


/********************************************************************
 * Function: get ATR from I2C SE
 * Input Parameter:None
 * Output Parameter:
 * rbuf: received ATR data
 * rlen: received ATR length
 * Return: result
 * 0-success
 * 1-SE CRC err
 * 2-SE INS err
 * 0xF2-SE WTX err
 * 11-null point err
 * 12-receive ack err
 * 13-received length err
 * 14-received bcc err
 * other-RFU
 *********************************************************************/
uint8_t fm_i2c_get_atr( uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t ret;

    ret = fm_i2c_send_frame( I2C_CMD_GET_ATR, 0, 0 );
    if ( ret )
        return (ret);

    init_timeout_ms( POLL_TIMEOUT );
    do
    {
        ret = fm_i2c_recv_frame( rbuf, rlen );
        if ( !ret )
            break;
    }
    while ( !check_timeout_ms() );

    return (ret);
}


/********************************************************************
 * Function: transcieve a frame to I2C SE
 * Input Parameter:
 * sbuf: send frame data
 * slen: send frame length
 * timeout: timeout for wait receive a frame
 * Output Parameter:
 * rbuf: received frame data
 * rlen: received frame length
 * Return: result
 * 0-success
 * 1-SE CRC err
 * 2-SE INS err
 * 0xF2-SE WTX err
 * 11-null point err
 * 12-receive ack err
 * 13-received length err
 * 14-received bcc err
 * other-RFU
 *********************************************************************/
uint8_t fm_i2c_transceive( uint8_t *sbuf, uint16_t slen, uint8_t *rbuf, uint16_t *rlen,
                           uint16_t poll_inv, uint32_t poll_timeout )
{
    uint8_t ret;

    /*debug */
#ifdef DEBUG_I2C_SEND
	dump_data(slen,sbuf);
#endif

    *rlen	= 0;
    ret		= fm_i2c_send_frame( I2C_CMD_IBLOCK, sbuf, slen );
#ifdef DEBUG_I2C
    printf( "fm_i2c_send_frame_ret=%02x\r\n", ret );
#endif
    if ( ret )
        return (ret);


    init_timeout_ms( poll_timeout );
    do
    {
        ret = fm_i2c_recv_frame( rbuf, rlen );

        if ( ret == 12 )        /*timeout */
        {
            delayms( poll_inv );
        }
        else if ( ret == 0x04 ) /*se err */
        {
            continue;
        }
        else
            break;
    }
    while ( !check_timeout_ms() );

#ifdef DEBUG_I2C
	 dump_data(*rlen,rbuf);
#endif

    return (ret);
}


/*global variable of I2C SE */
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

