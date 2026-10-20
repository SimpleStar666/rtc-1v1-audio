# 代码导读：每个功能是怎么实现的

> 本文回答：**“每个功能怎么实现？关键代码在哪？核心算法/设计是什么？”**
> `04-how-to-run.md` 讲“怎么跑、看什么输出”，本文讲“输出背后是哪段代码、为什么这么写”。
>
> **行号基准**：2026-10-20，当前工作区。`src/app/` 正在并行重构，本文不依赖它的行号。
> 9 个模块都用“函数名 + 行号”双锚点；若行号漂移，用 `grep -n "函数名" 文件` 重新定位。

---

## 如何用这份文档（面试时怎么快速定位）

| 时间 | 读什么 | 目标 |
|---|---|---|
| **3 分钟** | 一张图 + 15 问速查 | 建立“功能 → 文件 → 函数”索引 |
| **15 分钟** | 模块 2（抖动缓冲）、5（包格式）、8（DTLS-SRTP） | 这三个最能体现 RTC 功底 |
| **按问题查** | 下面的“日志/现象 → 代码”表 | 从运行输出直接跳到实现 |

### 从日志/现象反查代码

| 日志 tag / 现象 | 先看文件 | 入口函数 | 典型日志 |
|---|---|---|---|
| `[jitter]` / 卡顿、PLC | `media/jitter_buffer.cpp` | `push()` / `pop()` | `too late seq=...`、`PLC for seq=...` |
| `[opus]` / 码率、SNR | `media/opus_codec.cpp` | `init()` / `encode()` | `init ok: 16000Hz/1ch...` |
| `[adm]` / 缓冲爆满 | `media/virtual_device.cpp` | `play()` / `start_playout()` | `play buffer full` |
| `[pipeline]` / 发包节拍 | `media/audio_pipeline.cpp` | `send_loop()` / `playout_loop()` | `sent: seq=...` |
| `[packet]` / 包被拒 | `protocol/packet.cpp` | `parse()` / `ext_crc()` | `bad RTP version`、`ext crc mismatch` |
| `[signal_srv]` / `[signal_cli]` | `signal/` | `handle_join()` / `queue_text()` | `room '...' paired` |
| `[ice]` / 无 srflx、连不上 | `ice/ice_transport.cpp` | `init()` / `set_remote_sdp()` | `STUN configured`、`new selected pair` |
| `[dtls_srtp]` / 握手失败 | `crypto/dtls_srtp.cpp` | `prepare()` / `handshake()` | `local fingerprint`、`SRTP sessions ready` |
| `[loss]` / `[fec]` / `[gcc]` | `net/loss_protection.cpp` | `on_packet()` / `update_bitrate()` | `NACK request`、`bitrate DOWN` |
| `[metrics]` / 指标异常 | `metrics/metrics.cpp` | `snapshot()` / `to_string()` | `======== 通话指标` |

**通用定位法**：先判断失效模式 → 再找承担它的**状态变量** → 最后看谁写、谁读、在哪个线程、用什么时间基准。

- “乱序包被丢” → `play_seq_` → `push()` 的迟到判定；
- “码率不更新” → `last_update_ms_` → `update_bitrate()` 的节流；
- “媒体包被当成 DTLS” → `buf[0]` 版本位 → `Packet::serialize()` 的首字节；
- “server 读不到证书” → `SSL_get_peer_certificate()` → `SSL_VERIFY_PEER` 策略。

---

## 一张图：音频数据的完整旅程

```
发送方向 Tx
  ① 采集  VirtualAudioDevice::start_capture()   virtual_device.cpp:111-154  （20ms/帧）
  ② 编码  OpusCodec::encode()                   opus_codec.cpp:96-117       （16k/mono/VOIP）
  ③ 攒帧  AudioPipeline::send_loop()            audio_pipeline.cpp:60-140   （3 帧=60ms）
  ④ 拼包  OpusCodec::pack_frames()              opus_codec.cpp:145-163      （[count][len]...）
  ⑤ 封装  Packet::serialize()                   packet.cpp:62-94            （20B，前 12B 像 RTP）
  ⑥ 加密  DtlsSrtpSession::protect()            dtls_srtp.cpp:436-449       （+10B HMAC）
  ⑦ 发送  IceTransport::send()                  ice_transport.cpp:423-434   （候选对 UDP）

                    ═══ 网络（UDP，可丢/乱序/抖动）═══

接收方向 Rx
  ⑧ 收包  IceTransport::cb_recv()               ice_transport.cpp:134-138
  ⑨ 分流  首字节版本位 0x80 → SRTP，否则 DTLS    e2e_test.cpp:177-225（参考接线）
  ⑩ 解密  DtlsSrtpSession::unprotect()          dtls_srtp.cpp:451-466
  ⑪ 解析  Packet::parse()                       packet.cpp:96-137           （版本+CRC+长度）
  ⑫ 拆包  OpusCodec::unpack_frames()            opus_codec.cpp:165-182
  ⑬ 缓冲  JitterBuffer::push() / pop()          jitter_buffer.cpp:85-141 / 151-195
  ⑭ 解码  OpusCodec::decode()（空包=PLC）       opus_codec.cpp:119-131
  ⑮ 播放  VirtualAudioDevice::play() + DMA      virtual_device.cpp:205-225 / 160-199

控制面：信令（模块 6）→ ICE 选通（模块 7）→ DTLS 协商密钥（模块 8）→ 才有上面这条数据面
```

| 步骤 | 失效时你会看到 | 先查 |
|---|---|---|
| ① 采集 | 帧率漂移、采集帧数不对 | `sleep_until` 是否用了绝对节拍 |
| ② 编码 | 码率不对、SNR 低 | `init()` 参数、`encode()` 返回值 |
| ③④ 聚合/拼包 | 包太碎、拆包越界 | `aggregate_frames`、`[u16 len]` 边界检查 |
| ⑤ 封装 | SRTP 不处理、包被拒 | 首字节是否 0x80、CRC 覆盖范围 |
| ⑥⑩ 加解密 | 解密失败、HMAC 错误 | key/salt 方向、`srtp_protect` 长度 |
| ⑦⑧ ICE | 发不出去/收不到 | 候选对是否选通、回调是否注册 |
| ⑨ 分流 | 媒体进 DTLS 分支 | `(data[0] & 0xC0) == 0x80` |
| ⑪⑫ 解析/拆包 | 噪声、包被丢 | 版本/CRC/长度校验 |
| ⑬ 缓冲 | 卡顿、延迟大、丢包虚高 | 三个水位、目标延迟、统计口径 |
| ⑭⑮ 解码/播放 | 静音、断音、underrun | PLC 空包约定、DMA 消费线程 |

---

## 模块 1：Opus 编解码

### 要实现什么

16kHz 单声道 PCM 按 20ms 一帧编码；接收端解回 PCM；丢包时用 Opus 内置 PLC 补一帧；
多个 20ms 帧能拼进一个网络包；码率能在运行时被拥塞控制改掉。

### 代码在哪

| 功能 | 关键函数 | 位置 |
|---|---|---|
| 常量 | `kSampleRate` / `kFrameSamples` / `kMaxFrameBytes` | `opus_codec.h:20-25` |
| 创建 + 设参数 | `OpusCodec::init()` | `opus_codec.cpp:23-74` |
| 单帧编码 | `OpusCodec::encode()` | `opus_codec.cpp:96-117` |
| 解码 / PLC | `OpusCodec::decode()` | `opus_codec.cpp:119-131` |
| 运行时改码率 | `OpusCodec::set_bitrate()` | `opus_codec.cpp:80-94` |
| 多帧拼包 / 拆包 | `pack_frames()` / `unpack_frames()` | `opus_codec.cpp:145-163` / `165-182` |
| 码率估算 | `avg_encoded_bytes()` | `opus_codec.cpp:133-136` |

### 关键实现

**参数选择**（`opus_codec.cpp:23-74`）：

| 参数 | 取值 | 理由 | 不这么做会怎样 |
|---|---|---|---|
| 采样率 | 16 kHz | 语音能量主要在 4kHz 以下，比 48k 省约 2/3 带宽 | 48k 浪费；8k 发闷、辅音不清 |
| 声道 | 单声道 | 1v1 语音 | 立体声翻倍带宽，无收益 |
| 帧长 | 20 ms | 延迟与包头开销的折中点 | 10ms 包头占比高；60ms 算法延迟大、PLC 粒度粗 |
| 模式 | `OPUS_APPLICATION_VOIP` | 针对语音优化 | `AUDIO` 偏音乐，语音不一定更优 |
| complexity | 0 | 最低 CPU，16kbps 下质量差异很小 | complexity 10 更耗 CPU，实时性变差 |
| VBR | 关（CBR） | 码率可预测，方便预算/拥塞控制 | VBR 包长波动，码率统计难 |
| DTX | 关 | 静音也发，时序/指标简单 | DTX 省带宽，但要处理舒适噪声/不连续时间轴 |

控制参数统一用 `*_REQUEST`：

```cpp
// opus_codec.cpp:55-61
ok &= set(OPUS_SET_BITRATE_REQUEST, opus_int32(bitrate_), "bitrate");
ok &= set(OPUS_SET_COMPLEXITY_REQUEST, opus_int32(complexity_), "complexity");
ok &= set(OPUS_SET_VBR_REQUEST, use_vbr ? 1 : 0, "vbr");
ok &= set(OPUS_SET_INBAND_FEC_REQUEST, use_inband_fec ? 1 : 0, "inband_fec");
if (use_inband_fec) {
    set(OPUS_SET_PACKET_LOSS_PERC_REQUEST, 10, "packet_loss_perc");  // 预留 FEC 冗余
}
```

**为什么不能写 `OPUS_SET_BITRATE(x)`**：该宏展开成两个逗号分隔的表达式
`OPUS_SET_BITRATE_REQUEST, __opus_check_int(x)`，塞进函数调用会多出一个实参（坑 3）。
统一写 `opus_encoder_ctl(enc_, OPUS_SET_BITRATE_REQUEST, opus_int32(bps))`。

**多帧拼包格式**（`opus_codec.cpp:145-163`）：

```
[0]      u8   frame_count（1~255）
[1..2]   u16 LE len_0
[3..]    payload_0
         u16 LE len_1, payload_1 ...
```

```cpp
int need = 1 + 2 * int(frames.size());
for (const auto& f : frames) need += int(f.size());
if (need > out_cap) return -1;          // 先算总长，避免写一半越界
uint8_t* p = out;
*p++ = uint8_t(frames.size());
for (const auto& f : frames) {
    put_u16_le(p, uint16_t(f.size())); p += 2;
    memcpy(p, f.data(), f.size());     p += f.size();
}
```

- 外层 `payload_len` 给总长，但解码器仍需每帧长度才能找到帧边界；
- `u8 count` + `u16 len` 是小端内部格式；网络头（RTP 字段）用大端，刻意区分“包内/网络上”；
- `unpack_frames` 每读一个长度都检查 `p+2 > end` / `p+flen > end`，不信任 `frame_count`。

**运行时改码率**（`opus_codec.cpp:80-94`）：

```cpp
if (bps < 6000)   bps = 6000;
if (bps > 128000) bps = 128000;
const int rc = opus_encoder_ctl(enc_, OPUS_SET_BITRATE_REQUEST, opus_int32(bps));
if (rc != OPUS_OK) return false;
bitrate_ = bps;          // 成功才更新，保证 bitrate() 反映实际值
```

GCC 如果只能“算出目标值”而不能改编码器，闭环就是假的。集成示例见
`tests/e2e_test.cpp:321-331`：`gcc.update_bitrate()` → `codec_a.set_bitrate(target)`。

**编码统计**：`encode()` 把每帧字节数写进 50 帧环形窗口，`avg_encoded_bytes()`
返回窗口均值；`AudioPipeline::bitrate_kbps_locked()` 用它换算实时码率。
不用“总字节/总时长”，因为长通话里早期高码率会长期污染平均值。

### 设计取舍

| 决策 | 代价 | 替代方案 |
|---|---|---|
| 20ms 帧 | 包头占比仍不低，需要聚合 | 60ms 单帧：延迟大、PLC 粒度粗 |
| 3×20ms 传输聚合 | 最多多等 40ms | 直接 60ms Opus 帧：失去 20ms 解码粒度 |
| CBR | 静音也占带宽 | VBR+DTX：省带宽，时间轴/指标复杂 |
| complexity 0 | 同码率音质略低 | complexity 10：质量略好，CPU 高 |

> **Opus lookahead**：该配置约有 **26.9ms** 算法延迟（坑 2 实测）。
> 它不影响“能不能通”，但影响端到端延迟预算和逐样本质量测试——
> 直接逐样本比对会把时间偏移算成失真，必须先做互相关对齐（`tests/selftest.cpp:34-100`）。

### 对应踩坑

| 坑 | 一句话 | 落点 |
|---|---|---|
| **2** | 逐样本比对得 2.8dB SNR，实际是没对齐 lookahead | selftest 互相关对齐；26.9ms 延迟预算 |
| **3** | `OPUS_SET_XXX(x)` 宏展开成两个参数 | `init()` / `set_bitrate()` 用 `*_REQUEST` |

---

## 模块 2：抖动缓冲（NetEq）

> 全项目最值得讲的模块：算法、统计口径、可测试性、并发都在这里。

### 要实现什么

1. **吸收乱序**：按序号重排，不能把“晚到但有用”的包丢掉；
2. **吸收抖动**：自适应目标延迟，太小断音、太大会增加延迟；
3. **丢包隐藏**：该播的包没来，用 PLC 补一帧，播放线程不能停；
4. **统计可信**：丢包数不能把乱序算成丢包，否则监控全错。

### 代码在哪

| 功能 | 关键函数 | 位置 |
|---|---|---|
| 配置 / 统计 | `Config` / `Stats` | `jitter_buffer.h:27-48` |
| 序号差（回绕） | `seq_diff()` | `jitter_buffer.cpp:27-30` |
| 抖动估计 | `update_jitter()` | `jitter_buffer.cpp:32-55` |
| 目标延迟自适应 | `adapt_target_delay()` | `jitter_buffer.cpp:57-83` |
| 收包/去重/迟到/排序 | `push()` | `jitter_buffer.cpp:85-141` |
| 起播门槛 | `ready_to_play()` | `jitter_buffer.cpp:143-149` |
| 取帧 / PLC | `pop()` | `jitter_buffer.cpp:151-195` |
| 丢包快照 | `snapshot()` | `jitter_buffer.cpp:218-234` |
| 三个水位声明 | `play_seq_` / `highest_seq_` / `lowest_seq_` | `jitter_buffer.h:90-105` |

### 三个水位：为什么乱序和迟到必须用不同变量

| 变量 | 语义 | 谁更新 | 谁读 | 误用后果 |
|---|---|---|---|---|
| `play_seq_` | **下一个该播的帧号**（播放进度） | `pop()` 成功/PLC 后 +1 | `push()` 判迟到；`pop()` 找帧 | 用“最大序号”判迟到 → 乱序被丢（坑 1） |
| `highest_seq_` | 见过的最大序号 | `push()` 取 max | `snapshot()` 算跨度 | 拿它判迟到 → 先到 20 后到 18，18 被丢 |
| `lowest_seq_` | 见过的最小序号 | `push()` 取 min | `snapshot()` 算跨度下界 | 和 `play_seq_` 混用会重复统计 |
| `arrived_distinct_` | 独立到达包数（去重后） | 非重复包 +1 | `snapshot()` 算丢包 | 重复包算进来 → 少报丢包 |

**核心原则：`play_seq_` 是播放语义，`highest/lowest` 是统计语义，不能共用。**

坑 1 的错误版本用 `next_expected_seq_` 同时做两件事：先 push 20，水位变 21；
再 push 18，`diff=-3`，18 被误判迟到。但吸收乱序正是抖动缓冲的职责。正确实现：

```cpp
// jitter_buffer.cpp:107-118
// 只丢两类包：已播过的（seq < play_seq_）和重复包；其余一律接受
if (have_seq_ && seq_diff(seq, play_seq_) < 0) {
    stats_.late++;
    return false;
}
```

`seq_diff` 是 `static_cast<int16_t>(a - b)`：`65535` 在 `0` 之前差 `-1`，处理回绕。
隐含前提：两个序号距离 <32768；超过需要“序号周期”扩展（见末尾边界）。

### `push()` 的完整路径

```
push(seq, payload, len, arrive_ms)
  ├─ 1. arrive_ms < 0 ? now_ms() : arrive_ms      // 时间可注入
  ├─ 2. deque 线性查重 → dup++ / false
  ├─ 3. arrived_distinct_++；更新 highest/lowest
  ├─ 4. seq < play_seq_ ? late++ / false           // 只认播放水位
  ├─ 5. 第一个包：play_seq_=seq；deadline=arrive+target
  ├─ 6. 按 seq 升序插入 deque（upper_bound）
  └─ 7. received++；update_jitter()；adapt_target_delay()
```

几个容易忽略的点：

- **统计在迟到判定之前**：已经播过的包仍算“到达过”，所以 `arrived_distinct_` 包含它；
  对“丢包=跨度−到达”是正确的。
- **查重 O(n)**：队列通常几帧到十几帧，线性扫描比哈希表简单。
- **插入用 `upper_bound` + `seq_diff` 比较器**：队列始终按序号升序，且能处理 65535→0。
- **首包把 deadline 设为 `arrive + target`**：先等一个目标延迟，避免一上来就 underflow。

### `pop()` 的四条分支

```
A. 还没起播，now < deadline          → false（继续等）
B. 队列里正好有 play_seq_            → 取出/erase/played++
C. 没有 play_seq_，未超过容忍窗口     → false（可能还在路上）
D. 没有 play_seq_，超过容忍窗口       → true + 空 payload（要求 PLC）
```

```cpp
// jitter_buffer.cpp:165-183
if (it == queue_.end()) {
    // 不能一发现空洞就判丢——包可能还在网络上
    const int64_t wait_until = play_deadline_ms_ + cfg_.target_delay_ms + cfg_.frame_ms;
    if (t >= wait_until) {
        stats_.plc++;
        play_seq_ = uint16_t(play_seq_ + 1);
        play_deadline_ms_ += cfg_.frame_ms;
        playing_ = true;
        return true;                 // 空 payload = 请调用方 PLC
    }
    return false;
}
```

为什么容忍窗口是 `deadline + target + frame`：只等到 deadline 会把“因抖动晚到几十 ms”
的包误判为丢（坑 10）；但等太久会让用户听到更长停顿。多等一个“目标延迟+帧”=
再给网络一个抖动周期。

**PLC 的接口约定**：`pop()` 返回 true 但 payload 为空，调用方据此
`decode(nullptr, 0, ..., plc=1)`。决策（是否 PLC）留在抖动缓冲，动作（怎么补）留在解码器。

### 目标延迟怎么自适应

```cpp
// jitter_buffer.cpp:66-75
const double need_ms = std::max(jitter_ms_ * 2.0, recent_max_dev_ms_ * 1.5);
int want = int(need_ms) + cfg_.frame_ms;

if (want > cfg_.target_delay_ms) {
    cfg_.target_delay_ms = want;              // 上调要快：避免断音
} else if (want < cfg_.target_delay_ms && queue_.size() <= 2) {
    cfg_.target_delay_ms = std::max(want, cfg_.target_delay_ms - kAdaptStepMs);  // 下调慢
}
```

| 项 | 含义 | 为什么是这个系数 |
|---|---|---|
| `jitter_ms_ * 2.0` | 平滑抖动的 2 倍余量 | 覆盖大多数正常波动 |
| `recent_max_dev_ms_ * 1.5` | 近期最坏偏差的 1.5 倍 | 目标延迟必须覆盖突发，再留 50% |
| `+ frame_ms` | 一帧调度/解码余量 | 播放线程、解码器、设备消费都要时间 |
| `max(...)` | 取较大者 | 平均值低但突发高的网络不能被平均值骗 |

**为什么不用平均值**：`jitter_ms_` 是 RFC 3550 风格指数平滑（α=1/16），会把突发“平摊”。
±80ms 抖动平滑后可能只有 ~25ms，目标延迟算成 ~48ms，远小于需要的 ~120ms；
结果大量包在“该播的时刻”还没到，被判丢并 PLC：实测接受 215/500、PLC 333；
修复后接受 499/500、PLC 42，接受率 43%→99.8%（坑 10）。

近期最大值是“带指数遗忘的最大值”（`jitter_buffer.cpp:45-50`）：

```cpp
if (d > recent_max_dev_ms_) recent_max_dev_ms_ = d;
if (++recent_count_ >= kMaxDevWindow) {   // 50 帧 ≈ 1 秒
    recent_count_ = 0;
    recent_max_dev_ms_ *= 0.7;            // 窗口滚动，允许随网络改善下降
}
```

每 50 帧乘 0.7 而不是清零：网络持续改善时目标延迟能降，但不会因一个窗口没大抖动就失去保护。
上调立即生效（断音代价大于多几十 ms 延迟）；下调只在 `queue_.size() <= 2` 时以 20ms 步长收
（缓冲还深时降目标延迟没意义，反而削掉余量）。最终 clamp 到 `[20, 200]`。

### 丢包统计：集合口径，不是事件累加

```cpp
// jitter_buffer.cpp:227-232
if (have_highest_) {
    const uint64_t span = uint64_t(seq_diff(highest_seq_, lowest_seq_)) + 1;
    s.lost = (span > arrived_distinct_) ? (span - arrived_distinct_) : 0;
}
```

**丢包 = 序号空间里的“空洞”数量，与到达顺序无关。**

```
到达顺序：10, 13, 11, 12
  真实丢包 0（10~13 全到，只是乱序）
  按事件累加：收到 13 时水位 11→13，累加 2；收到 11/12 水位回落但已累加的 2 不扣回 → 报 2（假）
  按集合：span=4，arrived=4 → lost=0（正确）
```

坑 11 实测：8% 丢包时实际丢 38，按顺序累加报 **135**；0% 丢包的大抖动场景报 **245**。
改集合口径后 5 个用例逐一精确吻合。

**两个边界**：

1. **`lost` 是帧口径，不是包口径**：`AudioPipeline` 把 60ms 包拆成 3 帧，
   每帧一个虚拟序号；丢一个网络包 = 丢 3 帧。`NackRequester::lost_detected` 才是包口径，
   两个数字不要直接对比。
2. **末尾丢包统计不到**：算法依赖更晚的序号揭示空洞。最后几个包丢了，没有更大序号来暴露，
   这是算法边界（坑 34 末尾）；实测 25% 丢包、6 轮×6 秒共 138 丢包时累计偏差 0。

### 起播门槛与 PLC 的关系

```cpp
// jitter_buffer.cpp:143-149
if (playing_) return true;
if (queue_.size() < min_playout_frames_) return false;   // 先攒够帧
return t >= play_deadline_ms_;                           // 且目标延迟已过
```

两道门槛：时间门槛（首包后等 `target_delay`）+ 帧数门槛
（`max(1, target_ms / frame_ms / 2)`）。注意整数除法：60ms→1 帧、80ms→2 帧，
所以真正防“起播即 underflow”的主要是时间门槛。

| 概念 | 发生在哪 | 统计 | 含义 |
|---|---|---|---|
| **PLC** | 抖动缓冲/解码器 | `JitterBuffer::Stats.plc` | 包没来，但解码器用上一帧外推，**仍有音频** |
| **underrun** | 虚拟设备 DMA 线程 | `PlayStats.underruns` | 设备要数据时缓冲空，**真的没 PCM** |

正常链路 PLC 可以 >0（丢包），underrun 应接近 0；underrun 持续增长说明上游供给断了。

### 设计取舍

| 决策 | 选择 | 代价 | 不这么做会怎样 |
|---|---|---|---|
| 数据结构 | `deque` 按 seq 排序 | 查重/查找 O(n) | `priority_queue` 无法任意 seq 查找；`map` 开销大 |
| 自适应粒度 | 帧级（目标延迟 + PLC） | 不能像 NetEq 做 sample 级时间拉伸 | sample 级要 WSOLA/PSOLA，复杂度和音质风险高 |
| 目标延迟 | 60ms 起，20~200ms 自适应 | 弱网延迟变大 | 固定 60ms：大抖动下大量 PLC |
| 线程安全 | **内部不加锁**，调用方保证串行 | 集成层必须自己加锁/排队 | 内部加锁会让 `snapshot()`/测试更复杂 |
| 序号 | 16 位 RTP 风格 | 长通话需要扩展序号 | 32 位序号不兼容 RTP 语义 |

### 读代码时容易误读的四件事（面试加分）

1. **聚合传输会让抖动估计偏高。** 同一包的 3 帧连续 push，三次都取 `now_ms()`，
   到达间隔是 `0,0,60ms`；`update_jitter()` 看到 `20,20,40ms` 偏差，平滑后约 **26.7ms**。
   本机 e2e（0 丢包）显示“目标延迟 80ms / 抖动 26ms”（`docs/03-verification.md` 实测 26.19ms），
   不是网络真在抖，而是“帧级到达时间”与“包级发送间隔”口径错位。
   正确做法：包级估计抖动，或给 3 帧传错开的逻辑到达时间。
2. **`min_playout_frames_` 整数除法**：60ms 目标只要求 1 帧，不是 3 帧。
3. **`lost` 是帧口径**：丢一个网络包 = 3 帧；不要拿它和 NACK 的包口径对账。
4. **16 位跨度上限**：`lowest_seq_` 是全程最小值，`seq_diff(highest, lowest)` 是 `int16`；
   50 帧/秒时约 **655 秒（11 分钟）**后跨度超过 32767，`span` 变负再转 `uint64_t` 巨大值，
   丢包统计失真。生产实现要维护 RTP extended sequence number，或只统计滚动窗口。

### 对应踩坑

| 坑 | 一句话 | 落点 |
|---|---|---|
| **1** | 单一水位把乱序误判为迟到 | `push()` 只认 `play_seq_` |
| **9** | 组件内部读真实时钟，测试全为 0 | `push(arrive_ms=-1)` / `pop(now=-1)` |
| **10** | 目标延迟只跟平滑抖动，接受率 43% | `max(jitter*2, recent_max*1.5)` |
| **11** | 丢包按到达顺序累加，虚报 6 倍 | `snapshot()` 的 `span - arrived_distinct_` |
| **34** | FEC 包污染 NACK 统计 | 入口按 `session_id` 分流；JB 只收媒体流 |

---

## 模块 3：虚拟音频设备（ADM）

### 要实现什么

WSL 没有 `/dev/snd`。需要“假设备”，但接口语义与真设备一致：按采样率定时产 20ms PCM
（文件/正弦波）；接收 PCM 并按固定速率消费；能观测缓冲水位、欠载、落盘音频。
上层管线不需要知道“音频从哪来、到哪去”——这就是 ADM 的价值。

### 代码在哪

| 功能 | 关键函数 | 位置 |
|---|---|---|
| 配置 / 播放统计 | `Config` / `PlayStats` | `virtual_device.h:24-60` |
| 打开/关闭 | `open()` / `close()` | `virtual_device.cpp:71-109` |
| 采集线程 | `start_capture()` | `virtual_device.cpp:111-154` |
| DMA 消费线程 | `start_playout()` | `virtual_device.cpp:160-199` |
| 写入播放缓冲 | `play()` | `virtual_device.cpp:205-225` |
| 读统计 | `play_stats()` | `virtual_device.cpp:227-235` |

### 关键实现

**采集用绝对节拍，不是 `sleep_for`**（`virtual_device.cpp:125-151`）：

```cpp
auto next_tick = std::chrono::steady_clock::now();
while (d->cap_running) {
    // ... 产出一帧 PCM，调用 cap_cb ...
    next_tick += std::chrono::microseconds(frame_us);   // 绝对时间点
    std::this_thread::sleep_until(next_tick);           // 不是 sleep_for
}
```

`sleep_for(20ms)` 的误差会累积：每次晚 0.4ms，250 次就是 100ms；
`sleep_until(next_tick)` 自动补偿上一次偏差。音频里这就是“采样率漂移”。

**播放必须有独立 DMA 消费线程**（`virtual_device.cpp:177-195`）：

```cpp
while (d->play_running) {
    {
        std::lock_guard<std::mutex> lk(d->play_mutex);
        if (d->play_buf.size() >= size_t(chunk)) {
            d->play_buf.erase(d->play_buf.begin(), d->play_buf.begin() + chunk);
            d->played += chunk;
            d->play_clock_ms += int64_t(chunk) * 1000 / d->cfg.sample_rate;
        } else if (!d->play_buf.empty()) {
            d->played += d->play_buf.size();
            d->play_buf.clear();
        } else {
            d->underruns++;                 // 设备要数据，但缓冲空
        }
    }
    next_tick += std::chrono::microseconds(chunk_us);
    std::this_thread::sleep_until(next_tick);
}
```

`chunk = sample_rate / 100` = 160 样本 = 10ms，贴近真实声卡 period；
只有缓冲完全空才计 underrun；不足一个 chunk 时“全部消费”不算欠载。

**为什么必须有这个线程**：早期 `play()` 只写不读，缓冲必然填满并丢数据，
日志被 `play buffer full` 刷屏（坑 13）。真实声卡有 DMA 按采样率取数据；
虚拟设备不模拟消费端，测试替身就不保真。

**`play()` 的容量背压**（`virtual_device.cpp:211-216`）：缓冲默认 2 秒；满了整块丢并返回 false，
让上层知道“设备消费不过来”。正常时 DMA 每 10ms 取一次，水位稳定在一帧左右。

| `PlayStats` 字段 | 含义 | 异常意味着 |
|---|---|---|
| `played_samples` | 累计播放样本数 | 不增长 = 没有音频流到设备 |
| `buffer_level` | 当前缓冲样本数 | 持续增长 = 消费线程没跑/太慢 |
| `fill_level` | 水位 0~1 | 接近 1 = 即将溢出丢数据 |
| `underruns` | 缓冲空但设备要数据的次数 | 持续增长 = 上游供给断了 |

> **读代码提示**：`play()` 和 DMA 线程都会累加 `played` / `play_clock_ms`
> （`virtual_device.cpp:182-184` 与 `217-219`）。所以 `played_samples` 不是严格的
> “被设备消费的样本数”，而是“写入+消费”的累计吞吐，数值偏大。

### 设计取舍

| 决策 | 选择 | 代价 | 不这么做会怎样 |
|---|---|---|---|
| 设备实现 | Pimpl + 文件/正弦源 | 无真实麦克风 | 依赖声卡，WSL 跑不了 |
| 采集节拍 | `sleep_until` 绝对时间 | 编码超时会追赶式连发 | `sleep_for` 累积漂移 |
| 播放消费 | 独立线程 + 10ms chunk | 多一个线程和锁 | 只写不读，缓冲爆满（坑 13） |
| 时钟漂移 | **未模拟** | 采集/播放时钟严格相等 | 真实设备需重采样/丢帧补偿；本项目如实声明 |

### 对应踩坑

| 坑 | 一句话 | 落点 |
|---|---|---|
| **13** | 播放设备只写不读，缓冲爆满刷屏 | `start_playout()` 的 DMA 线程 |
| **14** | 条件变量固定超时导致帧率损失 | 采集/播放都用绝对时间点 `sleep_until` |

---

## 模块 4：音频管线（编排）

### 要实现什么

把“采集→编码→聚合→发送”和“接收→拆包→抖动缓冲→解码→播放”串起来，并处理三件跨线程的事：

1. **攒帧**：20ms 一帧，攒够 3 帧发 60ms 包；
2. **锁边界**：统计、发送队列、播放设备各有各的锁，不能互相嵌套死锁；
3. **节拍**：发送由采集线程驱动，播放由绝对时间点驱动，不用固定超时轮询。

### 代码在哪

| 功能 | 关键函数 | 位置 |
|---|---|---|
| 初始化编解码/JB | `AudioPipeline::init()` | `audio_pipeline.cpp:22-40` |
| 启动/停止发送 | `start_sending()` / `stop_sending()` | `audio_pipeline.cpp:45-58` |
| 发送主循环 | `AudioPipeline::send_loop()` | `audio_pipeline.cpp:60-140` |
| 接收一个媒体包 | `on_media_packet()` | `audio_pipeline.cpp:143-174` |
| 启动/停止播放 | `start_playout()` / `stop_playout()` | `audio_pipeline.cpp:176-188` |
| 播放主循环 | `playout_loop()` | `audio_pipeline.cpp:190-229` |
| 打断过滤 | `interrupt()` / `is_interrupted()` | `audio_pipeline.cpp:232-246` |
| 无锁版码率 | `bitrate_kbps_locked()` | `audio_pipeline.cpp:249-254` |
| 加锁版码率 / 快照 | `bitrate_kbps()` / `report()` | `audio_pipeline.cpp:256-259` / `266-272` |

### 关键实现

**线程模型**：

| 线程 | 谁创建 | 实际工作 | 节拍 |
|---|---|---|---|
| 采集线程 | `VirtualAudioDevice` | 产 20ms PCM，**同步调用编码回调** | `sleep_until` + 采样率 |
| 发送线程 | `AudioPipeline` | 等“攒够 3 帧”，拼包/封装/`send_fn_` | 条件变量（采集线程通知） |
| 播放线程 | `AudioPipeline` | `jb.pop()` → `decode()` → `device.play()` | `sleep_until` + 20ms |
| DMA 线程 | `VirtualAudioDevice` | 每 10ms 取 160 样本 | `sleep_until` + 10ms |

> 编码不在发送线程里做，而是在**采集线程的回调**里同步做（`audio_pipeline.cpp:64-80`）；
> 发送线程只负责“攒够 3 个编码结果 → 打一个包”。编码超过 20ms，采集会追赶式连发。

**发送主循环是事件驱动，不是定时轮询**：

```cpp
// audio_pipeline.cpp:88-104（节选）
while (sending_) {
    std::vector<std::vector<uint8_t>> batch;
    {
        std::unique_lock<std::mutex> lk(send_mutex_);
        send_cv_.wait(lk, [this] {
            return pending_frames_.size() >= size_t(cfg_.aggregate_frames) || !sending_;
        });
        if (pending_frames_.empty()) continue;
        if (pending_frames_.size() >= size_t(cfg_.aggregate_frames)) {
            batch.assign(pending_frames_.begin(),
                         pending_frames_.begin() + cfg_.aggregate_frames);
            pending_frames_.erase(pending_frames_.begin(),
                                  pending_frames_.begin() + cfg_.aggregate_frames);
        } else {
            batch.swap(pending_frames_);   // 退出时把剩余的也发出去
        }
    }
    // ... pack_frames + PacketHeader + send_fn_ ...
}
```

- **谓词等待**：只关心“够 3 帧了吗/要停了吗”，不设超时；
- **停止主动唤醒**：`stop_sending()` 里 `sending_=false; send_cv_.notify_all();`
  否则采集停、帧数永远不够时线程会永远阻塞；
- **退出前 flush**：`batch.swap(pending_frames_)`，不丢最后 1~2 帧。

**为什么是 3×20ms=60ms**（按 16kbps、每 20ms 约 40B Opus 载荷估算）：

| 每包帧数 N | 间隔 | 每帧分摊头开销 | 头开销占比 | 最多多等 |
|---|---|---|---|---|
| 1 | 20ms | 20B 头 + 1B count + 2B len = 23B | ~57% | 0 |
| 2 | 40ms | 20/2 + 1/2 + 2 ≈ 12.5B | ~31% | 20ms |
| **3** | **60ms** | **20/3 + 1/3 + 2 ≈ 9B** | **~22%** | **40ms** |
| 4 | 80ms | 20/4 + 1/4 + 2 ≈ 7.25B | ~18% | 60ms |

N=3 是“开销下降”和“额外延迟”的拐点；60ms 也是服务器期望帧长。
不用 60ms Opus 单帧：聚合是传输层行为，接收端仍拆成 3 个 20ms 帧，保留 20ms 解码/PLC 粒度。
单向延迟估算：等第 3 帧 40ms + 抖动缓冲 60~80ms + lookahead 26.9ms ≈ 127~147ms。

**锁边界：为什么需要 `bitrate_kbps_locked()`**：

```cpp
// audio_pipeline.cpp:249-271（节选）
int AudioPipeline::bitrate_kbps_locked() const {
    // 前提：调用方已持有 stat_mutex_
    const double bytes_per_frame = codec_.avg_encoded_bytes();
    ...
}
int AudioPipeline::bitrate_kbps() const {
    std::lock_guard<std::mutex> lk(stat_mutex_);
    return bitrate_kbps_locked();
}
AudioPipeline::Report AudioPipeline::report() const {
    std::lock_guard<std::mutex> lk(stat_mutex_);
    ...
    r.cur_bitrate_kbps = bitrate_kbps_locked();   // 不能调 bitrate_kbps()
    return r;
}
```

`std::mutex` **不是递归锁**。`report()` 已持有 `stat_mutex_`，再调 `bitrate_kbps()` 会第二次 lock，
同一线程永久阻塞（坑 12，gdb 里能看到 `__lll_lock_wait → std::mutex::lock → bitrate_kbps`）。
约定：公共方法自己加锁；`_locked` 后缀 = 调用方必须已持锁。

| 方案 | 优点 | 缺点 |
|---|---|---|
| `_locked` 后缀（本项目） | 零开销、意图明确 | 靠命名/code review，无编译期保证 |
| `std::recursive_mutex` | 不会自死锁 | 掩盖锁设计问题；持锁时间变长 |
| 缩小锁粒度 | 并发性更好 | 改动大，容易引入新竞态 |

**条件变量与绝对时间点**：

发送侧当前用 `wait(predicate)`，没有超时——发送节拍由采集线程（硬件时钟语义）驱动，
发送线程只等“事件”，比定时等待更彻底。历史错误写法：

```cpp
send_cv_.wait_for(lk, std::chrono::milliseconds(50));   // 每次可能早/晚，误差累积
```

5 秒实测 245 帧而非 250 帧，约 2% 损失（坑 14）。播放侧必须自己计时，用绝对时间点：

```cpp
// audio_pipeline.cpp:225-227
next_tick += frame_dur;                 // 绝对时间点，不是 now + 20ms
std::this_thread::sleep_until(next_tick);
```

原则：**定时用时间点，不用时间差。** 真需要带超时的等待，用
`wait_until(lk, deadline, predicate)`，不要用 `wait_for`。

**打断过滤**（`audio_pipeline.cpp:146-153`）：`has_interrupt_ && h.trans_id == interrupted_trans_id_`
的包在入口丢弃，除非带 `kFlagUninterruptible`（如系统提示音）。避免“打断后旧音频还在播”的串音。
当前 `cur_trans_id_` 只有初始值 1，没有公开的“推进事务号”接口；完整 START/BODY/END 语义要在集成层推进。

### 设计取舍

| 决策 | 选择 | 代价 | 不这么做会怎样 |
|---|---|---|---|
| 聚合 | 3 帧/包 | 最多 40ms 额外延迟 | 每 20ms 一包，头开销 ~57% |
| 发送节拍 | 事件驱动（条件变量） | 依赖采集线程持续产帧 | 固定超时轮询，误差累积 |
| 播放节拍 | 绝对时间点 `sleep_until` | 解码超时会追赶式连发 | 相对超时，长期漂移 |
| 锁 | `_locked` 约定 | 靠命名，无编译期检查 | `recursive_mutex` 掩盖问题 |

**三个需要知道的实现边界**：

1. **`set_target_bitrate_bps()` 目前只是日志占位**（`audio_pipeline.cpp:261-264`），
   注释写“简化：重建编码器代价高，这里仅记录”。但 `OpusCodec::set_bitrate()` 已实现，
   真正闭环示例在 `e2e_test.cpp:321-331`（直接调 `codec_a.set_bitrate()`）。
   产品化时应在这里调 `codec_.set_bitrate(bps)`。
2. **帧序号映射假设“每包满 3 帧”**：`fseq = h.seq * frames.size() + i`
   （`audio_pipeline.cpp:170-173`）。停止时最后一个包可能只有 1~2 帧，`frames.size()`
   变小会让虚拟序号与之前重叠。稳妥写法是用固定 `cfg_.aggregate_frames` 做乘数。
3. **JitterBuffer 内部不加锁**，而 `on_media_packet()`（网络回调线程）和 `playout_loop()`
   （播放线程）会并发访问它。当前测试靠低负载/时序掩盖；真实通话应在 `AudioPipeline`
   加接收锁，或给 `JitterBuffer` 内部加锁。

### 对应踩坑

| 坑 | 一句话 | 落点 |
|---|---|---|
| **12** | `std::mutex` 非递归，`report()` 调 `bitrate_kbps()` 自死锁 | `bitrate_kbps_locked()` 的由来 |
| **14** | 固定超时轮询导致 2% 帧率损失 | 发送谓词等待；播放绝对 `sleep_until` |

---

## 模块 5：包格式（RTP 兼容的 20 字节头）

> 注意：`00-architecture.md` 里的“16 字节头”是旧版描述；以 `packet.h` 的 **20 字节**为准。

### 要实现什么

把“载荷+序号+时间戳+会话语义”封装成自描述包，同时满足：
(1) 能被 SRTP 当成标准 RTP 处理；(2) 保留 RTP 没有的 `phase`/`flags`/`trans_id`。

### 代码在哪

| 功能 | 关键函数/结构 | 位置 |
|---|---|---|
| 布局说明 | 注释 | `packet.h:1-29` |
| 阶段/标志 | `Phase` / `Flag` | `packet.h:37-55` |
| RTP 常量 | `kRtpVersionByte` / `kRtpPayloadType` | `packet.h:60-61` |
| 头结构 | `PacketHeader` | `packet.h:63-74` |
| 头校验 | `ext_crc()` | `packet.cpp:28-57` |
| 序列化 / 反序列化 | `serialize()` / `parse()` | `packet.cpp:62-94` / `96-137` |

### 20 字节怎么排

| 偏移 | 长度 | 字段 | 编码 | RTP 对应 |
|---|---|---|---|---|
| 0 | 1 | V/P/X/CC | `0x80` | 版本=2，无填充/扩展/CSRC |
| 1 | 1 | M/PT | `M<<7 \| 96` | Marker + 动态 PT 96 |
| 2–3 | 2 | `seq` | 大端 u16 | Sequence Number |
| 4–7 | 4 | `timestamp` | 大端 u32 | Timestamp（按样本数推进） |
| 8–11 | 4 | `ssrc` | 大端 u32 | SSRC（低 16 位放 `session_id`） |
| 12 | 1 | `phase` | u8 | （无）START/BODY/END/BREAK... |
| 13 | 1 | `flags` | u8 位掩码 | （无）不可打断/FEC/重传... |
| 14–15 | 2 | `trans_id` | 大端 u16 | （无）打断过滤 |
| 16–17 | 2 | `payload_len` | 大端 u16 | （无）冗余长度字段 |
| 18–19 | 2 | `ext_crc` | 大端 u16 | （无）头校验 |

从 SRTP 视角看，字节 0~11 是 RTP 固定头，**12~19 是 RTP 载荷**——自定义字段会被
SRTP 加密并认证，接收端必须先 `unprotect()` 才能解析。

### 为什么必须兼容 RTP

坑 30：最初把 `session_id` 放最前面（`[0..1]`），`session_id=0` 时首字节 `0x00`。
接收端用 `(data[0] & 0xC0) == 0x80` 分流，`0x00 & 0xC0 = 0x00`，媒体包被送进 DTLS 分支，
SRTP 一次都没被调用。现象：包到了（153 字节），但“接收并解密包数=0，SRTP 加密/解密=62/0”。

修复后 `[0]=0x80`，`seq/timestamp/ssrc` 在标准位置，`session_id` 映射到 SSRC。
两条衍生经验：

1. **多流分隔必须用独立 SSRC**，不能用“源序号+10000”。SRTP 按 SSRC 跟踪序号；
   同一 SSRC 先跳到 10000 再回落会被判重放，丢包统计还会把跳变算成“丢了 1 万个包”。
   所以 FEC 包用 `session_id=2`（独立 SSRC），媒体流用 `session_id=1`。
2. **判断包类型看协议特征位**（RTP 版本位），不要依赖自定义字段取值。

### 头校验：为什么 CRC 覆盖 18 字节

```cpp
// packet.cpp:37-56（节选）
uint8_t tmp[18];
tmp[0] = kRtpVersionByte;
tmp[1] = uint8_t((h.marker ? 0x80 : 0x00) | (kRtpPayloadType & 0x7F));
put_u16_be(tmp + 2, h.seq);
put_u32_be(tmp + 4, h.timestamp);
put_u32_be(tmp + 8, uint32_t(h.session_id));
tmp[12] = static_cast<uint8_t>(h.phase);
tmp[13] = h.flags;
put_u16_be(tmp + 14, h.trans_id);
put_u16_be(tmp + 16, h.payload_len);

uint16_t sum = 0;
for (size_t i = 0; i < sizeof(tmp); i++) {
    sum = uint16_t((sum << 1) | (sum >> 15));   // 循环左移 1
    sum ^= tmp[i];
}
return uint16_t(~sum);
```

- **覆盖“除 CRC 外的全部 18 字节”**，包括 RTP 兼容段。原因：loopback/进程内路径没有
  SRTP HMAC；如果只覆盖 8 字节自定义段，篡改 `seq`/`timestamp`/版本位就检测不出来。
  selftest“损坏包头被 CRC 拒绝”正是抓到了最初只覆盖扩展段的问题。
- **算法是 rotate-left-xor + 取反**，不是标准 CRC-16/CCITT。足够检测意外位翻转，
  但**不是认证**：攻击者可以重算。真正的完整性由 SRTP HMAC-SHA1 负责。
- **解析顺序**：长度 ≥20 → 版本位 → 解析字段 → 重算 CRC 比对 → 最后才信任 `payload_len`
  并检查载荷长度。CRC 在长度检查前，避免用坏长度越界。

### 设计取舍

| 决策 | 选择 | 代价/边界 | 替代方案 |
|---|---|---|---|
| 头长度 | 固定 20 字节 | 无扩展位；未来加字段要改版本 | RTP header extension：灵活但解析复杂 |
| 自定义字段位置 | RTP 头之后（当作载荷） | 被 SRTP 加密，中间设备看不到 | CSRC/扩展头：可见但需 SRTP 配合 |
| `payload_len` | 冗余字段 | 多 2 字节；必须校验 | 依赖 UDP 长度：流式场景无法自描述 |
| 头校验 | 自定义 16 位 rotate-xor | 比 CRC-32 弱；不是认证 | 标准 CRC-32 更慢；SRTP HMAC 只在加密路径有 |
| 序号 | 16 位 RTP 风格 | 长通话要扩展序号 | 32 位：不兼容 RTP |

### 对应踩坑

| 坑 | 一句话 | 落点 |
|---|---|---|
| **30** | 自定义头首字节不是 0x80，SRTP 完全不处理 | 前 12 字节 RTP 兼容；FEC 独立 SSRC |
| **34** | FEC 包污染 NACK 统计 | 入口按 `session_id` 分流；FEC=2 |

---

## 模块 6：信令（WebSocket 服务器 + 客户端）

### 要实现什么

媒体通道建立前，让两个 peer：连服务器、加入同一房间（1v1，满 2 人拒绝）；
收到“对端就绪”；交换 offer/answer/candidate；一方离开/断线时通知另一方。
信令服务器**不碰媒体**，只做消息路由。

### 代码在哪

**服务器** `signal_server.cpp`：

| 功能 | 关键函数 | 位置 |
|---|---|---|
| 会话结构（含发送队列） | `struct Session` | `37-50` |
| 全局会话/房间表 | `g_by_wsi` / `g_rooms` | `52-55` |
| 入队一条文本 | `queue_text()` | `63-70` |
| 找对端 | `find_peer_wsi()` | `78-84` |
| 离开房间+通知 | `leave_room()` | `91-111` |
| 入房/满员/配对 | `handle_join()` | `113-165` |
| 转发 offer/answer/candidate | `handle_forward()` | `167-192` |
| 消息分发 | `on_message()` | `194-234` |
| LWS 回调 | `callback_ws()` | `237-315` |
| 可写回调（LWS_PRE） | `LWS_CALLBACK_SERVER_WRITEABLE` | `270-296` |

**客户端** `signaling_client.cpp`：

| 功能 | 关键函数 | 位置 |
|---|---|---|
| per-session 裸内存（只能 POD） | `PerSessionData` | `21-28` |
| 连接（非阻塞） | `connect()` | `40-72` |
| 发送（任意线程） | `send()` | `85-95` |
| 取队首 | `pop_outgoing()` | `113-119` |
| 收消息 | `handle_incoming()` | `121-131` |
| 连接状态 | `handle_established()` / `handle_closed()` | `133-151` |
| 服务循环+建连 | `service_loop()` | `153-212` |
| LWS 回调 | `client_callback()` | `216-290` |

### 关键实现

**服务器房间状态机**：

```
ESTABLISHED → new Session（peer_id++）
JOIN        → 校验 room；已在房间先 leave；count>=2 拒绝；加入 g_rooms
            → 给本人发 joined（sdp 字段复用为成员数）
            → 若对端已在：给对端发 peer_ready，给自己发 peer_ready
OFFER/ANSWER/CANDIDATE → 找同房间对端 → 打上 sender peer_id → 转发
BYE/CLOSED  → leave_room：移除成员；空房间删除；通知对端 kPeerLeft；delete Session
```

服务器不解析 SDP/candidate 内容，只检查类型和房间关系——保持“低带宽、只路由”的定位。

**发送队列必须是 `deque`，不能是单槽**：

```cpp
// signal_server.cpp:63-70
int queue_text(struct lws* wsi, const std::string& msg) {
    Session* s = find_session(wsi);
    if (!s) return -1;
    s->tx.push_back(msg);          // 入队（不覆盖）
    s->want_write = true;
    lws_callback_on_writable(wsi);
    return 0;
}
```

坑 19/20：处理一个 JOIN 时，服务器在同一次回调里连续发 `joined` 和 `peer_ready`。
单槽 `vector::assign()` 会让第二条覆盖第一条，客户端只收到 `peer_ready`，误判“入房超时”。
deque 每次取队首一条，发完 `pop_front()`，若还有积压就重新请求可写。

**`LWS_PRE`：`lws_write` 前必须留协议头空间**：

```cpp
// signal_server.cpp:281-290
std::vector<uint8_t> outbuf(LWS_PRE + msg.size());
memcpy(outbuf.data() + LWS_PRE, msg.data(), msg.size());
int n = lws_write(wsi, outbuf.data() + LWS_PRE, msg.size(), LWS_WRITE_TEXT);
...
s->tx.pop_front();
if (!s->tx.empty()) lws_callback_on_writable(wsi);   // 还有就继续
```

libwebsockets 会往 `data()` 之前写 WebSocket 协议头；直接传 `msg.data()` 会踩坏堆元数据，
报错延迟到后面的 `free()`：`double free or corruption (out)`（坑 18）。
**堆越界的报错位置和出错位置通常隔很远**，看到这类错误要往前找所有写缓冲区的地方。

**客户端：`opaque_user_data` 与 POD user data**：

```cpp
// signaling_client.cpp:183-193（节选）
lws_client_connect_info ci;
memset(&ci, 0, sizeof(ci));
ci.context = ctx;
ci.address = host_.c_str();
ci.port = port_;
ci.path = path_.c_str();
ci.protocol = protocols[0].name;
ci.local_protocol_name = protocols[0].name;
ci.opaque_user_data = this;          // 让回调能找回 C++ 对象
```

- 用 `opaque_user_data` 而不是 per-session user：`connect` 返回前 session 还没创建，
  没法填 per-session user（坑 15）。
- per-session user data 只能放 POD：lws 用 `malloc` 分配，不调用构造/析构；
  放 `std::string` 内部指针是野值，第一次 `clear()` 就段错误（坑 17）。
  接收缓冲放在 `SignalingClient::rx_buffer_` 成员里。
  对照：服务器的 `Session` 是自己 `new/delete` 的，所以可以安全持有 `deque`/`vector`。
- subprotocol 两端必须完全一致：服务器 `"rtc-signal"`，客户端也必须是；早期写成
  `"rtc-signal-cli"`，握手直接拒绝（坑 16）。
- `tx_queue_` 是 `deque<string>` + `tx_mutex_`；`lws_callback_on_writable()` 线程安全，
  业务线程可随时 `send()`；可写回调取一条、`LWS_PRE` 预留、发完若还有则继续请求。
- 服务循环 `lws_service(ctx, 50)` 在独立线程；`disconnect()` 置 `running_=false` 后 join，
  最多等 50ms 退出。接收侧用 `lws_is_final_fragment()` 处理分片，超过 64KB 清空丢弃。

### 设计取舍

| 决策 | 选择 | 代价 | 不这么做会怎样 |
|---|---|---|---|
| 传输 | WebSocket/TCP（可靠有序） | 建链慢于 UDP，但信令量小 | 信令丢包要自己做重传/去重 |
| 服务器职责 | 只路由，不解析 SDP | 不能转码/混流 | 服务器复杂、带宽压力大 |
| 房间模型 | 内存 map，1v1 满 2 拒绝 | 无持久化/鉴权/重连 | 需要数据库/会话服务 |
| 客户端 user data | 只放 POD 指针 | 访问成员要绕一层 | 放 `std::string` 直接段错误（坑 17） |
| 安全 | `ws://`，无鉴权 | 仅适合本地演示 | 生产要 `wss://` + 认证 |

### 对应踩坑

| 坑 | 一句话 | 落点 |
|---|---|---|
| **15** | 漏设 `opaque_user_data`，回调拿不到对象 | `ci.opaque_user_data = this` |
| **16** | subprotocol 两端名字不一致 | 两端都用 `"rtc-signal"` |
| **17** | 裸内存里放 `std::string` → 段错误 | `PerSessionData` 只放指针 |
| **18** | `lws_write` 未预留 `LWS_PRE` → 堆越界 | 服务器/客户端都先 `LWS_PRE + size` |
| **19** | 消息竞态导致误判超时 | 等待“状态达成”，不等待单条消息 |
| **20** | 发送缓冲覆盖式赋值 → 丢消息 | `deque` + 每次发队首 |

---

## 模块 7：ICE / STUN / Trickle ICE

### 要实现什么

在两个可能位于 NAT 后的 peer 之间找到可用的 UDP 路径：收集 host/srflx（/relay）候选；
经信令交换（整段 SDP 或 Trickle）；做连通性检查选出候选对；在候选对上收发媒体。

### 代码在哪

| 功能 | 关键函数 | 位置 |
|---|---|---|
| 强制 IPv4 解析 | `resolve_ipv4()` | `ice_transport.cpp:28-46` |
| 初始化 | `IceTransport::init()` | `ice_transport.cpp:210-313` |
| 附加接收回调 | `attach_recv()` | `ice_transport.cpp:320-326` |
| 候选收集完成 | `cb_gathering_done()` | `ice_transport.cpp:80-100` |
| 选中候选对 | `cb_new_selected_pair()` | `ice_transport.cpp:102-110` |
| 组件 READY | `cb_component_state()` | `ice_transport.cpp:112-132` |
| 收包 | `cb_recv()` | `ice_transport.cpp:134-138` |
| 注册 Trickle 回调（补发早到候选） | `set_on_trickle_candidate()` | `ice_transport.cpp:161-171` |
| Trickle 新候选 | `cb_new_candidate()` | `ice_transport.cpp:173-207` |
| 设置远端 SDP | `set_remote_sdp()` | `ice_transport.cpp:350-369` |
| 本地/远端凭证 | `local_credentials()` / `set_remote_credentials()` | `ice_transport.cpp:371-380` / `382-386` |
| 增量加远端候选 | `add_remote_candidate_sdp()` | `ice_transport.cpp:393-420` |
| 发送媒体 | `IceTransport::send()` | `ice_transport.cpp:423-434` |

### 关键实现

**最关键的坑：agent 与 glib 主循环必须共用同一个 `GMainContext`**：

```cpp
// ice_transport.cpp:231-238 / 294 / 324-325
ctx_ = g_main_context_new();                                  // 自己创建
agent_ = nice_agent_new(ctx_, NICE_COMPATIBILITY_RFC5245);    // agent 挂 ctx_
...
loop_ = g_main_loop_new(ctx_, FALSE);                         // 主循环迭代同一个 ctx_
...
nice_agent_attach_recv(agent_, cfg_.stream_id, kComponentRtp,
                       ctx_, cb_recv, this);                  // 接收也挂 ctx_
```

错误版本：`nice_agent_new(g_main_context_default(), ...)` + `g_main_loop_new(NULL, ...)`
（thread-default）。**这两个不是同一个上下文。** libnice 的 socket source 挂在 default 上，
主循环却迭代 thread-default，socket 可读事件永远不分发，STUN 响应躺在内核缓冲区，
agent 读不到 → 不断重传。

坑 23 的定位链：裸 socket 能收到响应 → 库日志看到 `STUN transaction retransmitted` →
`tcpdump` 看到“请求发出、响应回来、又重传” → 锁定事件循环层。修复后 `srflx×0 → srflx×3`。

**`stun-server` 只接受 IP，且必须强制 IPv4**：

libnice 0.1.16 的属性文档写 “The IP address (not the hostname) of the STUN server to use”。
传域名会被**静默忽略**：不报错、不警告、不收集 srflx（坑 24）。另外 `getaddrinfo`
可能优先返回 IPv6，WSL 的 IPv6 出网不通，请求会一直重传。所以代码自己做强制 IPv4：

```cpp
// ice_transport.cpp:30-45（节选）
struct in_addr tmp;
if (inet_pton(AF_INET, host.c_str(), &tmp) == 1) return host;  // 已是 IP
struct addrinfo hints;
memset(&hints, 0, sizeof(hints));
hints.ai_family = AF_INET;          // ★ 只要 IPv4
hints.ai_socktype = SOCK_DGRAM;
...
inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
```

配置：`g_object_set(agent_, "stun-server", stun_ip, "stun-server-port", port, ...)`。
**“配置项传进去了”不等于“配置生效了”**；排查静默失效可以去动态库字符串里找属性文档：
`strings libnice.so.10 | grep -i "STUN server"`。

**候选收集与交换**：

```
init()
  ├─ ctx_/agent_/STUN/controlling-mode
  ├─ nice_agent_add_stream(1)
  ├─ 连接信号：gathering-done / new-selected-pair / component-state / new-candidate
  ├─ g_main_loop_new(ctx_) + 线程跑 g_main_loop_run
  ├─ attach_recv()（必须在 gather 之前）
  └─ nice_agent_gather_candidates()

cb_gathering_done() → nice_agent_generate_local_sdp() → local_sdp_ → on_candidate_
set_remote_sdp(fragment) → nice_agent_parse_remote_sdp()  // 自动设候选+启动检查
```

**不要调用 `nice_agent_set_remote_candidates_done()`**：libnice 0.1.16 没有这个函数；
`parse_remote_sdp` 本身已完成“设置候选+启动检查”（坑 22）。

**Trickle ICE：候选和凭证要分开交换**：

```
本地 new-candidate → cb_new_candidate()
  ├─ nice_agent_get_local_candidates()
  ├─ 每个候选 → nice_agent_generate_local_candidate_sdp()
  └─ 用 sent_candidates_ 去重后 on_trickle_()

远端 add_remote_candidate_sdp()
  ├─ nice_agent_parse_remote_candidate_sdp() → NiceCandidate*（不是 GSList）
  ├─ 包成单元素 GSList → nice_agent_set_remote_candidates()
  └─ g_slist_free + nice_candidate_free
```

libnice 的 `new-candidate` 只告知“有新候选”，不告诉是哪一个。早期实现遍历全部候选并发送，
6 个候选发了 36 次；现在用 `sent_candidates_`（`ice_transport.h:164`）记录已发过的 SDP，
只发新的（`ice_transport.cpp:192-204`）。

还有一个时序坑：`init()` 内部就调用了 `nice_agent_gather_candidates()`，候选可能在调用方
注册 `set_on_trickle_candidate()` **之前**就产生。若直接丢弃，会出现“日志说收集到候选，
但对端一个都没收到”。现在的做法是：回调未注册时先把候选存进 `early_candidates_`，
注册瞬间补发（`ice_transport.cpp:161-171`、`201-204`）。

**Trickle 只交换候选是不够的**（坑 31）：单个候选片段不含 ufrag/pwd；而 STUN 连通性检查需要
`USERNAME = 远端ufrag:本地ufrag` 和 `MESSAGE-INTEGRITY = HMAC(密码)`。
只 trickle 候选 → 认证失败 → 候选 36/36 添加成功，但连接建不起来。必须先交换凭证：

```cpp
const auto ac = ta.local_credentials();
const auto bc = tb.local_credentials();
tb.set_remote_credentials(ac.ufrag, ac.pwd);
ta.set_remote_credentials(bc.ufrag, bc.pwd);
// 之后再逐个 add_remote_candidate_sdp
```

**连接状态与媒体发送**：`new-selected-pair` 和 `component-state == READY` 都会调
`mark_connected()`，后者去重只通知一次。`send()` 检查 `connected_` 后
`nice_agent_send()`；收包通过 `cb_recv → handle_incoming → on_recv_`，
运行在 glib 主循环线程，**不是** `init()` 的线程——集成时注意线程归属。

### 设计取舍

| 决策 | 选择 | 代价 | 不这么做会怎样 |
|---|---|---|---|
| ICE 库 | libnice（apt 包） | C API + glib 上下文坑 | libdatachannel 依赖 submodule，环境装不上（坑 5） |
| 候选交换 | 同时支持整段 SDP 和 Trickle | 两套接口/两套测试 | 只支持整段：建链要等最慢候选 |
| STUN 配置 | 自己做 hostname→IPv4 | 多一次解析 | 域名被静默忽略；IPv6 不通 |
| TURN | 只留配置字段，未实现 | 对称 NAT 下可能连不上 | 实现 relay 候选要 TURN 服务器 |
| 流/组件 | 1 stream / 1 component | 不支持多轨、不分离 RTCP | 复杂；本项目用 rtcp-mux 思路 |
| Trickle 去重 | `sent_candidates_` 集合 | 多存几段 SDP | 每来一个新候选就重发全部（6→36 次） |
| 早到候选 | `early_candidates_` 缓冲，注册时补发 | 多存几段 SDP | init 内已开始收集，回调注册前的候选会丢 |

### 对应踩坑

| 坑 | 一句话 | 落点 |
|---|---|---|
| **21** | 头文件暴露 glib 类型导致编译失败 | 回调用 `unsigned int`/`void*` |
| **22** | libnice 0.1.16 没有 `set_remote_candidates_done` | 只调 `parse_remote_sdp` |
| **23** | agent 与主循环上下文不一致，收不到 STUN 响应 | `ctx_ = g_main_context_new()` 三处共用 |
| **24** | `stun-server` 只接受 IP / IPv6 优先 | `resolve_ipv4()` 强制 AF_INET |
| **31** | Trickle 漏交换 ufrag/pwd，候选全对但连不上 | `local_credentials()` / `set_remote_credentials()` |

---

## 模块 8：DTLS-SRTP

### 要实现什么

| 层 | 负责 | 不负责 |
|---|---|---|
| **DTLS** | 双向证书认证、密钥协商、导出 SRTP 密钥材料 | 不逐包加密媒体 |
| **SRTP** | 每个 RTP 包的机密性、完整性、重放保护 | 不做密钥协商 |

流程：DTLS 握手 → RFC 5764 exporter 导出 60 字节 → 切 key/salt →
建立 SRTP 会话 → 逐包 protect/unprotect。

### 代码在哪

| 功能 | 关键函数 | 位置 |
|---|---|---|
| 全局 `srtp_init` | 构造函数 | `dtls_srtp.cpp:53-63` |
| 自签名证书 | `create_self_signed_cert()` | `dtls_srtp.cpp:74-121` |
| 证书指纹 | `compute_fingerprint()` | `dtls_srtp.cpp:123-138` |
| 第一阶段：准备 | `prepare()` | `dtls_srtp.cpp:144-209` |
| 第二阶段：握手+验指纹 | `handshake(transport, peer_fp)` | `dtls_srtp.cpp:212-259` |
| 一步到位（测试用） | `handshake(transport, cfg)` | `dtls_srtp.cpp:262-265` |
| 握手循环 | `do_handshake_loop()` | `dtls_srtp.cpp:268-327` |
| 冲刷 wbio | `flush_output()` | `dtls_srtp.cpp:331-348` |
| 导出密钥+建 SRTP | `export_and_setup_srtp()` | `dtls_srtp.cpp:350-433` |
| 加密 / 解密 | `protect()` / `unprotect()` | `dtls_srtp.cpp:436-449` / `451-466` |
| 重读对端指纹 | `refresh_peer_fingerprint()` | `dtls_srtp.cpp:468-482` |

### 关键实现

**两阶段 API：指纹必须在握手前拿到**：

```
prepare() → local_fingerprint() → 写进 offer/answer 经信令发走
          → 收到对端 fingerprint
          → handshake(transport, peer_fp) → 校验 → 导出 SRTP 密钥
```

如果只在 `handshake()` 里创建证书，调用方交换 SDP 时拿不到自己的指纹，无法做防 MITM 的比对。

`prepare()` 做六件事：`SSL_CTX_new(DTLS_method())` + 最低 DTLS1.2；设置证书校验策略；
`SSL_CTX_set_tlsext_use_srtp(ctx_, "SRTP_AES128_CM_SHA1_80")`；生成自签名证书装入 ctx；
`SSL_new` + 两个内存 BIO（`rbio_` 收、`wbio_` 发）；设置 client/server 状态并计算
`local_fingerprint_`。

**证书校验：`SSL_VERIFY_PEER` + 永远返回 1 的 callback**：

```cpp
// dtls_srtp.cpp:160-176（节选）
// OpenSSL 的 DTLS server 默认不向 client 索要证书（不发 CertificateRequest），
// 所以 server 侧 SSL_get_peer_certificate() 会是 NULL。
SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, [](int, X509_STORE_CTX*) -> int {
    return 1;   // 跳过 CA 链校验；身份由业务层“指纹比对”保证
});
```

- **`SSL_VERIFY_PEER` 的作用是“请求对端证书”**。没有它，server 不发 CertificateRequest，
  client 不出示证书，server 读不到对端指纹（坑 26）。
- **不能用 `SSL_VERIFY_FAIL_IF_NO_PEER_CERT`**：自签名证书过不了 CA 校验，
  加上它会直接把握手拒掉。
- **真正的身份验证在握手后**：SHA-256 指纹比对。WebRTC 也是这个模型：不允许 CA，
  只信 `a=fingerprint`。

**握手循环：先收包，再推进状态机**：

```cpp
// dtls_srtp.cpp:272-295（节选）
while (now_ms() < deadline) {
    int got = t->recv(buf.data(), buf.size(), 30);      // (a) 先收
    if (got > 0) BIO_write(rbio_, buf.data(), got);
    else if (got < 0) return false;
    // got == 0（超时）不算错误：握手可能正在等对端

    int r = SSL_do_handshake(ssl_);                     // (b) 再推进
    if (r == 1) { flush_output(t); ... return true; }

    int err = SSL_get_error(ssl_, r);
    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) return false;

    flush_output(t);                                    // (c) 把待发记录发出去
}
```

- 顺序不能反：DTLS 是数据报协议，缺对端报文时 `SSL_do_handshake` 返回 `WANT_READ`；
  先收包再推进，才能在同一轮“喂数据+前进”。
- **`recv` 必须真的阻塞到 `timeout_ms`**：队列空就立即返回 0 会让外层忙等，
  在极短真实时间内耗光握手预算（坑 29）。e2e 的 `IceDtlsTransport::recv` 用 2ms 轮询+deadline。
- **握手完成后额外驱动几轮**：两侧完成时机不完全对称，server 侧对端证书有时刚完成时不可见；
  代码最多再跑 5 轮“收包+握手+flush”让证书到达。

**RFC 5764：60 字节怎么切**：

```cpp
// dtls_srtp.cpp:356-366（节选）
uint8_t material[60];
SSL_export_keying_material(ssl_, material, sizeof(material),
                           "EXTRACTOR-dtls_srtp", 22, nullptr, 0, 0);
client_key_.assign(material, material + 16);
server_key_.assign(material + 16, material + 32);
client_salt_.assign(material + 32, material + 46);
server_salt_.assign(material + 46, material + 60);
```

| 偏移 | 长度 | 字段 | client 角色 | server 角色 |
|---|---|---|---|---|
| 0–15 | 16 | `client_key` | **发送**加密 | **接收**解密 |
| 16–31 | 16 | `server_key` | **接收**解密 | **发送**加密 |
| 32–45 | 14 | `client_salt` | 发送 salt | 接收 salt |
| 46–59 | 14 | `server_salt` | 接收 salt | 发送 salt |

双方导出同一段 60 字节，但按角色取相反的一半。

**libsrtp2 的 `policy.key` 要求 key+salt 连续**：

```cpp
// dtls_srtp.cpp:382-391（节选）
const uint8_t* out_key  = is_client_ ? client_key_.data()  : server_key_.data();
const uint8_t* out_salt = is_client_ ? client_salt_.data() : server_salt_.data();
...
std::vector<uint8_t> out_material;
out_material.insert(out_material.end(), out_key, out_key + 16);
out_material.insert(out_material.end(), out_salt, out_salt + 14);   // 紧接 key
...
out_pol.key = out_material.data();   // libsrtp 从这一个指针连续读 30 字节
```

- `srtp_policy_t` 没有 `keysalt` 字段（想当然会编译报错）；
- AES-128-CM 要求 16B key + 14B salt 连续，libsrtp 从 `key` 指针一次读 30B；
- `srtp_t` 本身就是指针类型；`srtp_create(&srtp_in_, &policy)` 传二级指针（坑 25）。

策略：`srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80` 同时设 RTP/RTCP；
`ssrc.type = ssrc_any_outbound/inbound`；`window_size = 128`。
`ssrc_any_*` 很关键：媒体流（session_id=1）和 FEC 流（session_id=2）是两个 SSRC，
同一对 SRTP 会话都要能处理。

**`protect` / `unprotect`：就地加解密**：

```cpp
// dtls_srtp.cpp:439-448（节选）
int out_len = *len;
srtp_err_status_t st = srtp_protect(srtp_out_, rtp, &out_len);
if (st != srtp_err_status_ok) { stats_.protect_failed++; return false; }
*len = out_len;
stats_.protected_pkts++;
```

- 就地修改：调用方必须保证缓冲区尾部有空间放认证标签（SHA1-80 是 10B）；
- 长度会变：加密后 `*len` 增加 10；
- 解密失败不是致命错误：可能丢包/篡改/密钥不匹配，只记统计；
- 必须在 `Packet::parse()` 之前解密：自定义 8 字节头在 SRTP 看来是载荷，已被加密。

### 设计取舍

| 决策 | 选择 | 代价 | 不这么做会怎样 |
|---|---|---|---|
| 密钥协商 | DTLS-SRTP（RFC 5764） | 握手多几个 RTT | SDES 把密钥写进 SDP，信令被窃听就全泄露 |
| 身份 | 自签名证书+指纹比对 | 依赖信令安全地传指纹 | CA 链在 WebRTC 场景不可用 |
| 证书算法 | RSA 2048 | 生成/握手比 ECDSA 慢 | ECDSA P-256 更快更小，但代码更长 |
| SRTP 套件 | AES-128-CM + HMAC-SHA1-80 | 比 GCM 老，兼容性最好 | AES-GCM 更快，但 profile 协商/实现不同 |
| 加解密 | 就地修改 | 调用方要预留尾部空间 | 拷贝一份更安全但多一次内存操作 |
| 明文信令 | `ws://` | 指纹可被中间人替换 | 生产必须 `wss://`+认证，否则 DTLS 防不了信令层 MITM |

> **诚实边界**：DTLS-SRTP 保护“媒体面”，它假设“信令面已经可信”。
> 本项目用 `ws://` 交换指纹，若信令被 MITM，攻击者可同时替换双方指纹。WebRTC 也假设 SDP 经安全信令传输。

### 对应踩坑

| 坑 | 一句话 | 落点 |
|---|---|---|
| **25** | libsrtp2 三处 API 不符预期（`srtp_t` 是指针、无 `keysalt`、most vexing parse） | `export_and_setup_srtp()` / `flush_output()` |
| **26** | DTLS server 默认不请求对端证书 | `SSL_VERIFY_PEER` + 放行 callback + 指纹比对 |
| **27** | 测试宏二次求值，全 PASS 却报失败 | e2e/selftest 的 `CHECK` 先存局部变量 |
| **29** | 接线顺序错 + `recv` 忙等 | 先注册 ICE 回调再起握手；`recv` 真等待 |

---

## 模块 9：弱网对抗（NACK / FEC / GCC）与指标

### 要实现什么

| 问题 | 时间尺度 | 手段 |
|---|---|---|
| 个别包丢了 | 一个 RTT | **NACK**：请求重传 |
| 丢包但不想等 RTT | 0 额外延迟 | **FEC**：冗余包本地恢复 |
| 链路持续排队/过载 | 几百 ms~几秒 | **GCC**：延迟梯度 + AIMD 调码率 |
| “到底哪一层坏了” | 全程 | **Metrics**：分层统计 + 跨层自洽 |

### 代码在哪

| 功能 | 关键函数 | 位置 |
|---|---|---|
| NACK 收包/检测 | `on_packet()` | `loss_protection.cpp:22-93` |
| NACK 取请求列表 | `take_nack_list()` | `loss_protection.cpp:95-116` |
| NACK 快照 | `snapshot()` | `loss_protection.cpp:118-123` |
| FEC 编码 | `add_source()` | `loss_protection.cpp:150-187` |
| FEC 记录源包 / 恢复 | `on_source()` / `on_fec()` | `loss_protection.cpp:192-195` / `197-253` |
| 延迟梯度 | `on_arrival()` | `loss_protection.cpp:291-316` |
| 码率决策 | `update_bitrate()` | `loss_protection.cpp:323-390` |
| 派生指标 | `CallMetrics::*` | `metrics.cpp:14-56` |
| 指标上报/快照/报告 | `on_*` / `snapshot()` / `to_string()` | `metrics.cpp:182-281` / `283-289` / `58-166` |

### 9.1 NACK：检测、重试、放弃

| 参数 | 默认 | 含义 | 为什么 |
|---|---|---|---|
| `max_nack_retries` | 10 | 同一包最多请求 10 次 | 防止无限重试占带宽 |
| `retry_interval_ms` | 40 | 重试间隔 | 约 1 个 RTT；太密浪费，太疏恢复慢 |
| `max_nack_age_ms` | 300 | 超时放弃 | 再重传也赶不上播放，交给 PLC 更划算 |
| `max_packets_in_window` | 2048 | 太旧的包直接丢 | 防止序号回绕/迟到包污染统计 |

`on_packet()` 流程：重复包拒绝 → 落后超过 2048 标记 `too_old` → 首包初始化 →
序号前进时把空洞登记到 `pending_` → 乱序补到时 `recovered++` → 清理超龄 pending →
更新 `lost_detected` / `loss_rate`。

```cpp
// loss_protection.cpp:99-109（节选）
for (auto& kv : pending_) {
    Pending& p = kv.second;
    if (p.retries >= cfg_.max_nack_retries) continue;
    if (p.last_request_ms != 0 &&
        (now - p.last_request_ms) < cfg_.retry_interval_ms) continue;
    out.push_back(p.seq);
    p.last_request_ms = now;   // 本次请求时间
    p.retries++;               // 次数 +1
}
```

**为什么 40ms/10 次/300ms**：40ms 接近一个 RTT，给“收 NACK→重传→到达”留时间；
10 次避免高丢包时把带宽全花在重传；300ms 是“播放还值得等”的上限，超过后让抖动缓冲走 PLC。
**这是“延迟 vs 完整性”的取舍**：NACK 追求完整，PLC 追求连续。

**两个语义边界**：

- `stats_.recovered` 统计“这个包最终到了”，**包括乱序补到，不一定是重传**。
  指标里的“NACK 恢复”应理解为“通过等待/重传恢复”。
- e2e 里 `NackRequester` 只做检测/统计，**没有把 NACK 列表编码成报文回传**；
  NACK 状态机（请求/重试/放弃/补到）在 `tests/loss_protection_test.cpp:28-75` 验证，
  但完整的“NACK 报文收发 + 发送端重传”通道本项目未实现，e2e 的恢复主要靠 FEC。

### 9.2 XOR FEC：为什么 (k=4, m=1) 只能防单点丢包

XOR 的本质：**m 个冗余包 = m 个独立方程**。解 d 个丢失源包，至少需要 d 个方程。

| 每组丢失数 | m=1 能否恢复 | 原因 |
|---|---|---|
| 0 | 不需要 | 全到了 |
| **1** | ✅ | 一个未知数，一个方程 |
| 2 | ❌ | 两个未知数，一个方程，解不唯一 |
| ≥3 | ❌ | 同上 |

所以 (k=4, m=1) 的能力是“**每组最多恢复 1 个**”，冗余率 1/4=25%。
要防连续丢包需要 m≥2 的独立校验包（如 Reed-Solomon），或把源包交错到不同组。

编码格式：`[u16 base_seq][u16 count][u8 len...][XOR payload]`。
XOR 载荷取组内最大长度，短包**补零**后异或；每个源包原始长度必须记录，恢复后按原长度截断。

```cpp
// loss_protection.cpp:224-242（节选）
if (missing.size() == 1) {
    std::vector<uint8_t> rec(fec_pkt + hdr, fec_pkt + hdr + max_len);
    for (size_t i = 0; i < count; i++) {
        const uint16_t s = uint16_t(base_seq + i);
        if (s == missing[0]) continue;
        auto it = src_.find(s);
        if (it == src_.end()) continue;
        for (size_t b = 0; b < it->second.size(); b++) rec[b] ^= it->second[b];
    }
    const uint16_t need_len = fec_pkt[4 + (missing[0] - base_seq)];
    if (rec.size() > need_len) rec.resize(need_len);
    ...
}
```

**动态冗余率**（e2e，`e2e_test.cpp:333-341`）：

| 丢包率 | k | 冗余开销 | 取舍 |
|---|---|---|---|
| <5% | 8 | 12.5% | 主要靠 NACK，少发冗余 |
| 5%~15% | 4 | 25% | 平衡 |
| >15% | 2 | 50% | 优先保连续，带宽让位 |

**实现边界**：FEC 头里每个源包长度是 `u8`（`loss_protection.cpp:176`），
组内单包载荷必须 ≤255B。60ms 聚合在 16kbps 下约 120B 没问题；
但码率超过约 **34kbps**（255B/0.06s）后长度截断，恢复出的包长度不对。
生产实现应改成 `u16`。

### 9.3 GCC：延迟梯度 + AIMD

延迟梯度的物理含义：**接收端看到的“到达间隔”比发送端的“发送间隔”多出来的部分，
就是这段时间在队列中累积的排队延迟。**

```cpp
// loss_protection.cpp:305-315（节选）
// d(i) = (recv_i - recv_0) - (send_i - send_0)
const int64_t rel_recv = recv_ms - base_recv_ms_;
const int64_t rel_send = send_ms - base_send_ms_;
const double d = double(rel_recv - rel_send);

trend_ = 0.9 * trend_ + 0.1 * d;   // 指数平滑，抑制单包噪声
```

基准是第一个样本，所以 `d` 是“从开始到现在累积的排队偏移”，不是相邻两包瞬时差。

| 状态 | 条件 | 动作 | 为什么 |
|---|---|---|---|
| **OVERUSE** | `trend_ > +20ms` 或丢包率 >10% | `target *= 0.85`（乘性降） | 快速退让，避免队列继续堆积 |
| **UNDERUSE** | `trend_ < -20ms` | 不升 | 链路刚空出来，立即升容易再次过载 |
| **NORMAL** | 其他 | `target += target * 8%` | 缓慢试探，发现新带宽 |

- 降完把 `trend_=0`，避免同一个排队事件连续触发降码率；
- 限幅 `[6000, 64000]`（e2e 上限 48000）；
- 丢包率 >10% 作为事后兜底：延迟梯度是早期信号，丢包是晚期信号。

**坑 28**：早期在构造函数里 `last_update_ms_ = now_ms()`，测试用逻辑时间 `0,20,40...`，
`now - last_update` 恒为负 → 所有更新被 100ms 节流掉，表现为“梯度 320ms，码率纹丝不动”。
修复是惰性初始化：

```cpp
// loss_protection.cpp:334-338
if (last_update_ms_ == 0) {
    last_update_ms_ = now;      // 首次调用建立基准
    return target_bps_;
}
```

**两个诚实的说明**：头文件注释说“最小二乘趋势线”，但代码实际是“累积偏移的 EWMA”，
不是 WebRTC 的滑动窗口最小二乘；对“路由切换导致的阶跃延迟”会持续误判，
靠降码率后重置 `trend_` 缓解。另外“加性升”在代码里是 `+8% 当前码率`，
严格说是乘性增（AIMD 的 A 通常是固定增量）；好处是不同码率档位增长速度一致。

### 9.4 指标：分层统计 + 跨层自洽

| 层 | 关键字段 | 回答什么 |
|---|---|---|
| 发送 | `frames_captured/encoded`、`packets_sent`、`fec_packets_sent` | 采集/编码/发包是否正常 |
| 字节 | `payload_bytes`、`wire_bytes` | 编码效率 vs 网络开销 |
| 接收 | `packets_received`、`packets_lost`、`packets_recovered_*` | 丢多少、恢复多少 |
| 抖动缓冲 | `jb_received/played/plc/dropped_late`、`jb_target_delay_ms` | 卡顿是网络还是缓冲 |
| 编解码 | `frames_decoded`、`decode_errors` | 解码是否失败 |
| 加密 | `srtp_protected/unprotected/failures` | 加解密是否逐包对应 |
| 自适应 | `target_bitrate_bps`、`delay_gradient_ms`、`rtt_ms` | GCC 是否在工作 |
| ICE | `ice_tx/rx`、`ice_connected` | 通道是否通 |

**两个字节计数器必须分开**（`metrics.cpp:187-198`）：`on_frame_encoded()` 累加
`payload_bytes`（编码载荷）；`on_packet_sent()` 累加 `wire_bytes`（网络整包）。
混用一个 `bytes_sent` 会算出既不是编码码率、也不是网络码率的数字（坑 33）。
拆开后：`actual_bitrate_bps()` 按 payload 算；`wire_bitrate_bps()` 按 wire 算；
`overhead_ratio()` = wire/payload，正常约 1.2~1.4。
注意 `on_packets_lost()` / `on_recovered_*()` 是**覆盖式赋值**，不是累加；
调用方要传“累计值”（e2e 在结束时传 `nst.lost_detected`），不能每包传增量。

**恢复率的分母只能是检测到的丢包**（`metrics.cpp:38-39`）：

```cpp
if (packets_lost == 0) return 1.0;   // 无丢包视为 100%
return double(packets_recovered_nack + packets_recovered_fec) / double(packets_lost);
```

坑 32 的错误写法是 `recovered / (lost + recovered)`：`recovered` 是 `lost` 的子集，
加到分母上等于把丢包数翻倍，8/8 算成 50%。`still_missing()` 明确“最终仍缺失”：

```cpp
return packets_lost > recovered ? packets_lost - recovered : 0;
```

**零值语义**：无丢包 → 恢复率 100%；无播放 → 连续性 100%；无编码 → 成功率 100%。
监控里“无数据被算成 0%”是最常见的误报警来源。

**时间必须可注入**：`snapshot(now=-1)` 用调用方时间算 `duration_ms`。
坑 32(b)：测试用逻辑时间 0，`snapshot()` 却用真实时钟，算出 `duration=3699778ms`、码率 49bps；
修复后 `snapshot(kLogicalNow)` 得到 5000ms、36704bps。
注意 `MetricsCollector` 构造函数仍 `start_ms_ = now_ms()`，测试必须显式 `mark_start(0)`。

**跨层自洽校验**（`e2e_test.cpp:513-537`）：

| 校验 | 说明 | 不成立说明什么 |
|---|---|---|
| `srtp_protected == srtp_unprotected`（无丢包） | 加密多少解密多少 | 有包没走 SRTP/解密失败/丢包 |
| `packets_sent + fec_packets_sent == srtp_protected` | 媒体+FEC 都加密了 | 有分支绕过加密 |
| `target_bitrate_bps == gcc 输出` | 指标里是闭环真实值 | 指标与控制器脱节 |
| `target_bitrate_bps ∈ [6000,48000]` | 限幅生效 | 控制器输出越界 |
| 解码最大幅度 >1000 | 真的解出音频 | 只解出静音/PLC 舒适噪声 |
| `recovery_rate ∈ [0,1]` | 派生指标合法 | 分母/计数口径错误 |

**两个“丢包率”不要混用**：

| 来源 | 口径 | 代码 | 用途 |
|---|---|---|---|
| `NackRequester::loss_rate` | **包**：`(span-arrived)/span` | `loss_protection.cpp:118-123` | 重传/恢复决策 |
| `MetricsCollector::loss_rate` | **帧**：`plc/(received+plc)` | `metrics.cpp:239-242` | 展示“PLC 比例” |

`to_string()` 里“检测丢包”来自 NACK（包口径），“丢包率”却来自 JB（帧口径），
两者不必逐位相等；核对丢包请用“检测丢包”与实际丢弃数对，不要看百分比。

### 设计取舍

| 决策 | 选择 | 代价 | 不这么做会怎样 |
|---|---|---|---|
| NACK vs FEC | 按丢包率动态组合 | 两套逻辑/统计 | 全开浪费带宽；全关弱网差 |
| FEC 算法 | XOR (k,1) | 只能防单点丢包 | Reed-Solomon 能防多点，但实现复杂 |
| 拥塞信号 | 延迟梯度为主，丢包兜底 | 需要准确时间戳/到达时间 | 只看丢包：反应太晚，已经卡了 |
| 码率调整 | AIMD（×0.85 / +8%） | 收敛速度一般 | 直接跳变：容易过冲/震荡 |
| 指标模型 | 各模块 push 到 collector | 调用方要记得上报 | pull（getStats）更自动，但要维护状态机 |

### 对应踩坑

| 坑 | 一句话 | 落点 |
|---|---|---|
| **28** | 构造函数用真实时钟做节流基准，仿真中码率永不更新 | `last_update_ms_=0` + 惰性初始化 |
| **32** | 恢复率分母错 + 指标时长耦合真实时钟 | 只用 `packets_lost`；`snapshot(now)` |
| **33** | 载荷/网络字节混用一个变量，码率虚高 73% | `payload_bytes` / `wire_bytes` 分开 |
| **34** | FEC 包污染 NACK 统计，丢包少报 | 入口按 `session_id` 分流 |

---

## 模块 10：通话编排（把 9 个模块接成真正的通话）—— `src/app/call_session.*`

> 前面 9 个模块都是"零件"。这个模块负责**把它们接线成一台能打电话的机器**。
>
> 这也是"实时通话工具"和"一堆测试程序"的分界线：
> `--peer-test` 只验证信令配对；**`--call` 才是真的两端互相传音频**。

### 要实现什么

两个**独立进程**，各自采音、编码、加密、走 ICE 发出去，
并解码对方音频播出来：

```
A 进程: 采集 -> Opus -> 打包 -> SRTP 加密 -> ICE(UDP) ──┐
                                                      │ 真实网络
B 进程: 采集 -> Opus -> 打包 -> SRTP 加密 -> ICE(UDP) ──┘
                                                      ↓
A 进程: ICE 收到 <- SRTP 解密 <- 抖动缓冲 <- 解码 <- 播放（听到 B）
B 进程: ICE 收到 <- SRTP 解密 <- 抖动缓冲 <- 解码 <- 播放（听到 A）
```

### 代码在哪

| 功能 | 位置 |
|---|---|
| **`run_call()` 六阶段主流程** | `call_session.cpp:89` |
| **`IceDtlsTransport` 适配器**（ICE 包装成 DTLS 传输层） | `call_session.cpp:33` |
| **`looks_like_rtp()` SRTP/DTLS 分流** | `call_session.cpp:80` |
| ICE 接收回调（解密后注入管线） | `call_session.cpp:330` |
| `pipe.set_send_fn()` 发送路径接线 | `call_session.cpp:394` |
| DTLS 两阶段调用（prepare / handshake） | `call_session.cpp:216` / `:359` |

### 六个阶段（WebRTC 建链的完整缩影）

```cpp
int run_call(const CallOptions& opt) {
    // 1/6 信令连接 + 入房 + 等配对
    // 2/6 ICE 初始化（开始收集候选）+ DTLS prepare（拿到自己的指纹）
    // 3/6 offer/answer 交换（携带 ICE 凭证 + DTLS 指纹）
    // 4/6 应用远端描述 + 候选，等 ICE 连通
    // 5/6 DTLS 握手 + 指纹校验 + SRTP 密钥派生
    // 6/6 启动音频管线，双向传输
}
```

**为什么必须是这个顺序**：

| 阶段 | 前置依赖 | 顺序错了会怎样 |
|---|---|---|
| ICE 候选收集 | 无 | — |
| DTLS prepare（拿指纹） | 无 | 拿不到指纹 → offer 里没有指纹 → 无法防 MITM |
| offer/answer | ICE 凭证 + DTLS 指纹 | 对端无法建链 |
| ICE 连通 | 双方凭证 + 候选 | 通道不通，DTLS 无处可跑 |
| DTLS 握手 | ICE 通道 + 对端指纹 | 无法校验身份 |
| 媒体传输 | SRTP 密钥 | 无法加解密 |

### 关键实现

**① 角色判定：先入房者为主叫**

```cpp
// 服务器在 joined 消息里给出房间成员数（复用 sdp 字段传计数）
// ★ 注意读的是 m.sdp，不是 m.reason（reason 是空的，会导致两端都判成被叫）
my_member_count = atoi(m.sdp.c_str());
const bool is_caller = (my_member_count == 1);   // 1 = 先到 = 主叫
```

好处：**天然避免双方同时发 offer 的 glare 问题**，
不需要 WebRTC 那套 polite/impolite peer 协商。
DTLS 角色跟着定：主叫 = client/active，被叫 = server/passive。

**② SRTP 与 DTLS 的分流**

```cpp
// call_session.cpp:80
bool looks_like_rtp(const uint8_t* d, size_t n) {
    return n >= 12 && (d[0] & 0xC0) == 0x80;   // RTP 版本位 = 2
}
// DTLS 记录首字节是 20~23（change_cipher_spec/alert/handshake/app_data）
```

两者首字节空间不重叠，可以**可靠分流**。

**③ 接线顺序是成败关键（最容易错的一步）**

```cpp
// ★ 必须"先接接收回调，再握手"
ice.set_on_recv([&](const uint8_t* data, size_t len) {
    if (!looks_like_rtp(data, len)) {
        dtls_transport.feed(data, len);   // DTLS 记录 → 喂给握手
        return;
    }
    ...  // SRTP 媒体包 → 解密 → 注入管线
});

// 然后才握手（双方并发，DTLS 是双向多轮协议）
dtls.handshake(&dtls_transport, remote_desc.fingerprint);
```

顺序错了 → 握手期间的 DTLS 记录无人接收 → **必然超时**（坑 29）。

**④ 复用而非重写媒体逻辑**

```cpp
// 发送：把管线产出的包加密后发走（call_session.cpp:394）
pipe.set_send_fn([&](const PacketHeader& h, const std::vector<uint8_t>& payload) {
    uint8_t buf[2048];
    int n = Packet::serialize(h, payload.data(), payload.size(), buf, sizeof(buf));
    int len = n;
    if (!dtls.protect(buf, &len)) return;     // SRTP 加密
    ice.send(buf, size_t(len));               // 走 ICE 通道
});

// 接收：解密后注入管线
pipe.on_media_packet(ph, payload);
```

**媒体管线（采集/编码/抖动缓冲/解码/播放）一行没改**——
只替换了"发送出口"和"接收入口"。这就是 `AudioPipeline::SendFn`
回调设计的价值：同一套已验证的媒体代码，loopback 和真实通话都能用。

**⑤ `IceDtlsTransport` 适配器**

```cpp
// call_session.cpp:33
class IceDtlsTransport : public DtlsTransport {
    int send(...) override { return ice_->send(data, len) ? len : -1; }
    int recv(uint8_t* buf, size_t cap, int timeout_ms) override {
        // ★ 必须真正等待 timeout_ms
        const int64_t deadline = now_ms() + timeout_ms;
        while (now_ms() <= deadline) { /* 有数据就返回 */ sleep 2ms; }
        return 0;   // 真超时
    }
    void feed(...);   // ICE 收到 DTLS 记录后塞进队列
};
```

**为什么 `recv` 必须真正阻塞**：若"队列空就立刻返回 0"，
DTLS 握手循环会变成**忙等**，在极短的真实时间内耗光逻辑超时预算
→ 握手必然失败（坑 29c）。

### 实测结果（真实双进程）

```
A 端: 发送 134 包 / 接收 134 包 / SRTP 解密 134 包（失败 0）
B 端: 发送 136 包 / 接收 134 包 / SRTP 解密 134 包（失败 0）
两端退出码 = 0
```

**决定性验证：证明"听到的是对方"而不是自环**

让 A 采扫频（200→3000Hz）、B 采单音（440Hz），分析各自录到的主导频率：

| 端点 | 采集源 | 录到的主导频率 | 结论 |
|---|---|---|---|
| A | 扫频 200→3000Hz | **440 Hz** | A 录到的是 **B** 的声音 |
| B | 单音 440Hz | **2400 Hz**（扫频中段） | B 录到的是 **A** 的声音 |

**频率交叉验证**是这里最硬的证据：如果两端只是各自 loopback，
A 的录音里应该是扫频，而不是 440Hz。

### 设计取舍

| 决策 | 选择 | 代价 | 不这么做会怎样 |
|---|---|---|---|
| 角色判定 | 先入房者为主叫 | 依赖服务器告知成员数 | 双方都发 offer → glare 冲突 |
| DTLS 角色 | 主叫=client | 与 ICE controlling 绑定 | 双方同角色 → 握手卡住 |
| 媒体路径 | 复用 AudioPipeline | 需要 SendFn 回调设计 | 重写一套 → 重复且易错 |
| 传输适配 | IceDtlsTransport 薄封装 | 多一层间接 | DTLS 直接调 ICE API → 耦合 |
| 分流依据 | RTP 版本位 | 依赖包头与 RTP 兼容 | 自定义字段判断 → 脆弱（坑 30） |

### 对应踩坑

> 注：坑 37/38 是这批并行修复在代码注释里的编号；`docs/02-pitfalls.md` 当前收录到坑 36，
> 编号可能随后补齐（以 `grep -n "坑 37" src/` 为准）。

| 坑 | 一句话 | 落点 |
|---|---|---|
| **29** ⭐ | 接线顺序错 → DTLS 握手必然超时 | 先 `set_on_recv` 再 `handshake`；双向都接；`recv` 真等待 |
| **30** ⭐ | 自定义头不兼容 RTP → SRTP 完全不处理 | `Packet` 前 12 字节对齐 RTP 固定头 |
| **31** | Trickle 漏交换 ufrag/pwd → 候选全对但连不上 | `SignalMsg` 的 `ice_ufrag`/`ice_pwd` 独立字段 |
| **37** | Trickle 候选重复发送 6 倍 | `sent_candidates_` 去重 |
| **38** | 回调注册晚于候选产生 → 候选丢失 | `early_candidates_` 缓冲 + 注册时补发 |

### 这一节最值得记住的一句话

> **分工件的模块能跑通，不等于接起来能通话。**
> 集成阶段最容易错的三件事：**接线顺序**、**双向都接**、**阻塞语义**。

---

## 跨模块的三个核心设计

### 1. 时间注入：为什么所有算法组件都能用逻辑时间驱动

统一约定：**`now=-1` 取真实时钟；`now>=0` 用调用方时间。**

| 组件 | 可注入接口 | 位置 |
|---|---|---|
| 抖动缓冲 | `push(..., arrive_ms=-1)` / `pop(now=-1)` / `ready_to_play(now=-1)` | `jitter_buffer.h:50-66` |
| NACK | `on_packet(seq, now=-1)` / `take_nack_list(now=-1)` | `loss_protection.h:56-62` |
| GCC | `update_bitrate(now=-1)`；`on_arrival(send,recv)` 显式传时间 | `loss_protection.h:195-212` |
| 指标 | `snapshot(now=-1)` / `mark_start(now=-1)` / `report(now=-1)` | `metrics.h:128-138` |

为什么必须这样（坑 9/28/32）：**可测试**（10 秒仿真只花几十毫秒真实时间）、
**可复现**（不受机器负载影响）、**可回放**（用包里的时间戳驱动）。

两个配套原则：

1. **初始化基准也不能取真实时钟**。坑 28 是构造函数取了一次 `now_ms()`，
   后续所有逻辑时间比较失效；正确做法是惰性初始化。
2. **定时用绝对时间点**。`sleep_until` / `wait_until` 不累积误差；`sleep_for` / `wait_for` 会。

例外：DTLS 握手、ICE 等待这类 I/O 超时用真实时钟，因为它们等的是真实世界事件。

### 2. 锁边界：`_locked` 后缀约定

| 锁 | 保护什么 | 位置 |
|---|---|---|
| `AudioPipeline::send_mutex_` | `pending_frames_` + 条件变量 | `audio_pipeline.h:108-110` |
| `AudioPipeline::stat_mutex_` | `Report report_` | `audio_pipeline.h:101-102` |
| `VirtualAudioDevice::Impl::play_mutex` | 播放缓冲+统计 | `virtual_device.cpp:34-40` |
| `SignalingClient::tx_mutex_` | 发送队列 | `signaling_client.h:91-92` |
| `MetricsCollector::m_` | 所有指标计数 | `metrics.h:141-143` |

约定：公共方法自己加锁（`bitrate_kbps()`、`report()`）；`_locked` 后缀 = 调用方必须已持锁
（`bitrate_kbps_locked()`）。理由：`std::mutex` 非递归，同一线程重复 lock 永久阻塞（坑 12）。

| 方案 | 优点 | 缺点 |
|---|---|---|
| `_locked` 约定（本项目） | 零开销、意图明确 | 靠命名/code review，无编译期保证 |
| `recursive_mutex` | 不会自死锁 | 掩盖锁设计问题；持锁时间被拉长 |
| 缩小锁粒度 | 并发性更好 | 改动大，容易引入新竞态 |

**两个残留边界**：

1. `stat_mutex_` 保护 `report_`，但 `bitrate_kbps_locked()` 读的
   `codec_.avg_encoded_bytes()` 由采集线程在 `encode()` 里更新，**没有**被这把锁保护。
   严格说是数据竞争；生产化应把直方图快照放进 `report_`，或给 `codec_` 单独加锁。
2. `JitterBuffer` 内部完全不加锁，设计假设“push/pop 同一 owner 串行调用”。
   `AudioPipeline` 目前在网络回调线程 push、播放线程 pop，集成层需要补锁/队列。
   这是“可测试性优先”的代价：算法组件保持纯粹，线程安全交给编排层。

### 3. 序号 / SSRC 的空间隔离

| 空间 | 标识 | 步进 | 谁在用 | 混用后果 |
|---|---|---|---|---|
| **RTP 媒体包序号** | `PacketHeader.seq` | 60ms +1，16 位回绕 | `Packet`、SRTP、NACK（session 1） | 与 FEC 混用 → 丢包统计乱（坑 34） |
| **JB 帧序号** | `h.seq * frames.size() + i` | 20ms +1（虚拟） | `JitterBuffer` | 与包序号对比 → 数字差 3 倍 |
| **FEC 流** | `session_id=2`（独立 SSRC） | 复用媒体 seq 值，SSRC 不同 | `FecEncoder/Decoder`、SRTP | 用“seq+10000” → SRTP 判重放、虚高（坑 30） |
| **ICE stream/component** | `stream_id=1, component=1` | 固定 | libnice | 多流会串 |
| **信令 peer/room** | `peer_id` / `room` | 服务器分配 | 信令 | 与媒体 SSRC 无关 |

三条规则：

1. **多流分隔用独立 SSRC，不要用序号偏移。** SRTP 重放窗口按 SSRC 维护；
   同一 SSRC 先跳 10000 再回落会被判重放。
2. **在入口处按 `session_id` 分流，不要在出口处过滤。** FEC 包只喂 `fec_dec`，
   媒体包才喂 `nack` / `jitter_buffer`（坑 34）。
3. **16 位序号要处理回绕**：`seq_diff` 用 `int16`；长通话还需要序号周期扩展。

---

## 面试速查：15 个高频问题的代码位置

| # | 问题 | 一句话答案 | 代码位置 | 坑 |
|---|---|---|---|---|
| 1 | 为什么 16k/20ms/VOIP？ | 语音 4k 足够；20ms 折中延迟/包头；VOIP 针对语音 | `opus_codec.h:20-25` / `opus_codec.cpp:23-74` | — |
| 2 | 为什么 60ms 聚合？ | 头开销 57%→22%，代价最多 40ms；3 帧是拐点 | `audio_pipeline.cpp:60-140` / `opus_codec.cpp:145-163` | — |
| 3 | 抖动缓冲怎么区分乱序/迟到？ | 只认 `play_seq_` 判迟到；最高/最低只做统计 | `jitter_buffer.cpp:107-118` | 1 |
| 4 | 目标延迟怎么定？ | `max(jitter*2, recent_max*1.5)+frame`；上调快、下调慢 | `jitter_buffer.cpp:57-83` | 10 |
| 5 | 丢包数怎么算？ | 序号跨度−独立到达数；集合口径，与顺序无关 | `jitter_buffer.cpp:218-234` | 11 |
| 6 | PLC 何时触发？ | 过了“deadline+target+frame”还没来 → 空 payload | `jitter_buffer.cpp:165-183` | 10 |
| 7 | 包头为什么兼容 RTP？ | SRTP 按 RTP 解析；首字节必须 0x80 | `packet.cpp:62-94` / `96-137` | 30 |
| 8 | session_id/SSRC？FEC 为何独立？ | 低 16 位映射 SSRC；FEC 用 SSRC=2 | `packet.cpp:82-83` / `e2e_test.cpp:367-397` | 30/34 |
| 9 | 信令发送队列为何 deque？ | 同一次处理连发 joined+peer_ready，单槽会覆盖 | `signal_server.cpp:63-70` / `270-296` | 19/20 |
| 10 | `LWS_PRE` 是什么？ | lws 在 payload 前写协议头，必须预留 | `signal_server.cpp:281-284` / `signaling_client.cpp:257-260` | 18 |
| 11 | user data 为何只能 POD？ | lws 用 malloc 分配裸内存，不调构造函数 | `signaling_client.cpp:26-28` | 17 |
| 12 | 为什么共用一个 GMainContext？ | libnice socket source 挂在创建 agent 的 ctx 上 | `ice_transport.cpp:231-238` / `294` / `324-325` | 23 |
| 13 | STUN 为何只接受 IP？ | libnice 0.1.16 只认 IP，域名静默忽略；还要强制 IPv4 | `ice_transport.cpp:28-46` / `250-262` | 24 |
| 14 | Trickle 为何还要 ufrag/pwd？ | 候选片段不含凭证，STUN 认证需要 | `ice_transport.cpp:371-386` | 31 |
| 15 | DTLS/SRTP 分工？60B 怎么切？ | DTLS 协商密钥；SRTP 逐包加密；16/16/14/14 | `dtls_srtp.cpp:350-433` / `436-466` | 25/26 |

**三个追问级问题**：

- **“本机目标延迟为什么 80ms？”** 3 帧聚合同时到达，抖动估计把间隔算成 `0/0/60`，
  估计抖动 ~26.7ms，公式给出 80ms；是聚合粒度与抖动估计口径错位，不是网络问题。
- **“JitterBuffer 线程安全吗？”** 内部不加锁，要求 push/pop 串行；当前管线在网络回调线程 push、
  播放线程 pop，集成层需补锁/队列。这是算法纯粹性 vs 线程安全的取舍。
- **“NACK 真的重传了吗？”** 检测/请求/重试/放弃已实现，状态机在 `loss_protection_test` 验证；
  但完整 NACK 报文收发/发送端重传通道未实现，e2e 只做统计，恢复主要靠 FEC。

---

## 附录 A：模块 / 文件 / 关键函数总表

| 模块 | 文件 | 关键函数 | 行号 |
|---|---|---|---|
| Opus | `media/opus_codec.cpp` | `init` / `encode` / `decode` / `set_bitrate` / `pack_frames` / `unpack_frames` | 23-74 / 96-117 / 119-131 / 80-94 / 145-163 / 165-182 |
| 抖动缓冲 | `media/jitter_buffer.cpp` | `push` / `pop` / `ready_to_play` / `update_jitter` / `adapt_target_delay` / `snapshot` | 85-141 / 151-195 / 143-149 / 32-55 / 57-83 / 218-234 |
| 虚拟设备 | `media/virtual_device.cpp` | `start_capture` / `start_playout` / `play` / `play_stats` | 111-154 / 160-199 / 205-225 / 227-235 |
| 音频管线 | `media/audio_pipeline.cpp` | `send_loop` / `on_media_packet` / `playout_loop` / `bitrate_kbps_locked` / `report` | 60-140 / 143-174 / 190-229 / 249-254 / 266-272 |
| 包格式 | `protocol/packet.cpp` | `ext_crc` / `serialize` / `parse` | 28-57 / 62-94 / 96-137 |
| 信令服务器 | `signal/signal_server.cpp` | `queue_text` / `handle_join` / `handle_forward` / `callback_ws` | 63-70 / 113-165 / 167-192 / 237-315 |
| 信令客户端 | `signal/signaling_client.cpp` | `connect` / `send` / `pop_outgoing` / `service_loop` / `client_callback` | 40-72 / 85-95 / 113-119 / 153-212 / 216-290 |
| ICE | `ice/ice_transport.cpp` | `init` / `set_remote_sdp` / `add_remote_candidate_sdp` / `send` | 210-313 / 350-369 / 393-420 / 423-434 |
| DTLS-SRTP | `crypto/dtls_srtp.cpp` | `prepare` / `handshake` / `export_and_setup_srtp` / `protect` / `unprotect` | 144-209 / 212-259 / 350-433 / 436-449 / 451-466 |
| NACK/FEC/GCC | `net/loss_protection.cpp` | `on_packet` / `take_nack_list` / `add_source` / `on_fec` / `on_arrival` / `update_bitrate` | 22-93 / 95-116 / 150-187 / 197-253 / 291-316 / 323-390 |
| 指标 | `metrics/metrics.cpp` | `recovery_rate` / `snapshot` / `on_*` | 29-40 / 283-289 / 182-281 |

## 附录 B：行号复核命令

```bash
cd /home/ts/rtc-1v1-audio

grep -n "JitterBuffer::\(push\|pop\|snapshot\|adapt_target_delay\)" src/media/jitter_buffer.cpp
grep -n "OpusCodec::\(init\|encode\|decode\|set_bitrate\|pack_frames\|unpack_frames\)" src/media/opus_codec.cpp
grep -n "Packet::\(serialize\|parse\|ext_crc\)" src/protocol/packet.cpp
grep -n "IceTransport::\(init\|set_remote_sdp\|add_remote_candidate_sdp\|local_credentials\)" src/ice/ice_transport.cpp
grep -n "DtlsSrtpSession::\(prepare\|handshake\|export_and_setup_srtp\|protect\|unprotect\)" src/crypto/dtls_srtp.cpp
grep -n "NackRequester::\|FecEncoder::\|FecDecoder::\|CongestionController::" src/net/loss_protection.cpp
grep -n "MetricsCollector::\|CallMetrics::" src/metrics/metrics.cpp
```

## 附录 C：术语表

| 术语 | 含义 | 本项目对应 |
|---|---|---|
| ADM | Audio Device Module | `VirtualAudioDevice` |
| APM | Audio Processing Module（3A） | 未实现（直通） |
| NetEq | WebRTC 抖动缓冲+丢包隐藏 | `JitterBuffer`（帧级简化版） |
| PLC | Packet Loss Concealment | `decode(nullptr, 0, ...)` |
| RTP | 实时传输协议 | `Packet` 前 12 字节 |
| SRTP | 安全 RTP | `DtlsSrtpSession::protect/unprotect` |
| SSRC | 同步源标识 | `session_id` 映射到 SSRC |
| ICE/STUN | 连通性建立/公网映射 | `IceTransport` / `resolve_ipv4` |
| Trickle ICE | 候选边收集边交换 | `set_on_trickle_candidate` / `add_remote_candidate_sdp` |
| DTLS | 数据报 TLS，协商密钥 | `prepare()` / `handshake()` |
| GCC | Google Congestion Control | `CongestionController` |
| AIMD | 加性增/乘性降 | `update_bitrate()` |
| NACK/FEC | 重传/前向纠错 | `NackRequester` / `FecEncoder/Decoder` |
| `LWS_PRE` | lws 协议头预留空间 | 信令收发缓冲区 |
| `GMainContext` | glib 事件循环上下文 | `IceTransport::ctx_` |

## 附录 D：如果只记三句话

1. **抖动缓冲的关键不是“排序”，而是“三个水位各管各的语义”**：
   `play_seq_` 管播放/迟到，`highest/lowest` 管丢包统计；混用就是坑 1/11。
2. **自定义协议一旦要过 SRTP，前 12 字节就必须是合法 RTP**：
   首字节 0x80、seq/timestamp/SSRC 在标准位置；多流用独立 SSRC，不要用序号偏移（坑 30）。
3. **所有算法组件的时间都必须可注入，所有统计的分母都必须说清楚**：
   前者决定能不能仿真（坑 9/28/32），后者决定指标可不可信（坑 11/32/33/34）。
