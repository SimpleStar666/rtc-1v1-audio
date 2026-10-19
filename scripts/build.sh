#!/bin/bash
# 构建脚本：环境探测 + 依赖检查 + 编译
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# CMake 4.x 由 pip 安装在 ~/.local/bin（系统自带 3.16 不满足新版依赖）
export PATH="$HOME/.local/bin:$PATH"

echo "=== 环境 ==="
echo "  cmake : $(cmake --version 2>/dev/null | head -1 || echo MISSING)"
echo "  g++   : $(g++ --version 2>/dev/null | head -1 || echo MISSING)"

echo "=== 依赖检查 ==="
missing=0
for p in opus libsrtp2 nice libwebsockets glib-2.0 openssl; do
  if pkg-config --exists "$p"; then
    echo "  ✅ $p $(pkg-config --modversion $p)"
  else
    echo "  ❌ $p (缺失)"
    missing=1
  fi
done
if [ $missing -ne 0 ]; then
  echo ""
  echo "安装依赖："
  echo "  sudo apt-get install -y libopus-dev libsrtp2-dev libnice-dev libwebsockets-dev libglib2.0-dev libssl-dev pkg-config"
  exit 1
fi

BUILD_DIR="${BUILD_DIR:-build}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo ""
echo "=== CMake 配置 ==="
cmake .. -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
         -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > cmake_configure.log 2>&1 \
  || { echo "配置失败："; tail -30 cmake_configure.log; exit 1; }

echo "=== 编译（$(nproc) 核）==="
cmake --build . -j"$(nproc)" 2>&1 | tee build.log | grep -E "error|Error|warning: unused|Built target" || true

echo ""
echo "=== 产物 ==="
ls -la selftest signal_server peer weaknet_test 2>/dev/null || true
