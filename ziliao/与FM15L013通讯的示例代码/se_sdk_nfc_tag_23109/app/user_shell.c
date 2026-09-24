
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "stm32f10x.h"
#include "se_app.h"
#include "se_cmd.h"
#include "fmse_onewire.h"
#include "fmse_onewireapp.h"
#include "timer.h"


/****************************************************/
uint8_t uart_buf[8];
uint8_t uart_inlen = 0;


/****************************************************/
void uart_send_byte( uint8_t ch )
{
    USART1->DR = ch;
    while ( !(USART1->SR & (1 << 7) ) )
        ;
}


void uart_send_string( char *s )
{
    while ( *s )
    {
        uart_send_byte( *s++ );
    }
}


int fputc( int ch, FILE *f )
{
    uart_send_byte( (uint8_t) ch );
    return (ch);
}


void USART1_IRQHandler( void )
{
    if ( USART1->SR & (1 << 5) )
    {
        uart_buf[uart_inlen++] = USART1->DR;
        if ( uart_inlen > 7 )
            uart_inlen = 0;
    }
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


uint8_t uart_recv( char *recv )
{
    uint8_t recvlen = 0;

    if ( uart_inlen )
    {
        fm_memmove( recv, uart_buf, uart_inlen );
        recvlen = uart_inlen;
        fm_memset( uart_buf, 0, sizeof(uart_buf) );
        uart_inlen = 0;
    }
    else
        recvlen = 0;

    return (recvlen);
}


uint8_t get_cmd( uint8_t* outlen, char* outbuf )
{
    char	rbuf[64];
    uint8_t inputlen = 0, cmdlen = 0;
    uint8_t tmplen;
    char	inbuf[8];

    fm_memset( rbuf, 0, sizeof(rbuf) );
    do
    {
        tmplen = 0;
        fm_memset( inbuf, 0, sizeof(inbuf) );
        tmplen = uart_recv( inbuf );
        if ( tmplen )
        {
            inputlen++;

            if ( inbuf[0] == '\r' )     /*0xd */
            {
                printf( "\r\n" );
                fm_memset( inbuf, 0, sizeof(inbuf) );
                break;
            }
            else if ( inbuf[0] == 0x8 ) /*BS */
            {
                if ( !cmdlen )
                    continue;
                cmdlen--;
                printf( "\b \b" );
                rbuf[cmdlen] = '\0';
            }
            else
            {
                sprintf( rbuf + cmdlen, "%c", inbuf[0] );
                printf( "%c", inbuf[0] );
                cmdlen++;
            }
        }
    }
    while ( 1 );

    printf( "(user_cmd:%s,%d,%d)\r\n", rbuf, cmdlen, inputlen );
    *outlen = cmdlen;
    if ( cmdlen )
        fm_memmove( outbuf, rbuf, cmdlen );

    return (inputlen);
}


char* const cmdlist[] = {
    "door open",
    "door close",
    "led on",
    "led off",
    "ascii on",
    "ascii off",        /* 5 */
    "beep test",
    "get nb info",
    "nb tcp test",
    "sleep lock",
    "test",             /* 10 */
    "motor forward",
    "motor reverse",
    "sm4 test",
    "one net test",
    "motor stop",       /* 15 */
    "one net close",
    "server test",
    "stop test",
    "http test",
    "interface test",   /*20 */
    "i2c test",
    "spi test",
    "onewire test",
    "onewireapp test",
    NULL
};
extern void se_onewire_test( void );


void process_cmd( uint8_t cmdlen, char* cmdbuf )
{
    uint8_t i;

    for ( i = 0; cmdlist[i] != NULL; i++ )
    {
        if ( !strcmp( cmdbuf, cmdlist[i] ) )
            break;
    }
    printf( "cmd num=%d\r\n", i );

    switch ( i )
    {
    case 0:
        /*door_open(); */
        break;
    case 1:
        /*door_close(); */
        break;
    case 2:
        /*led_on(); */
        break;
    case 3:
        /*led_off(); */
        break;
    case 4:
        break;
    case 5:
        break;
    case 6:
        break;
    case 7:
        /*get_nb_info(); */
        break;
    case 8:
        /*nb_tcp_test(0); */
        break;
    case 9:
        /*nb_uart_transceive(0,NB_InitCmd[20]); */
        break;
    case 10:
        break;
    case 11:
        /*motor_forward(); */
        break;
    case 12:
        /*motor_reverse(); */
        break;
    case 13:
        /*my_sm4_test(); */
        break;
    case 14:
        /*one_net_test(); */
        break;
    case 15:
        /*motor_stop(); */
        break;
    case 16:
        /*one_net_close(); */
        break;
    case 17:
        /*Server_Test(0); */
        break;
    case 18:
        /*stop_test(); */
        break;
    case 19:
        /*http_test(); */
        break;
    case 20:
        /*interface_test(); */
        break;
    case 21:
        se_test( 1 );
        break;
    case 22:
        se_test( 2 );
        break;
#if USE_ONEWIRE_DRIVER
    case 23:
        se_onewire_test();
        break;
#endif
#if USE_ONEWIRE_DRIVER_APP
    case 24:
        se_onewireapp_test();
        break;
#endif
    default: break;
    }
}


#define PROMPT "fmsh # "

void show_shell_info( void )
{
    printf( "start shell...\r\n" );
    printf( "*******************************************\r\n" );
    printf( "***    Shell Demo By Muxb  2018.06.21   ***\r\n" );
    printf( "***    Email:mouxiaobo@fmsh.com.cn      ***\r\n" );
    printf( "*******************************************\r\n" );
    printf( "System info:\r\n" );
    printf( "MCU:STM32F103VBT6 72MHz\r\n" );
    printf( "Version:FMSE SDK V2.00\r\n" );
    printf( PROMPT );
}


void user_shell( void )
{
    uint8_t cmdlen;
    char	cmd[64];

    cmdlen = 0;
    fm_memset( cmd, 0, sizeof(cmd) );
    if ( get_cmd( &cmdlen, cmd ) )
    {
        if ( cmdlen )
        {
            process_cmd( cmdlen, cmd );
        }
        printf( PROMPT );
    }
}


/***********************************************************/

