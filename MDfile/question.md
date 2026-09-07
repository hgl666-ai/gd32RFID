# FMSE 安全芯片对接 — 确认事项

## ~~问题 1：write_se_data 的 P1P2 参数~~ ✅ 已确认

**结论：** P1P2 = 0x0000，与 SDK 示例代码一致，I2C 接口下 P1P2 填 0 即可。

---

## ~~问题 2：write_se_data 的 inbuf 格式~~ ✅ 已确认

**结论：** inbuf 采用 TLV 格式，已根据 SDK 示例代码更新实现。

**TLV 格式 (写入):**
```
C0 02 00 03   — 模式标识
C1 02 00 00   — 参数
C2 XX data    — 实际数据 (XX = data 长度)
```

代码已更新为:
```c
uint8_t write_tlv[9 + plain_len];
write_tlv[0] = 0xC0; write_tlv[1] = 0x02; write_tlv[2] = 0x00; write_tlv[3] = 0x03;
write_tlv[4] = 0xC1; write_tlv[5] = 0x02; write_tlv[6] = 0x00; write_tlv[7] = 0x00;
write_tlv[8] = 0xC2; write_tlv[9] = plain_len;
memcpy(&write_tlv[10], pPlain, plain_len);
sw = write_se_data(0x0000, 9 + plain_len, write_tlv, rbuf, &rlen);
```

---

## ~~问题 3：get_se_data 的 inbuf 格式~~ ✅ 已确认

**结论：** inbuf 同样采用 TLV 格式，已根据 SDK 示例代码更新实现。

**TLV 格式 (读取):**
```
C0 02 00 03   — 模式标识
C1 02 00 00   — 参数
C2 01 XX      — 读取长度 (XX = 期望读取的字节数)
```

代码已更新为:
```c
uint8_t read_len = ((plain_len + 7) / 8) * 8;  /* 8 字节对齐 */
uint8_t read_tlv[11];
read_tlv[0] = 0xC0; read_tlv[1] = 0x02; read_tlv[2] = 0x00; read_tlv[3] = 0x03;
read_tlv[4] = 0xC1; read_tlv[5] = 0x02; read_tlv[6] = 0x00; read_tlv[7] = 0x00;
read_tlv[8] = 0xC2; read_tlv[9] = 0x01; read_tlv[10] = read_len;
sw = get_se_data(0x0000, 11, read_tlv, pCipher, &cipher_len16);
```

---

## 问题 4：SE 内部加密行为 (仍需 SE 侧确认)

**代码位置：** `Bsp/bsp_crypto.c` 第 104~152 行

**问题详述：**

当前采用两步法:
1. `write_se_data` — 把明文以 TLV 格式写入 SE
2. `get_se_data` — 从 SE 读取加密结果

**需要确认：** SE 收到 `write_se_data` 写入的明文后，是否自动对其执行加密？如果 `get_se_data` 读回的就是密文，当前实现正确。如果 SE 仅存储而不加密，需要改用 `DataEnDecrypt` 指令。

**向领导说：**
> write_se_data 写入明文后，SE 内部是否会自动加密？get_se_data 读回的是密文还是原样明文？如果 SE 不自动加密，应该用哪条指令做加密？

---

## 问题 5：密文长度与缓冲区 (仍需 SE 侧确认)

**代码位置：** `Bsp/bsp_crypto.c` 第 146~156 行，`Head/bsp_crypto.h` 第 16 行

**问题详述：**

当前加密输入为 40 字节明文，SE 返回的密文长度存入 `cipher_len16`（uint16_t），然后截断为 `uint8_t` 赋给调用方。

需要确认：
1. 40 字节明文经 SE 加密后，密文长度是多少字节？（40？48？其他？）
2. 如果密文超过 64 字节，需要增大 `CRYPTO_CIPHER_MAX_LEN`（当前 64）
3. 密文长度是否会超过 255？如果会，`uint8_t` 截断会丢失高位

**向领导说：**
> 40 字节明文加密后密文有多大？我们缓冲区预留了 64 字节够不够？密文长度会不会超过 255 字节？请 SE 侧确认。

---

## ~~问题 6：SE 认证凭证~~ ✅ 已确认

**结论：** mcu_uid[8] 和 mcu_com_key[16] 与 SE 侧预置值一致，直接使用 SDK 中的硬编码值即可。
