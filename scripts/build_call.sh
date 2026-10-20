#!/usr/bin/env bash
# 编译（ts 用户），显示所有错误
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
cp -r "$SRC/src" "$SRC/tests" /home/ts/rtc-1v1-audio/
cp "$SRC/CMakeLists.txt" /home/ts/rtc-1v1-audio/
chown -R ts:ts /home/ts/rtc-1v1-audio

cat > /tmp/b.sh <<'EOS'
#!/usr/bin/env bash
export PATH="$HOME/.local/bin:$PATH"
cd /home/ts/rtc-1v1-audio/build
cmake .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > /dev/null 2>&1
cmake --build . -j32 2>&1 | grep -E "error:|Error|Built target peer" | head -25
EOS
chmod +x /tmp/b.sh; chown ts:ts /tmp/b.sh
sudo -u ts bash /tmp/b.sh
