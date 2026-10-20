#!/usr/bin/env bash
# 完整验证：run.sh test（含视频）+ run.sh call（音视频通话）
set -u
SRC=/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio
WORK=/home/ts/rtc-1v1-audio

cp -f "$SRC/scripts/run.sh" "$WORK/scripts/"
chown -R ts:ts "$WORK"
chmod +x "$WORK/scripts/"*.sh

cat > /tmp/full_v.sh <<'EOS'
#!/usr/bin/env bash
export PATH="$HOME/.local/bin:$PATH"
cd /home/ts/rtc-1v1-audio

echo "================================================================"
echo "  ./scripts/run.sh test（全部测试，含视频）"
echo "================================================================"
./scripts/run.sh test 2>&1 | tail -18

echo ""
echo "================================================================"
echo "  ./scripts/run.sh call（音视频通话）"
echo "================================================================"
./scripts/run.sh call 2>&1 | tail -42
EOS
chmod +x /tmp/full_v.sh
chown ts:ts /tmp/full_v.sh
sudo -u ts bash /tmp/full_v.sh
