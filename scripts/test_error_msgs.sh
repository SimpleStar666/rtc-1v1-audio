#!/usr/bin/env bash
# 验证错误提示：故意制造两种故障，看提示是否清晰可操作
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
cp -f "$SRC/scripts/run.sh" "$WORK/scripts/"; chown ts:ts "$WORK/scripts/run.sh"

echo "##################################################"
echo "#  故障 1：build/ 属于 root（模拟用户遇到的原始问题）"
echo "##################################################"
rm -rf "$WORK/build"
mkdir -p "$WORK/build"
echo "root 的残留" > "$WORK/build/build.log"
chown -R root:root "$WORK/build"
ls -ld "$WORK/build"

cat > /tmp/t1.sh <<'EOS'
cd /home/ts/rtc-1v1-audio && ./scripts/run.sh build
EOS
chmod +x /tmp/t1.sh; chown ts:ts /tmp/t1.sh
echo ""
echo "--- ts 用户运行结果 ---"
sudo -u ts bash /tmp/t1.sh 2>&1 | tail -14

echo ""
echo "##################################################"
echo "#  故障 2：cmake 版本过低"
echo "##################################################"
rm -rf "$WORK/build"
chown -R ts:ts "$WORK"

# 用 PATH 屏蔽新版 cmake，只留系统的 3.16
cat > /tmp/t2.sh <<'EOS'
cd /home/ts/rtc-1v1-audio
# 故意让 PATH 里只有系统 cmake
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
# 覆盖 run.sh 里的 HOME 优先逻辑：临时把 ~/.local/bin 指向空目录
HOME=/tmp/fakehome_notexist
mkdir -p "$HOME/.local/bin"
export HOME
./scripts/run.sh build
EOS
chmod +x /tmp/t2.sh; chown ts:ts /tmp/t2.sh
echo ""
echo "--- ts 用户运行结果 ---"
sudo -u ts bash /tmp/t2.sh 2>&1 | tail -16

echo ""
echo "=== 清理，恢复正常 ==="
rm -rf "$WORK/build"
rm -rf /tmp/fakehome_notexist
chown -R ts:ts "$WORK"
echo "  已清理"
