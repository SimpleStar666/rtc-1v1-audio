#!/usr/bin/env bash
# 检查文档是否已包含视频内容（而不是只改了 README）
WORK=/home/ts/rtc-1v1-audio
cd "$WORK"

echo "=== 文档清单 ==="
for f in docs/*.md README.md; do
  printf "  %-34s %5d 行  %s\n" "$f" "$(wc -l < "$f")" "$(stat -c %y "$f" | cut -d. -f1)"
done

echo ""
echo "=== 各文档是否提到视频 ==="
for f in docs/*.md README.md; do
  n=$(grep -ciE "VP8|视频|libvpx|关键帧|PLI" "$f" 2>/dev/null || echo 0)
  printf "  %-34s 视频相关提及 %3d 次 %s\n" "$f" "$n" "$([ "$n" -gt 3 ] && echo '✓' || echo '← 缺')"
done

echo ""
echo "=== 03-verification.md 是否覆盖视频实测 ==="
for kw in "VP8" "PSNR" "分片" "关键帧" "PLI" "视频"; do
  n=$(grep -c "$kw" docs/03-verification.md 2>/dev/null || echo 0)
  printf "  %-10s %s 次 %s\n" "$kw" "$n" "$([ "$n" -gt 0 ] && echo '✓' || echo '← 缺')"
done

echo ""
echo "=== 01-progress-log.md 是否记录视频阶段 ==="
grep -nE "^## (Phase|阶段)" docs/01-progress-log.md | tail -6

echo ""
echo "=== 04-how-to-run.md 是否有视频运行说明 ==="
for kw in "video" "--video" "VP8" "视频"; do
  n=$(grep -c "$kw" docs/04-how-to-run.md 2>/dev/null || echo 0)
  printf "  %-10s %s 次 %s\n" "$kw" "$n" "$([ "$n" -gt 0 ] && echo '✓' || echo '← 缺')"
done

echo ""
echo "=== 00-architecture.md 是否有视频架构 ==="
for kw in "视频" "VP8" "video"; do
  n=$(grep -c "$kw" docs/00-architecture.md 2>/dev/null || echo 0)
  printf "  %-10s %s 次 %s\n" "$kw" "$n" "$([ "$n" -gt 0 ] && echo '✓' || echo '← 缺')"
done

echo ""
echo "=== 坑数 ==="
echo "  02-pitfalls.md: $(grep -c '^## 坑' docs/02-pitfalls.md) 个坑"
