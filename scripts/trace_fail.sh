#!/bin/bash
# 逐步追踪 g_fail 计数
cd /home/ts/rtc-1v1-audio/build
echo "=== 全部输出（含 stderr）==="
timeout 60 ./dtls_srtp_test 2>&1 | grep -vE "^\[" | head -60
echo ""
echo "=== 检查是否存在隐藏的 CHECK（未打印）==="
grep -n "CHECK" /home/ts/rtc-1v1-audio/tests/dtls_srtp_test.cpp | wc -l
echo "=== 源码里 CHECK 的位置 ==="
grep -n "CHECK" /home/ts/rtc-1v1-audio/tests/dtls_srtp_test.cpp
