#!/usr/bin/env bash
# 把 WSL 侧改好的 jitter_buffer 同步回 C 盘（以 WSL 为准），再构建
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio

echo "=== 把 WSL 侧改动同步回 C 盘 ==="
cp -f "$WORK/src/media/jitter_buffer.cpp" "$SRC/src/media/jitter_buffer.cpp"
cp -f "$WORK/src/media/jitter_buffer.h"   "$SRC/src/media/jitter_buffer.h"
cp -f "$WORK/src/media/audio_pipeline.cpp" "$SRC/src/media/audio_pipeline.cpp"
echo "  已同步 jitter_buffer.{h,cpp} / audio_pipeline.cpp"

echo ""
echo "=== 确认 set_config 存在 ==="
grep -c "JitterBuffer::set_config" "$SRC/src/media/jitter_buffer.cpp" | sed 's/^/  C盘实现数: /'
grep -c "set_config" "$SRC/src/media/jitter_buffer.h" | sed 's/^/  C盘声明数: /'

echo ""
echo "=== 构建（ts 用户）==="
chown -R ts:ts "$WORK"
cat > /tmp/b2.sh <<'EOS'
#!/usr/bin/env bash
export PATH="$HOME/.local/bin:$PATH"
cd /home/ts/rtc-1v1-audio/build
cmake --build . -j32 2>&1 | grep -E "error|undefined|Built target peer" | head -12
EOS
chmod +x /tmp/b2.sh; chown ts:ts /tmp/b2.sh
sudo -u ts bash /tmp/b2.sh
