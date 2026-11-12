/* log.h - 兩層 debug log 控制
 *
 * 第一層（編譯時）：COMPILE_LOG_LEVEL 決定哪些 LOG 巨集會被編進執行檔
 *     make          -> COMPILE_LOG_LEVEL=2，編進 ERROR、WARN、INFO
 *     make DEBUG=1  -> COMPILE_LOG_LEVEL=3，ERROR/WARN/INFO/DEBUG 全部編進去
 *   沒被編進去的巨集會變成 ((void)0)，執行檔裡完全沒有那些字串。
 *
 * 第二層（執行時）：g_log_level 決定「已編進去的」log 要印到哪一級
 *     ./server -v 3      或     LOG_LEVEL=3 ./server
 */
#ifndef LOG_H
#define LOG_H

enum { LV_ERROR = 0, LV_WARN = 1, LV_INFO = 2, LV_DEBUG = 3 };

#ifndef COMPILE_LOG_LEVEL
#define COMPILE_LOG_LEVEL 2
#endif

extern int g_log_level;                 /* 執行時等級，定義在 libnetutil.so */
void log_init(int cli_level);           /* cli_level < 0 表示沒指定 -v，改看環境變數 */
void log_write(int lv, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define LOG_AT(lv, ...) \
    do { if ((lv) <= g_log_level) log_write((lv), __FILE__, __LINE__, __VA_ARGS__); } while (0)

#define LOGE(...) LOG_AT(LV_ERROR, __VA_ARGS__)

#if COMPILE_LOG_LEVEL >= 1
#define LOGW(...) LOG_AT(LV_WARN, __VA_ARGS__)
#else
#define LOGW(...) ((void)0)
#endif

#if COMPILE_LOG_LEVEL >= 2
#define LOGI(...) LOG_AT(LV_INFO, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#endif

#if COMPILE_LOG_LEVEL >= 3
#define LOGD(...) LOG_AT(LV_DEBUG, __VA_ARGS__)
#else
#define LOGD(...) ((void)0)
#endif

#endif