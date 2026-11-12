/* client.c - 向 server 發出請求
 *
 * 用法: ./client [-h host] [-p port] [-v level] [-r retries] [-t timeout] [-n id] [-U] <command> [args]
 *   command:
 *     sysinfo         查詢 server 系統資訊
 *     time            查詢 server 時間
 *     echo <text>     回音
 *     idle <sec>      只連線不送資料，觀察 server 會不會踢掉閒置連線（測試用）
 *     badmagic        送出 magic 錯誤的封包（測試用）
 *     oversize        送出長度超過上限的封包（測試用）
 *   -r  連不上時重試幾次（預設 3，每次等待時間加倍）
 *   -t  等待回覆的 timeout 秒數（預設 15）
 *   -n  client 編號，只用來讓輸出好辨認
 *   -U  Unsafe 模式：不重試、不設 timeout
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "netutil.h"
#include "log.h"

static int g_robust = 1;

static double now_sec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* 機制：連線失敗時 exponential backoff 重試 */
static int connect_server(const char *host, int port, int retries)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        LOGE("invalid address: %s", host);
        return -1;
    }

    int wait = 1;
    for (int attempt = 0; ; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { LOGE("socket: %s", strerror(errno)); return -1; }
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            LOGD("connected to %s:%d", host, port);
            return fd;
        }
        int err = errno;
        close(fd);
        if (!g_robust || attempt >= retries) {
            LOGE("connect %s:%d failed: %s", host, port, strerror(err));
            return -1;
        }
        LOGW("connect failed (%s), retry %d/%d in %d s", strerror(err), attempt + 1, retries, wait);
        sleep(wait);
        wait *= 2;
    }
}

/* 送一個故意做壞的封包 */
static int send_bad(int fd, int oversize)
{
    pkt_hdr_t h;
    uint32_t len = oversize ? 4600 : 4;   /* 4600 > MAX_PAYLOAD(4096) */
    h.magic  = htons(oversize ? PROTO_MAGIC : 0xDEAD);
    h.type   = htons(REQ_ECHO);
    h.length = htonl(len);
    char *body = malloc(len);
    if (!body) return -1;
    memset(body, 'A', len);
    LOGI("sending malformed packet: magic=0x%04X length=%u", ntohs(h.magic), len);
    int ok = writen(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) &&
             writen(fd, body, len) == (ssize_t)len;
    free(body);
    if (!ok) LOGW("write failed: %s (server probably closed the connection)", strerror(errno));
    return 0;
}

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    int port = SERVER_PORT, cli_level = -1, retries = 3, timeout = 15, id = 0, opt;

    while ((opt = getopt(argc, argv, "h:p:v:r:t:n:U")) != -1) {
        switch (opt) {
        case 'h': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'v': cli_level = atoi(optarg); break;
        case 'r': retries = atoi(optarg); break;
        case 't': timeout = atoi(optarg); break;
        case 'n': id = atoi(optarg); break;
        case 'U': g_robust = 0; break;
        default: goto usage;
        }
    }
    if (optind >= argc) goto usage;
    log_init(cli_level);
    const char *cmd = argv[optind];

    if (g_robust) signal(SIGPIPE, SIG_IGN);  /* 機制：忽略 SIGPIPE(關閉的 Socket 寫資料，OS發送的訊號)，防對端中途斷線導致 client 崩潰 */

    int fd = connect_server(host, port, retries);
    if (fd < 0) return 1;
    if (g_robust) set_timeout(fd, timeout);   /* 機制：server 沒回應時不會永遠卡住 */

    double t0 = now_sec();
    uint16_t type;
    uint32_t len;
    char buf[MAX_PAYLOAD + 1];

    if (strcmp(cmd, "sysinfo") == 0) {
        send_pkt(fd, REQ_SYSINFO, NULL, 0);
    } else if (strcmp(cmd, "time") == 0) {
        send_pkt(fd, REQ_TIME, NULL, 0);
    } else if (strcmp(cmd, "echo") == 0 && optind + 1 < argc) { 
        const char *text = argv[optind + 1];
        send_pkt(fd, REQ_ECHO, text, strlen(text));
    } else if (strcmp(cmd, "idle") == 0) { 
        int sec = optind + 1 < argc ? atoi(argv[optind + 1]) : 30;
        set_timeout(fd, sec);
        printf("[client %d] connected, sending nothing, waiting up to %d s...\n", id, sec);
        fflush(stdout);
        int rc = recv_pkt(fd, &type, buf, sizeof(buf), &len);
        if (rc == PKT_EOF)
            printf("[client %d] server closed the idle connection after %.1f s\n", id, now_sec() - t0);
        else if (rc == PKT_IOERR)
            printf("[client %d] after %.1f s the server is STILL holding the connection\n", id, now_sec() - t0);
        else
            printf("[client %d] got %s: %s\n", id, type_name(type), buf);
        close(fd);
        return 0;
    } else if (strcmp(cmd, "badmagic") == 0) {
        send_bad(fd, 0);
    } else if (strcmp(cmd, "oversize") == 0) {
        send_bad(fd, 1);
    } else {
        close(fd);
        goto usage;
    }

    int rc = recv_pkt(fd, &type, buf, sizeof(buf), &len);
    double dt = now_sec() - t0;
    if (rc != PKT_OK) {
        LOGE("no valid reply: %s", pkt_strerror(rc));
        printf("[client %d] FAILED after %.2f s: %s\n", id, dt, pkt_strerror(rc));
        close(fd);
        return 1;
    }
    printf("[client %d] %s in %.2f s\n%s\n", id, type_name(type), dt, buf);
    close(fd);
    return type == RESP_OK ? 0 : 2;

usage:
    fprintf(stderr,
        "usage: %s [-h host] [-p port] [-v level] [-r retries] [-t timeout] [-n id] [-U] <command> [args]\n"
        "commands: sysinfo | time | echo <text> | idle <sec> | badmagic | oversize\n", argv[0]);
    return 1;
}