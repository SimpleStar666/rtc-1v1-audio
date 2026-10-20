# 架构设计

> 本项目是 **1v1 实时音视频通话系统**：音频（Opus）+ 视频（VP8），
> 复用同一条 ICE + DTLS-SRTP 通道，靠 SSRC 分流。
> 视频部分的架构见文末「音视频双流架构」。

## 整体数据流（音频）

```
┌──────────── 发送方向 (Tx) ────────────┐
│                                        │
│  VirtualAudioDevice                    │
│    (采集线程, 20ms/帧)                  │
│         │                              │
│         ▼                              │
│  OpusCodec::encode()                   │
│    (16kHz / 20ms / VOIP)                │
│         │                              │
│         ▼                              │
│  AudioPipeline 聚合 3 帧 → 60ms 包      │
│         │                              │
│         ▼                              │
│  Packet::serialize()  (16 字节包头+CRC) │
│         │                              │
│         ▼                              │
│  SendFn  ──► 传输层（后续阶段：          │
│               libnice / DTLS-SRTP）      │
└────────────────────────────────────────┘

┌──────────── 接收方向 (Rx) ────────────┐
│                                        │
│  传输层 ──► Packet::parse()             │
│              (CRC 校验)                 │
│         │                              │
│         ▼                              │
│  AudioPipeline::on_media_packet()      │
│    ├─ 打断过滤 (trans_id)                │
│    └─ 拆包 → N 个 Opus 帧                │
│         │                              │
│         ▼                              │
│  JitterBuffer::push()                  │
│    (乱序重排 + 目标延迟自适应)            │
│         │                              │
│         ▼                              │
│  JitterBuffer::pop()  ← 播放时钟驱动     │
│         │                              │
│         ▼                              │
│  OpusCodec::decode()                   │
│    (丢包时 → PLC)                       │
│         │                              │
│         ▼                              │
│  VirtualAudioDevice::play()            │
│    (消费线程模拟 DMA)                    │
└────────────────────────────────────────┘
```

## 分层与 WebRTC 的对应关系

| 本项目模块 | 文件 | 对应的 WebRTC 组件 | 说明 |
|---|---|---|---|
| `VirtualAudioDevice` | `media/virtual_device.*` | **ADM** (Audio Device Module) | 屏蔽"音频从哪来、到哪去" |
| （未实现） | — | **APM** (Audio Processing) | 3A 前处理，独立课题 |
| `OpusCodec` | `media/opus_codec.*` | Opus 编解码器 | 同一编解码器 |
| `JitterBuffer` | `media/jitter_buffer.*` | **NetEq** | 抖动吸收 + 自适应延迟 + PLC |
| `AudioPipeline` | `media/audio_pipeline.*` | 引擎的媒体主干 | 编排各层 |
| `Packet` | `protocol/packet.*` | **RTP** 载荷格式 | 序号/时间戳/会话标识 |
| `SignalMsg` | `protocol/signaling_msg.*` | JES P / SDP 协商 | WebRTC 不规定信令，自定义 |
| `signal_server` | `signal/signal_server.cpp` | 信令服务器 | 只路由，不碰媒体 |

## 本项目的包格式 vs RTP 字段

本项目的定长 16 字节包头，与 RTP 字段的语义映射：

| 本项目的字段 | 字节 | RTP 中的对应 | 作用 |
|---|---|---|---|
| `session_id` | 2 | SSRC | 区分不同会话/流 |
| `seq` | 2 | Sequence Number | 丢包检测、重排序（可回绕）|
| `timestamp` | 4 | Timestamp | 播放时钟（抖动计算）|
| `phase` | 1 | Payload Type | 载荷语义（媒体/控制）|
| `flags` | 1 | RTP 扩展头标志 | 不可打断 / 分片末 / FEC / 重传 |
| `trans_id` | 2 | （RTP 无直接对应）| **会话语义**：打断后过滤在途数据 |
| `payload_len` | 2 | （RTP 靠 UDP 长度）| 载荷边界 |
| `header_crc` | 2 | （RTP 靠 UDP 校验和）| 包头完整性 |

**设计要点**：`trans_id` 是 RTP 没有的概念。它解决的是应用层问题——
"打断信号到达时，管道里还有十几包旧数据在途，必须逐包过滤掉，否则会串音"。

## 关键设计决策

### 1. 为什么自建而不调 libwebrtc

见 [README](../README.md)。核心是环境约束（源码不可达 + 内存不足）与学习目标（理解而非调用）。

### 2. 抖动缓冲的三个核心子问题

```
问题 1：乱序到达怎么办？
  → 按 seq 排序入队，只有 `seq < play_seq_`（已播过）才丢弃

问题 2：目标延迟定多少？
  → 由"最坏抖动"决定：max(平滑抖动×2, 近期最大偏差×1.5) + 帧长
    上调要快（防断音），下调要慢（防抖动回来时不够）

问题 3：包没到怎么办？
  → 等一个容忍窗口（目标延迟 + 帧长）后仍没到，走 PLC
    用 Opus 内置 PLC 生成近似帧，让用户听不出丢包
```

### 3. 时间可注入原则

`JitterBuffer::pop(payload, now)` 把时间做成参数，而不是内部读 `now_ms()`。

**收益**：弱网仿真可以"快进"——跑 10 秒网络仿真只花几十毫秒真实时间。
**代价**：调用方需要显式传时间（生产路径传 `-1` 用真实时钟）。

### 4. 锁的边界：`bitrate_kbps_locked()`

`std::mutex` 非递归，公共方法间不能互相调用。
因此把"需要锁保护的实现"与"负责加锁的接口"分成两套函数：

```cpp
int bitrate_kbps_locked() const;   // 调用方必须已持锁
int bitrate_kbps() const {         // 公共接口，自己加锁
    std::lock_guard<std::mutex> lk(stat_mutex_);
    return bitrate_kbps_locked();
}
AudioPipeline::Report report() const {
    std::lock_guard<std::mutex> lk(stat_mutex_);
    r.cur_bitrate_kbps = bitrate_kbps_locked();   // ✓ 不重复加锁
}
```

## 线程模型

| 线程 | 职责 | 节拍来源 |
|---|---|---|
| 采集线程（设备内） | 产出 20ms PCM | `sleep_until(next_tick)` 精确节拍 |
| 播放消费线程（设备内） | 模拟 DMA 取走数据 | 10ms chunk |
| 发送线程（管线） | 攒帧 → 聚合 → 序列化 → 发送 | 条件变量等待"攒够 N 帧" |
| 播放线程（管线） | 从抖动缓冲取帧 → 解码 → 播放 | 20ms 帧节拍 |

**设计原则**：每个线程的节拍都由**硬件时钟语义**决定（采样率/帧长），
而不是"轮询间隔"——后者会累积漂移，在音频里表现为可听见的断音。

## 音视频双流架构

本项目从"纯音频"演进为**音视频双流**，两条流复用**同一条** ICE + DTLS-SRTP 通道，
靠 SSRC（`session_id`）区分。

```
                        ┌─────────────────────────────────┐
                        │   ICE 通道（一个候选对，UDP）    │
                        │   + DTLS-SRTP（一套密钥）        │
                        └───────────┬─────────────────────┘
                                    │
              ┌─────────────────────┴─────────────────────┐
              ▼                                            ▼
    ┌──────────────────┐                        ┌──────────────────┐
    │ 音频流 SSRC=1    │                        │ 视频流 SSRC=2    │
    │ Opus 16kbps      │                        │ VP8 400kbps      │
    │ 3 帧聚合 60ms    │                        │ 一帧 N 个分片    │
    │ 每包一帧         │                        │ 1100B/片         │
    └──────────────────┘                        └──────────────────┘
              │                                            │
    丢包 → PLC 补偿                            丢包 → 请求关键帧(PLI)
    （本地可恢复）                              （必须对端配合，1 RTT）
```

### 为什么复用一条通道而不是两条

| 方案 | 优点 | 缺点 |
|---|---|---|
| **复用一条（本项目）** | 只需一套 ICE/DTLS，NAT 穿透成本低 | SRTP 会话需加锁（多线程发送） |
| 音视频各一条 | 互不影响 | 两套 ICE 打洞、两次 DTLS 握手，成本翻倍 |

WebRTC 的做法是 **BUNDLE**（RFC 8843）：多条媒体流复用同一个传输通道。
本项目实现的就是 BUNDLE 的简化版。

### 关键：SSRC 分流

接收端靠 `PacketHeader.session_id` 分流（它被映射到 RTP 的 SSRC 字段）：

```cpp
// call_session.cpp 接收回调
if (ph.session_id == 1) {
    pipe.on_media_packet(ph, payload);        // 音频
} else if (ph.session_id == 2) {
    vpipe.on_video_fragment(ph, payload);     // 视频
}
```

**为什么必须分流**：音频和视频是两套独立的序号空间。
混在一起会让丢包检测、抖动缓冲、关键帧判断全部失真（见坑 30/34）。

### 音视频的四个本质差异

| 维度 | 音频（Opus） | 视频（VP8） |
|---|---|---|
| 单帧大小 | 40~120 字节 | 2000~8000 字节 |
| 一帧一包？ | 可以 | **不行，必须分片**（MTU ~1200） |
| 丢包后果 | PLC 补一帧，几乎无感 | **整帧作废**（缺一片就拼不出） |
| 丢关键帧 | 无特殊影响 | **后续 P 帧全部不可解码** |
| 恢复手段 | 本地 PLC | **PLI 请求关键帧（1 RTT）** |
| 码率量级 | 16 kbps | 400 kbps |

### 视频模块结构

```
src/media/vp8_codec.*       VP8 编解码（libvpx），YUV420，关键帧控制
src/media/video_device.*    虚拟摄像头（测试图案）+ 渲染器（PPM 落盘）
src/media/video_pipeline.*  采集→编码→分片→发送 / 接收→重组→解码→播放
                            + 关键帧请求（PLI）限流
```

### SRTP 并发的代价（复用一条通道的直接后果）

复用一条通道意味着 **智能指针 `DtlsSrtpSession` 会被多个线程同时使用**：

| 线程 | 调用 |
|---|---|
| 音频发送线程 | `protect()` |
| **视频发送线程** | `protect()` ← 新增，与上面并发 |
| libnice glib 网络线程 | `unprotect()` |

`libsrtp` 的 `srtp_t` 会话**不是线程安全的** → 必须加锁。
这是纯音频阶段不会暴露、加视频后才出现的真实 bug（坑 45）。

### 完整模块清单（对应代码）

| 层 | 文件 | 状态 |
|---|---|---|
| 音频编解码 | `media/opus_codec.*` | ✅ |
| **视频编解码** | `media/vp8_codec.*` | ✅ |
| 抖动缓冲（NetEq） | `media/jitter_buffer.*` | ✅ |
| **视频分片/重组/PLI** | `media/video_pipeline.*` | ✅ |
| 音频设备（ADM） | `media/virtual_device.*` | ✅ |
| **视频设备（摄像头/渲染）** | `media/video_device.*` | ✅ |
| 包格式（RTP 兼容） | `protocol/packet.*` | ✅ |
| 信令 | `signal/signal_server.cpp` + `signaling_client.*` | ✅ |
| ICE / STUN / Trickle | `ice/ice_transport.*` | ✅ |
| DTLS-SRTP | `crypto/dtls_srtp.*` | ✅ |
| NACK / FEC / GCC | `net/loss_protection.*` | ✅ |
| 指标统计 | `metrics/metrics.*` | ✅ |
| 通话编排 | `app/call_session.*` | ✅ |
