#ifndef __SE_CMD_H__
#define __SE_CMD_H__

#include <stdint.h>
#include <stdio.h>

/*param define */
#define BY_AID      1
#define BY_FID      2
#define BY_FID_EF   3

/*gen rsa key cmd para define */
#define RSA1024 0
#define RSA1280 1
#define RSA2048 2

#define EXP65537    0
#define EXP3        1

#define RSA_PUB_KEY 0
#define RSA_PRI_KEY 1

/*SM2 pub key calc */
#define SM2_PUBKEY_VERIFY   0
#define SM2_PUBKEY_ENCRYPT  1
/*SM2 pri key calc */
#define SM2_PRIKEY_SIGNATURE    0
#define SM2_PRIKEY_DECRYPT      1

/*se support interface */
#define SE_IF_7816      0
#define SE_IF_I2C       1
#define SE_IF_SPI       2
#define SE_IF_ONEWIRE   3

/*interface define SW Error Code */
#define SE_SUCCESS          0x0
#define IF_ERR_NULL_POINT   0xFFE1
#define IF_ERR_RECV_ACK     0xFFE2
#define IF_ERR_LENGTH       0xFFE3
#define IF_ERR_LRC          0xFFE4
#define IF_ERR_OTHER        0xFFE5
#define SE_ERR_CRC          0xFFE6
#define SE_ERR_INS          0xFFE7

/*config MCU Polling SE interval(uint:ms) */
#define POLL_INTERVAL 10

/*config MCU Polling SE time(uint:ms) */
#define POLL_TIMEOUT 4000

extern uint8_t session_key[16];

/* block num range from 0x0000 to 0x000b */
#define BLOCK_NUM0  0x0000
#define BLOCK_NUM1  0x0001
#define BLOCK_NUM2  0x0002

/* write read nfc reg command param */
#define WRITE_NFC_REG   0x00
#define READ_NFC_REG    0x01

#define P1_RESET        0x00
#define P1_NO_RESET     0x01

#define HARD_RST       0x01
#define SOFT_RST       0x00

/**
 * \brief sign and verify signature
 */
typedef enum {
    ALG_ECDSA_SHA           = 0x00,
    ALG_ECDSA_SHA_224       = 0x01,
    ALG_ECDSA_SHA_256       = 0x02,
    ALG_ECDSA_SHA_384       = 0x03,
    ALG_ECDSA_SHA_512       = 0x04,
    ALG_RSA_MD5_PKCS1       = 0x05,
    ALG_RSA_MD5_PSS         = 0x06,
    ALG_RSA_SHA_PKCS1       = 0x07,
    ALG_RSA_SHA_PSS         = 0x08,
    ALG_RSA_SHA_224_PKCS1   = 0x09,
    ALG_RSA_SHA_224_PSS     = 0x0A,
    ALG_RSA_SHA_256_PKCS1   = 0x0B,
    ALG_RSA_SHA_256_PSS     = 0x0C,
    ALG_RSA_SHA_384_PKCS1   = 0x0D,
    ALG_RSA_SHA_384_PSS     = 0x0E,
    ALG_RSA_SHA_512_PKCS1   = 0x0F,
    ALG_RSA_SHA_512_PSS     = 0x10,
    ALG_SM2_SM3             = 0x11,
} SIGN_VRFY_ALGO;

typedef struct {
    uint8_t cla;
    uint8_t ins;
    uint8_t p1;
    uint8_t p2;
    union {
        uint8_t lc;
        uint8_t le;
    }       p3;
    uint8_t capdu[256];
} StApduPack;

typedef struct {
    uint8_t se_name;
    void (*fm_driver_register)( void *user_drv );
    void (*fm_device_init)( void );
    void (*fm_open_device)( void );
    void (*fm_close_device)( void );
    uint8_t (*fm_dev_power_on)( uint8_t *rbuf, uint16_t *rlen );
    uint8_t (*fm_apdu_transceive)( uint8_t *sbuf, uint16_t slen, uint8_t *rbuf, uint16_t *rlen,
                                   uint16_t poll_inv, uint32_t poll_timeout );
    void (*fm_driver_unregister)( void );
} StSeFunc;

/*public func */
StSeFunc *fm_se_register( StSeFunc *fm_se );
void fm_se_unregister( void );
void* fm_memset( void* dst, int val, size_t count );
void* fm_memmove( void* dst, const void* src, size_t count );

/*SE private func */
uint16_t GetChallenge( uint16_t inlen, uint8_t *rbuf, uint16_t *rlen );
uint16_t SelectFile( uint16_t fid, uint8_t *rbuf, uint16_t *rlen );
uint16_t CreateFile( uint16_t fid, uint8_t type, uint16_t space, uint16_t rank );
uint16_t ReadBinary( uint16_t sfi, uint16_t inlen, uint8_t *rbuf, uint16_t *rlen );
uint16_t GenRSAKeyPair( uint8_t KeyLen, uint8_t Exp, uint32_t Fid, uint8_t *rbuf, uint16_t *rlen );
uint16_t GenSM2KeyPair( uint32_t Fid, uint8_t *rbuf, uint16_t *rlen );
uint16_t VerifyPIN( uint16_t inlen, uint8_t *inbuf );
uint16_t RSAPubKeyCal( uint16_t p1p2, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
uint16_t RSAPriKeyCal( uint16_t p1p2, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
uint16_t SM2PubKeyCal( uint8_t mode, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
uint16_t SM2PriKeyCal( uint8_t mode, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
uint16_t ImportSessionKey( uint16_t p1p2, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
uint16_t DataEnDecrypt( uint16_t p1p2, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
uint16_t InstallRSAKey( uint16_t p1p2, uint16_t inlen, uint8_t *inbuf );
uint8_t ImportRSAKeypair( uint8_t alg_flag, uint8_t key_type, uint8_t key_num, uint16_t key_len,
                          uint8_t *key_buf, uint16_t exp_len, uint8_t *exp_buf );
uint16_t DataCompress( uint16_t p1p2, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
int ModifyKey( uint8_t crypt_key[16] );
int EndPersonalization( void );
int GetPersonalization( uint8_t *status );
int InternalAuth( uint8_t plain_text[16], uint8_t crypt_text[16] );
uint8_t sm2_verify_signature( uint8_t mode,
                              uint16_t ida_len, uint8_t *sm2_ida,
                              uint16_t msg_len, uint8_t *sm2_msg,
                              uint16_t sig_len, uint8_t *sm2_sig_res );
uint8_t gen_pkcs_pubkey( uint8_t mode, uint8_t *inbuf, uint16_t inlen, uint8_t *outbuf );
uint8_t des_calc( uint8_t mode, uint8_t *key, uint16_t keylen, uint8_t *inbuf, uint16_t inlen, uint8_t *outbuf, uint16_t *outlen );
uint8_t aes_calc( uint8_t mode, uint8_t *key, uint16_t keylen, uint8_t *inbuf, uint16_t inlen, uint8_t *outbuf, uint16_t *outlen );
int priKey_sign( unsigned char *sig, int *p_sig_sz,
                 const unsigned char *data, int size,
                 SIGN_VRFY_ALGO algo, int Fid );
int pubKey_verify( const unsigned char *data, int size,
                   const unsigned char *sig, int sig_sz,
                   SIGN_VRFY_ALGO algo, int Fid );

/**
 * \brief tag channel
 */
typedef enum {
    Tag_Ch0 = 0x0000,
    Tag_Ch1 = 0x0100,
    Tag_Ch2 = 0x0200,
    Tag_SingleCh = 0xFF00
} TAG_CHN;

/**
 * \brief tag channel & root key
 */
typedef enum {
    Tag_Ch0_Key0 = 0x0001,
    Tag_Ch0_Key1,
    Tag_Ch0_Key2,
    Tag_Ch1_Key0 = 0x0101,
    Tag_Ch1_Key1,
    Tag_Ch1_Key2,
    Tag_Ch2_Key0 = 0x0201,
    Tag_Ch2_Key1,
    Tag_Ch2_Key2,
    Tag_SigCh_Key = 0xFF01,
} TAG_ROOT_KEY;

/* extern function */
void se_set_credentials(const uint8_t *pUid8, const uint8_t *pKey16);
uint16_t se_probe_cmd(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2,
                      uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen);
uint16_t se_probe_raw(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2,
                      uint16_t inlen, const uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen);
uint16_t mcu_l013_mutual_auth( uint8_t *rbuf, uint16_t *rlen );
uint16_t get_tag_uid( TAG_CHN para, uint8_t i2cAddr, uint8_t *rbuf, uint16_t *rlen );
uint16_t tag_active( TAG_ROOT_KEY para, uint8_t i2cAddr, uint8_t *rbuf, uint16_t *rlen );
uint16_t set_tag_reader( uint8_t rst, uint8_t wrmode, uint16_t inlen, uint8_t *inbuf, uint8_t i2cAddr, uint8_t rstmode, uint8_t *rbuf, uint16_t *rlen );
uint16_t write_tag_data( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
uint16_t get_tag_data( uint16_t para, uint8_t *rbuf, uint16_t *rlen );
uint16_t dec_tag_life_count( uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
uint16_t get_tag_life_count( uint8_t *rbuf, uint16_t *rlen );

uint16_t get_se_uid( uint16_t para, uint8_t *rbuf, uint16_t *rlen );
uint16_t se_active( uint16_t para, uint8_t *rbuf, uint16_t *rlen );
uint16_t write_se_data( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
uint16_t get_se_data( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
uint16_t dec_se_life_count( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
uint16_t get_se_life_count( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );
uint16_t set_se_otp_status( uint16_t para, uint16_t inlen, uint8_t *inbuf, uint8_t *rbuf, uint16_t *rlen );

#endif
