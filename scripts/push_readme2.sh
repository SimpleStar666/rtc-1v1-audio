#!/bin/bash
# 推送 README 更新
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

cp "$SRC/README.md" "$WORK/"
cp "$SRC/scripts/verify_remote4.sh" "$WORK/scripts/" 2>/dev/null || true

cd $WORK
git config http.sslVerify false
git add -A README.md scripts
git commit -q -m "docs: README 更新 Phase 3/4 完成状态与实测摘要" || echo "(无变更)"
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -2
echo "rc=$?"
git log --oneline -1
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
