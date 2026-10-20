#!/usr/bin/env bash
# 全量回归 + 真实通话验证（ts 用户身份）
set -u
cd /home/ts/rtc-1v1-audio
export PATH="$HOME/.local/bin:$PATH"

echo "=============================================="
echo "  全量回归"
echo "=============================================="
bash scripts/full_regression.sh 2>&1 | tail -14

echo ""
echo "=============================================="
echo "  真实通话（双进程）"
echo "=============================================="
pkill -f signal_server 2>/dev/null; pkill -f "peer --call" 2>/dev/null; sleep 0.5
cd build
./signal_server --port 8899 > /tmp/f_srv.log 2>&1 &
SRV=$!; sleep 1

timeout 45 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name A \
  --seconds 5 --out /tmp/f_A.pcm > /tmp/f_A.log 2>&1 &
PA=$!
sleep 1.5
timeout 45 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name B \
  --seconds 5 --out /tmp/f_B.pcm > /tmp/f_B.log 2>&1 &
PB=$!
wait $PA 2>/dev/null; RA=$?
wait $PB 2>/dev/null; RB=$?
kill $SRV 2>/dev/null

echo "  A 退出码=$RA  B 退出码=$RB"
for side in A B; do
  printf "  %s 端: " "$side"
  grep -E "发送包数|接收包数|SRTP 解密|解码错误" /tmp/f_${side}.log \
    | grep -oE '[0-9]+' | tr '\n' ' ' | sed 's/^/发送\/接收\/SRTP解密\/解码错误 = /'
  echo ""
done
grep -h "✅\|❌" /tmp/f_A.log /tmp/f_B.log | sed 's/^/  /'

echo ""
echo "=============================================="
echo "  文档完整性"
echo "=============================================="
cd /home/ts/rtc-1v1-audio
for f in docs/*.md README.md; do
  printf "  %-34s %5d 行\n" "$f" "$(wc -l < "$f")"
done
echo "  坑数: $(grep -c '^## 坑' docs/02-pitfalls.md)"
echo "  代码导读模块数: $(grep -c '^## 模块' docs/05-code-tour.md)"
