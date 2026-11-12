/* server.c - 用 fork() 為每個 client 建立一個子行程
 *
 * 用法: ./server [-p port] [-v level] [-d delay] [-m max] [-t timeout] [-U]
 *   -p  監聽的 port（預設 9734）
 *   -v  執行時 log 等級 0=ERROR 1=WARN 2=INFO 3=DEBUG（也可用環境變數 LOG_LEVEL）
 *   -d  每個請求故意延遲幾秒再回覆（展示並行用，預設 0）
 *   -m  同時連線上限（預設 20，題目要求 >= 10）
 *   -t  client 閒置多久就斷線（秒，預設 10）
 *   -U  Unsafe 模式：關閉所有 robustness 機制，用來做對照組實驗
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "netutil.h"
#include "log.h"

/* 全域狀態控制 */
static int g_robust = 1;                     /* 1 = 正常健壯模式；0 = -U (Unsafe 模式，關閉防護做對照) */
static int g_delay = 0;                      /* 模擬伺服器延遲秒數，便於展示多 Client 同時連線的並發度 */
static int g_idle_timeout = 10;              /* Socket 接收閒置超時時間 (秒) */
static int g_max_clients = 20;               /* 並發連線數上限 (防 DoS/行程耗盡) */
static volatile sig_atomic_t g_active = 0;   /* 當前活躍的子行程數 (跨訊號中斷安全讀寫的原子整數型別) */
static volatile sig_atomic_t g_stop = 0;     /* 伺服器終止旗標 (收到 SIGINT/SIGTERM 時設為 1) */

/* ---------- 訊號處理 (Signal Handling & Zombie Reaper) ---------- */

/* 子行程結束時核心會發送 SIGCHLD，由此 handler 非同步回收子行程資源 */
static void on_sigchld(int sig)
{
    (void)sig;
    int saved = errno;                       /* 儲存進入 handler 前的 errno，避免被 waitpid 覆蓋導致主程式出錯 */

    /* 關鍵設計：使用非阻塞 WNOHANG 搭配 while 迴圈。
     * 因為 Linux standard signals 不排隊，若多個子行程同時結束，SIGCHLD 可能會合併成一個，
     * 必須用迴圈呼叫 waitpid 直到清空所有已結束的子行程，才能徹底杜絕殭屍行程 (Zombie Process) */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        g_active--;                          /* 每成功回收一個子行程，活躍計數器減一 */

    errno = saved;                           /* 恢復原先的 errno */
}


/* 收到 Ctrl+C (SIGINT) 或 SIGTERM 時觸發，設定終止開關 */
static void on_stop(int sig) { (void)sig; g_stop = 1; }

/* ---------- 功能設計：系統資訊擷取 (Feature: System Info) ---------- */

/* 從 Linux 虛擬檔案系統 /proc/meminfo 解析指定關鍵字的記憶體數據 (KB) */
static long meminfo_kb(const char *key)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;
    char line[256];
    long val = -1;
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            sscanf(line + klen + 1, "%ld", &val);
            break;
        }
    }
    fclose(fp);
    return val;
}

/* 組裝系統狀態報表 (OS 核心、開機時間、Load Average、記憶體、硬碟、處理該連線的行程 PID) */
static int build_sysinfo(char *out, size_t size)
{
    struct utsname u;
    struct sysinfo si;
    struct statvfs vfs; 
    double load[3] = {0};

    if (uname(&u) < 0 || sysinfo(&si) < 0) return -1;
    getloadavg(load, 3);
    long mem_total = meminfo_kb("MemTotal");
    long mem_avail = meminfo_kb("MemAvailable");

    int n = snprintf(out, size,
        "Hostname    : %s\n"
        "OS / Kernel : %s %s (%s)\n"
        "Uptime      : %ldd %02ldh %02ldm\n"
        "Load avg    : %.2f %.2f %.2f\n"
        "Memory      : %ld MB total, %ld MB available\n"
        "Processes   : %d\n",
        u.nodename, u.sysname, u.release, u.machine,
        si.uptime / 86400, (si.uptime % 86400) / 3600, (si.uptime % 3600) / 60,
        load[0], load[1], load[2],
        mem_total / 1024, mem_avail / 1024,
        si.procs);

    /* 讀取根目錄磁碟容量 */
    if (statvfs("/", &vfs) == 0 && n > 0 && (size_t)n < size) {
        double gb = 1024.0 * 1024 * 1024;
        n += snprintf(out + n, size - n, "Disk /      : %.1f GB total, %.1f GB free\n",
                      (double)vfs.f_blocks * vfs.f_frsize / gb,
                      (double)vfs.f_bavail * vfs.f_frsize / gb);
    }
    /* 證明多行程：印出真正處理該請求的子行程 PID 與伺服器父行程 PID */
    if (n > 0 && (size_t)n < size)
        n += snprintf(out + n, size - n, "Served by   : child pid %d (parent %d)\n",
                      getpid(), getppid());
    return n;
}

/* ---------- 對照組：Unsafe 模式的封包接收 (無任何防護) ---------- */
/* 故意不做 magic 驗證、不做長度防護、不設 timeout，直接 read 一次，用來對照展示異常狀況 */
static int recv_pkt_unsafe(int fd, uint16_t *type, char *buf, uint32_t *len)
{
    pkt_hdr_t h;
    if (read(fd, &h, sizeof(h)) <= 0) return PKT_EOF;
    *type = ntohs(h.type);
    *len  = ntohl(h.length);              /* 脆弱點：盲目信任對方宣告的長度，容易遭受緩衝區溢位或 DoS */
    if (*len > 0 && readn(fd, buf, *len) <= 0) return PKT_EOF;
    buf[*len] = '\0';
    return PKT_OK;
}

/* ---------- 子行程：專屬服務一個客戶端連線 ---------- */
static void handle_client(int cfd, const char *who)
{
    char req[MAX_PAYLOAD + 1];
    char resp[MAX_PAYLOAD];
    uint16_t type;
    uint32_t len;

    LOGI("child started for %s", who);
    /* Robustness 機制：設定連線逾時，避免客戶端建立連線後閒置掛起導致子行程卡死 */
    if (g_robust) set_timeout(cfd, g_idle_timeout);

    for (;;) {
        /* 依據模式切換接收函式：健壯版 vs 不安全版 */
        int rc = g_robust ? recv_pkt(cfd, &type, req, sizeof(req), &len)
                          : recv_pkt_unsafe(cfd, &type, req, &len);
        if (rc == PKT_EOF) { LOGI("%s disconnected", who); break; }
        if (rc != PKT_OK) {
            const char *why = pkt_strerror(rc);
            LOGW("%s: bad request (%s), closing", who, why);
            send_pkt(cfd, RESP_ERR, why, strlen(why)); /* 回報具體錯誤給 Client 並主動關閉連線 */
            break;
        }

        LOGI("%s -> %s (%u bytes)", who, type_name(type), len);
        if (g_delay > 0) {
            LOGD("sleeping %d s before reply", g_delay);
            sleep(g_delay);                  /* 延遲模擬高耗時處理，測試多 Client 並行 */
        }

        int n;
        uint16_t rtype = RESP_OK;
        switch (type) {
        case REQ_SYSINFO:                    /* 處理系統資訊請求 */
            n = build_sysinfo(resp, sizeof(resp));
            if (n < 0) { rtype = RESP_ERR; n = snprintf(resp, sizeof(resp), "sysinfo failed"); }
            break;
        case REQ_ECHO:                       /* 處理 Echo 回音請求 */
            n = snprintf(resp, sizeof(resp), "%s", req);
            break;
        case REQ_TIME: {                     /* 處理時間查詢請求 */
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            n = strftime(resp, sizeof(resp), "%Y-%m-%d %H:%M:%S %Z", &tm);
            break;
        }
        default:                             /* 不支援的未知指令碼 */
            rtype = RESP_ERR;
            n = snprintf(resp, sizeof(resp), "unknown request type %u", type);
        }

        if (n < 0) n = 0;
        if ((size_t)n >= sizeof(resp)) n = sizeof(resp) - 1; /* 邊界截斷防護 */

        /* 將結果封裝為回應封包送回 */
        if (send_pkt(cfd, rtype, resp, (uint32_t)n) < 0) {
            LOGW("%s: send failed: %s", who, strerror(errno));
            break;
        }
        LOGD("replied %s to %s", type_name(rtype), who);
    }
    close(cfd);                              /* 關閉該連線的 socket */
    LOGI("child exiting");
}

/* ---------- main：父行程核心，專責監聽 (listen) 與派發 (fork) ---------- */
int main(int argc, char *argv[])
{
    int port = SERVER_PORT, cli_level = -1, opt;

    /* 解析命令列參數 */
    while ((opt = getopt(argc, argv, "p:v:d:m:t:U")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'v': cli_level = atoi(optarg); break;
        case 'd': g_delay = atoi(optarg); break;
        case 'm': g_max_clients = atoi(optarg); break;
        case 't': g_idle_timeout = atoi(optarg); break;
        case 'U': g_robust = 0; break;       /* 啟動 Unsafe 模式 */
        default:
            fprintf(stderr, "usage: %s [-p port] [-v level] [-d delay] [-m max] [-t timeout] [-U]\n", argv[0]);
            return 1;
        }
    }
    log_init(cli_level);                     /* 初始化 Log 等級 */

    /* 訊號處理設定 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_stop;                 /* 捕捉 SIGINT 與 SIGTERM，觸發優雅關機 */
    sigaction(SIGINT, &sa, NULL);            /* 註冊 SIGINT handler，捕捉 Ctrl+C */
    sigaction(SIGTERM, &sa, NULL);           /* 註冊 SIGTERM handler，捕捉 kill 指令 */

    if (g_robust) {
        sa.sa_handler = on_sigchld;          /* Robustness 機制 1：註冊 SIGCHLD handler 回收子行程 */
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; /*SA_RESTART 當訊號處理完之後，請作業系統自動重新呼叫 accept()，假裝剛剛的插隊沒發生過。SA_NOCLDSTOP 忽略子行程暫停/繼續訊號 */
        sigaction(SIGCHLD, &sa, NULL);        /* 註冊 SIGCHLD handler，避免殭屍行程 (Zombie Process) */
        signal(SIGPIPE, SIG_IGN);            /* Robustness 機制 2：忽略 SIGPIPE，防對端中途斷線導致伺服器崩潰 */
    }

    /* 建立 TCP 監聽 Socket */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { LOGE("socket: %s", strerror(errno)); return 1; }

    if (g_robust) {
        int on = 1;                          /* Robustness 機制 3：啟用 SO_REUSEADDR，防止重啟時發生 Port 佔用錯誤 */
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    }

    /* 設定並綁定伺服器 IP 與 Port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("bind port %d: %s", port, strerror(errno));
        return 1;
    }
    if (listen(lfd, 64) < 0) { LOGE("listen: %s", strerror(errno)); return 1; }

    LOGI("server listening on port %d (robust=%s, delay=%ds, max=%d, idle timeout=%ds)",
         port, g_robust ? "ON" : "OFF", g_delay, g_max_clients, g_idle_timeout);

    /* 準備 SIGCHLD 遮罩，用於保護 fork 與 g_active 計數的原子性 */
    sigset_t chld;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);

    /* 主伺服器事件循環 (Event Loop) */
    while (!g_stop) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(lfd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;    /* 若被中斷訊號打斷，直接重試並檢查 g_stop */
            LOGE("accept: %s", strerror(errno));
            continue;
        }

        char who[64];
        snprintf(who, sizeof(who), "%s:%d", inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));

        /* Robustness 機制 4：連線上限防護。達到上限時回覆 RESP_BUSY 並立即關閉連線 */
        if (g_robust && g_active >= g_max_clients) {
            LOGW("too many clients (%d), rejecting %s", (int)g_active, who);
            const char *msg = "server busy, try again later";
            send_pkt(cfd, RESP_BUSY, msg, strlen(msg));
            close(cfd);
            continue;
        }

        /* 關鍵防護：暫時遮蔽 SIGCHLD 訊號，防止 fork 成功後尚未執行 g_active++ 前
         * 子行程就瞬間結束並觸發 handler 執行 g_active--，造成嚴重的競爭危害 (Race Condition) */
        sigprocmask(SIG_BLOCK, &chld, NULL);
        pid_t pid = fork();                  /* 核心要求：呼叫 fork() 產生子行程處理連線 (嚴禁 Thread) */

        if (pid == 0) {
            /* ----- 子行程分支 (Child Process) ----- */
            sigprocmask(SIG_UNBLOCK, &chld, NULL); /* 子行程解除訊號遮蔽 */
            close(lfd);                      /* 子行程不負責接受新連線，關閉 listen socket */
            handle_client(cfd, who);         /* 進入專屬通訊迴圈 */
            exit(0);                         /* 服務結束後主動退出子行程 */
        }
        if (pid > 0) {
            /* ----- 父行程分支 (Parent Process) ----- */
            g_active++;                      /* 活躍行程數加一 */
            LOGI("accepted %s -> child pid %d (active=%d)", who, pid, (int)g_active);
        } else {
            LOGE("fork failed: %s", strerror(errno));
        }
        sigprocmask(SIG_UNBLOCK, &chld, NULL); /* 父行程完成計數，解除 SIGCHLD 遮蔽 */
        close(cfd);                          /* 父行程將已交接給子行程的 client socket 關閉，釋放描述子 */
    }

    LOGI("server shutting down");
    close(lfd);                              /* 關閉監聽 socket，釋放埠號 */
    return 0;
}