/* netutil.c - 編譯成 libnetutil.so，client 與 server 共用 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "netutil.h"
#include "log.h"

/* ---------------- log ---------------- */
int g_log_level = LV_INFO;   /* 預設執行時等級 */

static const char *lv_name[] = { "ERROR", "WARN ", "INFO ", "DEBUG" };

void log_init(int cli_level)
{
    if (cli_level >= 0) {                 /* 命令列 -v 優先 */
        g_log_level = cli_level;
    } else {
        const char *env = getenv("LOG_LEVEL");
        if (env) g_log_level = atoi(env);
    }
    if (g_log_level < LV_ERROR) g_log_level = LV_ERROR;
    if (g_log_level > LV_DEBUG) g_log_level = LV_DEBUG;
}

void log_write(int lv, const char *file, int line, const char *fmt, ...)
{
    struct timeval tv;
    struct tm tm;
    char ts[32];
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* 一次 fprintf 輸出整行，多個子行程同時寫時比較不會交錯 */
    fprintf(stderr, "%s.%03ld [%s][pid %d] %s:%d %s\n",
            ts, (long)(tv.tv_usec / 1000), lv_name[lv], getpid(), file, line, msg);
}

/* ---------------- 可靠讀寫 ---------------- */
/* TCP 是位元組串流，一次 read 不保證拿到完整資料，所以要用迴圈讀滿 */
ssize_t readn(int fd, void *buf, size_t n)
{
    size_t left = n;
    char *p = buf;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r < 0) {
            if (errno == EINTR) continue;   /* 被訊號打斷，重來 */
            return -1;
        }
        if (r == 0) break;                  /* EOF */
        left -= r;
        p += r;
    }
    return n - left;
}

ssize_t writen(int fd, const void *buf, size_t n)
{
    size_t left = n;
    const char *p = buf;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;                      /* 例如 EPIPE：對方已關閉 */
        }
        left -= w;
        p += w;
    }
    return n;
}

/* ---------------- 封包收送 ---------------- */
int send_pkt(int fd, uint16_t type, const void *payload, uint32_t len)
{
    pkt_hdr_t h;
    h.magic  = htons(PROTO_MAGIC);
    h.type   = htons(type);
    h.length = htonl(len);
    if (writen(fd, &h, sizeof(h)) != (ssize_t)sizeof(h)) return -1;
    if (len > 0 && writen(fd, payload, len) != (ssize_t)len) return -1;
    LOGD("send_pkt type=%s len=%u", type_name(type), len);
    return 0;
}

int recv_pkt(int fd, uint16_t *type, char *buf, uint32_t bufsize, uint32_t *len)
{
    pkt_hdr_t h;
    ssize_t r = readn(fd, &h, sizeof(h));
    if (r < 0) return PKT_IOERR;
    if (r < (ssize_t)sizeof(h)) return PKT_EOF;

    if (ntohs(h.magic) != PROTO_MAGIC) return PKT_BADMAGIC;   /* 檢查 1：magic */
    *type = ntohs(h.type);
    *len  = ntohl(h.length);
    if (*len >= bufsize) return PKT_TOOBIG;                   /* 檢查 2：長度上限（留 1 byte 放 '\0'） */

    if (*len > 0) {
        r = readn(fd, buf, *len);
        if (r < 0) return PKT_IOERR;
        if (r < (ssize_t)*len) return PKT_EOF;
    }
    buf[*len] = '\0';
    LOGD("recv_pkt type=%s len=%u", type_name(*type), *len);
    return PKT_OK;
}

int set_timeout(int fd, int sec)
{
    struct timeval tv = { .tv_sec = sec, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) return -1;
    return 0;
}

const char *type_name(uint16_t t)
{
    switch (t) {
    case REQ_SYSINFO: return "REQ_SYSINFO";
    case REQ_ECHO:    return "REQ_ECHO";
    case REQ_TIME:    return "REQ_TIME";
    case RESP_OK:     return "RESP_OK";
    case RESP_ERR:    return "RESP_ERR";
    case RESP_BUSY:   return "RESP_BUSY";
    default:          return "UNKNOWN";
    }
}

const char *pkt_strerror(int code)
{
    switch (code) {
    case PKT_OK:       return "ok";
    case PKT_EOF:      return "peer closed connection";
    case PKT_IOERR:    return errno == EAGAIN || errno == EWOULDBLOCK ? "timeout" : strerror(errno);
    case PKT_BADMAGIC: return "bad magic number";
    case PKT_TOOBIG:   return "payload too large";
    default:           return "unknown error";
    }
}