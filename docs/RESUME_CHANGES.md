# 简历修改清单（逐条核对，避免改漏）

> 目标：把"个人项目"从**基于 libwebrtc 的语音工具**改为**自建音视频 RTC**，
> 与真实做出来的代码一致；同时把视频能力补进技能栏。

## 需要改的位置（共 6 处）

| # | 位置 | 现状 | 问题 |
|---|---|---|---|
| **1** | 技能「音视频基础」 | 只有 Opus/抖动缓冲/蓝牙/TCP-UDP | **缺视频编解码**（VP8/H.264）|
| **2** | 技能「WebRTC 与实时通信」 | 只有音频管线（APM/NetEq/ADM） | **缺视频链路**（分片、关键帧请求）|
| **3** | 个人项目·技术栈 | `libwebrtc`、无视频 | 与事实不符（项目是自建），且缺 VP8 |
| **4** | 个人项目·项目描述 | "基于 libwebrtc 实现的…语音通话" | 同上，且是纯音频 |
| **5** | 个人项目·职责 1 | "基于 libwebrtc 的 PeerConnection 接口…" | 同上 |
| **6** | 个人项目·职责（缺视频） | 5 条全音频 | 需新增视频条目 |
| 附 | 自我评价 | "一年实时音频开发经历" | 可提音视频 |

## 关键原则

1. **不能写"基于 libwebrtc"** —— 项目实际是 Opus/libsrtp2/libnice/libwebsockets/OpenSSL 自建。
   面试官问"libwebrtc 里 NetEq 怎么实现的"就答不上来了，反而扣分。
2. **改后每一条都要有仓库里的实测数据支撑**（面试可打开 GitHub 验证）。
3. **不夸大**：无真实摄像头/声卡、未验证跨公网 NAT、未做 TURN —— 这些不在简历里吹。
4. **保持 2 页**：新增内容要精简，不能溢出。

## 改后要点（对应代码/实测）

| 简历写的 | 仓库证据 |
|---|---|
| 自建 WebSocket 信令 + Trickle ICE | `src/signal/` + `ice_transport.cpp` 的 trickle 接口 |
| STUN 打洞拿 srflx | `ice_test` 实测 `host×6 srflx×3` |
| DTLS-SRTP 双向证书指纹校验 | `dtls_srtp.cpp` + 21 项测试 |
| Opus 16kbps / SNR 29.8dB | `selftest` |
| 抖动缓冲自适应 / 丢包统计精确 | `weaknet_test` 检测丢包=实际丢包 |
| NACK/FEC/GCC 码率自适应 | `loss_protection_test` 恢复率 100%/89%/78%/52% |
| **VP8 编解码 PSNR 52dB** | `vp8_test` |
| **视频分片重组（缺片整帧丢弃）** | `video_test` 138 分片→46 帧 |
| **关键帧请求（PLI）自愈** | `video_test` 丢关键帧→PLI→恢复 |
