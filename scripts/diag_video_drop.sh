#!/usr/bin/env bash
# 定位丢帧根因：A 端收到 B 的分片，但重组后丢帧
WORK=/home/ts/rtc-1v1-audio
cd "$WORK/build"

echo "################################################################"
echo "#  定位：为什么 A 端丢帧（46 解码 / 29 丢弃）"
echo "################################################################"

# 连续跑，直到复现丢帧，然后详细分析
for round in $(seq 1 8); do
  pkill -f signal_server 2>/dev/null; pkill -f "peer --call" 2>/dev/null; sleep 0.5
  rm -rf /tmp/dg_A /tmp/dg_B; mkdir -p /tmp/dg_A /tmp/dg_B

  ./signal_server --port 8899 > /tmp/dg_srv.log 2>&1 &
  SRV=$!; sleep 1

  timeout 40 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name A \
    --seconds 5 --out /tmp/dg_A.pcm --video-source test:bars --video-sink /tmp/dg_A \
    --verbose > /tmp/dg_A.log 2>&1 &
  PA=$!
  sleep 1.5
  timeout 40 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name B \
    --seconds 5 --out /tmp/dg_B.pcm --video-source test:motion --video-sink /tmp/dg_B \
    --verbose > /tmp/dg_B.log 2>&1 &
  PB=$!
  wait $PA 2>/dev/null; wait $PB 2>/dev/null; kill $SRV 2>/dev/null

  DROP=$(grep "解码/丢弃帧" /tmp/dg_A.log | grep -oE '/ [0-9]+' | tr -d '/ ')
  PPM=$(ls /tmp/dg_A/*.ppm 2>/dev/null | wc -l)
  echo "第 $round 轮: A 端丢弃=$DROP 落盘=$PPM"

  if [ "${DROP:-0}" -gt 0 ]; then
    echo ""
    echo ">>> 复现了！分析 A 端日志 <<<"
    echo ""
    echo "=== A 端收到 B 的分片统计 ==="
    grep -E "接收分片|完整帧|解码/丢弃" /tmp/dg_A.log
    echo ""
    echo "=== A 端的关键帧请求 ==="
    grep -iE "keyframe|PLI|关键帧" /tmp/dg_A.log | head -20
    echo ""
    echo "=== A 端是否有 vpipe 警告 ==="
    grep -iE "\[vpipe\]|bad fragment|incomplete|timeout|dropped" /tmp/dg_A.log | head -30
    echo ""
    echo "=== A 端完整日志（非 INFO 部分）==="
    grep -vE "\[INFO \]" /tmp/dg_A.log | head -40
    echo ""
    echo "=== B 端发送统计 ==="
    grep -E "采集/编码|发送分片" /tmp/dg_B.log
    echo ""
    echo "=== B 端是否有 vpipe 日志 ==="
    grep -iE "\[vpipe\]" /tmp/dg_B.log | head -15
    break
  fi
done
