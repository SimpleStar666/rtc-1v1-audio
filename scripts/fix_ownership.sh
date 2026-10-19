#!/usr/bin/env bash
# 把仓库所有权限交还给 ts 用户
set -e
WORK=/home/ts/rtc-1v1-audio

echo "=== 修复前 ==="
ls -ld "$WORK" "$WORK/build" 2>/dev/null
echo "非 ts 拥有的条目: $(find "$WORK" -not -user ts 2>/dev/null | wc -l)"

echo ""
echo "=== 执行 chown -R ts:ts ==="
chown -R ts:ts "$WORK"

echo ""
echo "=== 修复后 ==="
ls -ld "$WORK"
ls -ld "$WORK/build" 2>/dev/null || echo "  build/ 不存在"
echo "非 ts 拥有的条目: $(find "$WORK" -not -user ts 2>/dev/null | wc -l)"

echo ""
echo "=== 验证 ts 用户可写 ==="
sudo -u ts test -w "$WORK" && echo "  ✓ 仓库根目录可写" || echo "  ✗ 仍不可写"
if [ -d "$WORK/build" ]; then
  sudo -u ts test -w "$WORK/build" && echo "  ✓ build/ 可写" || echo "  ✗ build/ 仍不可写"
  sudo -u ts rm -rf "$WORK/build"
  echo "  已删除旧的 root build/（避免残留 root 文件继续捣乱）"
fi
