#!/usr/bin/env bash
# 最终验收：全新克隆 → 构建 → 全部测试 → 5 轮稳定性 → 文档核对
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
V=/home/ts/accept_final2
rm -rf $V

echo "########################################################"
echo "#  最终验收（全新克隆 + ts 用户）"
echo "########################################################"

git -c http.sslVerify=false clone -q \
  "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" $V
chown -R ts:ts $V; chmod +x $V/scripts/*.sh

cd $V
echo "  提交: $(git log --oneline -1)"
echo "  提交数: $(git rev-list --count HEAD)"
echo ""
echo "=== 文档（全部应提到视频）==="
for f in docs/00-architecture.md docs/01-progress-log.md docs/02-pitfalls.md \
         docs/03-verification.md docs/04-how-to-run.md docs/05-code-tour.md README.md; do
  n=$(grep -ciE "VP8|视频|关键帧|PLI" "$f" 2>/dev/null || echo 0)
  printf "  %-32s %5d 行  视频提及 %3d %s\n" "$(basename "$f")" "$(wc -l < "$f")" "$n" \
    "$([ "$n" -gt 5 ] && echo OK || echo '← 少')"
done
echo "  坑数: $(grep -c '^## 坑' docs/02-pitfalls.md)"

cat > /tmp/af2.sh <<EOS
#!/usr/bin/env bash
set -u
export PATH="\$HOME/.local/bin:\$PATH"
cd $V

echo ""
echo "=== 构建 ==="
./scripts/run.sh build 2>&1 | grep -E "libvpx|✓|✗" | head -4

echo ""
echo "=== 全部测试 ==="
./scripts/run.sh test 2>&1 | grep -E "结果：|✓ 通过|全部通过" | head -14

echo ""
echo "=== 5 轮稳定性复验 ==="
P=0; F=0
for r in 1 2 3 4 5; do
  pkill -f signal_server 2>/dev/null; pkill -f "peer --call" 2>/dev/null; sleep 0.5
  D=/tmp/acc_\$\$_\$r; rm -rf \$D; mkdir -p \$D/A \$D/B
  ./build/signal_server --port 8920 > \$D/srv.log 2>&1 &
  S=\$!; sleep 0.9
  timeout 40 ./build/peer --call --signal ws://127.0.0.1:8920 --room demo --name A \
    --seconds 5 --out \$D/A.pcm --video-source test:bars --video-sink \$D/A > \$D/A.log 2>&1 &
  PA=\$!; sleep 1.2
  timeout 40 ./build/peer --call --signal ws://127.0.0.1:8920 --room demo --name B \
    --seconds 5 --out \$D/B.pcm --video-source test:motion --video-sink \$D/B > \$D/B.log 2>&1 &
  PB=\$!
  wait \$PA 2>/dev/null; RA=\$?; wait \$PB 2>/dev/null; RB=\$?
  kill \$S 2>/dev/null; sleep 0.3
  AD=\$(grep "解码/丢弃帧" \$D/A.log 2>/dev/null | grep -oE '[0-9]+$' | head -1)
  BD=\$(grep "解码/丢弃帧" \$D/B.log 2>/dev/null | grep -oE '[0-9]+$' | head -1)
  NA=\$(ls \$D/A/*.ppm 2>/dev/null | wc -l)
  NB=\$(ls \$D/B/*.ppm 2>/dev/null | wc -l)
  if [ "\$RA" -eq 0 ] && [ "\$RB" -eq 0 ] && [ "\${AD:-9}" -eq 0 ] && [ "\${BD:-9}" -eq 0 ] \
     && [ "\$NA" -gt 60 ] && [ "\$NB" -gt 60 ]; then
    echo "  轮\$r : PASS  A=\$NA帧 B=\$NB帧 丢弃=\${AD}/\${BD}"
    P=\$((P+1))
  else
    echo "  轮\$r : FAIL  rc=\$RA/\$RB 丢弃=\${AD:-?}/\${BD:-?} PPM=\$NA/\$NB"
    F=\$((F+1))
  fi
  rm -rf \$D
done
echo "  => PASS=\$P FAIL=\$F"
EOS
chmod +x /tmp/af2.sh; chown ts:ts /tmp/af2.sh
sudo -u ts bash /tmp/af2.sh
