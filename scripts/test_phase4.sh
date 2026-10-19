#!/bin/bash
# Phase 4 验证：DTLS-SRTP
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
export PATH=$HOME/.local/bin:$PATH

rm -rf $WORK/src $WORK/tests
cp -r "$SRC/src" "$SRC/tests" $WORK/
cp "$SRC/CMakeLists.txt" $WORK/
cd $WORK
mkdir -p build && cd build
cmake .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > c.log 2>&1 || { echo "配置失败"; tail -15 c.log; exit 1; }
cmake --build . -j32 > b.log 2>&1 || { echo "❌ 编译失败"; grep -E "error:" b.log | head -20; exit 1; }
echo "✅ 编译成功"
echo ""
echo "=== DTLS-SRTP 测试 ==="
timeout 90 ./dtls_srtp_test 2>&1 | tail -45
