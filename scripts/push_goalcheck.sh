#!/bin/bash
# 推送目标核对脚本
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

cp -f "$SRC/scripts/"*.sh "$WORK/scripts/"
cd $WORK
git config http.sslVerify false
git add -A scripts
git commit -q -m "chore: 新增 goal_check.sh（逐条核对目标完成度）" || echo "(无变更)"
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -2
echo "rc=$?"
git log --oneline -1
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
