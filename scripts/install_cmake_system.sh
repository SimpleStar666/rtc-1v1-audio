#!/usr/bin/env bash
# 把新版 cmake 装到系统级，让所有用户（含 ts）都能用
set -e

echo "=== 当前状态 ==="
echo "  系统 cmake: $(/usr/bin/cmake --version | head -1)"
echo "  root pip cmake: $(/root/.local/bin/cmake --version 2>/dev/null | head -1 || echo 无)"

echo ""
echo "=== 方式 A：pip 系统级安装到 /usr/local ==="
if python3 -m pip install --prefix=/usr/local --upgrade "cmake>=3.28" > /tmp/pipcmake.log 2>&1; then
  echo "  ✓ pip 系统级安装成功"
else
  echo "  ✗ pip 系统级安装失败，看日志："
  tail -8 /tmp/pipcmake.log | sed 's/^/      /'
fi

# pip --prefix 会把可执行文件放在 /usr/local/bin
if [ -x /usr/local/bin/cmake ]; then
  echo "  /usr/local/bin/cmake -> $(/usr/local/bin/cmake --version | head -1)"
fi

echo ""
echo "=== 方式 B（兜底）：把 root 的 pip cmake 暴露给全系统 ==="
if [ ! -x /usr/local/bin/cmake ] && [ -x /root/.local/bin/cmake ]; then
  # root 的 cmake 是 pip 装的脚本，依赖 root 的 site-packages，直接软链可能缺库
  # 更稳的做法：用 root 的 python 环境，但把入口脚本放 /usr/local/bin
  ROOT_PY=$(python3 -c 'import sys; print(sys.prefix)')
  echo "  root python prefix: $ROOT_PY"
  ln -sf /root/.local/bin/cmake /usr/local/bin/cmake
  echo "  已软链 /usr/local/bin/cmake -> /root/.local/bin/cmake"
fi

echo ""
echo "=== 验证：各用户看到的 cmake ==="
echo "--- root ---"
bash -lc 'which cmake; cmake --version | head -1'
echo "--- ts ---"
sudo -u ts -i bash -c 'which cmake; cmake --version | head -1'

echo ""
echo "=== 关键验证：ts 的 cmake 版本 >= 3.21 ？ ==="
VER=$(sudo -u ts -i bash -c 'cmake --version | head -1' | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
MAJ=$(echo "$VER" | cut -d. -f1)
MIN=$(echo "$VER" | cut -d. -f2)
echo "  ts 的 cmake 版本: $VER"
if [ "$MAJ" -gt 3 ] || { [ "$MAJ" -eq 3 ] && [ "$MIN" -ge 21 ]; }; then
  echo "  ✅ 满足要求（>= 3.21）"
else
  echo "  ❌ 仍不满足"
fi
