#!/usr/bin/env bash
# 精确抓取 weaknet 数据（避免解析错误）
cd /home/ts/rtc-1v1-audio/build

echo "=== 原始输出（丢包率 8%）==="
timeout 60 ./weaknet_test --loss 8 --jitter 20 --delay 50 --seconds 8 2>/dev/null | grep -E "发送帧数|丢包帧数|检测丢包|播放帧数|PLC|连续性"

echo ""
echo "=== 逐档精确抓取（用 grep -o 只取数字）==="
printf "%-8s %-10s %-10s %-10s %-10s\n" "丢包率" "实际丢失" "检测丢包" "播放帧数" "PLC补偿"
for L in 0 2 5 8 15; do
  out=$(timeout 60 ./weaknet_test --loss $L --jitter 20 --delay 50 --seconds 8 2>/dev/null)
  lost=$(echo "$out" | grep "丢包帧数" | grep -oE '[0-9]+' | head -1)
  det=$(echo  "$out" | grep "检测丢包" | grep -oE '[0-9]+' | head -1)
  played=$(echo "$out" | grep "播放帧数" | grep -oE '[0-9]+' | head -1)
  plc=$(echo   "$out" | grep "PLC 补偿次数" | grep -oE '[0-9]+' | head -1)
  printf "%-8s %-10s %-10s %-10s %-10s\n" "${L}%" "$lost" "$det" "$played" "$plc"
done
