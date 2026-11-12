CC = gcc
CFLAGS = -Wall -Wextra -Iinclude
LDFLAGS = -Llib -lnetutil -Wl,-rpath,'$$ORIGIN/../lib'
HEADERS = include/netutil.h include/protocol.h include/log.h

# 雙層 Log 第一層 (Compile-time)：預設等級為 2 (INFO)；輸入 make DEBUG=1 提升為 3 (DEBUG)
ifeq ($(DEBUG), 1)
    CFLAGS += -DCOMPILE_LOG_LEVEL=3 -g
else
    CFLAGS += -DCOMPILE_LOG_LEVEL=2 -O2
endif

SO_TARGET = lib/libnetutil.so
SERVER_TARGET = bin/server
CLIENT_TARGET = bin/client

all: $(SO_TARGET) $(SERVER_TARGET) $(CLIENT_TARGET)

# 編譯共用動態庫 .so
$(SO_TARGET): src/netutil.c $(HEADERS)
	@mkdir -p lib
	$(CC) $(CFLAGS) -fPIC -shared src/netutil.c -o $(SO_TARGET)
	@echo "==> Successfully built $(SO_TARGET)"

# 編譯 server，連結 libnetutil.so
$(SERVER_TARGET): src/server.c $(SO_TARGET) $(HEADERS)
	@mkdir -p bin
	$(CC) $(CFLAGS) src/server.c -o $(SERVER_TARGET) $(LDFLAGS)

# 編譯 client，連結 libnetutil.so
$(CLIENT_TARGET): src/client.c $(SO_TARGET) $(HEADERS)
	@mkdir -p bin
	$(CC) $(CFLAGS) src/client.c -o $(CLIENT_TARGET) $(LDFLAGS)

clean:
	rm -rf lib bin

.PHONY: all clean