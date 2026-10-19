#!/usr/bin/env bash
# 逐条验证手册里的命令是否真实可用
set -u
cd /home/ts/rtc-1v1-audio

echo "########## 1. 验证 make_listenable.sh（手册提到）##########"
[ -f scripts/make_listenable.sh ] && echo "  ✓ 脚本存在" || echo "  ✗ 缺失"
bash scripts/make_listenable.sh 2>&1 | tail -8

echo ""
echo "########## 2. 验证 weaknet 表格的真实数据 ##########"
cd build
printf "  %-8s %-10s %-10s %-10s\n" "丢包率" "实际丢失" "检测丢包" "PLC"
for L in 0 2 5 8 15; do
  out=$(timeout 60 ./weaknet_test --loss $L --jitter 20 --delay 50 --seconds 6 2>/dev/null)
  lost=$(echo "$out" | awk -F: '/丢包帧数/{gsub(/[^0-9]/,"",$2); print $2}')
  det=$(echo  "$out" | awk -F: '/检测丢包/{gsub(/[^0-9]/,"",$2); print $2}')
  plc=$(echo  "$out" | awk -F: '/PLC 补偿次数/{gsub(/[^0-9]/,"",$2); print $2}')
  printf "  %-8s %-10s %-10s %-10s\n" "${L}%" "$lost" "$det" "$plc"
done

echo ""
echo "########## 3. 验证手册里的 grep 命令能取到数据 ##########"
cd /home/ts/rtc-1v1-audio/build
echo "--- ./selftest | grep SNR ---"
./selftest 2>&1 | grep -E "SNR|对齐" | head -3
echo "--- ./e2e_test | grep -A24 通话指标（前 12 行）---"
timeout 60 ./e2e_test --seconds 2 2>/dev/null | grep -A24 "通话指标" | head -12
echo "--- ./loss_protection_test | grep bitrate ---"
./loss_protection_test 2>&1 | grep -E "bitrate (UP|DOWN)" | head -4
echo "--- ./ice_test --stun ... | grep srflx ---"
timeout 60 ./ice_test --stun stun.l.google.com --stun-port 19302 --seconds 1 2>&1 | grep -E "srflx" | head -2

echo ""
echo "########## 4. 验证 pcm2wav.py 存在且可用 ##########"
ls -la /tmp/rtc_demo/pcm2wav.py 2>/dev/null && python3 /tmp/rtc_demo/pcm2wav.py /tmp/peer_out.pcm /tmp/vtest.wav 16000

echo ""
echo "########## 5. 验证 full_regression / acceptance / goal_check 存在 ##########"
for s in full_regression acceptance goal_check verify_opus_bitrate e2e_loss_matrix check_docs check_pitfalls; do
  [ -f scripts/$s.sh ] && echo "  ✓ scripts/$s.sh" || echo "  ✗ scripts/$s.sh 缺失"
done
cd /home/ts/rtc-1v1-audio
