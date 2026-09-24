
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include "se_cmd.h"
#include "fmse_i2c.h"
#include "se_app.h"
#include "bitband.h"
#include "timer.h"
#include "port_i2c.h"
/*******************************************************/
static uint8_t	rbuf[300];
static uint8_t	sbuf[256];
static uint16_t	rlen;
static uint16_t	slen;
/***********************************************************/

/*
 * // C prototype : void StrToHex(BYTE *pbDest, BYTE *pbSrc, int nLen)
 * // parameter(s): [OUT] pbDest - 输出缓冲区
 * //	[IN] pbSrc - 字符串
 * //	[IN] nLen - 16进制数的字节数(字符串的长度/2)
 * // return value:
 * // remarks : 将字符串转化为16进制数
 */
void StrToHex( BYTE *pbDest, BYTE *pbSrc, int nLen )
{
    char	h1, h2;
    BYTE	s1, s2;
    int		i;

    for ( i = 0; i < nLen; i++ )
    {
        h1	= pbSrc[2 * i];
        h2	= pbSrc[2 * i + 1];

        s1 = toupper( h1 ) - 0x30;
        if ( s1 > 9 )
            s1 -= 7;

        s2 = toupper( h2 ) - 0x30;
        if ( s2 > 9 )
            s2 -= 7;

        pbDest[i] = s1 * 16 + s2;
    }
}

void dump_data( uint16_t len, uint8_t * buf )
{
    uint16_t i;

    for ( i = 0; i < len; i++ )
    {
        printf( "%02X,", buf[i] );
    }
    printf( "\r\n" );
}

u16 UpdateCrc(u8 ch, u16 *lpwCrc)
{
	ch = (ch^(u8)((*lpwCrc) & 0x00FF));
	ch = (ch^(ch<<4));
	*lpwCrc = (*lpwCrc >> 8)^((u16)ch << 8)^((u16)ch<<3)^((u16)ch>>4);
	return(*lpwCrc);
}

u16 ComputeCrc(const u16 InitCRC, const u8 *Data, const u32 Length)
{
	u8 chBlock;
	u32 InLength = Length;
	u16 wCrc = InitCRC;
	
	do 
	{
		chBlock = *Data++;
		UpdateCrc(chBlock, &wCrc);
	}while (--InLength);
	
	return wCrc;
}

void uart_init( void )
{
    /*enable uart1 clk */
    RCC->APB2ENR |= 1 << 14;

    /*
     * usart1,115200,8,n,1
     * uart1, PA9--tx,PA10--rx
     */
    GPIOA->CRH	&= 0xFFFFF00F;
    GPIOA->CRH	|= 0x000004B0;
    /*baudrate 115200bps */
    USART1->BRR = 39 << 4 | 1;

    NVIC_EnableIRQ( USART1_IRQn );

    USART1->CR1 |= 1 << 13 | 1 << 5 | 3 << 2;
}

void uart_send_byte( uint8_t ch )
{
    USART1->DR = ch;
    while ( !(USART1->SR & (1 << 7) ) )
        ;
}

int fputc( int ch, FILE *f )
{
    uart_send_byte( (uint8_t) ch );
    return (ch);
}

void show_shell_info( void )
{
	uint16_t firm_crc;
    printf( "start shell...\r\n" );
    printf( "System info:\r\n" );
    printf( "MCU:STM32F103VBT6 72MHz\r\n" );
    printf( "Version:FMSE SDK V2.00\r\n" );
	firm_crc = ComputeCrc(INIT_VECTOR,(uint8_t *)PROGRAM_BASE,PROGRAM_LENGTH);
	printf( "Firmware CRC: %04X!\r\n",firm_crc);
}


uint8_t tag_demo(void)
{
    uint16_t sw	= 0;
    uint16_t cnt = 2;
    uint16_t i;
	/* tag_dec_cnt length must set as 4 */
	uint8_t tag_dec_cnt[4]={0,0,0,1};
	/* tag_sn length must set as 4 */
	uint8_t tag_sn[4]={0xAA,0xBB,0xCC,0xDD};
    uint8_t i2c_addr;

	/* nfc reader param reg addr */
	uint8_t nfc_reg[8]={0x07,0x24,0x15,0x27,0x28,0x0C,0x26,0x18};
	/* nfc reader param : 072426154027f0281f0C1026401854 */
	uint8_t nfc_param[15]={0x07,0x24,0x26,0x15,0x40,0x27,0xf0,0x28,0x1f,0x0C,0x10,0x26,0x40,0x18,0x54};

    i2c_addr = 0x28;

	/* write tag reader param */
	sw = set_tag_reader(P1_RESET, WRITE_NFC_REG, sizeof(nfc_param), nfc_param, i2c_addr, SOFT_RST, rbuf, &rlen);
	printf( "set_tag_reader:sw=%04x,rlen=%d\r\n", sw, rlen );
	if ( sw != 0x9000 )
		return (0x02);

	/* read tag reader param */
	sw = set_tag_reader(P1_NO_RESET, READ_NFC_REG, sizeof(nfc_reg), nfc_reg, i2c_addr, SOFT_RST, rbuf, &rlen);
	printf( "set_tag_reader:sw=%04x,rlen=%d\r\n", sw, rlen );
	if ( sw != 0x9000 )
		return (0x03);
	dump_data(rlen, rbuf);

	/*polling tag */
    for ( i = 0; i < 200; i++ )
    {
        sw = get_tag_uid(Tag_SingleCh, i2c_addr, rbuf, &rlen);
        printf( "get_tag_uid:sw=%04x,rlen=%d\r\n", sw, rlen );
        if ( sw == 0x9000 )
            break;
        delayms( 500 );
    }

    if ( sw != 0x9000 )
    {
        printf( "request_tag fail! sw=%04x!\r\n", sw );
        return (0xff);
    }
    else
    {
        /*show tag info: */
        printf( "ATQA:\r\n" );
        dump_data( 2, rbuf );
        printf( "UID:\r\n" );
        dump_data( 7, rbuf + 2 );
        printf( "SAK:\r\n" );
        dump_data( 2, rbuf + 2 + 7 );
    }

    sw = tag_active( Tag_SigCh_Key, i2c_addr, rbuf, &rlen );
    printf( "tag_active:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 )
        return 0x11;

    memcpy(sbuf,tag_sn,sizeof(tag_sn));
    slen = sizeof(tag_sn);
    sw = write_tag_data( BLOCK_NUM0, slen, sbuf, rbuf, &rlen );
    printf( "write_tag_data:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 )
    {
        printf( "init_tag_sn fail!\r\n" );
        return 0x12;
    }

/**********************************************************/
    sw = get_tag_data( 0, rbuf, &rlen );
    printf( "get_tag_data:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 )
    {
        printf( "get_tag_data fail!\r\n" );
        return 0x13;
    }
    printf( "get_tag_data:\r\n" );
    dump_data( rlen, rbuf );

/**********************************************************/
	while ( cnt-- ){
		sw = get_tag_life_count( rbuf, &rlen );
        printf( "get_tag_life_count:sw=%04x,rlen=%d\r\n", sw, rlen );
        if ( sw != 0x9000 )
        {
            printf( "get_tag_life_count fail!\r\n" );
            break;
        }
        printf( "get_tag_life_count:\r\n" );
        dump_data( 4, rbuf );

		sw = dec_tag_life_count( sizeof(tag_dec_cnt), tag_dec_cnt, rbuf, &rlen );
        printf( "dec_tag_life_count:sw=%04x,rlen=%d\r\n", sw, rlen );
        if ( sw != 0x9000 )
        {
            printf( "dec_tag_life_count fail!\r\n" );
            break;
        }
		printf( "dec_tag_life_count:\r\n" );
        dump_data( 4, rbuf );
	}
    printf( "FMSE demo tag test success!\r\n");
    printf( "**************************************\r\n" );
    if ( sw != 0x9000 )
        return 0x13;
	return 0;
}

uint8_t se_iic_demo(void)
{
    uint16_t sw	= 0;
    uint16_t cnt = 2;
	uint8_t se_wdat[14]={0xC0,0x02,0x00,0x03,0xC1,0x02,0x00,0x00,0xC2,0x04,0x11,0x22,0x33,0x44};
	uint8_t se_rdat[11]={0xC0,0x02,0x00,0x03,0xC1,0x02,0x00,0x00,0xC2,0x01,0x10};
	uint8_t se_cdat[4]={0xC0,0x02,0x0C,0x01};
	uint8_t se_sdat[4]={0xC0,0x02,0x00,0x05};

	sw = get_se_uid(0, rbuf, &rlen);
    printf( "get_se_uid:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 ) return 0x10;
	dump_data( rlen, rbuf );

	sw = se_active( 0x0100, rbuf, &rlen );
	printf( "se_active:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 )
        return 0x11;
	dump_data( rlen, rbuf );

	memcpy(sbuf,se_wdat,14);
	slen = 14;
    sw = write_se_data( 0, slen, sbuf, rbuf, &rlen );
    printf( "write_se_data:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 )
    {
        printf( "write_se_data fail!\r\n" );
        return 0x12;
    }

	memcpy(sbuf,se_rdat,11);
	slen = 11;
	sw = get_se_data( 0, slen, sbuf, rbuf, &rlen );
    printf( "get_se_data:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 )
    {
        printf( "get_se_data fail!\r\n" );
        return 0x13;
    }
    printf( "get_se_data:\r\n" );
    dump_data( rlen, rbuf );

	while(cnt--){
		memcpy(sbuf,se_cdat,4);
		slen = 4;
		sw = get_se_life_count( 0, slen, sbuf, rbuf, &rlen );
	    printf( "get_se_life_count:sw=%04x,rlen=%d\r\n", sw, rlen );
	    if ( sw != 0x9000 )
	    {
	        printf( "get_se_life_count fail!\r\n" );
	        return 0x14;
	    }
	    printf( "get_se_life_count:\r\n" );
	    dump_data( 4, rbuf );

		memcpy(sbuf,se_cdat,4);
		slen = 4;
		sw = dec_se_life_count( 0, slen, sbuf, rbuf, &rlen );
	    printf( "dec_se_life_count:sw=%04x,rlen=%d\r\n", sw, rlen );
	    if ( sw != 0x9000 )
	    {
	        printf( "dec_se_life_count fail!\r\n" );
	        return 0x15;
	    }
	}

	memcpy(sbuf,se_sdat,4);
	slen = 4;
	sw = set_se_otp_status( 0, slen, sbuf, rbuf, &rlen );
	printf( "set_se_otp_status:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 )
        return 0x16;
	dump_data( rlen, rbuf );

    printf( "FMSE demo se test success!\r\n");
    printf( "**************************************\r\n" );
	return 0;
}

uint8_t se_swi_demo(void)
{
    uint16_t sw	= 0;
    uint16_t cnt = 2;
	uint8_t se_wdat[14]={0xC0,0x02,0x00,0x03,0xC1,0x02,0x00,0x00,0xC2,0x04,0x11,0x22,0x33,0x44};
	uint8_t se_rdat[11]={0xC0,0x02,0x00,0x03,0xC1,0x02,0x00,0x00,0xC2,0x01,0x10};
	uint8_t se_cdat[4]={0xC0,0x02,0x0C,0x01};
	uint8_t se_sdat[4]={0xC0,0x02,0x00,0x05};

	sw = get_se_uid(0x0001, rbuf, &rlen);
    printf( "get_se_uid:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 ) return 0x10;
	dump_data( rlen, rbuf );

	sw = se_active( 0x0101, rbuf, &rlen );
	printf( "se_active:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 )
        return 0x11;
	dump_data( rlen, rbuf );

	memcpy(sbuf,se_wdat,14);
	slen = 14;
    sw = write_se_data( 0x0001, slen, sbuf, rbuf, &rlen );
    printf( "write_se_data:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 )
    {
        printf( "write_se_data fail!\r\n" );
        return 0x12;
    }

	memcpy(sbuf,se_rdat,11);
	slen = 11;
	sw = get_se_data( 0x0001, slen, sbuf, rbuf, &rlen );
    printf( "get_se_data:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 )
    {
        printf( "get_se_data fail!\r\n" );
        return 0x13;
    }
    printf( "get_se_data:\r\n" );
    dump_data( rlen, rbuf );

	while(cnt--){
		memcpy(sbuf,se_cdat,4);
		slen = 4;
		sw = get_se_life_count( 0x0001, slen, sbuf, rbuf, &rlen );
	    printf( "get_se_life_count:sw=%04x,rlen=%d\r\n", sw, rlen );
	    if ( sw != 0x9000 )
	    {
	        printf( "get_se_life_count fail!\r\n" );
	        return 0x14;
	    }
	    printf( "get_se_life_count:\r\n" );
	    dump_data( 4, rbuf );

		memcpy(sbuf,se_cdat,4);
		slen = 4;
		sw = dec_se_life_count( 0x0001, slen, sbuf, rbuf, &rlen );
	    printf( "dec_se_life_count:sw=%04x,rlen=%d\r\n", sw, rlen );
	    if ( sw != 0x9000 )
	    {
	        printf( "dec_se_life_count fail!\r\n" );
	        return 0x15;
	    }
	}

	memcpy(sbuf,se_sdat,4);
	slen = 4;
	sw = set_se_otp_status( 0x0001, slen, sbuf, rbuf, &rlen );
	printf( "set_se_otp_status:sw=%04x,rlen=%d\r\n", sw, rlen );
    if ( sw != 0x9000 )
        return 0x16;
	dump_data( rlen, rbuf );

    printf( "FMSE demo swi test success!\r\n");
    printf( "**************************************\r\n" );
	return 0;
}


/*l013 test app */
/********************************************************/
void l013_test( uint8_t flag )
{
    uint16_t	sw		= 0;
    uint8_t		ret		= 0;
    StSeFunc	*pfm_SeFunc;
    uint8_t se_rst_times = 2;

    printf( "FMSE Demo start ...\r\n" );

    pfm_SeFunc = fm_se_register( &gfm_se_i2c );
    if ( !pfm_SeFunc )
        return ;

    pfm_SeFunc->fm_open_device();	//init mcu i2c port
    pfm_SeFunc->fm_device_init();	//power on se

_test_start:
    ret = pfm_SeFunc->fm_dev_power_on( rbuf, &rlen );//get se atr
    if ( ret )
    {
        printf( "FMSE PowerOn fail!ret=%x!\r\n", ret );
        goto _test_fail;
    }
    printf( "PowerOn:ret=%x,rlen=%d\r\n", ret, rlen );
    dump_data( rlen, rbuf );

	sw=mcu_l013_mutual_auth(rbuf,&rlen);
	printf( "mcu_l013_mutual_auth_sw=%x!\r\n", sw );
	if(sw!=0x9000){
		goto _test_fail;
	}
	memcpy(session_key,rbuf,rlen);
	printf( "**************************************\r\n" );

	ret = tag_demo();
    if (ret)
        goto _test_fail;
	//se_iic_demo();
	//se_swi_demo();

    pfm_SeFunc->fm_close_device();
    fm_se_unregister();

    return;

_test_fail:

    if (se_rst_times)
    {
        printf( "se_rst_times = %d\r\n",  se_rst_times );
        se_rst_times--;
        /* if fm_i2c_transceive fail, suggest reset se */
        se_reset();
        goto _test_start;
    }


}

/**********************EOF***********************/
