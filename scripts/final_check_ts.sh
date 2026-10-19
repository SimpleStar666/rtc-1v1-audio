#!/usr/bin/env bash
# 最终验证：以 ts 身份，从干净状态完整跑一遍
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio

# 同步最新脚本与文档
cp -f "$SRC/scripts/run.sh" "$WORK/scripts/"
cp -f "$SRC/docs/"*.md "$WORK/docs/"
cp -f "$SRC/README.md" "$WORK/"
chown -R ts:ts "$WORK"
chmod +x "$WORK/scripts/run.sh"

echo "##################################################"
echo "#  最终验证（以 ts 用户身份）"
echo "##################################################"
echo "  仓库所有者: $(ls -ld "$WORK" | awk '{print $3":"$4}')"
echo "  非 ts 条目: $(find "$WORK" -not -user ts 2>/dev/null | wc -l)"

cat > /tmp/final_check.sh <<'EOS'
#!/usr/bin/env bash
cd /home/ts/rtc-1v1-audio
export PATH="$HOME/.local/bin:$PATH"
echo ""
echo "  cmake: $(cmake --version | head -1)"
echo ""
echo "########## build ##########"
./scripts/run.sh build 2>&1 | tail -14
echo ""
echo "########## env ##########"
./scripts/run.sh env 2>&1 | tail -2
EOS
chmod +x /tmp/final_check.sh; chown ts:ts /tmp/final_check.sh
sudo -u ts bash /tmp/final_check.sh

echo ""
echo "########## test（ts 身份）##########"
cat > /tmp/final_test.sh <<'EOS'
#!/usr/bin/env bash
cd /home/ts/rtc-1v1-audio
./scripts/run.sh test 2>&1 | tail -14
EOS
chmod +x /tmp/final_test.sh; chown ts:ts /tmp/final_test.sh
sudo -u ts bash /tmp/final_test.sh
