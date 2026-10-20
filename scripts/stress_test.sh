#!/usr/bin/env bash
# 加压稳定性测试：10 轮 + 更长时长 + 变化参数
WORK=/home/ts/rtc-1v1-audio
cd "$WORK/build"

TOTAL_PASS=0
TOTAL_FAIL=0

run_one() {
  local round=$1
  local secs=$2
  local src_a=$3
  local src_b=$4
  local kbps=$5
  local size=$6

  pkill -f signal_server 2>/dev/null; pkill -f "peer --call" 2>/dev/null; sleep 0.5
  rm -rf /tmp/p_A /tmp/p_B; mkdir -p /tmp/p_A /tmp/p_B

  ./signal_server --port 8899 > /tmp/p_srv.log 2>&1 &
  local SRV=$!; sleep 0.9

  timeout 60 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name A \
    --seconds "$secs" --out /tmp/p_A.pcm --video-source "$src_a" --video-sink /tmp/p_A \
    --video-kbps "$kbps" --video-size "$size" > /tmp/p_A.log 2>&1 &
  local PA=$!
  sleep 1.2
  timeout 60 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name B \
    --seconds "$secs" --out /tmp/p_B.pcm --video-source "$src_b" --video-sink /tmp/p_B \
    --video-kbps "$kbps" --video-size "$size" > /tmp/p_B.log 2>&1 &
  local PB=$!

  wait $PA 2>/dev/null; local RA=$?
  wait $PB 2>/dev/null; local RB=$?
  kill $SRV 2>/dev/null
  sleep 0.3

  local A_PPM B_PPM A_DEC B_DEC A_DROP B_DROP
  A_PPM=$(ls /tmp/p_A/*.ppm 2>/dev/null | wc -l)
  B_PPM=$(ls /tmp/p_B/*.ppm 2>/dev/null | wc -l)
  A_DEC=$(grep "解码/丢弃帧" /tmp/p_A.log | grep -oE '[0-9]+ / [0-9]+' | head -1)
  B_DEC=$(grep "解码/丢弃帧" /tmp/p_B.log | grep -oE '[0-9]+ / [0-9]+' | head -1)
  A_DROP=$(echo "$A_DEC" | grep -oE '[0-9]+$')
  B_DROP=$(echo "$B_DEC" | grep -oE '[0-9]+$')

  local expect=$(( secs * 15 - 10 ))   # 15fps，允许少量启动损耗
  local pass=1
  [ "$RA" -ne 0 ] && pass=0
  [ "$RB" -ne 0 ] && pass=0
  [ "${A_DROP:-999}" -ne 0 ] && pass=0
  [ "${B_DROP:-999}" -ne 0 ] && pass=0
  [ "$A_PPM" -lt "$expect" ] && pass=0
  [ "$B_PPM" -lt "$expect" ] && pass=0

  if [ $pass -eq 1 ]; then
    printf "  轮%-2s %2ss %-12s/%-12s %-8s %4skbps : PASS  A=%s帧 B=%s帧\n" \
      "$round" "$secs" "$src_a" "$src_b" "$size" "$kbps" "$A_PPM" "$B_PPM"
    TOTAL_PASS=$((TOTAL_PASS+1))
  else
    printf "  轮%-2s %2ss %-12s/%-12s %-8s %4skbps : FAIL  rc=%s/%s A解码=%s B解码=%s PPM=%s/%s\n" \
      "$round" "$secs" "$src_a" "$src_b" "$size" "$kbps" "$RA" "$RB" "${A_DEC:-?}" "${B_DEC:-?}" "$A_PPM" "$B_PPM"
    TOTAL_FAIL=$((TOTAL_FAIL+1))
    echo "    --- A 错误 ---"
    grep -iE "decode failed|✗|error" /tmp/p_A.log | head -3 | sed 's/^/      /'
    echo "    --- B 错误 ---"
    grep -iE "decode failed|✗|error" /tmp/p_B.log | head -3 | sed 's/^/      /'
  fi
}

echo "################################################################"
echo "#  加压稳定性测试（10 轮，变化参数）"
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
[ "$TOTAL_FAIL" -eq 0 ] && echo "  ✓ 全部通过，无偶发失败" || echo "  ✗ 仍有失败"
