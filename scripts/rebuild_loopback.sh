#!/bin/bash
cp -r '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src' /home/ts/rtc-1v1-audio/
cd /home/ts/rtc-1v1-audio/build

echo "=== 编译 ==="
cmake --build . -j32 2>&1 | grep -E "error|Built target" | head -20

echo ""
echo "=== loopback 重测（30 秒超时）==="
timeout 30 ./peer --loopback --seconds 5 > /tmp/lo2.log 2>&1
RC=$?
echo "退出码: $RC $([ $RC -eq 124 ] && echo '(超时=仍然卡死)' || echo '(正常退出)')"
echo ""
tail -25 /tmp/lo2.log
