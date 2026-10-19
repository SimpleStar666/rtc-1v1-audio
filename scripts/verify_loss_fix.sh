#!/usr/bin/env bash
# 验证修复后：检测丢包 == 实际丢弃
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
cp -f "$SRC/tests/e2e_test.cpp" /home/ts/rtc-1v1-audio/tests/
cd /home/ts/rtc-1v1-audio/build
cmake --build . -j32 2>&1 | grep -E 'error|Built target e2e' | head -5

echo ""
echo "=== 修复后：丢包统计对照 ==="
printf "%-10s %-10s %-10s %-8s %-12s\n" "丢包率" "实际丢弃" "检测丢包" "吻合" "SRTP 加/解"
for L in 0 0.10 0.25; do
  out=$(timeout 90 ./e2e_test --loss "$L" --seconds 4 2>/dev/null)
  lost=$(echo "$out" | grep "模拟丢弃(源)" | grep -oE '[0-9]+' | head -1)
  det=$(echo  "$out" | grep "^  检测丢包"     | grep -oE '[0-9]+' | head -1)
  fec=$(echo  "$out" | grep "FEC 恢复"        | grep -oE '[0-9]+' | head -1)
  srtp=$(echo "$out" | grep "SRTP 加密/解密" | grep -oE '[0-9]+ / [0-9]+' | head -1)
  mark="✗"
  [ "${lost:-x}" = "${det:-y}" ] && mark="✓"
  printf "%-10s %-10s %-10s %-8s %-12s\n" "$L" "$lost" "$det" "$mark" "$srtp"
done
echo ""
echo "=== 注意：FEC 恢复能让\"仍缺失\"小于\"检测丢包\"，这是正常的 ==="
echo "   检测丢包 = 检测到的丢包总数（恢复之前）"
echo "   恢复(NACK+FEC) = 其中被救回来的"
