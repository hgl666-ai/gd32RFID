#ifndef __PROTOCOL_H
#define __PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* 定义支持的最大帧长度，可根据实际单片机 RAM 资源调整 */
#define MAX_FRAME_LEN       256U

/* 帧头定义 */
#define FRAME_HEADER_0      0xA5U
#define FRAME_HEADER_1      0x5AU

/* 帧结构偏移量定义:
 * HEADER(2B) + CMD(1B) + LENGTH(1B) + DATA(LENGTH B) + CRC16_H(1B) + CRC16_L(1B)
 */
#define FRAME_HEADER_SIZE   2U    /* 帧头字节数 */
#define FRAME_CMD_OFFSET    2U    /* CMD 在帧中的偏移 */
#define FRAME_LEN_OFFSET    3U    /* LENGTH 在帧中的偏移 */
#define FRAME_DATA_OFFSET   4U    /* DATA 在帧中的偏移 */
#define FRAME_TAIL_SIZE     2U    /* CRC16 尾部字节数 */
#define FRAME_MIN_SIZE      6U    /* 最小帧长度 (HEADER + CMD + LEN(0) + CRC16) */

/* 协议命令码定义 */
#define CMD_QUERY_UID       0x01U  /* 查询主控UID */
#define CMD_WRITE_KEY       0x02U  /* 写入16字节KEY */
#define CMD_UPLOAD_TAG      0x01U  /* 上传标签数据 (与UID查询共用CMD，通过LENGTH区分) */

/* 协议解析状态 (状态机) */
typedef enum {
    PARSE_WAIT_HEADER_0 = 0,  /* 等待帧头第1字节 0xA5 */
    PARSE_WAIT_HEADER_1,      /* 等待帧头第2字节 0x5A */
    PARSE_WAIT_CMD,           /* 等待命令码 */
    PARSE_WAIT_LENGTH,        /* 等待数据长度 */
    PARSE_WAIT_DATA,          /* 等待数据内容 */
    PARSE_WAIT_CRC_H,         /* 等待CRC高字节 */
    PARSE_WAIT_CRC_L          /* 等待CRC低字节 */
} parse_state_enum;

/* 协议解析结果 */
typedef enum {
    PARSE_RESULT_OK       = 0,  /* 解析成功，完整帧已就绪 */
    PARSE_RESULT_WAITING  = 1,  /* 正在接收，尚未完成 */
    PARSE_RESULT_CRC_ERR  = 2,  /* CRC 校验失败 */
    PARSE_RESULT_LEN_ERR  = 3,  /* 数据长度异常 */
    PARSE_RESULT_FRAME_ERR= 4   /* 帧格式错误 */
} parse_result_enum;

/* 解析后的帧数据结构 */
typedef struct {
    uint8_t  cmd;                        /* 命令码 */
    uint8_t  length;                     /* 数据长度 */
    uint8_t  data[MAX_FRAME_LEN - FRAME_MIN_SIZE]; /* 数据域 */
    uint16_t crc16;                      /* 接收到的 CRC16 值 */
} parsed_frame_t;

/* ================= 接口函数声明 ================= */

/* 计算 CRC16-Modbus (内部使用或开放给外部校验用) */
uint16_t Calculate_CRC16_Modbus(uint8_t *pData, uint16_t len);

/* 组装标准数据帧 */
uint16_t Pack_Data_Frame(uint8_t cmd, uint8_t *pData, uint8_t dataLen, uint8_t *outBuffer);

/* 初始化协议解析状态机 */
void protocol_parser_init(void);

/* 输入一个字节到协议解析状态机 */
parse_result_enum protocol_parse_byte(uint8_t byte);

/* 获取最近一次成功解析的帧数据 */
const parsed_frame_t* protocol_get_parsed_frame(void);

/* 校验接收帧的 CRC16 (对已解析的帧进行校验) */
uint8_t protocol_verify_crc(const parsed_frame_t *pFrame);

#endif /* __PROTOCOL_H */
