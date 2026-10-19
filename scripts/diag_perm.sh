#!/usr/bin/env bash
# 诊断：为什么 ts 用户写不了 build.log
echo "=== 当前身份 ==="
id
echo ""
echo "=== 工作目录所有者 ==="
ls -ld /home/ts/rtc-1v1-audio
echo ""
echo "=== build 目录所有者（关键）==="
ls -ld /home/ts/rtc-1v1-audio/build 2>/dev/null || echo "  build/ 不存在"
echo ""
echo "=== build 里的文件所有者（前 10 个）==="
ls -la /home/ts/rtc-1v1-audio/build 2>/dev/null | head -10
echo ""
echo "=== build.log 的情况 ==="
ls -la /home/ts/rtc-1v1-audio/build/*.log 2>/dev/null || echo "  没有 .log 文件"
echo ""
echo "=== ts 用户能否写入 build/ ==="
sudo -u ts test -w /home/ts/rtc-1v1-audio/build && echo "  ✓ ts 可写 build/" || echo "  ✗ ts 不可写 build/  ← 这就是 Permission denied 的原因"
echo ""
echo "=== ts 用户能否往 build/ 里创建文件 ==="
sudo -u ts touch /home/ts/rtc-1v1-audio/build/.write_test 2>&1 && { echo "  ✓ 可创建"; sudo -u ts rm -f /home/ts/rtc-1v1-audio/build/.write_test; } || echo "  ✗ 不可创建"
echo ""
echo "=== 仓库根目录是否 ts 可写 ==="
sudo -u ts test -w /home/ts/rtc-1v1-audio && echo "  ✓ 可写" || echo "  ✗ 不可写"
echo ""
echo "=== 统计 root 拥有的文件数 ==="
find /home/ts/rtc-1v1-audio -not -user ts 2>/dev/null | wc -l | sed 's/^/  非 ts 拥有的条目: /'
