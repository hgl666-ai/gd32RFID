#include "protocol.h"
#include <string.h>

/*
 * 协议解析状态机模块:
 *   - 采用逐字节状态机解析，适合中断+主循环架构
 *   - 每收到一个字节调用 protocol_parse_byte() 推入状态机
 *   - 状态机自动完成帧头识别、数据接收、CRC校验
 *   - 解析成功后通过 protocol_get_parsed_frame() 获取帧数据
 */

/* 解析状态机上下文 */
static parse_state_enum s_parse_state = PARSE_WAIT_HEADER_0;
static parsed_frame_t   s_parsed_frame;
static uint8_t          s_data_index = 0;  /* 当前数据域接收计数 */

/**
 * @brief  计算 CRC16-Modbus 校验值
 * @param  pData: 需要校验的数据指针
 * @param  len:   数据长度
 * @retval 计算得到的 16 位 CRC 校验码
 * @note   多项式 0x8005 (反序为 0xA001), 初始值 0xFFFF, 结果不异或
 */
uint16_t Calculate_CRC16_Modbus(uint8_t *pData, uint16_t len) {
    uint16_t crc = 0xFFFF; /* 初始预置值 */

    for (uint16_t i = 0; i < len; i++) {
        crc ^= pData[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001; /* 0x8005 的反序 */
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/**
 * @brief  组装通信数据帧
 * @note   帧结构: HEADER(A55A) + CMD(1Byte) + LENGH(1Byte) + DATA(N Bytes) + CRC16(2Bytes)
 * @param  cmd       命令码
 * @param  pData     有效数据指针 (如果此帧不需要携带数据，传入 NULL 即可)
 * @param  dataLen   有效数据长度 (如果此帧不需要携带数据，传入 0 即可)
 * @param  outBuffer 组装完成后的发送缓冲区数组
 * @return           组装完成后的整帧总字节数，直接用于串口发送
 */
uint16_t Pack_Data_Frame(uint8_t cmd, uint8_t *pData, uint8_t dataLen, uint8_t *outBuffer) {
    uint16_t offset = 0;

    /* 1. 填充帧头 HEADER (0xA5, 0x5A) */
    outBuffer[offset++] = 0xA5;
    outBuffer[offset++] = 0x5A;

    /* 2. 填充命令码 CMD */
    outBuffer[offset++] = cmd;

    /* 3. 填充数据长度 LENGH */
    outBuffer[offset++] = dataLen;

    /* 4. 填充有效数据 DATA */
    if (pData != NULL && dataLen > 0) {
        for(uint8_t i = 0; i < dataLen; i++) {
            outBuffer[offset++] = pData[i];
        }
    }

    /* 5. 计算当前包（Header+CMD+Length+Data）的 CRC16 */
    uint16_t crc16_val = Calculate_CRC16_Modbus(outBuffer, offset);

    /* 6. 追加 CRC16 校验码 (大端格式：高字节在前，低字节在后) */
    outBuffer[offset++] = (uint8_t)(crc16_val >> 8);   /* 高 8 位 */
    outBuffer[offset++] = (uint8_t)(crc16_val & 0xFF); /* 低 8 位 */

    /* 返回最终的总长度 */
    return offset;
}

/**
 * @brief  初始化协议解析状态机
 */
void protocol_parser_init(void)
{
    s_parse_state = PARSE_WAIT_HEADER_0;
    memset(&s_parsed_frame, 0, sizeof(s_parsed_frame));
    s_data_index = 0;
}

/**
 * @brief  输入一个字节到协议解析状态机
 * @param  byte: 接收到的字节
 * @retval 解析结果
 * @note   每次从串口接收到一个字节后调用此函数
 *         返回 PARSE_RESULT_OK 表示一帧完整数据已就绪
 */
parse_result_enum protocol_parse_byte(uint8_t byte)
{
    parse_result_enum result = PARSE_RESULT_WAITING;

    switch (s_parse_state) {
    /* ---- 等待帧头第1字节 0xA5 ---- */
    case PARSE_WAIT_HEADER_0:
        if (byte == FRAME_HEADER_0) {
            s_parse_state = PARSE_WAIT_HEADER_1;
        }
        break;

    /* ---- 等待帧头第2字节 0x5A ---- */
    case PARSE_WAIT_HEADER_1:
        if (byte == FRAME_HEADER_1) {
            s_parse_state = PARSE_WAIT_CMD;
            s_data_index = 0;
            memset(&s_parsed_frame, 0, sizeof(s_parsed_frame));
        } else if (byte == FRAME_HEADER_0) {
            /* 连续两个 0xA5，保留状态继续等待 0x5A */
        } else {
            s_parse_state = PARSE_WAIT_HEADER_0;
        }
        break;

    /* ---- 等待命令码 ---- */
    case PARSE_WAIT_CMD:
        s_parsed_frame.cmd = byte;
        s_parse_state = PARSE_WAIT_LENGTH;
        break;

    /* ---- 等待数据长度 ---- */
    case PARSE_WAIT_LENGTH:
        s_parsed_frame.length = byte;
        if (byte == 0) {
            /* 无数据域，直接等待 CRC */
            s_parse_state = PARSE_WAIT_CRC_H;
        } else if (byte > (MAX_FRAME_LEN - FRAME_MIN_SIZE)) {
            /* 数据长度异常，超出最大允许值 */
            s_parse_state = PARSE_WAIT_HEADER_0;
            result = PARSE_RESULT_LEN_ERR;
        } else {
            s_parse_state = PARSE_WAIT_DATA;
            s_data_index = 0;
        }
        break;

    /* ---- 等待数据内容 ---- */
    case PARSE_WAIT_DATA:
        s_parsed_frame.data[s_data_index++] = byte;
        if (s_data_index >= s_parsed_frame.length) {
            s_parse_state = PARSE_WAIT_CRC_H;
        }
        break;

    /* ---- 等待 CRC 高字节 ---- */
    case PARSE_WAIT_CRC_H:
        s_parsed_frame.crc16 = (uint16_t)byte << 8;
        s_parse_state = PARSE_WAIT_CRC_L;
        break;

    /* ---- 等待 CRC 低字节 ---- */
    case PARSE_WAIT_CRC_L:
        s_parsed_frame.crc16 |= (uint16_t)byte;

        /* 一帧接收完成，校验 CRC */
        if (protocol_verify_crc(&s_parsed_frame)) {
            result = PARSE_RESULT_OK;
        } else {
            result = PARSE_RESULT_CRC_ERR;
        }

        /* 无论校验成功与否，重置状态机等待下一帧 */
        s_parse_state = PARSE_WAIT_HEADER_0;
        break;

    default:
        s_parse_state = PARSE_WAIT_HEADER_0;
        break;
    }

    return result;
}

/**
 * @brief  获取最近一次成功解析的帧数据
 * @retval 指向解析帧结构体的只读指针
 */
const parsed_frame_t* protocol_get_parsed_frame(void)
{
    return &s_parsed_frame;
}

/**
 * @brief  校验接收帧的 CRC16
 * @param  pFrame: 指向已解析的帧结构体
 * @retval 1: CRC 校验通过  0: CRC 校验失败
 * @note   CRC 校验范围: HEADER(2B) + CMD(1B) + LENGTH(1B) + DATA(LENGTH B)
 */
uint8_t protocol_verify_crc(const parsed_frame_t *pFrame)
{
    if (pFrame == NULL) return 0;

    /* 构建待校验缓冲区: HEADER + CMD + LENGTH + DATA
     * 使用 static 避免栈上分配 256 字节 (总栈仅 1KB) */
    static uint8_t temp_buf[MAX_FRAME_LEN];
    uint16_t offset = 0;

    temp_buf[offset++] = FRAME_HEADER_0;
    temp_buf[offset++] = FRAME_HEADER_1;
    temp_buf[offset++] = pFrame->cmd;
    temp_buf[offset++] = pFrame->length;

    for (uint8_t i = 0; i < pFrame->length; i++) {
        temp_buf[offset++] = pFrame->data[i];
    }

    /* 计算 CRC16 并与帧中的 CRC 比较 */
    uint16_t calc_crc = Calculate_CRC16_Modbus(temp_buf, offset);

    return (calc_crc == pFrame->crc16) ? 1 : 0;
}
