#!/bin/bash
# 远程验证：全新克隆 -> 编译 -> 自检 -> 弱网 -> 信令配对
set -e
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
export PATH=$HOME/.local/bin:$PATH
V=/home/ts/verify2
rm -rf $V

echo "=== 1. 全新克隆 ==="
git -c http.sslVerify=false clone -q \
  "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" $V
cd $V
echo "  提交: $(git log --oneline -1)"
echo "  文件数: $(git ls-files | wc -l)"
echo "  含 build/: $(git ls-files | grep -c '^build/' || true)"

echo ""
echo "=== 2. 编译 ==="
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > c.log 2>&1 || { echo "配置失败"; tail -15 c.log; exit 1; }
cmake --build . -j32 > b.log 2>&1 || { echo "编译失败"; grep -E 'error:' b.log | head -10; exit 1; }
echo "  ✅ 编译成功"
ls selftest signal_server peer weaknet_test | tr '\n' ' '

echo ""
echo "=== 3. 自检 ==="
./selftest 2>/dev/null | tail -3

echo ""
echo "=== 4. 弱网测试（8% 丢包）==="
./weaknet_test --loss 8 --jitter 30 --delay 60 --seconds 10 2>/dev/null | grep -E "丢包帧|检测丢包|播放帧|连续性"

echo ""
echo "=== 5. 信令配对（两端 + 服务器）==="
pkill -f signal_server 2>/dev/null || true
sleep 0.3
./signal_server --port 8899 > /tmp/vs.log 2>&1 &
SRV=$!
sleep 1
timeout 12 ./peer --signal ws://127.0.0.1:8899 --room demo --name A --peer-test --seconds 2 > /tmp/va.log 2>&1 &
A=$!
sleep 0.6
timeout 12 ./peer --signal ws://127.0.0.1:8899 --room demo --name B --peer-test --seconds 2 > /tmp/vb.log 2>&1 &
B=$!
wait $A; RA=$?
wait $B; RB=$?
sleep 0.3

echo "  peer A 退出码=$RA"
grep -E "入房成功|对端就绪|收到 offer|收到 answer|收到候选|信令面结果" /tmp/va.log | sed 's/^/    /'
echo "  peer B 退出码=$RB"
grep -E "入房成功|对端就绪|收到 offer|收到 answer|收到候选|信令面结果" /tmp/vb.log | sed 's/^/    /'
echo "  服务器转发："
grep -E "forwarded|paired" /tmp/vs.log | sed 's/^/    /'
kill $SRV 2>/dev/null || true

echo ""
echo "################ 远程验证完成 ################"
