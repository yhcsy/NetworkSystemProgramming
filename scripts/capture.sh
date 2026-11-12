#!/bin/bash
# 擷取 loopback 上 port 9734 的封包，存成 midterm.pcap（可用 Wireshark 開）
# 用法：先執行 ./bin/server，再執行  sudo ./scripts/capture.sh
cd "$(dirname "$0")/.."

tcpdump -i lo -nn -X "tcp port 9734" -w midterm.pcap &
TCPDUMP_PID=$!
sleep 1

./bin/client sysinfo
./bin/client echo "hello midterm"
./bin/client badmagic          # 異常封包，可以在擷取中看到 server 回 RESP_ERR

sleep 1
kill -INT $TCPDUMP_PID
wait $TCPDUMP_PID 2>/dev/null
echo "=== saved to midterm.pcap, summary: ==="
tcpdump -nn -r midterm.pcap