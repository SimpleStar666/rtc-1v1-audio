#!/bin/bash
# 远程验证：克隆 -> 编译 -> 全部测试（含 ICE）
set -e
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
export PATH=$HOME/.local/bin:$PATH
V=/home/ts/verify3
rm -rf $V

echo "=== 1. 全新克隆 ==="
git -c http.sslVerify=false clone -q \
  "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" $V
cd $V
echo "  提交: $(git log --oneline -1)"
echo "  文件数: $(git ls-files | wc -l)"

echo ""
echo "=== 2. 编译 ==="
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > c.log 2>&1 \
  || { echo "配置失败"; tail -15 c.log; exit 1; }
cmake --build . -j32 > b.log 2>&1 || { echo "编译失败"; grep -E 'error:' b.log | head -10; exit 1; }
echo "  ✅ 编译成功"
ls selftest signal_server peer weaknet_test ice_test 2>/dev/null | tr '\n' ' '

echo ""
echo "=== 3. 自检（媒体层）==="
./selftest 2>/dev/null | tail -3

echo ""
echo "=== 4. ICE 穿透 ==="
timeout 60 ./ice_test --seconds 3 2>&1 | grep -E "候选类型|选中候选对|收到来自|结果" | sed 's/^/  /'

echo ""
echo "=== 5. STUN（公网）==="
timeout 60 ./ice_test --stun stun.l.google.com --stun-port 19302 --seconds 2 2>&1 \
  | grep -E "候选类型|结果" | sed 's/^/  /'

echo ""
echo "=== 6. 信令配对 ==="
pkill -f signal_server 2>/dev/null || true
sleep 0.3
./signal_server --port 8899 > /tmp/v3s.log 2>&1 &
SRV=$!
sleep 1
timeout 12 ./peer --signal ws://127.0.0.1:8899 --room demo --name A --peer-test --seconds 1 > /tmp/v3a.log 2>&1 &
A=$!
sleep 0.6
timeout 12 ./peer --signal ws://127.0.0.1:8899 --room demo --name B --peer-test --seconds 1 > /tmp/v3b.log 2>&1 &
B=$!
wait $A; wait $B
grep -h "信令面结果" /tmp/v3a.log /tmp/v3b.log | sed 's/^/  /'
kill $SRV 2>/dev/null || true

echo ""
echo "################ 远程验证完成 ################"
