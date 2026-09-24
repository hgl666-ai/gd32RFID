#include <stdio.h>
#include <stdlib.h>
#include "se_app.h"
#include "se_cmd.h"
#include "fmse_i2c.h"
#include "port_i2c.h"
#include "des.h"


/*
 * fm cos api
 * +--------------+
 * |  APP Layer   |
 * +--------------+
 * | CA Cmd Layer |
 * +--------------+
 * |Protocol Layer|
 * +--------------+
 * | Driver Layer |
 * +--------------+
 */

extern void dump_data( uint16_t len, uint8_t * buf );


extern void StrToHex( BYTE *pbDest, BYTE *pbSrc, int nLen );


extern unsigned char *base64_encode( unsigned char *str, uint16_t inlen, uint8_t *rbuf );


/*global variable */
static StApduPack	gfm_SeCmdHand;
static StSeFunc		*pgfm_SeFunc = NULL;
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
static uint8_t apdu_plain[32];
static uint8_t apdu_rbuf[32];
static uint16_t apdu_rlen;
uint8_t session_key[16];

/*set mem */
void* fm_memset( void* dst, int val, size_t count )
{
    char	* tmpdst	= (char *) dst;
    char	tmpval		= (char) val;

    if ( dst == NULL || !count )
        return (0);

    while ( count-- )
    {
        *tmpdst++ = tmpval;
    }

    return (dst);
}


/*mem data copy */
void* fm_memmove( void* dst, const void* src, size_t count )
{
    char* tmpdst	= (char *) dst;
    char* tmpsrc	= (char *) src;

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
        tmpdst	= tmpdst + count - 1;
        tmpsrc	= tmpsrc + count - 1;
        while ( count-- )
        {
            *tmpdst-- = *tmpsrc--;
        }
    }

    return (dst);
}

/* padding and encrypt */
void cmd_data_wrap(u8 *inbuf, u8 inlen, u8 *keybuf , u8 keylen, u8 *outbuf, u8 *outlen)
{
    u8 padding_num;
    u8 apdu_padding[8]={0x80,0,0,0,0,0,0,0};
    u8 apdu_cipher[32];

    //add padding 80
    padding_num = 8 - inlen%8;
    if(padding_num){
        fm_memmove( outbuf, inbuf, inlen);
        fm_memmove( outbuf + inlen, apdu_padding, padding_num );
    }
    *outlen = inlen+padding_num;

    des3_ecb_encrypt( apdu_cipher, outbuf, *outlen, keybuf, keylen );

    fm_memmove( outbuf, apdu_cipher, *outlen );
}

void response_data_unwrap(u8 *inbuf, u8 inlen, u8 *keybuf , u8 keylen, u8 *outbuf, u16 *outlen)
{
    u8 tempbuf[32];

    if ( inlen == 0 )
    {
        *outlen = 0;
        return;
    }

    des3_ecb_decrypt( tempbuf, inbuf, inlen, keybuf, 16 );
    //find pad "0x80"
    while(inlen --)
    {
        if ( tempbuf[inlen] == 0x80)
        {
            break;
        }
    }
    
    fm_memmove( outbuf, tempbuf, inlen );
    *outlen = inlen;

}


/********************************************************************
 * Function: register SE interface and driver
 * Input Parameter:
 * fm_se: struct point of SE interface
 * Output Parameter:None
 * Return: struct point of SE interface
 *********************************************************************/
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


/********************************************************************
 * Function: unregister SE interface and driver
 * Input Parameter:None
 * Output Parameter:None
 * Return: None
 *********************************************************************/
void fm_se_unregister( void )
{
    if ( pgfm_SeFunc )
        pgfm_SeFunc->fm_driver_unregister();
    pgfm_SeFunc = NULL;
}


/********************************************************************
 * Function: check error result and return error code for user
 * Input Parameter:
 * result: input error result
 * Output Parameter:None
 * Return: error code for user
 *********************************************************************/
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

/********************************************************************
 * Function: get_tag_uid
 * Input Parameter:
 * para: command param of TAG_CHN
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t get_tag_uid( TAG_CHN para, uint8_t i2cAddr, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x77;
    gfm_SeCmdHand.p1	= para>>8;
    gfm_SeCmdHand.p2	= para;
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

    //no tag!
    if(SW != 0x9000)
        return SW;

    response_data_unwrap(apdu_rbuf, apdu_rlen - 2, session_key, 16, rbuf, rlen);

    return (SW);
}

/********************************************************************
 * Function: set_tag_reader
 * Input Parameter:
 * rst: reset or no rst
 * mode: write or read
 * inlen: the input data length
 * inbuf: the input data buffer
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t set_tag_reader( uint8_t rst, uint8_t wrmode, uint16_t inlen, uint8_t *inbuf, uint8_t i2cAddr, uint8_t rstmode, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x70;
    gfm_SeCmdHand.p1	= rst;
    gfm_SeCmdHand.p2	= wrmode;
    gfm_SeCmdHand.p3.lc = inlen + 2;
    fm_memmove( gfm_SeCmdHand.capdu, inbuf, inlen );
    gfm_SeCmdHand.capdu[inlen] = i2cAddr;
    gfm_SeCmdHand.capdu[inlen + 1] = rstmode;

    cmd_data_wrap( gfm_SeCmdHand.capdu, gfm_SeCmdHand.p3.lc, session_key, 16, gfm_SeCmdHand.capdu, & gfm_SeCmdHand.p3.lc);
    slen = 5 + gfm_SeCmdHand.p3.lc;
    dump_data(slen, (uint8_t *) &gfm_SeCmdHand);

    if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

    //no tag!
    if(SW != 0x9000)
        return SW;

    response_data_unwrap(apdu_rbuf, apdu_rlen - 2, session_key, 16, rbuf, rlen);

    return (SW);
}

/********************************************************************
 * Function: tag_active
 * Input Parameter:
 * para: command param of TAG_ROOT_KEY
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t tag_active( TAG_ROOT_KEY para, uint8_t i2cAddr, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x75;
    gfm_SeCmdHand.p1	= para>>8;
    gfm_SeCmdHand.p2	= para;
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

	//no tag!
	if(SW != 0x9000)
		return SW;

    response_data_unwrap(apdu_rbuf, apdu_rlen - 2, session_key, 16, rbuf, rlen);

    return (SW);
}

/********************************************************************
 * Function: write_tag_data
 * Input Parameter:
 * para: command param
 * inlen: the input data length
 * inbuf: the input data buffer
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t write_tag_data( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x7F;
    gfm_SeCmdHand.p1	= para>>8;
    gfm_SeCmdHand.p2	= para;
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

/********************************************************************
 * Function: get_tag_data
 * Input Parameter:
 * para: command param
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t get_tag_data( uint16_t para, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x79;
    gfm_SeCmdHand.p1	= para>>8;
    gfm_SeCmdHand.p2	= para;
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

    //no tag!
    if(SW != 0x9000)
        return SW;

    response_data_unwrap(apdu_rbuf, apdu_rlen - 2, session_key, 16, rbuf, rlen);

    return (SW);
}

/********************************************************************
 * Function: dec_tag_life_cycle
 * Input Parameter:
 * inlen: the input data length
 * inbuf: the input data buffer
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t dec_tag_life_count( uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x7D;
    gfm_SeCmdHand.p1	= 0;
    gfm_SeCmdHand.p2	= 0;
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

    //no tag!
    if(SW != 0x9000)
        return SW;

    response_data_unwrap(apdu_rbuf, apdu_rlen - 2, session_key, 16, rbuf, rlen);

    return (SW);
}

/********************************************************************
 * Function: get_tag_life_cycle
 * Input Parameter:None
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t get_tag_life_count( uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x7B;
    gfm_SeCmdHand.p1	= 0;
    gfm_SeCmdHand.p2	= 0;
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

    //no tag!
    if(SW != 0x9000)
        return SW;

    response_data_unwrap(apdu_rbuf, apdu_rlen - 2, session_key, 16, rbuf, rlen);

    return (SW);
}

/********************************************************************
 * Function: se auth1
 * Input Parameter:
 * inlen: the input data length
 * inbuf: the input data buffer
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t se_auth1( uint16_t inlen,uint8_t *inbuf,uint8_t *rbuf,uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x71;
    gfm_SeCmdHand.p1	= 0;
    gfm_SeCmdHand.p2	= 0;
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


/********************************************************************
 * Function: se auth2
 * Input Parameter:
 * inlen: the input data length
 * inbuf: the input data buffer
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t se_auth2( uint16_t inlen,uint8_t *inbuf,uint8_t *rbuf,uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;

    gfm_SeCmdHand.cla	= 0x84;
    gfm_SeCmdHand.ins	= 0x73;
    gfm_SeCmdHand.p1	= 0x01;
    gfm_SeCmdHand.p2	= 0;
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

/********************************************************************
 * Function: mcu and se mutual auth
 * Input Parameter:None
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
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

	memcpy(mcu_rnd,mcu_uid,8);
    sw=se_auth1(slen,mcu_rnd,recv_buf,&recv_len);
	if(sw!=0x9000)
		return sw;
	
	//tdes decrypt r1
	des3_ecb_decrypt(rbuf,recv_buf,8,mcu_com_key,16);
	if(memcmp(rbuf,mcu_rnd+8,8))
		return 0xffce;

	//session key r1'
	fm_memmove(tmpkey,recv_buf,8);

	//tdes encrypt r2,gen session key r2'
	des3_ecb_encrypt(tmpkey+8,recv_buf+8,8,mcu_com_key,16);

	slen = 8;
	sw=se_auth2(slen,tmpkey+8,recv_buf,&recv_len);
	if(sw!=0x9000)
		return sw;


	//tdes tmpkey to gen session key
	des3_ecb_encrypt(rbuf,tmpkey,16,mcu_com_key,16);
	*rlen = 16;
	
    return (sw);
}

/********************************************************************
 * Function: get_se_uid
 * Input Parameter:
 * para: command param
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t get_se_uid( uint16_t para, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x72;
    gfm_SeCmdHand.p1	= para>>8;
    gfm_SeCmdHand.p2	= para;
    gfm_SeCmdHand.p3.lc = 8;
    slen = 5;

	des3_ecb_encrypt(apdu_cipher,apdu_padding,8,session_key,16);
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

	//no tag!
	if(SW != 0x9000)
		return SW;

	des3_ecb_decrypt(apdu_plain,apdu_rbuf,apdu_rlen-2,session_key,16);
	fm_memmove(rbuf,apdu_plain,apdu_rlen-2-1);	//no padding
    *rlen = apdu_rlen-2-1;

    return (SW);
}

/********************************************************************
 * Function: se_active
 * Input Parameter:
 * para: command param
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t se_active( uint16_t para, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x74;
    gfm_SeCmdHand.p1	= para>>8;
    gfm_SeCmdHand.p2	= para;
    gfm_SeCmdHand.p3.lc = 8;
    slen = 5;

	des3_ecb_encrypt(apdu_cipher,apdu_padding,8,session_key,16);
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

	//no tag!
	if(SW != 0x9000)
		return SW;

	des3_ecb_decrypt(apdu_plain,apdu_rbuf,apdu_rlen-2,session_key,16);
	fm_memmove(rbuf,apdu_plain,apdu_rlen-2-1);	//no padding
    *rlen = apdu_rlen-2-1;

    return (SW);
}

/********************************************************************
 * Function: write_se_data
 * Input Parameter:
 * para: command param
 * inlen: the input data length
 * inbuf: the input data buffer
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t write_se_data( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;
	uint8_t padding_num;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x76;
    gfm_SeCmdHand.p1	= para>>8;
    gfm_SeCmdHand.p2	= para;
    gfm_SeCmdHand.p3.lc = inlen;
    fm_memmove( gfm_SeCmdHand.capdu, inbuf, inlen );
    slen = 5 + inlen;
    
	//add padding 80
	padding_num = 8-inlen%8;
	if(padding_num){
		fm_memmove(gfm_SeCmdHand.capdu+inlen,apdu_padding,padding_num);
		gfm_SeCmdHand.p3.lc = inlen+padding_num;
		slen = 5+inlen+padding_num;
	}
	
	des3_ecb_encrypt(apdu_cipher,(uint8_t *)&gfm_SeCmdHand.capdu,inlen+padding_num,session_key,16);
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

/********************************************************************
 * Function: get_se_data
 * Input Parameter:
 * para: command param
 * inlen: the input data length
 * inbuf: the input data buffer
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t get_se_data( uint16_t para, uint16_t inlen,uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;
	uint8_t padding_num;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x78;
    gfm_SeCmdHand.p1	= para>>8;
    gfm_SeCmdHand.p2	= para;
    gfm_SeCmdHand.p3.lc = inlen;
    slen = 5;
	memcpy((uint8_t *)&gfm_SeCmdHand.capdu,inbuf,inlen);
	slen += inlen;


	//add padding 80
	padding_num = 8-inlen%8;
	if(padding_num){
		fm_memmove(gfm_SeCmdHand.capdu+inlen,apdu_padding,padding_num);
		gfm_SeCmdHand.p3.lc = inlen+padding_num;
		slen = 5+inlen+padding_num;
	}
	
	des3_ecb_encrypt(apdu_cipher,(uint8_t *)&gfm_SeCmdHand.capdu,inlen+padding_num,session_key,16);
	fm_memmove( (uint8_t *)&gfm_SeCmdHand.capdu, apdu_cipher, inlen+padding_num );


	if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

	//no tag!
	if(SW != 0x9000)
		return SW;

	des3_ecb_decrypt(apdu_plain,apdu_rbuf,apdu_rlen-2,session_key,16);
	fm_memmove(rbuf,apdu_plain,apdu_rlen-2);
    *rlen = apdu_rlen-2;


    return (SW);
}

/********************************************************************
 * Function: dec_se_life_cycle
 * Input Parameter:
 * para: command param
 * inlen: the input data length
 * inbuf: the input data buffer
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t dec_se_life_count( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;
	uint8_t padding_num;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x7C;
    gfm_SeCmdHand.p1	= para>>8;
    gfm_SeCmdHand.p2	= para;
    gfm_SeCmdHand.p3.lc = inlen;
	fm_memmove( gfm_SeCmdHand.capdu, inbuf, inlen );
    slen = 5+inlen;

	//add padding 80
	padding_num = 8-inlen%8;
	if(padding_num){
		fm_memmove(gfm_SeCmdHand.capdu+inlen,apdu_padding,padding_num);
		gfm_SeCmdHand.p3.lc = inlen+padding_num;
		slen = 5+inlen+padding_num;
	}
	
	des3_ecb_encrypt(apdu_cipher,(uint8_t *)&gfm_SeCmdHand.capdu,inlen+padding_num,session_key,16);
	fm_memmove( (uint8_t *)&gfm_SeCmdHand.capdu, apdu_cipher, inlen+padding_num );

	if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

	//no tag!
	if(SW != 0x9000)
		return SW;

	*rlen = apdu_rlen-2;

    return (SW);
}

/********************************************************************
 * Function: get_se_life_cycle
 * Input Parameter:
 * para: command param
 * inlen: the input data length
 * inbuf: the input data buffer
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t get_se_life_count( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;
	uint8_t padding_num;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x7A;
    gfm_SeCmdHand.p1	= para>>8;
    gfm_SeCmdHand.p2	= para;
    gfm_SeCmdHand.p3.lc = inlen;
    slen = 5;
	memcpy((uint8_t *)&gfm_SeCmdHand.capdu,inbuf,inlen);
	slen += inlen;

	//add padding 80
	padding_num = 8-inlen%8;
	if(padding_num){
		fm_memmove(gfm_SeCmdHand.capdu+inlen,apdu_padding,padding_num);
		gfm_SeCmdHand.p3.lc = inlen+padding_num;
		slen = 5+inlen+padding_num;
	}
	
	des3_ecb_encrypt(apdu_cipher,(uint8_t *)&gfm_SeCmdHand.capdu,inlen+padding_num,session_key,16);
	fm_memmove( (uint8_t *)&gfm_SeCmdHand.capdu, apdu_cipher, inlen+padding_num );

	if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

	//no tag!
	if(SW != 0x9000)
		return SW;

	des3_ecb_decrypt(apdu_plain,apdu_rbuf,apdu_rlen-2,session_key,16);
	fm_memmove(rbuf,apdu_plain,apdu_rlen-2-4);	//no padding
    *rlen = apdu_rlen-2-4;

    return (SW);
}

/********************************************************************
 * Function: set_se_otp_status
 * Input Parameter:
 * para: command param
 * inlen: the input data length
 * inbuf: the input data buffer
 * Output Parameter:
 * rbuf: the return data buffer
 * rlen: the return data length
 * Return: SW
 *********************************************************************/
uint16_t set_se_otp_status( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen )
{
    uint8_t		result		= 0;
    uint16_t	slen		= 0;
    uint16_t	SW			= IF_ERR_NULL_POINT;
    uint16_t	interval	= POLL_INTERVAL;
    uint32_t	timeout		= POLL_TIMEOUT;
	uint8_t padding_num;

    gfm_SeCmdHand.cla	= 0x80;
    gfm_SeCmdHand.ins	= 0x7E;
    gfm_SeCmdHand.p1	= para>>8;
    gfm_SeCmdHand.p2	= para;
    gfm_SeCmdHand.p3.lc = inlen;
    slen = 5;
	memcpy((uint8_t *)&gfm_SeCmdHand.capdu,inbuf,inlen);
	slen += inlen;

	//add padding 80
	padding_num = 8-inlen%8;
	if(padding_num){
		fm_memmove(gfm_SeCmdHand.capdu+inlen,apdu_padding,padding_num);
		gfm_SeCmdHand.p3.lc = inlen+padding_num;
		slen = 5+inlen+padding_num;
	}
	
	des3_ecb_encrypt(apdu_cipher,(uint8_t *)&gfm_SeCmdHand.capdu,inlen+padding_num,session_key,16);
	fm_memmove( (uint8_t *)&gfm_SeCmdHand.capdu, apdu_cipher, inlen+padding_num );

	if ( pgfm_SeFunc )
    {
        result = pgfm_SeFunc->fm_apdu_transceive( (uint8_t *) &gfm_SeCmdHand, slen, apdu_rbuf, &apdu_rlen, interval, timeout );
        if ( !result )
            SW = apdu_rbuf[apdu_rlen - 2] << 8 | apdu_rbuf[apdu_rlen - 1];
        else
            SW = fm_chk_result( result );
    }

	//no tag!
	if(SW != 0x9000)
		return SW;

	des3_ecb_decrypt(apdu_plain,apdu_rbuf,apdu_rlen-2,session_key,16);
	fm_memmove(rbuf,apdu_plain,apdu_rlen-2-1);	//no padding
    *rlen = apdu_rlen-2-1;

    return (SW);
}

/***********************EOF********************************/
