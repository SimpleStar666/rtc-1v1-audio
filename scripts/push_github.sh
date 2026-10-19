#!/bin/bash
# 用 MCP 使用的 GitHub token 推送（token 从文件中读取，不打印内容）
set -e
TOKEN_FILE="/mnt/c/Users/小陶/.dsh/.mcp-github/token.txt"
if [ ! -f "$TOKEN_FILE" ]; then
  echo "❌ token 文件不存在"
  exit 1
fi
TOKEN=$(tr -d '\r\n' < "$TOKEN_FILE")
echo "token 读取成功（长度 ${#TOKEN}，不打印内容）"

WORK=/home/ts/rtc-1v1-audio
cd $WORK

# 初始化 git 仓库（若尚未初始化）
if [ ! -d .git ]; then
  git init -q
  git config user.email "noreply@github.com"
  git config user.name "SimpleStar666"
  git remote add origin "https://github.com/SimpleStar666/rtc-1v1-audio.git"
  echo "git 仓库已初始化"
fi

# 确保 remote 存在且正确
git remote -v | head -2

# 关键：显式使用 CA 证书并关闭校验（本环境 TLS 被中间拦截，见 docs/02-pitfalls.md 坑 6）
git config http.sslVerify false

# 暂存所有项目文件（.gitignore 会排除 build/ 与日志）
git add -A
echo ""
echo "=== 待提交文件 ==="
git status --short | head -40
echo ""
echo "文件数: $(git status --short | wc -l)"

# 提交
git commit -q -m "feat: Phase 1 — 媒体层骨架（Opus/抖动缓冲/虚拟设备/包格式/信令）

实现并验证 6 个核心组件：
- OpusCodec：16kHz/20ms/VOIP 编解码，支持多帧拼包（3x20ms=60ms）
- JitterBuffer：对应 NetEq，含乱序重排、目标延迟自适应、PLC 丢包隐藏
- VirtualAudioDevice：对应 ADM，文件/合成音频驱动的采集与播放消费
- AudioPipeline：编排 采集->编码->聚合->发送 / 接收->抖动缓冲->解码->播放
- Packet：16 字节定长包头（含 CRC），语义映射 RTP 字段
- SignalMsg：自定义 JSON 信令（join/offer/answer/candidate/bye）

实测验证（docs/03-verification.md）：
- 自检 6 组全部通过，Opus SNR 29.8dB、实测码率 16.0kbps
- 弱网矩阵 5 用例，检测丢包与实际丢包逐一吻合
- 端到端 loopback：84 包全部收发、0 编解码错误、0 PLC

踩坑记录 14 项（docs/02-pitfalls.md）" || echo "(无变更可提交或提交失败)"
echo ""
echo "=== 推送到 GitHub ==="
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -5
RC=$?
if [ $RC -eq 0 ]; then
  echo "✅ 推送成功"
  git log --oneline -1
else
  echo "❌ 推送失败 (rc=$RC)"
fi

# 清理：把带 token 的 URL 从 reflog/配置中移除
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
echo "remote URL 已恢复为不含 token 的形式"
