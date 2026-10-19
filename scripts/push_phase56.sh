#!/bin/bash
# 推送 Phase 5 + Phase 6（弱网对抗 + 端到端集成）
set -e
SRC="/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio"
WORK=/home/ts/rtc-1v1-audio
TOKEN=$(tr -d '\r\n' < /mnt/c/Users/小陶/.dsh/.mcp-github/token.txt)

for d in src tests scripts docs; do rm -rf "$WORK/$d"; cp -r "$SRC/$d" "$WORK/"; done
cp "$SRC/CMakeLists.txt" "$SRC/README.md" "$SRC/.gitignore" "$WORK/"

cd $WORK
git config http.sslVerify false
git add -A
echo "变更："
git status --short | head -30

git commit -q -m "feat(phase5+6): 弱网对抗 + 端到端集成，简历个人项目四个部分全部落地

Phase 5 新增 src/net/loss_protection.{h,cpp}：
- NackRequester   检测丢包/生成重传请求/超时放弃
- FecEncoder/Decoder  XOR 冗余(k=4,m=1)，丢1包可恢复
- CongestionController  延迟梯度 + AIMD，丢包兜底
实测：恢复率 2%->100%、5%->89.3%、10%->77.8%、20%->51.7%
      码率自适应 升/降/恢复全部通过

Phase 6 新增 tests/e2e_test.cpp：音频经 ICE + DTLS-SRTP 完整链路
  采集->Opus->打包->FEC->SRTP加密->ICE(UDP)->解密->抖动缓冲->解码->播放
实测：SRTP 加密/解密 62/62，解密失败 0
      抖动缓冲 150 接收 / 146 播放，PLC 0
      丢包矩阵：检测丢包=实际丢包(0/3/4/6)，PLC 随丢包上升(0/9/12/18)

包格式重构（关键修复）：
  Packet 头由 16 字节改为 20 字节，前 12 字节完全兼容 RTP 固定头
  （版本位 0x80 / seq / timestamp / SSRC），自定义字段置于其后
  原因：SRTP 按标准 RTP 头解析，原格式首字节是 session_id=0x00，
        被判定为非 RTP，导致媒体包完全不进入 SRTP 解密路径

本阶段踩坑 3 项（累计 30 项，见 docs/02-pitfalls.md）：
- 坑 28：拥塞控制节流基准用真实时钟 -> 仿真中码率永不更新
         （'部分正常工作'比完全失效更难查；修复：节流基准惰性初始化）
- 坑 29：集成接线顺序错 -> DTLS 握手必然超时
         （回调注册在握手之后 + 只接单侧 + recv 忙等，三个错误叠加）
- 坑 30：自定义包头不兼容 RTP -> SRTP 完全不处理
         （连带发现 FEC 序号空间冲突致丢包统计虚高 10032，
           以及只看最后一帧导致误判'解码出静音'）

全量回归：selftest / weaknet / loopback / ice / dtls_srtp / loss / e2e 全部通过" || echo "(无变更)"

echo ""
echo "=== 推送 ==="
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "rc=$?"
git log --oneline -3
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
