/* netutil.h - libnetutil.so 對外提供的函式（client、server 共用） */
#ifndef NETUTIL_H
#define NETUTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "protocol.h"

/* recv_pkt 的回傳值 */
#define PKT_OK        0
#define PKT_EOF      -1   /* 對方正常關閉連線 (收到 0 byte) */
#define PKT_IOERR    -2   /* I/O 失敗（包含 network error 或 timeout） */
#define PKT_BADMAGIC -3   /* 協定魔術數字不符 (非法連線/畸形封包) */
#define PKT_TOOBIG   -4   /* 宣告長度超過 MAX_PAYLOAD 或本地緩衝區容量 */

/* 底層確保讀滿/寫滿 n bytes 的工具函式 (W. Richard Stevens 經典設計) */
ssize_t readn(int fd, void *buf, size_t n);
ssize_t writen(int fd, const void *buf, size_t n);

/* 高階封包傳收介面：自動處理網路位元組序 (Endianness)、標頭解析與防呆邊界檢查 */
int send_pkt(int fd, uint16_t type, const void *payload, uint32_t len);
int recv_pkt(int fd, uint16_t *type, char *buf, uint32_t bufsize, uint32_t *len);

/* Socket 工具函式 */
int set_timeout(int fd, int sec);   /* 設定收送超時 (SO_RCVTIMEO / SO_SNDTIMEO)，0 代表不限時 */
const char *type_name(uint16_t type);      /* 轉印字串 (如 "REQ_SYSINFO")，便於 Debug 追蹤 */
const char *pkt_strerror(int code);        /* 將錯誤代碼轉為人類可讀字串，報告呈現極佳 */

#endif /* NETUTIL_H */