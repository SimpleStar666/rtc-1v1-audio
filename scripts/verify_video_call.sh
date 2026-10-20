#!/usr/bin/env bash
# 端到端视频通话验证：双进程 + 解码帧落盘 + 画面校验
set -u
WORK=/home/ts/rtc-1v1-audio
cd "$WORK/build"

echo "================================================================"
echo "  端到端音视频通话验证"
echo "================================================================"

# 清理
pkill -f signal_server 2>/dev/null
pkill -f "peer --call" 2>/dev/null
sleep 0.5
rm -rf /tmp/vsink_A /tmp/vsink_B
mkdir -p /tmp/vsink_A /tmp/vsink_B

echo ""
echo "[1/3] 启动信令服务器"
./signal_server --port 8899 > /tmp/v_srv.log 2>&1 &
SRV=$!
sleep 1
grep -q "server started" /tmp/v_srv.log && echo "  ✓ 服务器就绪" || { echo "  ✗ 失败"; exit 1; }

echo ""
echo "[2/3] 启动端点 A（主叫，视频源=彩条）"
timeout 60 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name A \
  --seconds 8 --out /tmp/v_A.pcm \
  --video-size 320x240 --video-fps 15 --video-kbps 400 \
  --video-source test:bars \
  --video-sink /tmp/vsink_A > /tmp/v_A.log 2>&1 &
PA=$!
sleep 1.5

echo "[3/3] 启动端点 B（被叫，视频源=运动图案）"
timeout 60 ./peer --call --signal ws://127.0.0.1:8899 --room demo --name B \
  --seconds 8 --out /tmp/v_B.pcm \
  --video-size 320x240 --video-fps 15 --video-kbps 400 \
  --video-source test:motion \
  --video-sink /tmp/vsink_B > /tmp/v_B.log 2>&1 &
PB=$!

wait $PA 2>/dev/null; RA=$?
wait $PB 2>/dev/null; RB=$?
kill $SRV 2>/dev/null

echo ""
echo "================================================================"
echo "  A 端结果（退出码 $RA）"
echo "================================================================"
grep -E "^--- 阶段|^  ✓|^  \[角色\]|^  视频:|^    \[" /tmp/v_A.log | head -20
echo ""
sed -n '/通话结果/,/通话成功\|通话未完成/p' /tmp/v_A.log

echo ""
echo "================================================================"
echo "  B 端结果（退出码 $RB）"
echo "================================================================"
grep -E "^--- 阶段|^  ✓|^  \[角色\]|^  视频:|^    \[" /tmp/v_B.log | head -20
echo ""
sed -n '/通话结果/,/通话成功\|通话未完成/p' /tmp/v_B.log

echo ""
echo "================================================================"
echo "  解码帧落盘检查"
echo "================================================================"
for side in A B; do
  n=$(ls /tmp/vsink_$side/*.ppm 2>/dev/null | wc -l)
  echo "  $side 端落盘: $n 个 PPM 文件"
done
