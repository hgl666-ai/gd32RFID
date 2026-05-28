#ifndef DES_H
#define DES_H
#include <stdint.h>
#define DES_ENCRYPT 1
#define DES_DECRYPT 0

#define ERR_DES_INVALID_INPUT_LENGTH -0x0002              /**< The data input has an invalid length. */

#define DES_KEY_SIZE	8
#define DES3_KEY2_SIZE	(16)
#define DES3_KEY3_SIZE	(24)

typedef struct
{
    uint32_t sk[32];            /*!<  DES subkeys       */
}des_context;


/**
 * \brief          Triple-DES context structure
 */
typedef struct
{
    uint32_t sk[96];            /*!<  3DES subkeys      */
}des3_context;
/*
 * 3DES-ECB buffer encryption API nlen must be a multiple of 8
 */
unsigned int des3_ecb_encrypt( unsigned char *pout,
                               unsigned char *pdata,
                               unsigned int nlen,
                               unsigned char *pkey,
                               unsigned int klen );
unsigned int des3_ecb_decrypt( unsigned char *pout,
                               unsigned char *pdata,
                               unsigned int nlen,
                               unsigned char *pkey,
                               unsigned int klen );
#endif
