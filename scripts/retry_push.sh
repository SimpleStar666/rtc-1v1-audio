#!/usr/bin/env bash
# 重试推送（正确检测 push 的退出码，不经过管道）
set -u
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)
cd "$WORK"

git config http.sslVerify false
git config http.postBuffer 524288000

URL="https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git"

for attempt in 1 2 3 4 5; do
  echo "=== 推送尝试 $attempt ==="
  # 不接管道，直接捕获退出码
  if git push "$URL" HEAD:main > /tmp/push_out.txt 2>&1; then
    echo "  [OK] 推送成功"
    sed 's/^/    /' /tmp/push_out.txt | tail -5
    git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
    echo ""
    echo "=== 远端确认 ==="
    git ls-remote "$URL" HEAD 2>/dev/null | sed 's/^/  /'
    git -c http.sslVerify=false log --oneline -1
    exit 0
  else
    echo "  [FAIL] 第 $attempt 次失败："
    tail -3 /tmp/push_out.txt | sed 's/^/    /'
    sleep $((attempt * 3))
  fi
done

echo "  [!] 5 次均失败，改用 HTTPS 分块传输"
git config http.version HTTP/1.1
git push "$URL" HEAD:main > /tmp/push_out.txt 2>&1
echo "  rc=$?"
tail -5 /tmp/push_out.txt | sed 's/^/    /'
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
exit 1
