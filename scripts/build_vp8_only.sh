#!/usr/bin/env bash
# 同步源码并只构建 vp8_test（视频编解码自检）
set -e
SRC=/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio
WORK=/home/ts/rtc-1v1-audio

echo "=== 同步源码 ==="
cp -r "$SRC/src" "$WORK/"
cp -r "$SRC/tests" "$WORK/"
cp "$SRC/CMakeLists.txt" "$WORK/"

echo "=== 临时移除未实现的视频管线（先只测 VP8 编解码）==="
sed -i '/src\/media\/video_device.cpp/d' "$WORK/CMakeLists.txt"
sed -i '/src\/media\/video_pipeline.cpp/d' "$WORK/CMakeLists.txt"
sed -i 's|add_executable(video_test tests/video_test.cpp)|# video_test 待实现|' "$WORK/CMakeLists.txt"
sed -i '/target_link_libraries(video_test/d' "$WORK/CMakeLists.txt"
sed -i '/target_include_directories(video_test/d' "$WORK/CMakeLists.txt"

grep -n "vp8_codec\|vp8_test\|video_test" "$WORK/CMakeLists.txt" | sed 's/^/  /'

chown -R ts:ts "$WORK"

cat > /tmp/build_vp8.sh <<'EOS'
#!/usr/bin/env bash
export PATH="$HOME/.local/bin:$PATH"
cd /home/ts/rtc-1v1-audio/build
echo "=== configure ==="
cmake .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5 2>&1 | grep -E "libvpx|Error|error" | head -5
echo "=== build vp8_test ==="
cmake --build . -j32 --target vp8_test 2>&1 | grep -E "error:|Error|Built target" | head -15
EOS
chmod +x /tmp/build_vp8.sh
chown ts:ts /tmp/build_vp8.sh
sudo -u ts bash /tmp/build_vp8.sh
