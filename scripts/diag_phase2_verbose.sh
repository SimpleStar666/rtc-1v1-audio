#!/bin/bash
# 完整日志的 Phase 2 诊断（含服务器转发细节）
WORK=/home/ts/rtc-1v1-audio
cd $WORK/build
pkill -f signal_server 2>/dev/null; pkill -f "peer --signal" 2>/dev/null; sleep 0.5

./signal_server --port 8899 --verbose > /tmp/s.log 2>&1 &
SRV=$!
sleep 1

timeout 12 ./peer --signal ws://127.0.0.1:8899 --room demo --name A --peer-test --seconds 2 > /tmp/a.log 2>&1 &
A=$!
sleep 0.6
timeout 12 ./peer --signal ws://127.0.0.1:8899 --room demo --name B --peer-test --seconds 2 > /tmp/b.log 2>&1 &
B=$!
wait $A; wait $B
sleep 0.5

echo "########## 服务器：转发明细 ##########"
grep -E "joined|paired|forwarded|recv|leaving" /tmp/s.log

echo ""
echo "########## peer B：发送与接收 ##########"
grep -E "recv|sent|queued|我发|已回|等对方" /tmp/b.log

echo ""
echo "########## peer A：发送与接收 ##########"
grep -E "recv|sent|queued|我发|已回|等对方" /tmp/a.log

kill $SRV 2>/dev/null
