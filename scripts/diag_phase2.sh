#!/bin/bash
# 诊断 Phase 2：单独跑两个 peer，收集完整日志
WORK=/home/ts/rtc-1v1-audio
cd $WORK/build

pkill -f signal_server 2>/dev/null || true
pkill -f "peer --signal" 2>/dev/null || true
sleep 0.5

echo "=== 启动信令服务器 ==="
./signal_server --port 8899 > /tmp/srv2.log 2>&1 &
SRV=$!
sleep 1
if ! kill -0 $SRV 2>/dev/null; then echo "服务器挂了:"; cat /tmp/srv2.log; exit 1; fi
echo "  pid=$SRV"

echo ""
echo "=== peer A（后台，15 秒超时）==="
timeout 15 ./peer --signal ws://127.0.0.1:8899 --room demo --name A --peer-test --seconds 3 > /tmp/pa.log 2>&1 &
A=$!
sleep 1

echo "=== peer B（后台，15 秒超时）==="
timeout 15 ./peer --signal ws://127.0.0.1:8899 --room demo --name B --peer-test --seconds 3 > /tmp/pb.log 2>&1 &
B=$!

wait $A; echo "A 退出码=$?"
wait $B; echo "B 退出码=$?"

echo ""
echo "########## peer A 日志 ##########"
cat /tmp/pa.log
echo ""
echo "########## peer B 日志 ##########"
cat /tmp/pb.log
echo ""
echo "########## 服务器日志 ##########"
cat /tmp/srv2.log

kill $SRV 2>/dev/null || true
