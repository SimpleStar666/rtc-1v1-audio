#!/bin/bash
# 文档一致性检查
cd /home/ts/accept
echo "=== 文档清单 ==="
for f in docs/*.md README.md; do
  printf "  %-32s %5d 行\n" "$f" "$(wc -l < "$f")"
done

echo ""
echo "=== 03-verification.md 对各阶段的覆盖 ==="
for kw in "Phase 3" "Phase 4" "Phase 5" "Phase 6" "DTLS" "ICE" "NACK" "FEC" "指标" "srflx" "Trickle"; do
  n=$(grep -c "$kw" docs/03-verification.md 2>/dev/null)
  [ -z "$n" ] && n=0
  if [ "$n" -gt 0 ]; then mark="✓"; else mark="← 缺"; fi
  printf "  %-12s 出现 %2s 次 %s\n" "$kw" "$n" "$mark"
done

echo ""
echo "=== 踩坑文档的坑数统计 ==="
grep -c "^## 坑" docs/02-pitfalls.md | sed 's/^/  实际坑数: /'
grep -m1 "共 .* 个坑" docs/02-pitfalls.md | sed 's/^/  文档声明: /'

echo ""
echo "=== 源码文件统计 ==="
find src -name '*.cpp' | wc -l | sed 's/^/  .cpp 文件: /'
find src -name '*.h' | wc -l | sed 's/^/  .h 文件: /'
find src tests -name '*.cpp' -o -name '*.h' | xargs wc -l 2>/dev/null | tail -1 | sed 's/^/  总行数: /'
