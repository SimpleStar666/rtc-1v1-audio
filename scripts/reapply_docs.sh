#!/usr/bin/env bash
# 重新应用文档修改（坑 39-41 + 代码导读更新），并校验
set -e
S='/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/scripts'
D='/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/docs'

echo "=== 应用前 ==="
echo "  坑数: $(grep -c '^## 坑' "$D/02-pitfalls.md")"

echo ""
echo "=== 坑 39/40/41 ==="
python3 "$S/add_pitfalls_394041.py" || true

echo ""
echo "=== 代码导读更新 ==="
python3 "$S/update_codetour.py" || true

echo ""
echo "=== 应用后校验 ==="
echo "  坑数      : $(grep -c '^## 坑' "$D/02-pitfalls.md")"
echo "  声明坑数  : $(grep -oE '共 [0-9]+ 个坑' "$D/02-pitfalls.md" | head -1)"
echo "  考坑39存在: $(grep -c '坑 39' "$D/02-pitfalls.md")"
echo "  坑40存在  : $(grep -c '坑 40' "$D/02-pitfalls.md")"
echo "  坑41存在  : $(grep -c '坑 41' "$D/02-pitfalls.md")"
echo "  导读已声明: $(grep -c '坑 39（JitterBuffer 加锁）' "$D/05-code-tour.md")"
echo "  FEC u16   : $(grep -c 'u16 len\.\.\.' "$D/05-code-tour.md")"
