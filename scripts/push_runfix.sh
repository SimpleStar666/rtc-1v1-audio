#!/usr/bin/env bash
# 推送 run.sh 修复
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

cp -f "$SRC/scripts/run.sh" "$WORK/scripts/"
cp -f "$SRC/scripts/accept_manual.sh" "$WORK/scripts/"

cd $WORK
git config http.sslVerify false
git add -A scripts
git commit -q -m "fix(run.sh): 修复输出重复行与表格列对齐" || echo "(无变更)"
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -2
echo "rc=$?"
git log --oneline -1
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
