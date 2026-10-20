#!/usr/bin/env bash
# 重新编译并跑稳定性测试
set -u
SRC=/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio
WORK=/home/ts/rtc-1v1-audio

cp -r "$SRC/src" "$WORK/"
chown -R ts:ts "$WORK"

cat > /tmp/rb2.sh <<'EOS'
#!/usr/bin/env bash
export PATH="$HOME/.local/bin:$PATH"
cd /home/ts/rtc-1v1-audio/build
cmake --build . -j32 2>&1 | grep -E "error:|Built target (peer|signal_server)" | head -10
EOS
chmod +x /tmp/rb2.sh; chown ts:ts /tmp/rb2.sh
sudo -u ts bash /tmp/rb2.sh