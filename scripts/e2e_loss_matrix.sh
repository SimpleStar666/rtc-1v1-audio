#!/bin/bash
# 端到端丢包场景对比
cd /home/ts/rtc-1v1-audio/build
for L in 0.05 0.15 0.25; do
  echo "======== 丢包率 ${L} ========"
  timeout 90 ./e2e_test --loss "$L" --seconds 4 2>/dev/null \
    | grep -E "发送包数|模拟丢弃|接收并解密|抖动缓冲接收|抖动缓冲播放|PLC|解码成功|检测丢包|FEC 恢复|SRTP 加密|结果|PASS|FAIL"
  echo ""
done
