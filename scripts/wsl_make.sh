#!/bin/bash
DST=/home/ts/rtc-1v1-audio
export PATH=$HOME/.local/bin:$PATH
cd $DST/build

echo "=== 编译（$(nproc) 核）==="
cmake --build . -j$(nproc) > build.log 2>&1
RC=$?
if [ $RC -ne 0 ]; then
  echo "❌ 编译失败 (rc=$RC)"
  echo "--- 错误摘要 ---"
  grep -E "error:|Error [0-9]" build.log | head -30
  echo "--- 末尾上下文 ---"
  tail -35 build.log
  exit 1
fi

echo "✅ 编译成功"
echo ""
echo "=== 产物 ==="
ls -la selftest signal_server peer weaknet_test 2>/dev/null

echo ""
echo "=== 运行自检 ==="
./selftest 2>&1 | tail -60
