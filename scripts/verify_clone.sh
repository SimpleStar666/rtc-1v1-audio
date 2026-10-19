#!/bin/bash
# 最终验证：从 GitHub 全新克隆 -> 编译 -> 运行全部测试
# 这是"远程代码真实可用"的决定性证据
set -e
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
export PATH=$HOME/.local/bin:$PATH

VERIFY=/home/ts/verify_clone
rm -rf $VERIFY
echo "=== 从 GitHub 全新克隆 ==="
git -c http.sslVerify=false clone -q \
  "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" $VERIFY 2>&1 | tail -2
cd $VERIFY
echo "  克隆成功，提交: $(git log --oneline -1)"
echo "  文件数: $(git ls-files | wc -l)"
echo "  是否含 build/ : $(git ls-files | grep -c '^build/' || echo 0)"

echo ""
echo "=== 编译 ==="
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > cfg.log 2>&1 \
  || { echo "❌ 配置失败"; tail -20 cfg.log; exit 1; }
cmake --build . -j32 > build.log 2>&1 \
  || { echo "❌ 编译失败"; grep -E "error" build.log | head -10; exit 1; }
echo "  ✅ 编译成功"
ls -la selftest signal_server peer weaknet_test | awk '{print "   "$9" "$5" bytes"}'

echo ""
echo "=== 运行自检 ==="
./selftest 2>/dev/null | tail -6

echo ""
echo "=== 运行弱网测试（中度丢包）==="
./weaknet_test --loss 8 --jitter 30 --delay 60 --seconds 10 2>/dev/null | grep -E "丢包帧|检测丢包|播放帧|PLC|连续性"

echo ""
echo "=== 运行端到端自环 ==="
timeout 30 ./peer --loopback --seconds 3 2>/dev/null | tail -12
