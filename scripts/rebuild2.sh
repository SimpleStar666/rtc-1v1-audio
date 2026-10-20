#!/usr/bin/env bash
# 同步 src+tests 并重编译（把复杂引号全部关在脚本里）
set -u
SRC=/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio
WORK=/home/ts/rtc-1v1-audio

cp -r "$SRC/src" "$WORK/"
cp -r "$SRC/tests" "$WORK/"
cp "$SRC/CMakeLists.txt" "$WORK/"
chown -R ts:ts "$WORK"

cat > /tmp/rb_y.sh <<'EOS'
#!/usr/bin/env bash
export PATH="$HOME/.local/bin:$PATH"
cd /home/ts/rtc-1v1-audio/build
cmake --build . -j32 2>&1 | grep -E "error:|Built target" | tail -8
EOS
chmod +x /tmp/rb_y.sh
chown ts:ts /tmp/rb_y.sh
sudo -u ts bash /tmp/rb_y.sh
