#ifndef __SE_APP_H__
#define __SE_APP_H__
#include <stdint.h>

/*param define */
#define FILE_ID_11	0x0A010A91
#define FILE_ID_22	0x0A020A92
#define FILE_ID_88	0x0A080A98
#define FILE_ID_01	0x00000A91

#define PUB_FID8	0x0a08
#define PRI_FID8	0x0a98

#define FILE_TYPE_26	0x26
#define FILE_TYPE_28	0x28

#define FILE_SPACE_1	0x0106
#define FILE_SPACE_2	0x02F0

#define FILE_RANK_1 0x8181
#define FILE_RANK_2 0x0


#define SHA1	1
#define SHA256	2
#define SM3		3

#define PKCS1	0
#define PKCS8	1


/*
 * user config
 * #define GET_UID_TEST
 * #define EXTERN_AUTH_TEST
 */
#define BASE_CMD_TEST
/*
 * #define RSA1024_KEY_TEST
 * #define RSA2048_IMPORT_KEY_TEST
 * #define RSA2048_SIG_TEST
 * #define RSA2048_ENCRYPT_TEST
 * #define GEN_2048RSA
 * #define GEN_1024RSA
 * #define GEN_SM2_KEY
 * #define SM2_KEY_TEST
 * #define SM2_VERIFY_SIG_TEST
 * #define DATA_ENDECRYPT_TEST
 * #define PKCS_TEST
 * #define AES_TEST
 * #define DES_TEST
 * #define TDES_TEST
 * #define HASH_TEST
 * #define RSA_OAEP_TEST
 */


//firm crc info
#define INIT_VECTOR			0x6363
#define PROGRAM_BASE		0x08000000
#define PROGRAM_LENGTH		0x1738


/*test config */
#define LOOP_TEST_CNT 0 /*10 */

typedef  unsigned char BYTE;

/*function declaration */
void l013_test( uint8_t flag );
void show_shell_info( void );
void uart_init( void );

#endif
