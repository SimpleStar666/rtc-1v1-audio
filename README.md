# rtc-1v1-audio

1v1 实时**音视频**通话实现 —— 从开源组件自建，逐层理解 WebRTC 音视频栈。

## 这个项目是什么

一个**可运行的** 1v1 音视频通话系统：信令、NAT 穿透、加密、音频编解码（Opus）、**视频编解码（VP8）**、抖动缓冲、分片重组、**关键帧请求（PLI）**、弱网对抗。
与"调 libwebrtc API"不同，本项目**用开源组件从零搭建每一层**，目的是把 RTC 的机制真正搞懂。

| 层 | 选用组件 | 对应 WebRTC 中的 | 状态 |
|---|---|---|---|
| 音频编解码 | **Opus** (libopus) | Opus 编解码器 | ✅ SNR 29.8dB |
| **视频编解码** | **VP8** (libvpx) | VP8 编解码器（RFC 7742 必选） | ✅ **PSNR 52dB** |
| **视频分片/重组** | 自研 | RTP 分片 + FU-A 思路 | ✅ 缺片整帧丢弃 |
| **关键帧请求** | 自研 | **RTCP PLI / FIR** | ✅ 丢帧后 1 RTT 自愈 |
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

## 快速开始

**只想看效果？一条命令：**

```bash
./scripts/run.sh
```

菜单里选 **"完整演示"**（编译 → 测试 → 自环听声音 → 双端通话）。

或者直接用子命令：

```bash
./scripts/run.sh build      # 编译
./scripts/run.sh test       # 全部测试
./scripts/run.sh loopback   # 本地自环，会播放音频给你听
./scripts/run.sh call       # ★ 双端音视频通话（互传音频+视频，可听可看）
./scripts/run.sh weaknet    # 弱网矩阵（丢包 0/2/5/8/15% 对照）
./scripts/run.sh help       # 全部命令说明
```

### 真正的通话长什么样

`./scripts/run.sh call` 会启动**两个独立进程**，完整跑一遍
信令 → ICE → DTLS-SRTP → 双向音频：

```
    [ 1s] 音频 16/16 包 16kbps | 视频 16/16 帧 258kbps 解码16 丢弃0 PLI发0收0
    [ 4s] 音频 67/66 包 16kbps | 视频 61/61 帧 430kbps 解码61 丢弃0 PLI发0收0
    [ 8s] 音频 133/133 包 16kbps | 视频 122/122 帧 423kbps 解码122 丢弃0 PLI发0收0

A 端: 发送 136 包 / 接收 135 包 / SRTP 解密 527 包（失败 0）
B 端: 发送 135 包 / 接收 134 包 / SRTP 解密 257 包（失败 0）
视频: A 端解码 122 帧 / B 端解码 123 帧，丢弃 0
两端退出码 = 0
```

并且**自动验证"听到的是对方"**（而不是各自自环）：
A 采集扫频、B 采集 440Hz 单音，最后给出的录音里

| 端点 | 采集源 | 录到的主导频率 | 说明 |
|---|---|---|---|
| A | 扫频 200→3000Hz | **440 Hz** | A 录到的是 B 的声音 |
| B | 440 Hz 单音 | **~2400 Hz** | B 录到的是 A 的扫频 |

```bash
# 也可以手工跑（三个终端）
./build/signal_server --port 8899
./build/peer --call --signal ws://127.0.0.1:8899 --room demo --name A --seconds 8 \
  --audio-file /tmp/rtc_demo/sweep.pcm --out /tmp/call_A.pcm
./build/peer --call --signal ws://127.0.0.1:8899 --room demo --name B --seconds 8 \
  --audio-file /tmp/rtc_demo/sine440.pcm --out /tmp/call_B.pcm

# 试听（转 WAV 后播放）
python3 /tmp/rtc_demo/pcm2wav.py /tmp/call_A.pcm /tmp/call_A.wav 16000
paplay /tmp/call_A.wav
```

> **完整操作手册（怎么跑、怎么玩、怎么演示）见
> [docs/04-how-to-run.md](docs/04-how-to-run.md)** —— 含环境准备、
> 逐命令详解、各组件单独运行、听声音、面试演示脚本、常见问题、命令速查表。
>
> **想知道每段代码怎么实现的，见
> [docs/05-code-tour.md](docs/05-code-tour.md)** —— 10 个模块逐个讲
> "要实现什么 / 代码在哪（文件:行号）/ 关键算法 / 设计取舍 / 对应踩坑"。

## 文档导航

| 文档 | 内容 |
|---|---|
| [**docs/04-how-to-run.md**](docs/04-how-to-run.md) | **操作手册：怎么跑、怎么玩、怎么演示** |
| [**docs/05-code-tour.md**](docs/05-code-tour.md) | **代码导读：每个功能怎么实现、代码在哪、为什么这么写** |
| [docs/00-architecture.md](docs/00-architecture.md) | 整体架构、数据流、包格式与 RTP 的映射 |
| [docs/01-progress-log.md](docs/01-progress-log.md) | 开发进度与重点记录（每步做了什么、学到什么） |
| [docs/02-pitfalls.md](docs/02-pitfalls.md) | **踩坑记录（47 项，含真实报错与解法）** |
| [docs/03-verification.md](docs/03-verification.md) | **验证数据与实测结果** |

## 构建

```bash
sudo apt-get install -y libopus-dev libsrtp2-dev libnice-dev \
  libwebsockets-dev libglib2.0-dev libssl-dev pkg-config
python3 -m pip install --user "cmake>=3.28"
export PATH=$HOME/.local/bin:$PATH

cd rtc-1v1-audio
./scripts/run.sh build
```

## 运行

```bash
# 一键演示（推荐）
./scripts/run.sh demo

# 或单独运行各组件
./build/selftest                                          # 媒体层 6 组自检
./build/weaknet_test --loss 8 --jitter 30 --delay 60 --seconds 10
./build/peer --loopback --seconds 3                       # 本地自环
./build/ice_test --seconds 3                              # ICE 穿透 + Trickle ICE
./build/ice_test --stun stun.l.google.com --stun-port 19302 --seconds 2
./build/dtls_srtp_test                                    # DTLS-SRTP（21 项）
./build/loss_protection_test                              # NACK/FEC/拥塞控制
./build/metrics_test                                      # 指标统计
./build/e2e_test --seconds 4                              # 完整链路
./build/e2e_test --loss 0.25 --seconds 4                  # 弱网下的完整链路

# 信令配对（三个终端）
./build/signal_server --port 8899
./build/peer --signal ws://127.0.0.1:8899 --room demo --name A --peer-test
./build/peer --signal ws://127.0.0.1:8899 --room demo --name B --peer-test
```

### 听到声音

```bash
./scripts/run.sh loopback          # 自动播放解码后的音频
# 或手工：
python3 /tmp/rtc_demo/pcm2wav.py /tmp/peer_out.pcm /tmp/peer_out.wav 16000
paplay /tmp/peer_out.wav
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

VP8 视频（320x240 @15fps，400kbps）：
  编码 30 帧全部解码成功，平均 PSNR 52.02 dB
  关键帧 7127 字节 / P 帧 2121 字节（关键帧是 P 帧的 3.4 倍）
  分片重组：138 分片 → 46 帧，丢弃 0
  15% 分片丢失：37 帧成功解码，22 帧整帧丢弃（不花屏）
  丢关键帧 → PLI 请求 1 次 → 恢复后解码 45 帧（自愈成功）
  码率自适应：400→实测 360kbps，1200→532kbps，150→163kbps

端到端音视频通话（双进程）：
  A 端: 编码 123 帧 / 解码 122 帧 / 丢弃 0
  B 端: 编码 122 帧 / 解码 123 帧 / 丢弃 0
  画面交叉验证：A 收到 B 的运动图案（5397 色），B 收到 A 的彩条（8 色）
  → 两端真的在互传视频，不是自环

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
| ③+ | **VP8 视频编解码 + 分片重组 + 关键帧请求** | PSNR 52dB；138 分片→46 帧；PLI 自愈 |
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
  media/     Opus/VP8 编解码、抖动缓冲（NetEq）、音视频虚拟设备、音视频管线
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
