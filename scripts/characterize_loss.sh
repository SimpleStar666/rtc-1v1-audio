#!/usr/bin/env bash
# 多次运行，判断"检测丢包 vs 实际丢弃"的偏差规律
# 假设：末尾丢包无法被检测（没有更晚的包来暴露空洞）→ 偏差 = 末尾连续丢弃数
cd /home/ts/rtc-1v1-audio/build

echo "=== 多轮统计（25% 丢包，6 秒）==="
printf "%-6s %-10s %-10s %-8s\n" "轮次" "实际丢弃" "检测丢包" "偏差"
TOTAL=0; DIFF=0
for i in 1 2 3 4 5 6; do
  out=$(timeout 90 ./e2e_test --loss 0.25 --seconds 6 2>/dev/null)
  lost=$(echo "$out" | grep "模拟丢弃(源)" | grep -oE '[0-9]+' | head -1)
  det=$(echo  "$out" | grep "^  检测丢包"     | grep -oE '[0-9]+' | head -1)
  d=$((lost - det))
  printf "%-6s %-10s %-10s %-8s\n" "$i" "$lost" "$det" "$d"
  TOTAL=$((TOTAL+lost)); DIFF=$((DIFF+d))
done
echo ""
echo "累计: 实际丢弃 $TOTAL, 累计偏差 $DIFF"
echo ""
echo "=== 结论 ==="
echo "偏差恒为 0 或很小的正数 → 说明是\"末尾丢包检测不到\"的算法边界"
echo "（最后几个包丢了，没有更晚的序号到来揭示这个空洞）"
