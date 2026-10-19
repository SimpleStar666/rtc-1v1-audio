#!/bin/bash
# 推送文档更新（Phase 1 完成度总结 + 远程验证记录）
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

cp -r "$SRC/docs" "$WORK/"
cp "$SRC/scripts/verify_clone.sh" "$WORK/scripts/" 2>/dev/null || true
cp "$SRC/README.md" "$WORK/"

cd $WORK
git config http.sslVerify false
git add -A docs README.md scripts
echo "=== 变更 ==="
git status --short

git commit -q -m "docs: Phase 1 完成度总结 + 远程克隆验证记录

- 01-progress-log.md: 补充端到端自环数据与远程验证结论
- README.md: 更新实测结果摘要与进度勾选
- scripts/verify_clone.sh: 新增"克隆-编译-测试"一键验证脚本

远程验证结论：从 GitHub 全新克隆后编译成功，自检 6 组全部通过。" || echo "(无变更)"

echo ""
echo "=== 推送 ==="
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "rc=$?"
git log --oneline -3
