#!/bin/bash
# 检查踩坑文档的坑标题
cd /home/ts/accept
echo "=== 02-pitfalls.md 的坑标题列表 ==="
grep -n "^## 坑" docs/02-pitfalls.md | sed 's/^\([0-9]*\):/  \1: /'
echo ""
echo "=== 编号是否连续（1..33）==="
for i in $(seq 1 33); do
  if ! grep -q "^## 坑 $i：" docs/02-pitfalls.md; then
    echo "  缺失编号: $i"
  fi
done
echo "  （无输出表示 1~33 全部存在）"
echo ""
echo "=== 坑 15~33 是否都写了内容（检查每个标题后是否有正文）==="
for i in 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33; do
  lines=$(awk "/^## 坑 $i：/{f=1;next} /^## 坑 [0-9]+：/{f=0} f" docs/02-pitfalls.md | wc -l)
  printf "  坑 %-3s 正文 %4d 行 %s\n" "$i" "$lines" "$([ "$lines" -gt 5 ] && echo ✓ || echo '← 内容过少')"
done
