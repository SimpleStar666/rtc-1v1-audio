#!/usr/bin/env bash
# 加压稳定性测试 v2：统一用 ts 身份，唯一文件名，避免权限冲突
WORK=/home/ts/rtc-1v1-audio
cd "$WORK/build"

TAG="pk$$"
TOTAL_PASS=0
TOTAL_FAIL=0

run_one() {
  local round=$1 secs=$2 src_a=$3 src_b=$4 kbps=$5 size=$6
  local LOGDIR="/tmp/stress_${TAG}_${round}"
  rm -rf "$LOGDIR"; mkdir -p "$LOGDIR/A" "$LOGDIR/B"

  pkill -f signal_server 2>/dev/null; pkill -f "peer --call" 2>/dev/null; sleep 0.5

  ./signal_server --port 8901 > "$LOGDIR/srv.log" 2>&1 &
  local SRV=$!; sleep 0.9

  timeout 60 ./peer --call --signal ws://127.0.0.1:8901 --room demo --name A \
    --seconds "$secs" --out "$LOGDIR/A.pcm" --video-source "$src_a" \
    --video-sink "$LOGDIR/A" --video-kbps "$kbps" --video-size "$size" \
    > "$LOGDIR/A.log" 2>&1 &
  local PA=$!
  sleep 1.2
  timeout 60 ./peer --call --signal ws://127.0.0.1:8901 --room demo --name B \
    --seconds "$secs" --out "$LOGDIR/B.pcm" --video-source "$src_b" \
    --video-sink "$LOGDIR/B" --video-kbps "$kbps" --video-size "$size" \
    > "$LOGDIR/B.log" 2>&1 &
  local PB=$!

  wait $PA 2>/dev/null; local RA=$?
  wait $PB 2>/dev/null; local RB=$?
  kill $SRV 2>/dev/null
  sleep 0.3

  local A_PPM B_PPM A_DROP B_DROP
  A_PPM=$(ls "$LOGDIR/A"/*.ppm 2>/dev/null | wc -l)
  B_PPM=$(ls "$LOGDIR/B"/*.ppm 2>/dev/null | wc -l)
  A_DROP=$(grep "解码/丢弃帧" "$LOGDIR/A.log" 2>/dev/null | grep -oE '[0-9]+$' | head -1)
  B_DROP=$(grep "解码/丢弃帧" "$LOGDIR/B.log" 2>/dev/null | grep -oE '[0-9]+$' | head -1)

  local expect=$(( secs * 15 - 12 ))
  local pass=1
  [ "$RA" -ne 0 ] && pass=0
  [ "$RB" -ne 0 ] && pass=0
  [ "${A_DROP:-999}" -ne 0 ] && pass=0
  [ "${B_DROP:-999}" -ne 0 ] && pass=0
  [ "$A_PPM" -lt "$expect" ] && pass=0
  [ "$B_PPM" -lt "$expect" ] && pass=0

  if [ $pass -eq 1 ]; then
    printf "  轮%-2s %2ss %-12s/%-12s %-9s %4skbps : PASS  A=%s帧 B=%s帧\n" \
      "$round" "$secs" "$src_a" "$src_b" "$size" "$kbps" "$A_PPM" "$B_PPM"
    TOTAL_PASS=$((TOTAL_PASS+1))
  else
    printf "  轮%-2s %2ss %-12s/%-12s %-9s %4skbps : FAIL  rc=%s/%s 丢弃=%s/%s PPM=%s/%s (期望>=%s)\n" \
      "$round" "$secs" "$src_a" "$src_b" "$size" "$kbps" "$RA" "$RB" \
      "${A_DROP:-?}" "${B_DROP:-?}" "$A_PPM" "$B_PPM" "$expect"
    TOTAL_FAIL=$((TOTAL_FAIL+1))
    grep -iE "decode failed|✗|超时" "$LOGDIR/A.log" 2>/dev/null | head -2 | sed 's/^/      A: /'
    grep -iE "decode failed|✗|超时" "$LOGDIR/B.log" 2>/dev/null | head -2 | sed 's/^/      B: /'
  fi
}

echo "################################################################"
echo "#  加压稳定性测试（10 轮，变化参数）  TAG=$TAG"
echo "################################################################"
echo ""

run_one 1  5  test:bars   test:motion 400 320x240
run_one 2  5  test:motion test:bars   400 320x240
run_one 3  6  test:motion test:motion 400 320x240
run_one 4  5  test:bars   test:bars   200 320x240
run_one 5  5  test:motion test:motion 800 320x240
run_one 6  5  test:bars   test:motion 400 640x480
run_one 7  5  test:motion test:bars   400 640x480
run_one 8  6  test:noise  test:noise  600 320x240
run_one 9  5  test:bars   test:motion 400 320x240
run_one 10 8  test:motion test:motion 400 320x240

echo ""
echo "################################################################"
echo "#  汇总：PASS=$TOTAL_PASS  FAIL=$TOTAL_FAIL"
echo "################################################################"
[ "$TOTAL_FAIL" -eq 0 ] && echo "  [OK] 全部通过，无偶发失败" || echo "  [FAIL] 仍有失败"
rm -rf /tmp/stress_${TAG}_* 2>/dev/null
