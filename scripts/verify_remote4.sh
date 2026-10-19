#!/bin/bash
# 远程验证（Phase 4）：克隆 -> 编译 -> 全部测试
set -e
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
export PATH=$HOME/.local/bin:$PATH
V=/home/ts/verify4
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
ls selftest signal_server peer weaknet_test ice_test dtls_srtp_test 2>/dev/null | tr '\n' ' '
echo ""

echo "=== 3. 自检（媒体层）==="
./selftest 2>/dev/null | tail -2

echo ""
echo "=== 4. DTLS-SRTP ==="
timeout 90 ./dtls_srtp_test 2>/dev/null | grep -E "结果|PASS.*握手|PASS.*指纹|PASS.*加密" | sed 's/^/  /'

echo ""
echo "=== 5. ICE 穿透 ==="
timeout 60 ./ice_test --seconds 2 2>&1 | grep -E "候选类型|结果" | sed 's/^/  /'

echo ""
echo "=== 6. STUN ==="
timeout 60 ./ice_test --stun stun.l.google.com --stun-port 19302 --seconds 2 2>&1 \
  | grep -E "候选类型|结果" | sed 's/^/  /'

echo ""
echo "=== 7. 弱网矩阵 ==="
./weaknet_test --loss 8 --jitter 30 --delay 60 --seconds 8 2>/dev/null \
  | grep -E "丢包帧|检测丢包|播放连续性" | sed 's/^/  /'

echo ""
echo "################ 远程验证完成 ################"
