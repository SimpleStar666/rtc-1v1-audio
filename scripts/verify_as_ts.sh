#!/usr/bin/env bash
# 以 ts 用户从零完整验证（清掉 build/，模拟用户真实场景）
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio

cp -f "$SRC/scripts/run.sh" "$WORK/scripts/"
chown ts:ts "$WORK/scripts/run.sh"
chmod +x "$WORK/scripts/run.sh"

echo "=== 清理残留 build/（模拟从零）==="
rm -rf "$WORK/build"
echo "  已删除 build/"

echo ""
echo "=== 确认环境状态 ==="
echo "  仓库所有者: $(ls -ld "$WORK" | awk '{print $3":"$4}')"
sudo -u ts bash -c 'export PATH="$HOME/.local/bin:$PATH"; echo "  ts 的 cmake: $(cmake --version | head -1)"'

echo ""
echo "##################################################"
echo "#  ts 用户执行 ./scripts/run.sh build"
echo "##################################################"
cat > /tmp/ts_build.sh <<'EOS'
#!/usr/bin/env bash
cd /home/ts/rtc-1v1-audio
./scripts/run.sh build
EOS
chmod +x /tmp/ts_build.sh; chown ts:ts /tmp/ts_build.sh
sudo -u ts bash /tmp/ts_build.sh 2>&1 | tail -20

echo ""
echo "##################################################"
echo "#  ts 用户执行 ./scripts/run.sh test"
echo "##################################################"
cat > /tmp/ts_test.sh <<'EOS'
#!/usr/bin/env bash
cd /home/ts/rtc-1v1-audio
./scripts/run.sh test
EOS
chmod +x /tmp/ts_test.sh; chown ts:ts /tmp/ts_test.sh
sudo -u ts bash /tmp/ts_test.sh 2>&1 | tail -16

echo ""
echo "=== 产物归属检查（应全是 ts）==="
ls -l "$WORK/build/e2e_test" 2>/dev/null
echo "  非 ts 拥有的条目: $(find "$WORK" -not -user ts 2>/dev/null | wc -l)"
