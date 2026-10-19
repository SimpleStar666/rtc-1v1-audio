#!/bin/bash
# 用 gdb 定位段错误
WORK=/home/ts/rtc-1v1-audio
cd $WORK/build

pkill -f signal_server 2>/dev/null || true
sleep 0.5
./signal_server --port 8899 > /tmp/srv3.log 2>&1 &
SRV=$!
sleep 1

which gdb >/dev/null 2>&1 || { echo "安装 gdb..."; sudo apt-get install -y -qq gdb > /dev/null 2>&1; }

echo "=== 在 gdb 中运行 peer ==="
timeout 40 gdb -batch \
  -ex "run --signal ws://127.0.0.1:8899 --room demo --name A --peer-test --seconds 2" \
  -ex "bt" \
  -ex "info threads" \
  ./peer 2>&1 | grep -vE "^\[Thread|^\[New|Reading symbols|no debugging" | head -50

echo ""
echo "=== 服务器日志 ==="
cat /tmp/srv3.log
kill $SRV 2>/dev/null || true
