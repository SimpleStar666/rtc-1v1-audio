#!/usr/bin/env bash
# 深度定位：抓失败时的完整 vpipe 日志
WORK=/home/ts/rtc-1v1-audio
cd "$WORK/build"

for round in $(seq 1 6); do
  pkill -f signal_server 2>/dev/null; pkill -f "peer --call" 2>/dev/null; sleep 0.5
  rm -rf /tmp/x_A /tmp/x_B; mkdir -p /tmp/x_A /tmp/x_B

  ./signal_server --port 8899 > /tmp/x_srv.log 2>&1 &
  SRV=$!; sleep 1

  timeout 40 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name A \
    --seconds 5 --out /tmp/x_A.pcm --video-source test:bars --video-sink /tmp/x_A \
    --verbose > /tmp/x_A.log 2>&1 &
  PA=$!
  sleep 1.5
  timeout 40 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name B \
    --seconds 5 --out /tmp/x_B.pcm --video-source test:motion --video-sink /tmp/x_B \
    --verbose > /tmp/x_B.log 2>&1 &
  PB=$!
  wait $PA 2>/dev/null; wait $PB 2>/dev/null; kill $SRV 2>/dev/null

  DROP=$(grep "解码/丢弃帧" /tmp/x_A.log | grep -oE '/ [0-9]+' | tr -d '/ ')
  echo "轮$round: A 丢弃=$DROP"

  if [ "${DROP:-0}" -gt 0 ]; then
    echo ""
    echo "=========== 复现！分析 ==========="
    echo ""
    echo "--- A 端 vp8 解码错误统计 ---"
    grep -c "decode failed" /tmp/x_A.log | sed 's/^/  decode failed 次数: /'
    grep "decode failed" /tmp/x_A.log | sed 's/.*failed: //' | sort | uniq -c | sed 's/^/  /'
    echo ""
    echo "--- A 端收到的分片/帧统计 ---"
    grep -E "接收分片|完整帧|解码/丢弃" /tmp/x_A.log | sed 's/^/  /'
    echo ""
    echo "--- B 端发送的帧类型分布 ---"
    grep -E "采集/编码" /tmp/x_B.log | sed 's/^/  /'
    echo ""
    echo "--- A 端时间线（vp8 + vpipe，前后各 30 条）---"
    grep -E "\[vp8\]|\[vpipe\]|\[jitter\]" /tmp/x_A.log | head -40 | sed 's/^/  /'
    echo ""
    echo "--- A 端是否收到关键帧 ---"
    grep -iE "keyframe|关键帧" /tmp/x_A.log | head -10 | sed 's/^/  /'
    echo ""
    echo "--- 检查：A 端 pending 是否堆积 ---"
    grep -iE "incomplete|reassemble|pending" /tmp/x_A.log | head -10 | sed 's/^/  /'
    echo ""
    echo "--- A 端收包与解密的时序（前 20 条 vpipe 相关）---"
    grep -E "recv|unprotect|fragment" /tmp/x_A.log | head -20 | sed 's/^/  /'
    break
  fi
done
