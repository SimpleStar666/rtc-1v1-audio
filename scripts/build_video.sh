#!/usr/bin/env bash
# 构建并运行视频测试（vp8_test + video_test）
set -e
SRC=/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio
WORK=/home/ts/rtc-1v1-audio

echo "=== 同步 ==="
cp -r "$SRC/src" "$WORK/"
cp -r "$SRC/tests" "$WORK/"
cp "$SRC/CMakeLists.txt" "$WORK/"
chown -R ts:ts "$WORK"

echo "=== CMake 校验 ==="
grep -c "vpx\|VPX" "$WORK/CMakeLists.txt" | sed 's/^/  vpx 引用: /'
grep -n "vp8_codec\|video_device\|video_pipeline\|vp8_test\|video_test" "$WORK/CMakeLists.txt" | sed 's/^/  /'

cat > /tmp/build_video.sh <<'EOS'
#!/usr/bin/env bash
export PATH="$HOME/.local/bin:$PATH"
cd /home/ts/rtc-1v1-audio/build
echo "=== configure ==="
cmake .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5 2>&1 | grep -E "libvpx|Error" | head -5
echo "=== build ==="
cmake --build . -j32 2>&1 | grep -E "error:|Error [0-9]|Built target (vp8_test|video_test|peer)" | head -25
EOS
chmod +x /tmp/build_video.sh
chown ts:ts /tmp/build_video.sh
sudo -u ts bash /tmp/build_video.sh
