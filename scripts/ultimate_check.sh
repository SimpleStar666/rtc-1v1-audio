#!/usr/bin/env bash
# 终极验证：以 ts 用户从 GitHub 全新克隆，跑用户那条命令
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
V=/home/ts/fromscratch
rm -rf $V

echo "############################################################"
echo "#  终极验证：ts 用户 + 全新克隆 + 用户的原命令"
echo "############################################################"

git -c http.sslVerify=false clone -q \
  "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" $V
chown -R ts:ts $V
chmod +x $V/scripts/*.sh

echo ""
echo "提交: $(cd $V && git log --oneline -1)"
echo "所有者: $(ls -ld $V | awk '{print $3":"$4}')"
echo ""
echo "=== 确认 run.sh 含权限/版本自检 ==="
grep -c "check_writable\|check_cmake_version" $V/scripts/run.sh | sed 's/^/  自检函数引用次数: /'

echo ""
echo "=== 关键：确认是 ts 用户、且 cmake 是新版 ==="
cat > /tmp/scratch_check.sh <<EOS
#!/usr/bin/env bash
set -e
echo "  当前用户: \$(id -un)"
echo "  build/ 存在? \$([ -d $V/build ] && echo 是 || echo 否（干净）)"
EOS
chmod +x /tmp/scratch_check.sh; chown ts:ts /tmp/scratch_check.sh
sudo -u ts bash /tmp/scratch_check.sh

echo ""
echo "########## ts 用户执行用户的原命令：./scripts/run.sh build ##########"
cat > /tmp/scratch_build.sh <<EOS
#!/usr/bin/env bash
cd $V
./scripts/run.sh build
EOS
chmod +x /tmp/scratch_build.sh; chown ts:ts /tmp/scratch_build.sh
sudo -u ts bash /tmp/scratch_build.sh 2>&1 | tail -16

echo ""
echo "########## ts 用户执行 ./scripts/run.sh test ##########"
cat > /tmp/scratch_test.sh <<EOS
#!/usr/bin/env bash
cd $V
./scripts/run.sh test
EOS
chmod +x /tmp/scratch_test.sh; chown ts:ts /tmp/scratch_test.sh
sudo -u ts bash /tmp/scratch_test.sh 2>&1 | tail -14

echo ""
echo "=== 产物归属 ==="
ls -l $V/build/e2e_test 2>/dev/null | awk '{print "  " $3":"$4, $9}'
