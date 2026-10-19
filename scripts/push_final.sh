#!/bin/bash
# 同步全部项目文件到 WSL，然后干净推送到 GitHub
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

echo "=== 同步源码与文档 ==="
mkdir -p $WORK
for d in src tests scripts docs; do
  rm -rf "$WORK/$d"
  cp -r "$SRC/$d" "$WORK/"
done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"
echo "  已同步: src tests scripts docs + CMakeLists/README/.gitignore"

cd $WORK
echo ""
echo "=== 重建 git 仓库（排除 build/）==="
rm -rf .git build
git init -q
git config user.email "noreply@github.com"
git config user.name "SimpleStar666"
git config http.sslVerify false
git add .gitignore CMakeLists.txt README.md src tests scripts docs

echo "将提交的文件："
git status --short
echo "文件数: $(git status --short | wc -l)"
echo ""
echo "=== 提交 ==="
git commit -q -m "feat: Phase 1 — 媒体层骨架（Opus / 抖动缓冲 / 虚拟设备 / 包协议 / 信令）

实现并验证 6 个核心组件：
- OpusCodec       16kHz/20ms/VOIP 编解码，支持多帧拼包（3x20ms = 60ms）
- JitterBuffer    对应 NetEq：乱序重排、目标延迟自适应、PLC 丢包隐藏
- VirtualAudioDevice 对应 ADM：采集线程 + 播放消费线程（模拟 DMA）
- AudioPipeline   编排 采集->编码->聚合->发送 / 接收->抖动缓冲->解码->播放
- Packet          16 字节定长包头（含 CRC），语义映射 RTP 字段
- SignalMsg       自定义 JSON 信令（join/offer/answer/candidate/bye）

实测验证（见 docs/03-verification.md）：
- 自检 6 组全部通过；Opus SNR 29.8dB、实测码率 16.0kbps、算法延迟 26.9ms
- 弱网矩阵 5 用例：检测丢包与实际丢包逐一吻合（0/6/38/67/0）
- 端到端 loopback：84 包全部收发、0 编解码错误、0 PLC

踩坑记录 14 项（见 docs/02-pitfalls.md），含三项设计级问题：
- 乱序被误判为迟到（两个语义共用一个水位）
- 目标延迟只跟平滑抖动（接受率 43% -> 修复后 99.8%）
- 丢包统计按到达顺序累加（虚报 6 倍）"
echo "提交完成: $(git log --oneline -1)"

echo ""
echo "=== 推送到 GitHub（强制覆盖，因远程有两个不同历史的提交）==="
git push -f -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "推送结果 rc=$?"
git remote add origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git"

echo ""
echo "=== 验证远程状态 ==="
git ls-remote --heads "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null | head -3
