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

## Phase 4：DTLS-SRTP 媒体加密

### 做了什么

1. 实现 `DtlsSrtpSession`：DTLS 1.2 握手 + RFC 5764 密钥派生 + SRTP 加解密
2. 实现 `dtls_srtp_test`：进程内两个端点完成真实 DTLS 握手并互发加密 RTP
3. 定位并修复了三个隐蔽问题（库 API 细节 / 协议默认行为 / 测试宏缺陷）

### 实测结果（21 项全部通过）

```
=== 1. DTLS 握手（双方并发）===
  [PASS] A 侧 DTLS 握手成功
  [PASS] B 侧 DTLS 握手成功
  [PASS] 双方 SRTP 会话就绪
  [PASS] 双方都产出了证书指纹
    协商出的加密套件: ECDHE-RSA-AES256-GCM-SHA384

=== 2. 证书指纹一致性（防 MITM 的基础）===
  [PASS] A 看到的对端指纹 == B 的本地指纹
  [PASS] B 看到的对端指纹 == A 的本地指纹

=== 3. RFC 5764 密钥派生 ===
  [PASS] 双方导出的 client_key 一致       （16 字节）
  [PASS] 双方导出的 server_key 一致
  [PASS] 双方导出的 client_salt 一致      （14 字节）
  [PASS] 双方导出的 server_salt 一致

=== 4. SRTP 加密 / 解密往返 ===
  [PASS] A 加密成功
    明文 35 字节 -> 加密后 45 字节（+10 字节 HMAC-SHA1 认证标签）
  [PASS] 密文与明文不同（确实加密了）
  [PASS] B 解密成功 / 长度还原 / 内容与明文完全一致

=== 5. 防篡改 ===
  [PASS] 篡改后的密文被解密端拒绝（HMAC-SHA1 认证生效）

=== 6. 明文包被拒绝 ===
  [PASS] 未加密的明文包被 SRTP 拒绝

################ 结果：全部通过（失败 0 项）################
```

### 关键数据

| 项 | 值 |
|---|---|
| DTLS 版本 | 1.2 |
| 协商套件 | ECDHE-RSA-AES256-GCM-SHA384 |
| 证书 | RSA 2048 自签名（WebRTC 不允许 CA 证书） |
| 指纹算法 | SHA-256（与 SDP `a=fingerprint` 格式一致） |
| SRTP profile | SRTP_AES128_CM_SHA1_80 |
| 密钥材料 | 60 字节 = client_key(16) + server_key(16) + client_salt(14) + server_salt(14) |
| 认证标签开销 | 10 字节/包 |

### 本阶段踩的坑（3 个，累计 27 个）

| # | 坑 | 定位方式 |
|---|---|---|
| **25** | libsrtp2 API 三处不符预期 | 编译错误 + 查系统头文件 |
| **26** | **DTLS 默认不请求对端证书** | 握手成功但 server 读不到证书 |
| **27** | **测试宏二次求值** | "21 全 PASS 却报失败 2 项" |

### 坑 25：libsrtp2 的三处 API 陷阱

```
(a) srtp_t 本身就是指针类型 —— 成员不能写 srtp_t*，srtp_create 才要 srtp_t*
(b) 没有 keysalt 字段 —— key 与 salt 必须"连续存放"，由一个指针指向
(c) std::vector<uint8_t> out(size_t(n)); 被解析成函数声明（most vexing parse）
```

**(c) 最隐蔽**：报错是 "request for member 'data' in 'rtc::out',
which is of non-class type 'std::vector<unsigned char>(size_t)'"——
看起来像类型写错，实际是**解析歧义**。修复：`static_cast<size_t>(n)`。

### 坑 26：DTLS 双向证书交换（"握手成功 ≠ 双向已验证"）

**现象**：
```
A.peer_fingerprint() = E6:84:0C:D1:...   ← client 读到了
B.peer_fingerprint() = (空)              ← server 读不到
```

**根因**：**OpenSSL 的 DTLS server 默认不向 client 索要证书**
（不发送 CertificateRequest）。所以：
- client 必然收到 server 的证书 → A 能读到 ✓
- server 不请求 client 证书 → client 不出示 → B 读不到 ✗

**这是默认策略，不是缺陷。**

**修复过程中的两次尝试**：

| 尝试 | 结果 |
|---|---|
| `SSL_VERIFY_PEER \| SSL_VERIFY_FAIL_IF_NO_PEER_CERT` | ❌ 握手直接失败（自签名证书过不了 CA 校验）|
| `SSL_VERIFY_PEER` + **始终放行的 verify_callback** | ✅ 双向证书交换成功 |

```cpp
SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER,
    [](int, X509_STORE_CTX*) -> int { return 1; });   // 跳过 CA 链校验
// 真正的身份保证来自握手后的"指纹比对"——这正是 WebRTC 的做法
```

**验证**：修复后双方指纹互验均通过。

### 坑 27：测试宏二次求值（最隐蔽）

**现象**（自相矛盾）：
```
21 项全部显示 [PASS]
FAIL 行数：0
汇总：存在失败（失败 2 项）        ← 数字对不上
```

**根因**：测试宏对条件求值了两次：
```cpp
#define CHECK(c, m) do {                              \
    printf("  [%s] %s\n", (c) ? "PASS" : "FAIL", m);  /* ← 第一次 */ \
    if (!(c)) g_fail++;                               /* ← 第二次 */ \
} while (0)
```
对 `CHECK(a.protect(rtp, &len), "加密成功")` 这类**带副作用**的表达式：
1. 第一次执行 `protect` → 成功 → 打印 **PASS**
2. 第二次执行 `protect` → 包已加密、再加密失败 → **g_fail++**

本项目正好有 2 处这样的调用，对应报出的"失败 2 项"。

**修复**：条件只求值一次：
```cpp
#define CHECK(c, m) do {                                    \
    const bool _ok = (c);                                   \
    printf("  [%s] %s\n", _ok ? "PASS" : "FAIL", m);        \
    if (!_ok) g_fail++;                                     \
} while (0)
```

**验证**：`全部通过（失败 0 项）`

**这个坑最危险的地方**：它不崩溃、不报编译错，只让"输出自相矛盾"。
若不加核对 PASS 数量就去改**被测代码**，排查方向完全错了。

### 与 WebRTC 的对应

| 本项目 | WebRTC | 说明 |
|---|---|---|
| `DtlsSrtpSession::handshake` | DTLS 握手（ICE 选通后自动触发） | 密钥协商 |
| `local_fingerprint()` | SDP `a=fingerprint:sha-256 ...` | 指纹交换 |
| `peer_fingerprint()` 校验 | 证书指纹比对（抗 MITM） | 身份验证 |
| `SSL_verify_callback 放行` | WebRTC 不允许 CA 证书 | 自签名策略 |
| `SSL_export_keying_material` | libwebrtc 内部同样调用该 API | RFC 5764 密钥导出 |
| `srtp_protect / unprotect` | 收发 RTP 时的加解密 | 媒体加密 |

---

## Phase 5：弱网对抗（NACK / FEC / 拥塞控制）

### 做了什么

实现 `src/net/loss_protection.{h,cpp}`，三个组件：

| 组件 | 作用 | 对应 WebRTC |
|---|---|---|
| `NackRequester` | 检测丢包、生成重传请求、超时放弃 | NACK（RTCP FB） |
| `FecEncoder` / `FecDecoder` | XOR 冗余（k=4, m=1），丢 1 包可恢复 | ULPFEC 的简化版 |
| `CongestionController` | 延迟梯度 + AIMD 调码率，丢包兜底 | GCC |

### 实测结果（全部通过）

**1. NACK 重传**
```
[PASS] 顺序到达时不发 NACK
[PASS] 检测到缺失的 5、6
    NACK 请求序号: 5 6
[PASS] 生成 2 个 NACK 请求
[PASS] 未到重试间隔时不再重复请求
[PASS] 重传到达后标记为已恢复
[PASS] 超时未到的包被放弃（交给 PLC 兜底）
```

**2. FEC 冗余**
```
[PASS] 组满后生成了 FEC 冗余包（4 源包 × 10 字节 → 1 冗余包 18 字节）
[PASS] 丢 1 包时可恢复，恢复内容与原始包完全一致
[PASS] 丢 2 包时 XOR FEC 无法恢复（如实体现能力边界）
```

**3. 拥塞控制**
```
良好网络 40 轮后: 16000 -> 27417 bps    （加性增生效）
排队后:           27417 -> 13136 bps    （乘性降生效，state=OVERUSE）
高丢包 15%:       16000 ->  7099 bps    （丢包兜底生效）
网络恢复:          9826 -> 45774 bps    （回升生效）
统计: 增 8 次 / 降 5 次 / overuse 5 次
```

**4. 组合恢复率（NACK + FEC）**

| 丢包率 | 实际丢失 | NACK 恢复 | FEC 恢复 | 仍缺失 | 综合恢复率 |
|---|---|---|---|---|---|
| 2% | 11 | 1 | 11 | 0 | **100.0%** |
| 5% | 28 | 1 | 24 | 3 | **89.3%** |
| 10% | 54 | 3 | 39 | 12 | **77.8%** |
| 20% | 89 | 3 | 43 | 43 | **51.7%** |

**曲线符合预期**：丢包率越高，单 XOR FEC（只能防单点丢包）的恢复占比越低，
剩余未恢复的包由抖动缓冲的 PLC 兜底（Phase 1 已实现）。

### 本阶段踩的坑（1 个，累计 28 个）

**坑 28：节流基准用真实时钟 → 仿真中码率永不更新**

现象：延迟梯度已累积到 **320ms**，但码率纹丝不动，且 `overuse 0 次`。

根因：`update_bitrate()` 的 100ms 节流基准 `last_update_ms_` 在**构造函数里用真实时钟初始化**：

```cpp
CongestionController::CongestionController(const Config& cfg) {
    last_update_ms_ = now_ms();     // ← 真实时钟，如 500000
}
// 测试传的逻辑时间：0, 20, 40, ...
// now - last_update_ms_ = 20 - 500000 = -499980  →  恒 < 100  →  永远节流
```

修复：**节流基准惰性初始化**（首次调用时设置）：

```cpp
last_update_ms_ = 0;
// update_bitrate 里：
if (last_update_ms_ == 0) { last_update_ms_ = now; return target_bps_; }
```

**这个坑比坑 9 更隐蔽**：坑 9 是"每次都读真实时钟"（测试结果全为 0，容易发现）；
本例是"只在初始化时读一次"——大部分逻辑正常工作（**梯度算对了**），
只有节流分支静默失效。

决定性线索是统计值 `overuse_events == 0`：它精确指出"状态判定分支从未进入"，
从而把排查范围从"算法错"缩小到"节流逻辑错"。

### 与 WebRTC 的对应

| 本项目 | WebRTC | 说明 |
|---|---|---|
| `NackRequester` | RTCP NACK（PT=205 FMT=1） | 按序号请求重传 |
| `FecEncoder/Decoder` | ULPFEC + RED | 发冗余包，不等 RTT |
| `CongestionController` | GCC（Google Congestion Control） | 延迟梯度 + AIMD |
| 丢包率阈值 5%/15%/20% | 工程经验法则 | 决定 NACK/FEC 的取舍 |

---

## Phase 6：端到端集成（音频经 ICE + DTLS-SRTP 完整链路）

### 完整链路

```
采集(虚拟设备) → Opus 编码 → 打包(20 字节 RTP 兼容头) → FEC 冗余
  → SRTP 加密 → ICE 通道(UDP) ──真实网络──▶
  → ICE 接收 → SRTP 解密 → NACK/FEC 恢复 → 抖动缓冲
  → Opus 解码 → 播放(虚拟设备)
```

### 实测结果（全部通过）

**无丢包场景（3 秒）**：
```
发送包数(源)      : 50
发送包数(FEC)     : 12
接收并解密包数    : 62          ← 62 = 50 + 12，全部到达
SRTP 加密/解密    : 62 / 62     ← 加密与解密次数完全对应
抖动缓冲接收帧    : 150
抖动缓冲播放帧    : 146
PLC 补偿次数      : 0           ← 零丢包链路无 PLC
检测丢包          : 0           ← 统计正确
解码成功帧数      : 146
解码后最大幅度    : 8218        ← 确认不是静音
解密失败次数      : 0
```

**丢包场景矩阵**：

| 丢包率 | 模拟丢弃 | **检测丢包** | FEC 恢复 | PLC | SRTP 加/解 | 结果 |
|---|---|---|---|---|---|---|
| 0% | 0 | **0** | 0 | 0 | 62 / 62 | ✅ |
| 5% | 3 | **3** ✅ | 2 | 9 | 78 / 78 | ✅ |
| 15% | 4 | **4** ✅ | 1 | 12 | 72 / 72 | ✅ |
| 25% | 6 | **6** ✅ | 2 | 18 | 69 / 69 | ✅ |

**关键结论**：
1. **检测丢包与实际丢包逐一精确吻合**（0/3/4/6）—— 抖动缓冲的丢包统计口径正确
2. **SRTP 加解密次数完全对应，无一次失败** —— 加密链路可靠
3. **PLC 补偿随丢包率上升**（0 → 9 → 12 → 18）—— 符合设计预期
4. 丢包场景下 DTLS 握手与指纹验证仍全部通过

### 本阶段踩的坑（2 个，累计 30 个）

**坑 29：集成接线顺序错 → DTLS 握手必然超时**

三个接线错误叠加：
```
(a) ICE 接收回调注册在握手之后 → 握手期间的 DTLS 记录无人接收
(b) 只接了 B 侧的 ICE 回调，A 侧是空回调 → client 收不到 server 的报文
(c) 传输层 recv 队列空就立即返回 → 外层变忙等，极短时间内耗光超时预算
```
修复：先注册回调再启动握手、双向都接、recv 真正等待 timeout。

**坑 30：自定义包头不兼容 RTP 布局 → SRTP 完全不处理**

现象（打日志才发现）：
```
[DEBUG][e2e] B: ICE recv 153 bytes, first=0x00   ← 首字节 0x00，不是 0x80
接收并解密包数 : 0
SRTP 加密/解密 : 62 / 0
```

根因：原包头把 `session_id` 放在最前面，`session_id=0` 时首字节是 `0x00`；
而 SRTP 要求首字节低 2 位是 RTP 版本号 `2`（`0x80`）→ 被判为非 RTP → 送进 DTLS 分支。

**修复：包格式改为前 12 字节完全兼容 RTP 固定头**：
```
[0]      V/P/X/CC = 0x80       ← 版本位 = 2（关键）
[1]      M/PT（96 动态载荷类型）
[2..3]   seq
[4..7]   timestamp
[8..11]  ssrc（承载 session_id）
[12..19] 自定义：phase / flags / trans_id / payload_len / ext_crc
```
总头长 20 字节。

**连带发现两个衍生问题**：

**(a) 丢包统计虚高到 10032** —— FEC 包用"源序号+10000"导致同一 SSRC 序号跳变，
被 SRTP 判为重放，统计把跳变算成丢了 1 万个包。
修复：FEC 用**独立 session_id（独立 SSRC）**。

**(b) "解码出静音"的误判** —— 幅度检查只看最后一帧，而它可能正好是 PLC 静音帧。
修复：统计**所有**解码帧的最大幅度 → 8218。

### 与 WebRTC 的完整对应

| 本项目组件 | WebRTC 对应 | 验证方式 |
|---|---|---|
| `VirtualAudioDevice` | ADM | selftest 采集节拍 |
| `OpusCodec` | Opus | SNR 29.8dB / 16kbps |
| `Packet`（20 字节 RTP 兼容头） | RTP 头 + 扩展 | e2e 收发一致 |
| `NackRequester` / `FecEncoder` | NACK / ULPFEC | 丢包场景恢复统计 |
| `DtlsSrtpSession` | DTLS-SRTP | 21 项 + e2e 62/62 |
| `IceTransport` | ICE / STUN | srflx×3 + 候选对选通 |
| `JitterBuffer` | NetEq | 丢包检测精确吻合 |
| `CongestionController` | GCC | 升/降/恢复全通过 |

**至此，简历个人项目的四个部分全部落地并有实测数据支撑。**

---

## Phase 6 补充：Trickle ICE 与统一指标统计

简历个人项目描述里还有两个要点需要落实：**Trickle ICE** 与 **指标统计**。

### 1. Trickle ICE（候选增量发送）

**为什么重要**：非 Trickle 模式要等所有候选收集完（含 STUN 往返，可能几秒）
才交换 SDP；Trickle 让每个候选一产生就发出去，对端立刻开始连通性检查，
把建链时间从"最慢候选"降到"最先可用候选"。

**实现**（对应 WebRTC 的 `onicecandidate` / `addIceCandidate`）：

```cpp
void set_on_trickle_candidate(TrickleCallback cb);      // onicecandidate
bool add_remote_candidate_sdp(const std::string& sdp);  // addIceCandidate
Credentials local_credentials() const;                  // a=ice-ufrag / a=ice-pwd
void set_remote_credentials(ufrag, pwd);
```

**实测结果**：
```
A 通过 trickle 送出候选: 36 个
B 通过 trickle 送出候选: 36 个
A 逐个添加 B 的候选: 36/36 成功
B 逐个添加 A 的候选: 36/36 成功
A 凭证: ufrag=V5Y/ (pwd 22 字节)
[PASS] Trickle 模式下 A/B 连接建立
Trickle 建链后收发: A 收 20 / B 收 20
```

**踩的坑（坑 31）**：只交换候选时会"候选 36/36 全加成功，但连接建不起来"——
因为**单个 `a=candidate` 行不含 ufrag/pwd**，而 ICE 连通性检查的 STUN 报文
需要它们做认证。Trickle ICE 必须**同时**交换凭证。

### 2. 统一指标统计（对应 getStats / RTCP SR·RR）

**实现** `src/metrics/metrics.{h,cpp}`：

| 分层 | 统计项 |
|---|---|
| 发送侧 | 采集帧、编码成功/错误、RTP/FEC 包数、字节、NACK、重传 |
| 接收侧 | 接收包、检测丢包、丢包率、NACK/FEC 恢复数 |
| 抖动缓冲 | 接收/播放帧、PLC、迟到丢弃、目标延迟、抖动 |
| 编解码 | 解码成功/错误 |
| 加密 | SRTP 加密/解密/失败 |
| 自适应 | 目标码率、延迟梯度、RTT |
| ICE | 通道收发、连接状态 |

**派生指标**：实测码率、恢复率、播放连续性、编码成功率。
**输出**：`to_string()`（人类可读）+ `to_json()`（可上报）。

**e2e 集成与跨层自洽性校验**：
```
======== 通话指标（3177 ms）========
  [发送侧] 采集 151 / 编码 151(100%) / RTP 50 / FEC 12 / 15250 字节(38401 bps)
  [接收侧] 接收 62 / 检测丢包 0 (0.00%) / 恢复率 100.0%
  [抖动缓冲] 接收 150 / 播放 146 / PLC 0 (连续性 100.0%) / 目标 80ms / 抖动 26.24ms
  [编解码] 解码 146 / 错误 0
  [加密] SRTP 加密 62 / 解密 62 (失败 0)
  [ICE] 通道收发 64 / 64 (已连接)
====================================
  [PASS] SRTP 加密次数 == 解密次数（逐包对应）
  [PASS] 发出的媒体+FEC 包数 == SRTP 加密次数
  [PASS] 恢复率落在 [0,1] 区间
  [PASS] 指标可导出为 JSON
```

**跨层自洽**是这里最有价值的验证：
`62(SRTP加密) == 62(SRTP解密)`、`50+12 == 62`、`接收62 == 发送62`——
三层数据互相印证，不是孤立统计。

**踩的坑（坑 32）**：
- 恢复率分母把 `recovered` 加到 `lost` 上 → 8/8 算成 **50%**
- 指标 duration 用真实时钟、start 用逻辑时间 → **3699778 ms** 的荒谬时长

修复后：恢复率 100%、时长 5000ms、码率 36704 bps（数值自洽）。

### 3. 拥塞控制闭环（让自适应真正生效）

**问题**：Phase 5 实现的 `CongestionController` 只是"算出目标码率"，
但没有作用到编码器上 —— 那只是"决策"没有"执行"。

**补齐闭环**：
```
测量(延迟梯度+丢包) → 决策(GCC: AIMD) → 执行(codec.set_bitrate) → 再测量
```

为此给 `OpusCodec` 加了运行时码率接口：

```cpp
bool set_bitrate(int bps);   // OPUS_SET_BITRATE_REQUEST + 值
```

**实测闭环有效性**：

| 场景 | 目标码率 | 编码器实际 |
|---|---|---|
| 无丢包（网络良好） | 16000 → **48000** bps | 48000 bps |
| 20% 丢包 | 16000 → **6000** bps | 6000 bps |

**动态 FEC 冗余率**（按丢包率调整，体现"带宽 vs 抗损"取舍）：

| 丢包率 | 冗余方案 | 开销 |
|---|---|---|
| <5% | k=8, m=1 | 12.5% |
| 5~15% | k=4, m=1 | 25% |
| >15% | k=2, m=1 | 50% |

**Opus 运行时码率遵循性单独验证**（`scripts/verify_opus_bitrate.sh`）：
```
target=16000 bps -> 实测 16000 bps
target=32000 bps -> 实测 32000 bps
target=48000 bps -> 实测 48000 bps     ← 精确遵循
```

**踩的坑（坑 33）**：指标里"实测码率 82933 bps"比目标 48000 还高 73%——
因为 `bytes_sent` 一个变量同时累加了"编码载荷"和"网络整包"两种字节。
修复：拆成 `payload_bytes` 与 `wire_bytes`，并给出 `overhead_ratio()`（开销比 1.23）。

**这类"一变量承载两种语义"的错误在本项目中出现了 4 次**（坑 1/11/32/33），
是最值得主动防御的设计盲区。

---

## 项目完成度总结

| 阶段 | 内容 | 状态 | 关键实测数据 |
|---|---|---|---|
| Phase 0 | 环境侦察 | ✅ | 依赖版本、内存限制、无声卡边界 |
| Phase 1 | 媒体层 | ✅ | SNR 29.8dB / 16.0kbps / 25帧·500ms 无漂移 |
| Phase 2 | WebSocket 信令 | ✅ | 两端配对，双向消息零丢失 |
| Phase 3 | ICE / STUN 穿透 | ✅ | 候选对选通 + **srflx×3** |
| Phase 4 | DTLS-SRTP | ✅ | 21 项通过，密钥四方一致，62/62 解密 |
| Phase 5 | 弱网对抗 | ✅ | 恢复率 100%/89%/78%/52%，码率自适应全通过 |
| Phase 6 | 端到端集成 | ✅ | 检测丢包=实际丢包(0/3/4/6)，PLC 随丢包上升 |
| 补充 | **Trickle ICE** | ✅ | 候选 36/36 增量添加，建链后可收发 |
| 补充 | **统一指标统计** | ✅ | 跨层自洽：加密62=解密62=50+12 |
| 补充 | **拥塞控制闭环** | ✅ | 无丢包 16000→48000，20% 丢包 →6000 bps |

### 简历个人项目描述的四个部分 —— 逐条对照

| # | 简历描述 | 实现 | 实测证据 |
|---|---|---|---|
| ① | WebSocket 信令服务（offer/answer/ICE 转发、**Trickle ICE**） | `signal/`（服务器+客户端）+ ICE Trickle 接口 | 两端配对零丢失；trickle 36/36 建链 |
| ② | STUN/ICE 穿透与 **DTLS-SRTP 加密** | `ice/` + `crypto/` | srflx×3；DTLS 握手+双向指纹互验；62/62 解密 |
| ③ | Opus 编解码 + **抖动缓冲** | `media/` | SNR 29.8dB / 16kbps；丢包检测精确吻合 |
| ④ | 弱网对抗（**NACK/FEC/码率自适应**）与**指标统计** | `net/` + `metrics/` | 恢复率 100%/89%/78%/52%；码率自适应；跨层自洽指标 |

**四个部分全部落地，每项都有可复现的实测数据。**

**共 32 个真实踩坑记录**，涵盖算法设计、统计口径、C/C++ 混用、
事件循环、协议默认行为、集成接线、协议接口约束、可测试性设计八大类。

### 诚实声明的能力边界

| 项目 | 现状 | 原因 |
|---|---|---|
| 真实声卡采集/播放 | ❌ 用虚拟设备替代 | WSL 无 `/dev/snd` |
| 真实 3A（AEC/ANS/AGC） | ❌ 未实现 | 无真实麦克风回采信号 |
| 真实 NAT 穿透（跨公网） | ⚠️ 本机验证通过 | 无第二台公网主机 |
| TURN 中继 | ❌ 未实现 | 无 TURN 服务器需求场景 |
| 硬件时钟漂移 | ❌ 未模拟 | 虚拟设备时钟理想 |
| libwebrtc 对比 | ❌ 未使用 | 环境限制（详见 README） |

这些边界在文档中明确标注，**不夸大也不隐藏**。

