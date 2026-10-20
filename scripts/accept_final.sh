#!/usr/bin/env bash
# 最终验收：全新克隆 -> 构建 -> 回归 -> 真通话 -> 文档齐全
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
V=/home/ts/accept_final
rm -rf $V

echo "########################################################"
echo "#  最终验收"
echo "########################################################"

git -c http.sslVerify=false clone -q \
  "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" $V
chown -R ts:ts $V; chmod +x $V/scripts/*.sh
echo "  提交: $(cd $V && git log --oneline -1)"
echo "  提交数: $(cd $V && git rev-list --count HEAD)"
echo ""

echo "=== 文档 ==="
cd $V
for f in docs/*.md README.md; do
  printf "  %-34s %5d 行\n" "$f" "$(wc -l < "$f")"
done
echo "  坑数: $(grep -c '^## 坑' docs/02-pitfalls.md)"
echo "  代码导读模块数: $(grep -c '^## 模块' docs/05-code-tour.md)"

cat > /tmp/af.sh <<EOS
#!/usr/bin/env bash
set -u
export PATH="\$HOME/.local/bin:\$PATH"
cd $V

echo ""
echo "=== 编译 ==="
./scripts/run.sh build 2>&1 | grep -E "✓|✗|编译" | head -4

echo ""
echo "=== 全量回归 ==="
bash scripts/full_regression.sh 2>&1 | tail -12

echo ""
echo "=== 真实通话 ==="
./scripts/run.sh call 2>&1 | grep -E "A 端:|B 端:|两端通话|主导频率|采集源" | head -8
EOS
chmod +x /tmp/af.sh; chown ts:ts /tmp/af.sh
sudo -u ts bash /tmp/af.sh
