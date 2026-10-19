#!/usr/bin/env bash
# 给 ts 用户装 cmake（用临时脚本文件，避免 sudo -i -c 的引号问题）
set -u

echo "=== 1. 清理错误软链 ==="
[ -L /usr/local/bin/cmake ] && { rm -f /usr/local/bin/cmake; echo "  已删除 /usr/local/bin/cmake"; }

echo ""
echo "=== 2. 生成安装脚本并以 ts 身份执行 ==="
cat > /tmp/install_cmake_ts.sh <<'EOS'
#!/usr/bin/env bash
set -e
echo "  以用户 $(id -un) 安装 cmake 到 ~/.local"
python3 -m pip install --user --upgrade "cmake>=3.28" 2>&1 | tail -6
EOS
chmod +x /tmp/install_cmake_ts.sh
chown ts:ts /tmp/install_cmake_ts.sh

sudo -u ts bash /tmp/install_cmake_ts.sh

echo ""
echo "=== 3. 验证 ts 的 cmake ==="
cat > /tmp/verify_ts_cmake.sh <<'EOS'
#!/usr/bin/env bash
export PATH="$HOME/.local/bin:$PATH"
echo "  which  : $(command -v cmake)"
echo "  version: $(cmake --version | head -1)"
EOS
chmod +x /tmp/verify_ts_cmake.sh
chown ts:ts /tmp/verify_ts_cmake.sh
sudo -u ts bash /tmp/verify_ts_cmake.sh

echo ""
echo "=== 4. 版本判定 ==="
VER=$(sudo -u ts bash /tmp/verify_ts_cmake.sh | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
echo "  ts 的 cmake: ${VER:-未知}"
MAJ=${VER%%.*}; REST=${VER#*.}; MIN=${REST%%.*}
if [ "${MAJ:-0}" -gt 3 ] || { [ "${MAJ:-0}" -eq 3 ] && [ "${MIN:-0}" -ge 21 ]; }; then
  echo "  ✅ 满足 >= 3.21"
else
  echo "  ❌ 不满足，需要兜底方案"
fi
