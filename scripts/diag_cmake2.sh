#!/usr/bin/env bash
# 诊断为什么 ts 仍用系统 cmake
echo "=== ts 的 PATH ==="
sudo -u ts -i bash -c 'echo $PATH' | tr ':' '\n' | head -12

echo ""
echo "=== /usr/local/bin/cmake 是什么 ==="
ls -l /usr/local/bin/cmake 2>/dev/null
echo "  内容前 2 行:"
head -2 /usr/local/bin/cmake 2>/dev/null | sed 's/^/    /'

echo ""
echo "=== ts 能否执行 /usr/local/bin/cmake ==="
sudo -u ts /usr/local/bin/cmake --version 2>&1 | head -3

echo ""
echo "=== /root 是否对 ts 可读（软链失效的原因）==="
ls -ld /root
sudo -u ts test -r /root/.local/bin/cmake && echo "  ts 可读" || echo "  ✗ ts 不可读 /root（软链方案行不通）"

echo ""
echo "=== pip --prefix 装的东西在哪 ==="
find /usr/local -maxdepth 3 -name 'cmake*' 2>/dev/null | head -8
