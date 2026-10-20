#!/usr/bin/env python3
# 补 00-architecture.md 的视频架构 + 更新过时的"接入点"表
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/docs/00-architecture.md'
s = open(P, encoding='utf-8').read()
n = 0

def rep(old, new, label):
    global s, n
    if old not in s:
        print(f'  ✗ {label}')
        return
    s = s.replace(old, new, 1)
    print(f'  ✓ {label}')
    n += 1

# ---- 1) 把过时的"后续接入点"整段替换成"音视频双流架构" ----
old_tail = """## 后续阶段的接入点

| 待实现 | 接入位置 | 说明 |
|---|---|---|
| WebSocket 信令客户端 | `peer.cpp` | 连接 `signal_server`，交换 SDP/候选 |
| libnice ICE / STUN | `SendFn` 出口 + 收包入口 | 替换 loopback 直连 |
| DTLS-SRTP 加密 | 传输层包一层 | libsrtp2 + OpenSSL |
| NACK / FEC | `on_media_packet` 前后 | 丢包检测 → 重传请求 / 冗余恢复 |
| 拥塞控制 | `set_target_bitrate_bps` | 延迟梯度估计 → 码率调节 |"""

new_tail = """## 音视频双流架构

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
| 通话编排 | `app/call_session.*` | ✅ |"""

rep(old_tail, new_tail, '替换过时的接入点表为音视频双流架构')

# ---- 2) 顶部数据流里补视频 ----
rep('# 架构设计\n\n## 整体数据流',
    '# 架构设计\n\n> 本项目是 **1v1 实时音视频通话系统**：音频（Opus）+ 视频（VP8），\n'
    '> 复用同一条 ICE + DTLS-SRTP 通道，靠 SSRC 分流。\n'
    '> 视频部分的架构见文末「音视频双流架构」。\n\n## 整体数据流（音频）',
    '顶部加定位说明')

open(P, 'w', encoding='utf-8').write(s)
print(f'\n✅ 00-architecture 共 {n} 处，现 {s.count(chr(10))+1} 行')
