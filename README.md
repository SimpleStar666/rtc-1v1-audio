# rtc-1v1-audio

1v1 实时语音通话实现 —— 从开源组件自建，逐层理解 WebRTC 音频栈。

## 这个项目是什么

一个**可运行的** 1v1 语音通话系统：信令、NAT 穿透、加密、编解码、抖动缓冲、弱网对抗。
与"调 libwebrtc API"不同，本项目**用开源组件从零搭建每一层**，目的是把 RTC 的机制真正搞懂。

| 层 | 选用组件 | 对应 WebRTC 中的 | 状态 |
|---|---|---|---|
| 音频编解码 | **Opus** (libopus) | Opus 编解码器 | ✅ SNR 29.8dB |
| 抖动缓冲 / 播放调度 | 自研 | **NetEq** | ✅ 丢包检测精确吻合 |
| 音频设备 | 自研虚拟设备 | **ADM** | ✅ 节拍无漂移 |
| 信令 | **libwebsockets** | WebSocket 信令（标准不规定） | ✅ 两端配对零丢失 |
| NAT 穿透 | **libnice** | ICE / STUN / **Trickle ICE** | ✅ srflx×3 + trickle 建链 |
| 媒体加密 | **OpenSSL DTLS** + **libsrtp2** | DTLS-SRTP (RFC 5764) | ✅ 21 项通过 |
| 抗丢包 | 自研 | NACK / ULPFEC | ✅ 恢复率 100%/89%/78%/52% |
| 拥塞控制 | 自研 | GCC | ✅ 升/降/恢复全通过 |
| 指标统计 | 自研 | `getStats()` / RTCP SR·RR | ✅ 跨层自洽校验 |

**端到端链路**（Phase 6 验收）：

```
采集 → Opus 编码 → 打包(RTP 兼容头) → FEC 冗余 → SRTP 加密
  → ICE 通道(UDP) ──网络──▶ ICE 接收 → SRTP 解密 → NACK/FEC 恢复
  → 抖动缓冲(NetEq) → Opus 解码 → 播放
```

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

**已知环境限制**（如实记录，见 [docs/03-verification.md](docs/03-verification.md)）：
- WSL 无 `/dev/snd` 声卡设备 → 用**虚拟音频设备**（文件/合成 PCM）替代真实麦克风与扬声器

## 文档导航

| 文档 | 内容 |
|---|---|
| [docs/00-architecture.md](docs/00-architecture.md) | 整体架构、数据流、包格式与 RTP 的映射 |
| [docs/01-progress-log.md](docs/01-progress-log.md) | **开发进度与重点记录**（每步做了什么、学到什么） |
| [docs/02-pitfalls.md](docs/02-pitfalls.md) | **踩坑记录（20 项，含真实报错与解法）** |
| [docs/03-verification.md](docs/03-verification.md) | **验证数据与实测结果** |

## 构建

```bash
sudo apt-get install -y libopus-dev libsrtp2-dev libnice-dev \
  libwebsockets-dev libglib2.0-dev libssl-dev pkg-config
python3 -m pip install --user "cmake>=3.28"
export PATH=$HOME/.local/bin:$PATH

cd rtc-1v1-audio
./scripts/build.sh
```

## 运行

```bash
# 自检（6 组组件测试）
./build/selftest

# 弱网测试（可指定丢包/抖动/延迟）
./build/weaknet_test --loss 8 --jitter 30 --delay 60 --seconds 10

# 本地媒体链路自环验证（不经网络）
./build/peer --loopback --seconds 5

# 信令配对测试（需先启动服务器）
./build/signal_server --port 8888 &
./build/peer --signal ws://127.0.0.1:8888 --room demo --name A --peer-test --seconds 3 &
./build/peer --signal ws://127.0.0.1:8888 --room demo --name B --peer-test --seconds 3
```

## 实测结果摘要

```
自检 6 组：全部通过
  Opus SNR 29.8 dB（16kbps），实测码率 16.0 kbps，算法延迟 26.9ms
  采集节拍 25 帧/500ms（精确无漂移）

弱网矩阵：检测丢包与实际丢包逐一吻合
  0%  丢包 → 检测 0        2%  → 检测 6  （实际 6）
  8%  丢包 → 检测 38       15% → 检测 67 （实际 67）

ICE 穿透：
  host 候选：各 6 个候选，候选对选通，双向收发确认内容来自对端
  STUN：host×6 srflx×3 relay×0（成功获取公网映射候选）

DTLS-SRTP（21 项全部通过）：
  套件 ECDHE-RSA-AES256-GCM-SHA384，双向指纹互验
  RFC 5764 密钥四方一致，加密 35→45 字节（+10 认证标签）
  篡改密文被拒 / 未加密明文包被拒

弱网对抗恢复率（NACK + FEC）：
  2% 丢包 → 100.0%     5% 丢包 → 89.3%
  10% 丢包 → 77.8%     20% 丢包 → 51.7%
拥塞控制：良好网络 16000→27417，排队 →13136，高丢包 →7099，恢复 →45774 bps

端到端集成（音频经 ICE + DTLS-SRTP）：
  无丢包：SRTP 加密/解密 62/62，解密失败 0，PLC 0，解码最大幅度 8218
  丢包矩阵：模拟丢弃 3/4/6 → 检测丢包 3/4/6（精确吻合）
            PLC 补偿 9/12/18（随丢包率上升）

Trickle ICE（候选增量发送）：
  A/B 各 trickle 36 个候选，增量添加 36/36 成功，建链后双向收发正常

统一指标统计（对应 getStats）：
  [发送侧] 采集 151 / 编码 151(100%) / RTP 50 / FEC 12 / 38401 bps
  [接收侧] 接收 62 / 检测丢包 0 / 恢复率 100%
  [抖动缓冲] 接收 150 / 播放 146 / PLC 0 / 连续性 100% / 目标 80ms
  [加密] SRTP 加密 62 / 解密 62 (失败 0)
  跨层自洽：62 == 62，50+12 == 62，接收 == 发送
```

## 简历个人项目描述对照

| # | 描述 | 实测证据 |
|---|---|---|
| ① | WebSocket 信令（offer/answer/ICE 转发、**Trickle ICE**） | 两端配对零丢失；trickle 36/36 建链 |
| ② | STUN/ICE 穿透与 **DTLS-SRTP 加密** | srflx×3；双向指纹互验；62/62 解密 |
| ③ | **Opus 编解码 + 抖动缓冲** | SNR 29.8dB / 16kbps；丢包检测精确吻合 |
| ④ | 弱网对抗（**NACK/FEC/码率自适应**）与**指标统计** | 恢复率 100%/89%/78%/52%；跨层自洽指标 |

## 进度

- [x] **Phase 0**：环境侦察与依赖验证
- [x] **Phase 1**：媒体层（Opus / 抖动缓冲 / 虚拟设备 / 包格式）
- [x] **Phase 2**：WebSocket 信令（客户端 + 服务器，两端配对跑通）
- [x] **Phase 3**：libnice ICE / STUN 穿透（实测拿到 srflx 候选）
- [x] **Phase 4**：DTLS-SRTP 媒体加密（21 项验证通过）
- [x] **Phase 5**：弱网对抗（NACK / FEC / 拥塞控制）
- [x] **Phase 6**：端到端集成（音频经 ICE + DTLS-SRTP 完整链路）
- [x] **补充**：Trickle ICE + 统一指标统计

## 已知边界（如实声明）

| 项目 | 现状 | 原因 |
|---|---|---|
| 真实声卡采集/播放 | 用虚拟设备替代 | WSL 无 `/dev/snd` |
| 真实 3A（AEC/ANS/AGC） | 未实现 | 无真实麦克风回采信号 |
| 跨公网 NAT 穿透 | 本机验证通过 | 无第二台公网主机 |
| TURN 中继 | 未实现 | 无对应场景需求 |
| 硬件时钟漂移 | 未模拟 | 虚拟设备时钟理想 |

## 目录结构

```
src/
  common/    日志、字节序、CRC32 等工具
  media/     Opus 封装、抖动缓冲（NetEq）、虚拟设备（ADM）、音频管线
  protocol/  RTP 兼容包格式、信令消息（JSON）
  signal/    WebSocket 信令服务器 + 客户端
  ice/       ICE / STUN / Trickle ICE（libnice 封装）
  crypto/    DTLS-SRTP 加密（OpenSSL + libsrtp2）
  net/       弱网对抗（NACK / FEC / 拥塞控制）
  metrics/   统一指标统计（对应 getStats）
  app/       通话端点（--loopback / --peer-test 两种模式）
docs/        架构 / 进度 / 踩坑（32 项）/ 验证
tests/       自检、弱网、ICE、DTLS-SRTP、抗损、指标、端到端
scripts/     构建、诊断与验证脚本
```
