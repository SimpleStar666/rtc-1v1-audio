#!/bin/bash
# Phase 3 验证：ICE 穿透
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
export PATH=$HOME/.local/bin:$PATH

echo "=== 同步 ==="
rm -rf $WORK/src $WORK/tests
cp -r "$SRC/src" "$SRC/tests" $WORK/
cp "$SRC/CMakeLists.txt" $WORK/
cd $WORK
mkdir -p build && cd build
cmake .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > c.log 2>&1 || { echo "配置失败"; tail -15 c.log; exit 1; }
echo "=== 编译 ==="
cmake --build . -j32 > b.log 2>&1 || { echo "❌ 编译失败"; grep -E "error:" b.log | head -20; exit 1; }
echo "✅ 编译成功"

echo ""
echo "=== ICE 穿透测试（host 候选）==="
timeout 60 ./ice_test --seconds 4 2>&1 | tail -40
