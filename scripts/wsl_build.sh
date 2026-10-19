#!/bin/bash
# 首次构建验证（从 C 盘工作区同步到 WSL 原生目录后编译）
set -e
SRC=/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio
DST=/home/ts/rtc-1v1-audio
export PATH=$HOME/.local/bin:$PATH

echo "=== 同步到 WSL 原生目录（性能更好）==="
mkdir -p $DST
cp -r $SRC/* $DST/ 2>/dev/null || true
chmod +x $DST/scripts/*.sh 2>/dev/null || true
cd $DST
echo "  同步完成: $(find $DST -type f | wc -l) 个文件"

echo ""
echo "=== 依赖检查 ==="
for p in opus libsrtp2 nice libwebsockets glib-2.0 openssl; do
  v=$(pkg-config --modversion $p 2>/dev/null)
  [ -n "$v" ] && echo "  ✅ $p $v" || echo "  ❌ $p MISSING"
done

echo ""
echo "=== CMake 配置 ==="
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > cfg.log 2>&1
if [ $? -ne 0 ]; then
  echo "❌ 配置失败"; tail -40 cfg.log; exit 1
fi
echo "  ✅ 配置成功"
grep -E "^(opus|libsrtp2|libnice|libwebsockets|openssl)" cfg.log || true
