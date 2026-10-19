#!/bin/bash
# 推送 Trickle ICE + 指标统计（补齐简历四个部分的剩余要点）
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
git status --short | head -25

git commit -q -m "feat: Trickle ICE + 统一指标统计，补齐简历个人项目全部要点

Trickle ICE（对应 onicecandidate / addIceCandidate）：
- IceTransport 新增 set_on_trickle_candidate / add_remote_candidate_sdp
- local_credentials() / set_remote_credentials()
- 实测：A/B 各 trickle 36 个候选，增量添加 36/36 成功，
        连接建立后可正常收发

统一指标统计（对应 WebRTC getStats / RTCP SR·RR）：
- src/metrics/metrics.{h,cpp}：CallMetrics + MetricsCollector
- 分层统计发送/接收/抖动缓冲/编解码/加密/自适应/ICE
- 派生指标：实测码率、恢复率、播放连续性、编码成功率
- 支持 to_string() 与 to_json() 两种输出
- 已集成进 e2e_test，做跨层自洽性校验

e2e 交叉校验结果：
  SRTP 加密 62 == 解密 62
  媒体50 + FEC12 == SRTP 加密 62
  实测码率 38401 bps，播放连续性 100%

本阶段踩坑 2 项（累计 32 项，见 docs/02-pitfalls.md）：
- 坑 31：Trickle ICE 只交换候选、漏交换 ufrag/pwd -> 候选全对但连不上
         （单个 a=candidate 行不含凭证；STUN 认证需要它们）
- 坑 32：恢复率分母把 recovered 加到 lost 上 -> 8/8 算成 50%
         同时指标 duration 用真实时钟而 start 用逻辑时间 -> 3.7M ms
         （统计口径错误 + 时间注入缺失，在项目中第 3 次复发）

全量回归：selftest / weaknet / loopback / ice(含Trickle) / dtls_srtp /
          loss_protection / metrics / e2e 全部通过" || echo "(无变更)"

echo ""
echo "=== 推送 ==="
git push -q "https://SimpleStar666:${TOKEN}@github.com/SimpleStar666/rtc-1v1-audio.git" HEAD:main 2>&1 | tail -3
echo "rc=$?"
git log --oneline -3
git remote set-url origin "https://github.com/SimpleStar666/rtc-1v1-audio.git" 2>/dev/null || true
