#!/bin/bash
# 推送拥塞控制闭环 + 指标口径修正
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"

cd $WORK
git config http.sslVerify false
git add -A
git status --short | head -20

git commit -q -m "feat: 拥塞控制闭环生效 + 指标口径修正

闭环补齐（决策 -> 执行）：
- OpusCodec 新增 set_bitrate(bps)：运行时调整目标码率
  （用 OPUS_SET_BITRATE_REQUEST，不能写 OPUS_SET_BITRATE(x)）
- e2e_test 把 GCC 输出接到编码器，形成
  测量(延迟梯度+丢包) -> 决策(AIMD) -> 执行(set_bitrate) -> 再测量
- FEC 冗余率按丢包率动态调整（k=8 / k=4 / k=2，即 12.5%/25%/50% 开销）

实测闭环有效性：
  无丢包：16000 -> 48000 bps（编码器实际也是 48000）
  20% 丢包：16000 -> 6000 bps（编码器实际 6000）
  指标里的目标码率 == GCC 输出（闭环已接通）

Opus 运行时码率遵循性单独验证：
  target 16000/32000/48000 -> 实测 16000/32000/48000 bps（精确遵循）

指标口径修正（坑 33）：
- 原 bytes_sent 同时累加'编码载荷'与'网络整包'两种字节
  -> '实测码率 82933 bps' 比目标 48000 高 73%
- 拆分为 payload_bytes / wire_bytes，新增 wire_bitrate_bps() 与 overhead_ratio()
  载荷码率 37772 / 网络码率 46559 bps（开销比 1.23）—— 数值自洽

累计 33 项踩坑。'一变量承载两种语义'已出现 4 次（坑 1/11/32/33），
是最值得主动防御的设计盲区，已在文档中列为系统性盲区。

全量回归：selftest / weaknet / loopback / ice / dtls_srtp / loss /
          metrics / e2e 全部通过（失败 0 项）" || echo "(无变更)"

echo ""
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "rc=$?"
git log --oneline -3
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
