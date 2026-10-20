#!/usr/bin/env bash
# 统一的重编译脚本（避免引号问题）
set -u
SRC=/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio
WORK=/home/ts/rtc-1v1-audio

cp -r "$SRC/src" "$WORK/"
chown -R ts:ts "$WORK"

cat > /tmp/rb_x.sh <<'EOS'
#!/usr/bin/env bash
export PATH="$HOME/.local/bin:$PATH"
cd /home/ts/rtc-1v1-audio/build
cmake --build . -j32 2>&1 | grep -E "error:|Built target" | head -12
EOS
chmod +x /tmp/rb_x.sh
chown ts:ts /tmp/rb_x.sh
sudo -u ts bash /tmp/rb_x.sh
