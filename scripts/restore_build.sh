#!/usr/bin/env bash
# 恢复：以 ts 身份重新编译并验证
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
cp -f "$SRC/scripts/run.sh" "$WORK/scripts/"; chown ts:ts "$WORK/scripts/run.sh"

rm -rf "$WORK/build"
chown -R ts:ts "$WORK"

cat > /tmp/rebuild.sh <<'EOS'
cd /home/ts/rtc-1v1-audio && ./scripts/run.sh build
EOS
chmod +x /tmp/rebuild.sh; chown ts:ts /tmp/rebuild.sh
sudo -u ts bash /tmp/rebuild.sh 2>&1 | tail -14

echo ""
echo "=== 最终状态 ==="
ls -ld "$WORK" "$WORK/build"
echo "  非 ts 条目: $(find "$WORK" -not -user ts 2>/dev/null | wc -l)"
sudo -u ts bash -c 'export PATH="$HOME/.local/bin:$PATH"; echo "  ts cmake: $(cmake --version | head -1)"'
echo "  e2e_test: $(ls -l /home/ts/rtc-1v1-audio/build/e2e_test 2>/dev/null | awk '{print $3":"$4}')"
