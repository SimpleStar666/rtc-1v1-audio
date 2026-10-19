#!/bin/bash
# Phase 2 验证：两个 peer 通过信令服务器配对
set -e

SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
export PATH=$HOME/.local/bin:$PATH

echo "=== 同步源码 ==="
rm -rf $WORK/src $WORK/tests
cp -r "$SRC/src" "$SRC/tests" $WORK/
cp "$SRC/CMakeLists.txt" $WORK/
cd $WORK
mkdir -p build
cd build
cmake .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > cfg.log 2>&1 || { echo "❌ configure 失败"; tail -20 cfg.log; exit 1; }
cmake --build . -j32 > b.log 2>&1 || { echo "❌ 编译失败"; grep -E "error:" b.log | head -15; exit 1; }
echo "✅ 编译成功"

echo ""
echo "=== 启动信令服务器 ==="
./signal_server --port 8899 > /tmp/srv.log 2>&1 &
SRV=$!
sleep 1
kill -0 $SRV 2>/dev/null && echo "  服务器 pid=$SRV 运行中" || { echo "  ❌ 服务器启动失败"; cat /tmp/srv.log; exit 1; }

echo ""
echo "=== 启动两个 peer（通过信令配对）==="
# peer A 和 B 用不同音频源，双端各自 loopback 传输
./peer --signal ws://127.0.0.1:8899 --room demo --name A --peer-test --seconds 6 > /tmp/peerA.log 2>&1 &
A=$!
sleep 0.8
./peer --signal ws://127.0.0.1:8899 --room demo --name B --peer-test --seconds 6 > /tmp/peerB.log 2>&1 &
B=$!

wait $A; RA=$?
wait $B; RB=$?
echo "  peer A 退出码=$RA, peer B 退出码=$RB"

echo ""
echo "=== peer A 输出 ==="
tail -20 /tmp/peerA.log
echo ""
echo "=== peer B 输出 ==="
tail -20 /tmp/peerB.log

echo ""
echo "=== 服务器日志 ==="
tail -15 /tmp/srv.log

kill $SRV 2>/dev/null || true
