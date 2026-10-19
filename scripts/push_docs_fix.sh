#!/bin/bash
# 推送文档补齐（坑 12/13/14 + 03-verification 全阶段覆盖）
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"

cd $WORK
git config http.sslVerify false
git add -A
git status --short | head -15

git commit -q -m "docs: 补齐缺失的坑 12/13/14，03-verification 扩展到全阶段

发现并修复两个真实文档缺口：

1) 踩坑速览表列出 12/13/14，但正文缺失（只有表格行没有内容）
   补写：
   - 坑 12：std::mutex 非递归导致自死锁
             （report() 已持锁又调 bitrate_kbps()；修复：_locked 后缀约定）
   - 坑 13：虚拟播放设备只写不读导致缓冲爆满刷屏
             （缺少模拟 DMA 的消费线程；修复：独立消费线程 + underrun 统计）
   - 坑 14：条件变量固定超时导致帧率损失
             （wait_for 相对超时误差累积，2% 帧率损失；
               修复：wait_until + 绝对时间点 + 谓词 + 主动唤醒）
   现在坑 1~33 标题与正文全部齐备（2035 -> 2253 行）

2) 03-verification.md 只覆盖到 Phase 1，后续阶段数据散落在进度日志里
   重写为全阶段验证文档（244 -> 532 行）：
   - 总览表（9 个阶段的关键数据与结果）
   - 各阶段独立可复现命令 + 实测输出
   - 端到端指标报告与跨层自洽性校验
   - 编码码率 vs 网络码率的区分（坑 33）
   - 诚实的边界声明表（已验证 vs 环境受限）
   - 完整复现步骤（含最终验收实测输出）

新增脚本：check_docs.sh / check_pitfalls.sh / sync_docs.sh / verify_opus_bitrate.sh" || echo "(无变更)"

echo ""
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "rc=$?"
git log --oneline -3
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
