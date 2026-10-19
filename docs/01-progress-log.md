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
selftest       2.2 MB   自检（6 组测试）
signal_server  1.7 MB   WebSocket 信令服务器
peer           1.8 MB   通话端点（loopback 模式可用）
weaknet_test   0.83 MB  弱网测试工具
```

### 端到端自环验证

```bash
./build/peer --loopback --seconds 5
```

```
--- 自环结果（5 秒）---
  发送包数      : 84
  自环回灌包数  : 84
  接收包数      : 84
  编码错误      : 0
  解码错误      : 0
  抖动缓冲接收  : 252
  播放帧数      : 246
  PLC 补偿      : 0
  目标延迟      : 81 ms
  实测码率      : 16.0 kbps

✅ 自环验证通过：媒体链路端到端可用
```

**意义**：编码 → 序列化 → 反序列化 → 抖动缓冲 → 解码 → 播放
这条完整链路在本地跑通，不依赖网络。网络部分出问题时，
可以先用 loopback 判断"是媒体链路的问题，还是网络的问题"——这是一个实用的隔离手段。

### 远程验证（最终证据）

从 GitHub 全新克隆后重新构建，确认仓库内容真实可用：

```bash
git clone https://github.com/SimpleStar666/rtc-1v1-audio.git
cd rtc-1v1-audio && ./scripts/build.sh && ./build/selftest
```

```
✅ 克隆成功（36 个文件，不含 build/）
✅ 编译成功（4 个可执行文件）
✅ 自检 6 组全部通过
✅ 弱网测试：丢包 38 → 检测 38（精确吻合）
✅ 端到端自环：51 包收发、0 错误、0 PLC
```

### 仓库地址

https://github.com/SimpleStar666/rtc-1v1-audio

---

## Phase 1 完成度总结

| 组件 | 状态 | 验证方式 |
|---|---|---|
| Opus 编解码封装 | ✅ | SNR 29.8dB、码率 16.0kbps |
| 多帧拼包 | ✅ | 拼包/解包内容一致 |
| 包格式 + CRC | ✅ | 序列化往返 + 损坏包被拒 |
| 抖动缓冲 | ✅ | 乱序接受、重复拒绝、丢包精确统计 |
| 虚拟音频设备 | ✅ | 采集节拍精确（无漂移） |
| 音频管线 | ✅ | 端到端自环 84 包 0 错误 |
| 信令消息 | ✅ | JSON 往返一致 |
| 信令服务器 | ✅ | 已编译，待联调 |
| 文档体系 | ✅ | 架构/进度/踩坑/验证 4 篇 |

**踩坑 14 项**，其中 5 项为设计级（坑 1、9、10、11、12），均已修复并量化验证。

---

## Phase 2：WebSocket 信令客户端接入

### 做了什么

1. 实现 `SignalingClient`（libwebsockets 客户端封装）
2. 在 `peer` 中加入 `--peer-test` 模式：两端通过服务器配对
3. 端到端验证了信令交互（见下方实测日志）

### 信令交互流程

```
peer A                          signal_server                        peer B
   │                                  │                                 │
   │── join(room=demo) ──────────────►│                                 │
   │◄─ joined (members=1) ────────────│                                 │
   │                                  │◄──────── join(room=demo) ───────│
   │◄─ peer_ready ────────────────────│──── peer_ready ────────────────►│
   │                                  │                                 │
   │── offer(sdp) ───────────────────►│──── forwarded offer ───────────►│
   │◄─ forwarded answer ──────────────│◄──── answer(sdp) ───────────────│
   │◄─ forwarded candidate ───────────│◄──── candidate ─────────────────│
   │── candidate ────────────────────►│──── forwarded candidate ───────►│
```

### 实测结果

**服务器日志（转发明细）**：
```
peer#1 joined room 'demo' (1/2 members)
peer#2 joined room 'demo' (2/2 members)
room 'demo' paired: both peers ready
forwarded offer      peer#1 -> peer#2  (169 bytes)
forwarded answer     peer#2 -> peer#1  (170 bytes)
forwarded candidate  peer#2 -> peer#1  (88 bytes)
forwarded candidate  peer#1 -> peer#2  (88 bytes)
```

**两端信令状态**：
```
[peer A] 入房成功=是  对端就绪=是  收到 answer=是  收到候选=是  收发=4/3  ✅ 通过
[peer B] 入房成功=是  对端就绪=是  收到 offer=是   收到候选=是  收发=4/3  ✅ 通过
```

**双向消息零丢失**——这是修复了"消息竞态"之后的结果（见坑 20）。

### 本阶段踩的坑（4 个）

| # | 坑 | 现象 | 根因 |
|---|---|---|---|
| 15 | opaque_user_data 未设置 | 回调拿不到 C++ 对象 | `connect_info` 里漏了 `opaque_user_data` |
| 16 | **协议名不一致** | 服务器：`No supported protocol "rtc-signal-cli"` | 客户端写了 `rtc-signal-cli`，服务器是 `rtc-signal` |
| 17 | **per-session user data 放 std::string** | 连接后立即段错误 | lws 的 user data 是 malloc 裸内存，不调用 C++ 构造函数 |
| 18 | **lws_write 未预留 LWS_PRE** | `double free or corruption` | 缓冲区前部被 lws 写协议头 → 堆越界 |
| 19 | 消息竞态导致误判超时 | 一端报"入房超时" | `joined` 被同批 `peer_ready` 覆盖（见坑 20） |
| 20 | **发送缓冲是单槽（assign 覆盖）** | 一条消息被另一条覆盖丢失 | 应改为队列（`std::deque`），不得用覆盖式赋值 |

**其中坑 16~18、20 是本阶段最有价值的四个**——都是 C 库与 C++ 混用的典型陷阱。

### 验证方式

```bash
# 终端1：信令服务器
./build/signal_server --port 8899

# 终端2/3：两个端点配对
./build/peer --signal ws://127.0.0.1:8899 --room demo --name A --peer-test --seconds 3
./build/peer --signal ws://127.0.0.1:8899 --room demo --name B --peer-test --seconds 3
```

---

## Phase 3：libnice ICE / STUN 穿透

### 做了什么

1. 实现 `IceTransport`（libnice 封装）：候选收集、交换、连通性检查、媒体收发
2. 实现 `ice_test`：两个 agent 在同机建立真实 UDP 候选对并双向传输
3. 用 tcpdump 抓包定位了 STUN 收不到响应的疑难问题

### 实测结果（host 候选）

```
=== 1. 候选收集 ===
  A/B 各产生 6 个候选（SDP 538 字节）
  候选类型: host×6 srflx×0 relay×0

=== 2. 候选交换与连通性检查 ===
  B 设置 A 的候选 ✓
  A 设置 B 的候选 ✓
  A/B 均选出可用候选对，component 状态 -> READY ✓

=== 3. 双向媒体传输（4 秒）===
  A 收到 39 包 / 692 字节，内容: PKT_FROM_B seq=38  ✓
  B 收到 40 包 / 710 字节，内容: PKT_FROM_A seq=39  ✓
  双向都确认"收到的数据确实来自对端"
```

### STUN 打洞验证（带公网 STUN）

```
[INFO][ice] STUN configured: stun.l.google.com (74.125.250.129):19302
A 候选类型: host×6 srflx×3 relay×0     ← 成功获得 3 个公网映射候选
```

**srflx 候选 = 公网映射地址**，这是 STUN 打洞的核心产出。

### 本阶段踩的坑（4 个，累计 24 个）

| # | 坑 | 定位方式 |
|---|---|---|
| 21 | 头文件暴露 glib 类型 | 编译错误 |
| 22 | `nice_agent_set_remote_candidates_done` 在 0.1.16 不存在 | grep 头文件 |
| **23** | **agent 与主循环上下文不一致** | **tcpdump 抓包**（见下） |
| **24** | stun-server 只接受 IP + IPv6 优先 | 从二进制字符串找文档 |

### 坑 23 的完整定位过程（本项目最难的一个）

**现象**：STUN 配置正确、网络可达，但永远拿不到 srflx 候选（`host×6 srflx×0`），
且 gathering 在 0.2 秒内就"完成"——远快于 STUN 往返应有的时间。

**四步排查（逐步缩小范围）**：

```
① 裸 socket 手工发 STUN Binding Request
   → 收到 32 字节响应          ⇒ 网络可达，排除网络问题

② 开启 libnice 内部日志（G_MESSAGES_DEBUG=all）
   → "STUN transaction retransmitted (timeout 397ms)" 反复出现
                              ⇒ 请求发出了，但"收不到"响应

③ tcpdump 抓包（决定性证据）
   192.168.0.197.45878 > 74.125.250.129.19302: UDP, length 20  ← 请求发出
   74.125.250.129.19302 > 192.168.0.197.45878: UDP, length 32  ← 响应回来了！
                              ⇒ 响应到达网卡却未被处理，问题在事件循环层

④ 检查 glib 上下文：
   agent  = nice_agent_new(g_main_context_default())   // default 上下文
   loop   = g_main_loop_new(NULL, FALSE)               // thread-default 上下文
                              ⇒ 两者不匹配！libnice 的 socket source 挂在前者，
                                而主循环迭代后者 → socket 事件永不分发
```

**修复**：让 agent 与主循环共用一个显式创建的上下文：

```cpp
ctx_   = g_main_context_new();
agent_ = nice_agent_new(ctx_, NICE_COMPATIBILITY_RFC5245);
loop_  = g_main_loop_new(ctx_, FALSE);
nice_agent_attach_recv(agent_, stream, comp, ctx_, cb, this);
```

**验证**：
```
修复前: host×6 srflx×0 relay×0
修复后: host×6 srflx×3 relay×0     ← 公网映射候选收集成功
```

### 坑 24 的定位（配置被静默忽略）

```bash
# 从动态库字符串里找到属性文档（比查在线文档更可靠，版本差异就在二进制里）
$ strings libnice.so.10 | grep -i "STUN server"
"The IP address (not the hostname) of the STUN server to use"
                                       ↑ 只接受 IP，传域名会被静默忽略
```

同时 `getent hosts stun.l.google.com` 返回的是 **IPv6**，而 WSL 的 IPv6 出网不通。
修复：自己做一次强制 IPv4 的 `getaddrinfo`（`ai_family = AF_INET`）。

### 与 WebRTC 的对应

| 本项目 | WebRTC | 说明 |
|---|---|---|
| `IceTransport::init` + gather | `RTCPeerConnection` + ICE gathering | 候选收集 |
| `local_sdp()` | `localDescription` 的 `a=candidate` 行 | 本地候选 |
| `set_remote_sdp()` | `setRemoteDescription` + `addIceCandidate` | 远端候选 |
| `new-selected-pair` 信号 | `iceConnectionState === 'connected'` | 选通候选对 |
| `nice_agent_send()` | RTP 发包（经 DTLS-SRTP 加密） | 媒体传输 |
| host / srflx / relay | 三类候选（优先级 host > srflx > relay） | 候选类型 |

---

## 下一步（Phase 4+）

- [ ] Phase 4：DTLS-SRTP 加密（libsrtp2 + OpenSSL）—— 把 ICE 通道上的明文换成加密媒体
- [ ] Phase 5：弱网对抗（NACK / FEC / 拥塞控制）
- [ ] Phase 6：端到端联调（音频经 ICE 通道传输）+ 指标上报

