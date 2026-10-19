#!/bin/bash
# 抓取完整的参数列表（peer 的用法被截断了）
cd /home/ts/rtc-1v1-audio

echo "=== peer 完整用法 ==="
timeout 5 ./build/peer 2>&1 | sed -n '/用法/,/^$/p' | head -30

echo ""
echo "=== 各测试程序的参数解析（从源码提取）==="
for f in tests/ice_test.cpp tests/weaknet_test.cpp tests/e2e_test.cpp tests/loss_protection_test.cpp; do
  echo "--- $f ---"
  grep -oE '"--[a-z-]+"' "$f" | sort -u | tr '\n' ' '
  echo ""
done

echo ""
echo "=== signal_server 参数 ==="
grep -oE '"--[a-z-]+"' src/signal/signal_server.cpp | sort -u | tr '\n' ' '
echo ""

echo ""
echo "=== app/peer.cpp 参数 ==="
grep -oE '"--[a-z-]+"' src/app/peer.cpp | sort -u | tr '\n' ' '
echo ""
