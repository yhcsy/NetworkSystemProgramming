/* protocol.h - client 與 server 共同約定的封包格式 */
#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <stdint.h>

#define SERVER_PORT  9734
#define PROTO_MAGIC  0x4D54      /* 'M''T'，用來辨識是不是我們的封包 */
#define MAX_PAYLOAD  4096

/* 封包種類 */
enum msg_type {
    REQ_SYSINFO = 1,   /* 查詢系統資訊 */
    REQ_ECHO    = 2,   /* 回音 */
    REQ_TIME    = 3,   /* 查詢 server 時間 */
    RESP_OK     = 100,
    RESP_ERR    = 101,
    RESP_BUSY   = 102  /* 連線數已滿 */
};

/* 封包標頭：8 bytes，傳送時一律轉成 network byte order */
typedef struct {
    uint16_t magic;
    uint16_t type;
    uint32_t length;   /* 後面 payload 的長度 */
} __attribute__((packed)) pkt_hdr_t;

#endif