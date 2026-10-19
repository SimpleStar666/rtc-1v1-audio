# rtc-1v1-audio

1v1 实时语音通话实现 —— 从开源组件自建，逐层理解 WebRTC 音频栈。

## 这个项目是什么

一个**可运行的** 1v1 语音通话系统：信令、NAT 穿透、加密、编解码、抖动缓冲、弱网对抗。
与"调 libwebrtc API"不同，本项目**用开源组件从零搭建每一层**，目的是把 RTC 的机制真正搞懂。

| 层 | 选用组件 | 对应 WebRTC 中的 | 状态 |
|---|---|---|---|
| 音频编解码 | **Opus** (libopus) | Opus 编解码器 | ✅ 已实现并验证 |
| 抖动缓冲 / 播放调度 | 自研 | **NetEq** | ✅ 已实现并验证 |
| 音频设备 | 自研虚拟设备 | **ADM** | ✅ 已实现并验证 |
| 信令 | **libwebsockets** | WebSocket 信令（标准不规定） | ✅ 两端配对已跑通 |
| NAT 穿透 | **libnice** | ICE / STUN (RFC 8445/5389) | ⬜ Phase 3 |
| 媒体加密 | **OpenSSL DTLS** + **libsrtp2** | DTLS-SRTP (RFC 5764) | ⬜ Phase 4 |
| 抗丢包 | 自研 | NACK / ULPFEC | ✅ 抖动缓冲侧已实现 |
| 拥塞控制 | 自研 | GCC | ⬜ Phase 5 |

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

弱网矩阵（5 用例）：检测丢包与实际丢包逐一吻合
  0%  丢包 → 检测 0        2%  → 检测 6  （实际 6）
  8%  丢包 → 检测 38       15% → 检测 67 （实际 67）
  ±80ms 抖动 0 丢包 → 检测 0，接受率 99.8%

端到端 loopback：84 包全部收发，0 编解码错误，0 PLC，码率 16.0kbps

信令配对（两端 + 服务器）：
  双向消息零丢失（joined / peer_ready / offer / answer / candidate）
  服务器转发明细完整，无内存错误
```

## 进度

- [x] **Phase 0**：环境侦察与依赖验证
- [x] **Phase 1**：媒体层（Opus / 抖动缓冲 / 虚拟设备 / 包格式）
- [x] **Phase 2**：WebSocket 信令（客户端 + 服务器，两端配对已跑通）
- [ ] Phase 3：libnice ICE / STUN 穿透
- [ ] Phase 4：DTLS-SRTP 加密
- [ ] Phase 5：弱网对抗（NACK / FEC / 拥塞控制）
- [ ] Phase 6：端到端联调 + 指标统计

## 目录结构

```
src/
  common/    日志、字节序、CRC32 等工具
  media/     Opus 封装、抖动缓冲（NetEq）、虚拟设备（ADM）、音频管线
  protocol/  包格式（RTP 映射）、信令消息（JSON）
  signal/    WebSocket 信令服务器 + 客户端
  app/       通话端点（--loopback / --peer-test 两种模式）
docs/        架构 / 进度 / 踩坑（20 项）/ 验证
tests/       自检与弱网测试
scripts/     构建与诊断脚本
```
