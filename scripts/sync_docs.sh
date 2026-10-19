#!/bin/bash
# 同步文档到 WSL 并检查完整性
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
cp -f "$SRC/docs/"*.md "$WORK/docs/"
cp -f "$SRC/README.md" "$WORK/"
cp -f "$SRC/scripts/check_docs.sh" "$WORK/scripts/"
cp -f "$SRC/scripts/check_pitfalls.sh" "$WORK/scripts/"

cd $WORK
echo "=== 同步后的文档 ==="
for f in docs/*.md README.md; do
  printf "  %-32s %5d 行\n" "$f" "$(wc -l < "$f")"
done
echo ""
echo "=== 坑编号连续性 ==="
miss=0
for i in $(seq 1 33); do
  grep -q "^## 坑 $i：" docs/02-pitfalls.md || { echo "  缺失: $i"; miss=1; }
done
[ $miss -eq 0 ] && echo "  ✅ 坑 1~33 全部存在"
echo ""
echo "=== 03-verification.md 覆盖检查 ==="
for kw in "Phase 3" "Phase 4" "Phase 5" "Phase 6" "DTLS" "ICE" "NACK" "FEC" "指标" "srflx" "Trickle"; do
  n=$(grep -c "$kw" docs/03-verification.md)
  echo "  $kw: $n 次"
done
