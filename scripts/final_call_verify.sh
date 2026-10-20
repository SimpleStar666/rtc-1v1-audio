#!/usr/bin/env bash
# 终极验证：全新克隆 + ts 用户 + 真实通话
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
V=/home/ts/final_call
rm -rf $V

echo "######################################################"
echo "#  全新克隆 -> 编译 -> 真实双进程通话（ts 身份）"
echo "######################################################"

git -c http.sslVerify=false clone -q \
  "https://SimpleStar666/rtc-1v1-audio.git" $V 2>/dev/null || \
git -c http.sslVerify=false clone -q \
  "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" $V
chown -R ts:ts $V; chmod +x $V/scripts/*.sh
echo "  提交: $(cd $V && git log --oneline -1)"
echo "  文档: $(ls $V/docs/*.md | wc -l) 个"

cat > /tmp/fc.sh <<EOS
#!/usr/bin/env bash
set -u
export PATH="\$HOME/.local/bin:\$PATH"
cd $V

echo ""
echo "--- 编译 ---"
./scripts/run.sh build 2>&1 | tail -13

echo ""
echo "--- 真实通话 ---"
./scripts/run.sh call 2>&1 | tail -32
EOS
chmod +x /tmp/fc.sh; chown ts:ts /tmp/fc.sh
sudo -u ts bash /tmp/fc.sh
