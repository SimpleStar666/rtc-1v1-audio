#!/usr/bin/env bash
# 推送权限修复（用 -F 提交信息文件，避免 shell 解析特殊字符）
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"
chown -R ts:ts "$WORK"
chmod +x "$WORK/scripts/"*.sh

cd $WORK
git config http.sslVerify false
git add -A
echo "=== 待提交文件（前 15）==="
git status --short | head -15

git commit -q -F scripts/commit_msg_permfix.txt || echo "  (无变更可提交)"

echo ""
echo "=== 推送 ==="
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "rc=$?"
git log --oneline -3
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
