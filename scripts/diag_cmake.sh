#!/usr/bin/env bash
# 诊断 cmake 工具链归属
echo "=== root 看到的 cmake ==="
which -a cmake 2>/dev/null
cmake --version 2>/dev/null | head -1

echo ""
echo "=== root 的 pip 安装位置 ==="
ls -la /root/.local/bin/cmake 2>/dev/null && /root/.local/bin/cmake --version | head -1

echo ""
echo "=== ts 看到的 cmake ==="
sudo -u ts -i bash -c 'which -a cmake; echo "---"; cmake --version | head -1'

echo ""
echo "=== ts 的 ~/.local/bin ==="
ls -la /home/ts/.local/bin/ 2>/dev/null | head -10 || echo "  不存在或为空"

echo ""
echo "=== ts 能否 pip install ==="
sudo -u ts -i bash -c 'python3 -m pip --version' 2>&1 | head -2

echo ""
echo "=== 系统 cmake 版本 ==="
/usr/bin/cmake --version | head -1

echo ""
echo "=== 残留的失败 CMakeCache ==="
ls -la /home/ts/rtc-1v1-audio/build/CMakeCache.txt 2>/dev/null || echo "  无"
