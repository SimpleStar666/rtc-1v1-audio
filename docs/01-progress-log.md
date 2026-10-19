# 开发进度与重点记录

> 本文档记录每一步**做了什么、为什么这么做、得到什么数据**。
> 目的是让项目不只是"能跑"，而是能讲清楚每个设计决策的来源。

---

## Phase 0：环境侦察与依赖验证

### 做了什么

在动手写代码前，先验证环境的真实约束——避免定下无法实现的方案。

### 关键发现

| 项目 | 结果 | 影响 |
|---|---|---|
| googlesource 可达性 | ❌ 不可达 | **无法获取 libwebrtc 源码** |
| WSL 内存 | 7.8GB + 2GB swap | libwebrtc 链接需 16GB+，**不足** |
| MSVC / VS | ❌ 不存在 | 无法在 Windows 侧构建 |
| 声卡设备 | 只有 `/dev/snd/timer` | **无真实麦克风/扬声器** |
| apt | ✅ 完全可用 | 可安装依赖 |
| GitHub HTTPS | TLS 被中间拦截 | git clone 失败，但 `curl -k` 可用 |
| **apt 的 RTC 组件** | ✅ 全部可用 | **决定了技术选型** |

### 决策：改用开源组件自建，而非 libwebrtc

三重约束（源码不可达 + 内存不足 + 无声卡）让 libwebrtc 路线不可行。
但 apt 提供了完整可用的 RTC 组件栈：

```
libopus-dev      1.3.1   ← 音频编解码
libsrtp2-dev     2.3.0   ← 媒体加密
libnice-dev      0.1.16  ← ICE / STUN
libwebsockets-dev 3.2.1  ← 信令通道
libssl-dev       1.1.1f  ← DTLS
```

**五库联合链接验证**（真实编译并运行）：

```
Opus 编码 320 样本 -> 33 字节
Opus 解码 -> 320 样本
SRTP init OK
OpenSSL init OK
libnice agent created OK
✅ ALL FIVE LIBS WORK
```

### 这次侦察的价值

避免了两个可能的巨大浪费：
1. 花数小时尝试拉 libwebrtc 源码（必然失败）
2. 花数小时尝试编译 libdatachannel（tarball 缺 submodule，CMake 版本不足）

**经验**：在受限环境中，"先侦察 15 分钟" 比 "先写 2 小时代码" 划算得多。

---

## Phase 1：项目骨架 + 构建系统 + 媒体层

### 做了什么

1. 建立 CMake 项目结构（分层：common / media / protocol / signal / app）
2. 实现 6 个核心组件并通过自检
3. 建立文档体系（架构 / 进度 / 踩坑 / 验证）

### 交付的组件

| 组件 | 文件 | 对应 WebRTC 中的 |
|---|---|---|
| Opus 编解码封装 | `media/opus_codec.*` | Opus 编解码器 |
| 抖动缓冲 | `media/jitter_buffer.*` | **NetEq** |
| 虚拟音频设备 | `media/virtual_device.*` | **ADM** |
| 音频管线 | `media/audio_pipeline.*` | 采集→编码→发送 / 接收→解码→播放 |
| 包格式 | `protocol/packet.*` | **RTP** 载荷格式 |
| 信令消息 | `protocol/signaling_msg.*` | JSEP / SDP 协商 |

### 关键设计决策

#### 1. 为什么是 16kHz / 20ms / VOIP 模式

```
采样率 16kHz  ：语音能量集中在 4kHz 内，16kHz 足够且省带宽（对比 48kHz 省 2/3）
帧长   20ms   ：延迟与压缩率的经典折中点（帧太短包头开销大，帧太长延迟高）
模式   VOIP   ：针对语音优化（对比 AUDIO 模式面向音乐）
复杂度 0      ：嵌入式/低功耗场景友好，编码更快
VBR   关闭    ：恒定码率便于带宽预算（与工作经历中的 BES 配置一致）
```

#### 2. 多帧拼包（20ms × 3 = 60ms）

**需求来源**：服务器期望 60ms 帧，但本地 VAD/采集以 20ms 为粒度。

**实现**：`OpusCodec::pack_frames()` 把 N 个独立 Opus 帧封进一个载荷：
```
[u8 frame_count][u16 len_0][payload_0][u16 len_1][payload_1]...
```

**权衡**：聚合增加最多 `(N-1) × 20ms` 延迟，换来包头开销从 N 个降到 1 个。
这是**真实的工程折中**，不是理论最优——面试被问"为什么这么设计"时，要能讲出这个取舍。

#### 3. 抖动缓冲的核心算法

**问题**：网络包到达间隔不均匀，播放设备必须匀速消费。

**三个关键子问题与解法**：

**(a) 乱序处理 —— 最容易做错的地方**

初版实现有一个典型 bug：用 `next_expected_seq_` 判断迟到，导致"先到 20、后到 18"时把 18 误判为迟到丢弃。
但**吸收乱序正是抖动缓冲的核心职责**。

正确做法是区分两个水位：
- `next_expected_seq_`：统计用的"见过的最大序号"→ 用于算丢包数
- `play_seq_`：播放进度 → 只有 `seq < play_seq_` 才是真正没用的迟到包

**这个 bug 是自检发现的**（测试用例：先 push 20 再 push 18），修复后乱序包被正确接受。

**(b) 自适应目标延迟**

策略：**保守上调、激进下调**——宁可多等一点，也不要断音。
```cpp
if (jitter * 3 > target)              // 抖动接近目标延迟的 1/3
    target = jitter * 3 + frame_ms;   // 加深缓冲
else if (jitter * 4 < target && depth <= 2)
    target -= 20ms;                   // 网络很稳，尝试收窄
```
抖动估计用 RFC 3550 风格指数平滑（α = 1/16）。

**(c) 丢包隐藏（PLC）**

两类触发：
- 队列空且已过播放时刻 → 该包不会来了
- 队首序号已大于 `play_seq_` → 该包确定丢失

两种都调 `OpusCodec::decode(nullptr, 0, ...)` 触发 Opus 内置 PLC，
用前一帧的波形外推生成近似帧，让用户"感觉不到"丢包。

**与 NetEq 的诚实地照**：
NetEq 做 **sample 级时间拉伸**（accelerate / preemptive expand）；
本项目做 **帧级粗调**（调整目标延迟 + PLC）。解决同一类问题，精度不同。

#### 4. 虚拟音频设备（ADM 抽象）

因为无真实声卡，实现文件/合成驱动的设备，但**保持与真实设备一致的接口语义**：
- **采集**：按采样率定时产出 PCM（文件回放或 440Hz 正弦波）
- **播放**：接收 PCM，带容量上限模拟背压

**关键实现细节**：采集线程用 `sleep_until(next_tick)` 而非 `sleep_for(20ms)`。
后者会累积调度漂移，导致实验中的抖动统计失真——这是嵌入式音频的经典坑。

**这个设计的价值**：换成真实设备时只需替换实现，上层管线不动——这正是 ADM 存在的意义。

### 验证数据（自检 6 项全通过）

```
测试 1：Opus 编解码往返
  最佳对齐偏移=430 样本(26.9 ms)
  RMSE=257.5, SNR=29.8 dB
  平均帧=40.0 B, 实测码率=16.0 kbps

测试 2：多帧拼包        → 4 项全通过
测试 3：包序列化/CRC    → 8 项全通过（含"损坏包头被 CRC 拒绝"）
测试 4：抖动缓冲        → 5 项全通过（含乱序接受、重复拒绝、丢包检测）
测试 5：信令 JSON 往返  → 7 项全通过
测试 6：虚拟设备采集     → 500ms 采集 25 帧 / 8000 样本（精确匹配 20ms 节拍）
```

### 编译产物

```
selftest       2.1 MB   自检
signal_server  1.7 MB   WebSocket 信令服务器
peer           1.7 MB   通话端点（loopback 模式可用）
weaknet_test   0.77 MB  弱网测试工具
```

---

## 下一步（Phase 2+）

- [ ] Phase 2：完善音频管线（聚合准确性、统计上报）
- [ ] Phase 3：WebSocket 信令客户端接入
- [ ] Phase 4：libnice ICE / STUN 穿透
- [ ] Phase 5：DTLS-SRTP 加密
- [ ] Phase 6：弱网对抗（NACK / FEC / 拥塞控制）
- [ ] Phase 7：端到端联调 + 指标统计
