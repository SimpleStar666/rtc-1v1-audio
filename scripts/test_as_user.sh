#!/usr/bin/env bash
# 以 ts 用户身份真实测试（这才是用户的真实场景）
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
cp -f "$SRC/scripts/run.sh" "$WORK/scripts/"
chmod +x "$WORK/scripts/"*.sh
chown ts:ts "$WORK/scripts/run.sh"

echo "##################################################
#  以 ts 用户身份运行（模拟真实使用场景）
##################################################"

echo ""
echo "=== 1. 确认 run.sh 归属 ==="
ls -l "$WORK/scripts/run.sh"

echo ""
echo "=== 2. ts 用户跑 ./scripts/run.sh build ==="
sudo -u ts -i bash -c "cd $WORK && ./scripts/run.sh build" 2>&1 | tail -20

echo ""
echo "=== 3. ts 用户跑 ./scripts/run.sh test ==="
sudo -u ts -i bash -c "cd $WORK && ./scripts/run.sh test" 2>&1 | tail -16

echo ""
echo "=== 4. 编译产物归属检查（应为 ts，不是 root）==="
ls -l "$WORK/build/e2e_test" 2>/dev/null
echo "非 ts 拥有的条目: $(find "$WORK" -not -user ts 2>/dev/null | wc -l)"
