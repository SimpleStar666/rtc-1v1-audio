# rtc-1v1-audio

1v1 实时语音通话实现 —— 从开源组件自建，逐层理解 WebRTC 音频栈。

## 这个项目是什么

一个**可运行的** 1v1 语音通话系统，包含信令、NAT 穿透、加密、编解码、抖动缓冲和弱网对抗。
与"调 libwebrtc API"不同，本项目**用开源组件从零搭建每一层**，目的是把 RTC 的机制真正搞懂：

| 层 | 选用组件 | 对应 WebRTC 中的 |
|---|---|---|
| 音频编解码 | **Opus** (libopus) | Opus（同一个编解码器） |
| 抖动缓冲 / 播放调度 | 自研 | NetEq |
| 音频设备 | 自研虚拟设备 | ADM (Audio Device Module) |
| NAT 穿透 | **libnice** | ICE / STUN（同一套 RFC 8445/5389） |
| 媒体加密 | **OpenSSL DTLS** + **libsrtp2** | DTLS-SRTP（同一套 RFC 5764） |
| 信令 | **libwebsockets** | WebSocket 信令（WebRTC 标准不规定信令） |
| 抗丢包 | 自研 NACK / FEC | NACK / ULPFEC |
| 拥塞控制 | 自研 | GCC |

## 为什么不用 libwebrtc

不是回避，而是环境与目标双重考虑：

1. **环境约束**：本机 WSL 内存 7.8GB（libwebrtc 链接需 16GB+），且 `webrtc.googlesource.com` 不可达。
2. **目标考虑**：调 libwebrtc 只能学到 API；自建能学到"**为什么这么设计**"——
   比如抖动缓冲的目标延迟该怎么收敛、NACK 和 FEC 什么时候该用哪个、DTLS 握手和 SRTP 密钥怎么衔接。

> 面试价值：当被问"libwebrtc 提供什么 / 你自己写了什么"，本项目能给出**具体到每一层**的答案。

## 环境（实测记录）

```
OS      : Ubuntu 20.04 (WSL2)
Compiler: g++ 9.4.0
CMake   : 4.4.4 (pip 安装，系统自带 3.16 不满足新版依赖)
内存     : 7.8GB RAM + 2GB swap
依赖     : libopus-dev 1.3.1 / libsrtp2-dev 2.3.0 / libnice-dev 0.1.16
          libwebsockets-dev 3.2.1 / libssl-dev 1.1.1f
```

**已知环境限制**（如实记录，见 `docs/03-verification.md`）：
- WSL 无 `/dev/snd` 声卡设备 → 用**虚拟音频设备**（文件/合成 PCM）替代真实麦克风与扬声器
- `git` HTTPS 被 TLS 中间拦截 → 通过 GitHub API 推送（不影响项目本身）

## 文档导航

| 文档 | 内容 |
|---|---|
| [docs/00-architecture.md](docs/00-architecture.md) | 整体架构与数据流 |
| [docs/01-progress-log.md](docs/01-progress-log.md) | **开发进度与重点记录**（每步做了什么、学到什么） |
| [docs/02-pitfalls.md](docs/02-pitfalls.md) | **踩坑记录**（真实报错与解法） |
| [docs/03-verification.md](docs/03-verification.md) | 验证数据与实测结果 |

## 构建

```bash
cd rtc-1v1-audio
./scripts/build.sh
```

## 运行（1v1 通话演示）

```bash
# 终端 1：信令服务器
./build/signal_server --port 8888

# 终端 2 / 3：两端客户端
./build/peer --signal ws://127.0.0.1:8888 --room demo --audio-file assets/speech.pcm
```

## 进度

- [x] Phase 0：环境侦察与依赖验证
- [ ] Phase 1：项目骨架 + 构建系统
- [ ] Phase 2：Opus 编解码封装 + 抖动缓冲
- [ ] Phase 3：协议设计（包格式 / 序列化）
- [ ] Phase 4：WebSocket 信令服务
- [ ] Phase 5：ICE/STUN 穿透
- [ ] Phase 6：DTLS-SRTP 加密
- [ ] Phase 7：弱网对抗（NACK / FEC / 拥塞控制）
- [ ] Phase 8：端到端集成 + 指标统计

