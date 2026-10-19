#!/usr/bin/env bash
# 正确方案：给 ts 用户自己装 cmake（--user），并清理错误的系统软链
set -u

echo "=== 1. 清理错误的软链 ==="
if [ -L /usr/local/bin/cmake ]; then
  rm -f /usr/local/bin/cmake
  echo "  已删除 /usr/local/bin/cmake（它指向 /root，ts 不可读）"
fi
# 恢复系统 cmake 的正常可见性（/usr/bin/cmake 本来就在 PATH 里）
hash -r

echo ""
echo "=== 2. 给 ts 用户安装 cmake（pip --user）==="
sudo -u ts -i bash -c '
  export PATH="$HOME/.local/bin:$PATH"
  python3 -m pip install --user --upgrade "cmake>=3.28" 2>&1 | tail -5
'

echo ""
echo "=== 3. 验证 ts 用户 ==="
sudo -u ts -i bash -c '
  export PATH="$HOME/.local/bin:$PATH"
  echo "  which cmake: $(which cmake)"
  echo "  version: $(cmake --version | head -1)"
'

echo ""
echo "=== 4. 版本是否满足 >= 3.21 ==="
VER=$(sudo -u ts -i bash -c 'export PATH="$HOME/.local/bin:$PATH"; cmake --version | head -1' | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
echo "  版本: $VER"
MAJ=${VER%%.*}; REST=${VER#*.}; MIN=${REST%%.*}
if [ "${MAJ:-0}" -gt 3 ] || { [ "${MAJ:-0}" -eq 3 ] && [ "${MIN:-0}" -ge 21 ]; }; then
  echo "  ✅ 满足"
else
  echo "  ❌ 不满足"
fi
