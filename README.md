# 專案架構
```
.
├── Makefile
├── include/
│   ├── protocol.h      # 封包格式、port、magic、type 定義
│   ├── log.h           # 兩層 log 巨集（LOGE / LOGW / LOGI / LOGD）
│   └── netutil.h       # 共用函式宣告
├── src/
│   ├── netutil.c       # 編譯成 lib/libnetutil.so
│   ├── server.c        # fork 多行程 server
│   └── client.c        # 命令列 client
├── scripts/
│   ├── test_concurrency.sh   # 並行測試
│   └── capture.sh            # tcpdump 封包擷取
├── lib/                # [編譯產出] libnetutil.so
└── bin/                # [編譯產出] server、client
```
