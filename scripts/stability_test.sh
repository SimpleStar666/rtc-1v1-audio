#!/usr/bin/env bash
# 稳定性测试：反复跑 5 遍音视频通话，检查是否有偶发失败
WORK=/home/ts/rtc-1v1-audio
cd "$WORK/build"

echo "################################################################"
echo "#  稳定性测试：连续 5 轮音视频通话"
echo "################################################################"

PASS=0
FAIL=0
declare -a RESULTS

for round in 1 2 3 4 5; do
  echo ""
  echo "================ 第 $round 轮 ================"

  pkill -f signal_server 2>/dev/null
  pkill -f "peer --call" 2>/dev/null
  sleep 0.6
  rm -rf /tmp/st_A /tmp/st_B; mkdir -p /tmp/st_A /tmp/st_B

  ./signal_server --port 8899 > /tmp/st_srv.log 2>&1 &
  SRV=$!
  sleep 1

  timeout 45 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name A \
    --seconds 5 --out /tmp/st_A.pcm \
    --video-source test:bars --video-sink /tmp/st_A > /tmp/st_A.log 2>&1 &
  PA=$!
  sleep 1.5

  timeout 45 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name B \
    --seconds 5 --out /tmp/st_B.pcm \
    --video-source test:motion --video-sink /tmp/st_B > /tmp/st_B.log 2>&1 &
  PB=$!

  wait $PA 2>/dev/null; RA=$?
  wait $PB 2>/dev/null; RB=$?
  kill $SRV 2>/dev/null
  sleep 0.3

  # 提取关键指标
  A_OK=$(grep -c "✅ 通话成功" /tmp/st_A.log)
  B_OK=$(grep -c "✅ 通话成功" /tmp/st_B.log)
  A_VDEC=$(grep "解码/丢弃帧" /tmp/st_A.log | grep -oE '[0-9]+ / [0-9]+' | head -1)
  B_VDEC=$(grep "解码/丢弃帧" /tmp/st_B.log | grep -oE '[0-9]+ / [0-9]+' | head -1)
  A_SRTP=$(grep "SRTP 解密" /tmp/st_A.log | grep -oE '[0-9]+ 包（失败 [0-9]+）' | head -1)
  B_SRTP=$(grep "SRTP 解密" /tmp/st_B.log | grep -oE '[0-9]+ 包（失败 [0-9]+）' | head -1)
  A_PPM=$(ls /tmp/st_A/*.ppm 2>/dev/null | wc -l)
  B_PPM=$(ls /tmp/st_B/*.ppm 2>/dev/null | wc -l)

  echo "  A: rc=$RA 成功=$A_OK 视频解码=$A_VDEC SRTP=$A_SRTP 落盘=$A_PPM"
  echo "  B: rc=$RB 成功=$B_OK 视频解码=$B_VDEC SRTP=$B_SRTP 落盘=$B_PPM"

  # 判定
  if [ "$RA" -eq 0 ] && [ "$RB" -eq 0 ] && [ "$A_OK" -ge 1 ] && [ "$B_OK" -ge 1 ] \
     && [ "$A_PPM" -gt 50 ] && [ "$B_PPM" -gt 50 ]; then
    echo "  => 第 $round 轮 PASS"
    PASS=$((PASS+1))
    RESULTS+=("轮$round: PASS (A解码${A_VDEC} B解码${B_VDEC})")
  else
    echo "  => 第 $round 轮 FAIL"
    FAIL=$((FAIL+1))
    RESULTS+=("轮$round: FAIL (rc=$RA/$RB 成功=$A_OK/$B_OK ppm=$A_PPM/$B_PPM)")
    echo "  --- A 日志尾部 ---"
    tail -15 /tmp/st_A.log | sed 's/^/    /'
    echo "  --- B 日志尾部 ---"
    tail -15 /tmp/st_B.log | sed 's/^/    /'
  fi
done

echo ""
echo "################################################################"
echo "#  汇总：PASS=$PASS  FAIL=$FAIL"
echo "################################################################"
for r in "${RESULTS[@]}"; do echo "  $r"; done

# 如果有失败，提取错误关键字
if [ "$FAIL" -gt 0 ]; then
  echo ""
  echo "=== 失败日志中的错误关键字 ==="
  grep -hiE "error|fail|✗|timeout|超时|失败" /tmp/st_A.log /tmp/st_B.log 2>/dev/null | head -20
fi
