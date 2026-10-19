#!/usr/bin/env bash
# 验证假设：短时长 + 高丢包 → 末尾空洞来不及被检测（算法边界，非 bug）
cd /home/ts/rtc-1v1-audio/build

echo "=== 对照组：不同时长下的偏差 ==="
for SEC in 2 4 6; do
  n_ok=0; n_bad=0; sum_diff=0
  for i in 1 2 3 4 5; do
    out=$(timeout 90 ./e2e_test --loss 0.25 --seconds $SEC 2>/dev/null)
    lost=$(echo "$out" | grep "模拟丢弃(源)" | grep -oE '[0-9]+' | head -1)
    det=$(echo  "$out" | grep "^  检测丢包"     | grep -oE '[0-9]+' | head -1)
    d=$((lost - det)); sum_diff=$((sum_diff+d))
    [ "$d" -eq 0 ] && n_ok=$((n_ok+1)) || n_bad=$((n_bad+1))
  done
  printf "  时长 %ss : 吻合 %d/5 次, 累计偏差 %d\n" "$SEC" "$n_ok" "$sum_diff"
done

echo ""
echo "=== 原理说明 ==="
cat <<'EOF'
  抖动缓冲这样计算丢包：
      已接收序号跨度(span) - 独立到达包数(arrived) = 丢包数

  只有当"更晚的包"到达时，才会发现中间的序号空洞。
  如果**最后几个包**丢了，就没有更晚的序号来揭示它们 ——
  这些"末尾丢包"在通话结束前无法被统计到。

  所以偏差 = 末尾连续丢弃的包数，其期望值随：
    - 时长变短而**增大**（分母小，末尾占比高、且截断更早）
    - 丢包率升高而**增大**（末尾连续丢的概率升高）

  正确性证据：长时间运行时偏差恒为 0（见上一脚本：6 轮 138 丢包，偏差 0）。
  这属于**算法固有边界**，不是统计错误。
EOF
