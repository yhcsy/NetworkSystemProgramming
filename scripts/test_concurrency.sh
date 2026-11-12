#!/bin/bash
# 證明 server 能同時服務 N 個 client，並在執行期間取得行程樹、連線狀態與無殭屍行程的佐證

# 讀取命令列第一個參數作為連線數；若未提供，預設使用 10 個 client (${1:-10})
N=${1:-10}

# 切換工作目錄至專案根目錄（無論在終端機哪裡執行此腳本，都能正確定位 ./bin/）
cd "$(dirname "$0")/.."

# 印出啟動當下的時間戳記 (時:分:秒)
echo "=== launching $N clients at $(date +%T) ==="

# 記錄壓測開始的高精度時間戳記 (含秒數與奈秒)
start=$(date +%s.%N)

# 迴圈啟動 N 個 client 實體
for i in $(seq 1 "$N"); do
    # 背景執行 (&)：將每個 client 的 stdout 與 stderr 全部導入 /tmp/ 專屬暫存檔
    # -n 指定 client 編號，查詢 sysinfo 系統資訊
    ./bin/client -n "$i" sysinfo > "/tmp/client_$i.out" 2>&1 &
done

# 關鍵暫停 1 秒：等待所有 client 完成 TCP 三向交握，並讓 server fork() 出子行程
# 此時伺服器端因設定 -d 5 還在 sleep 模擬耗時處理，正好可以抓取瞬間系統狀態
sleep 1

echo "=== process tree (1 parent + $N children) ==="
# 使用 --forest 顯示行程階層樹，[b] 技巧可避免 grep 自身出現在輸出結果中
# 預期顯示 1 個 server 主行程底下掛著 N 個分岔的子行程，證明使用 fork() 而非 Thread
ps -ef --forest | grep "[b]in/server"

echo "=== ESTABLISHED connections on port 9734 ==="
# 檢查核心 Socket 狀態，證明所有 Client 同時處於連線狀態
if command -v ss >/dev/null; then
    # 若系統有安裝現代網路診斷工具 ss，列出本機 9734 port 處於 ESTABLISHED 的連線
    ss -tn state established '( sport = :9734 )'
else
    # 備援方案：若無 ss 則使用 netstat 過濾已建立的 TCP 連線
    netstat -tn | grep ":9734 .*ESTABLISHED" | grep -v "9734 *[0-9.]*:9734"
fi

# 等待背景執行的所有 N 個 client 處理完成並關閉連線
wait

# 記錄全部連線完成的高精度時間戳記
end=$(date +%s.%N)

echo "=== results ==="
# 從 /tmp 日誌中擷取每個 client 的回應，並按照 client 編號 (-k2 -n) 排序印出
grep -h "^\[client" /tmp/client_*.out | sort -t' ' -k2 -n

# 使用系統內建的 awk 計算總耗時（保留兩位小數）
elapsed=$(awk "BEGIN {printf \"%.2f\", $end - $start}")

# 印出總耗時：在 -d 5 延遲下，若總耗時接近 5~6 秒，即可數學證明 N 個連線為平行處理
printf "=== all %d clients finished in %s s ===\n" "$N" "$elapsed"

# 檢查殭屍行程：grep 搜尋狀態為 <defunct> 的行程數量
# 預期輸出為 0，證明 SIGCHLD 與 waitpid(-1, NULL, WNOHANG) 正確回收了所有結束的子行程
echo "=== zombie processes: $(ps -ef | grep -c '[d]efunct') ==="