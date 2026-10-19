# 验证数据与实测结果

> 所有数据均为**实测产出**，命令可复现。
> 环境：Ubuntu 20.04 (WSL2)，g++ 9.4.0，CMake 4.4.4，32 核，7.8GB RAM
>
> **最终验收方式**：从 GitHub 全新克隆 → 独立编译 → 运行全部测试，
> 见文末「复现步骤」。所有结论均来自该流程。

---

## 0. 总览

| 阶段 | 验证对象 | 关键实测数据 | 结果 |
|---|---|---|---|
| Phase 0 | 五库可用性 | Opus 往返 320→33→320 样本 | ✅ |
| Phase 1 | 媒体层 | SNR 29.8dB / 16.0kbps / 25帧·500ms | ✅ 6/6 |
| Phase 2 | WebSocket 信令 | 双向消息零丢失 | ✅ |
| Phase 3 | ICE / STUN | 候选对选通 + srflx×3 | ✅ |
| Phase 4 | DTLS-SRTP | 21 项通过，62/62 解密 | ✅ |
| Phase 5 | 弱网对抗 | 恢复率 100%/89%/78%/52% | ✅ |
| Phase 6 | 端到端集成 | 检测丢包=实际丢包 | ✅ |
| 补充 | Trickle ICE | 候选 36/36 增量添加 | ✅ |
| 补充 | 指标统计 | 跨层自洽 62==62==50+12 | ✅ |
| 补充 | 拥塞控制闭环 | 16000→48000 / →6000 bps | ✅ |

---

## 1. 组件可用性验证（Phase 0）

### 命令

```bash
g++ t.cpp -o t $(pkg-config --cflags glib-2.0 nice libwebsockets) \
  $(pkg-config --libs opus nice libwebsockets glib-2.0 openssl) -lsrtp2
./t
```

### 实测输出

```
Opus 编码 320 样本 -> 33 字节
Opus 解码 -> 320 样本
SRTP init OK
OpenSSL init OK
libnice agent created OK
✅ ALL FIVE LIBS WORK
```

**结论**：apt 提供的 5 个组件全部可用，Opus 真实编解码往返成功。

---

## 2. 依赖版本（实测）

| 组件 | 版本 | 用途 |
|---|---|---|
| libopus | 1.3.1 | 音频编解码 |
| libsrtp2 | 2.3.0 | 媒体加密 |
| libnice | 0.1.16 | ICE / STUN |
| libwebsockets | 3.2.1 | WebSocket 信令 |
| glib-2.0 | 2.64.6 | libnice 依赖 |
| OpenSSL | 1.1.1f | DTLS |
| g++ | 9.4.0 | 编译器 |
| CMake | 4.4.4（pip 安装） | 构建 |

---

## 3. 媒体层（Phase 1）

### 3.1 自检 6 组（全部通过）

```bash
./build/selftest
```

```
=== 测试 1：包格式 ===
  [PASS] 序列化/反序列化往返一致
  [PASS] 损坏包头被 CRC 拒绝
=== 测试 2：Opus 编解码 ===
  [PASS] 编码成功
  [PASS] 解码成功
  实测 SNR: 29.8 dB
=== 测试 3：抖动缓冲 ===
  [PASS] 顺序到达按序播放
  [PASS] 乱序到达被正确重排
  [PASS] 重复包被丢弃
=== 测试 4~6：虚拟设备 / 节拍 / 统计 ===
  [PASS] 采集节拍接近实时（约 25 帧/500ms）
################ 结果：全部通过（失败 0 项）################
```

### 3.2 Opus 编解码质量

| 指标 | 实测值 |
|---|---|
| 采样率 / 声道 | 16000 Hz / 单声道 |
| 帧长 | 20 ms（320 样本） |
| 目标码率 | 16 kbps |
| **实测 SNR** | **29.8 dB** |
| 编解码往返延迟 | 26.9 ms（430 样本 @16kHz） |
| 实测码率 | 16.0 kbps |

**SNR 从 2.8dB 修正到 29.8dB 的过程**（坑 2）：
最初逐样本比对，忽略了 Opus 的**前瞻延迟（lookahead）**，
导致波形错位、SNR 荒谬。改用**互相关对齐**后再计算，得到真实值 29.8dB，
同时精确测出算法延迟 430 样本（26.9ms）。

### 3.3 Opus 运行时码率遵循性（Phase 6 补充验证）

```bash
bash scripts/verify_opus_bitrate.sh
```

```
target=16000 bps -> 实测 16000 bps (250 帧, 10000 字节)
target=32000 bps -> 实测 32000 bps (250 帧, 20000 字节)
target=48000 bps -> 实测 48000 bps (250 帧, 30000 字节)
```

**结论**：Opus 精确遵循运行时设定的码率（CBR），
证明拥塞控制的 `set_bitrate` 执行环节真实有效。

### 3.4 弱网矩阵（抖动缓冲）

```bash
./build/weaknet_test --loss 8 --jitter 30 --delay 60 --seconds 10
```

| 用例 | 丢包率 | 检测丢包 | 实际丢包 | 播放帧数 | 连续性 |
|---|---|---|---|---|---|
| A | 0% | **0** | 0 | 500 | 100% |
| B | 2% | **6** | 6 | — | — |
| C | 8% | **38** | 38 | 462 | 85.6% |
| D | 15% | **67** | 67 | — | — |
| E | 0% + ±80ms 抖动 | **0** | 0 | 499/500 | **99.8%** |

**关键结论**：
1. **检测丢包与实际丢包逐一精确吻合**（0/6/38/67/0）—— 修复坑 11 后达成
2. 抖动场景接受率 **99.8%** —— 修复坑 10（目标延迟只跟平滑抖动）后，
   从 43% 提升到 99.8%

### 3.5 端到端 loopback

```bash
./build/peer --loopback --seconds 3
```

```
  发送包数      : 51
  自环回灌包数  : 51
  接收包数      : 51
  编码错误      : 0
  解码错误      : 0
  抖动缓冲接收  : 150
  播放帧数      : 146
  PLC 补偿      : 0
  目标延迟      : 81 ms
  实测码率      : 16.0 kbps
✅ 自环验证通过：媒体链路端到端可用
```

---

## 4. 信令（Phase 2）

```bash
./build/signal_server --port 8899 &
./build/peer --signal ws://127.0.0.1:8899 --room demo --name A --peer-test --seconds 3
./build/peer --signal ws://127.0.0.1:8899 --room demo --name B --peer-test --seconds 3
```

### 服务器转发明细（实测）

```
peer#1 joined room 'demo' (1/2 members)
peer#2 joined room 'demo' (2/2 members)
room 'demo' paired: both peers ready
forwarded offer      peer#1 -> peer#2  (169 bytes)
forwarded answer     peer#2 -> peer#1  (170 bytes)
forwarded candidate  peer#2 -> peer#1  (88 bytes)
forwarded candidate  peer#1 -> peer#2  (88 bytes)
```

### 两端结果

| 端点 | 入房 | 对端就绪 | 收到 offer | 收到 answer | 候选 | 收/发 |
|---|---|---|---|---|---|---|
| A（主叫） | 是 | 是 | 否 | **是** | 是 | 4/3 |
| B（被叫） | 是 | 是 | **是** | 否 | 是 | 4/3 |

**双向消息零丢失**（修复坑 19/20 后达成）。
主叫收 answer、被叫收 offer 是**正确**的——两端不可能同时收到两者。

---

## 5. ICE / STUN 穿透（Phase 3）

```bash
./build/ice_test --seconds 3            # host 候选
./build/ice_test --stun stun.l.google.com --stun-port 19302 --seconds 2
```

### 5.1 host 候选场景（全部通过）

| 验证项 | 实测结果 |
|---|---|
| 候选收集 | A/B 各 6 个候选（SDP 538 字节） |
| 候选交换 | 双向 SDP 解析成功 |
| 连通性检查 | 双方选出候选对，状态 → READY |
| **双向媒体传输** | A 收 39 包 / B 收 40 包 |
| **内容验证** | 确认收到的数据来自对端（非回环） |

### 5.2 STUN 场景（公网映射，全部通过）

```
[INFO][ice] STUN configured: stun.l.google.com (74.125.250.129):19302
A 候选类型: host×6 srflx×3 relay×0
```

**srflx×3** = 成功获得 3 个**公网映射地址**，这是 STUN 打洞的核心产出。

### 5.3 Trickle ICE（全部通过）

```
A 通过 trickle 送出候选: 36 个
B 通过 trickle 送出候选: 36 个
A 凭证: ufrag=V5Y/ (pwd 22 字节)
A 逐个添加 B 的候选: 36/36 成功
B 逐个添加 A 的候选: 36/36 成功
[PASS] Trickle 模式下 A/B 连接建立
Trickle 建链后收发: A 收 20 / B 收 20
```

**关键认知**（坑 31）：Trickle ICE 必须**同时交换 ICE 凭证（ufrag/pwd）**——
单个 `a=candidate` 行不含凭证，而 STUN 认证需要它们。

---

## 6. DTLS-SRTP 加密（Phase 4 + Phase 6）

### 6.1 独立模块测试（21 项全部通过）

```bash
./build/dtls_srtp_test
```

| 验证组 | 结果 |
|---|---|
| DTLS 握手 | 成功，套件 **ECDHE-RSA-AES256-GCM-SHA384** |
| 证书指纹 | **双向互验通过**（A 验 B ✓ / B 验 A ✓） |
| RFC 5764 密钥派生 | client/server key(16B) + salt(14B) **四方一致** |
| SRTP 加解密 | 35 → **45 字节**（+10 字节 HMAC-SHA1 认证标签） |
| 内容还原 | 解密后与明文**完全一致** |
| 防篡改 | 篡改密文被拒 ✓ |
| 明文包 | 未加密的明文包被拒 ✓ |

**密钥材料（RFC 5764）**：
```
client_key(16) | server_key(16) | client_salt(14) | server_salt(14) = 60 字节
```

### 6.2 端到端集成（在 ICE 通道上）

```
SRTP 加密/解密    : 62 / 62     ← 逐包对应，无一次失败
解密失败次数      : 0
抖动缓冲接收帧    : 150
抖动缓冲播放帧    : 146
PLC 补偿次数      : 0
```

---

## 7. 弱网对抗（Phase 5）

```bash
./build/loss_protection_test
```

### 7.1 NACK 重传

| 验证项 | 结果 |
|---|---|
| 顺序到达时不发 NACK | ✓ |
| 检测到缺失序号（5、6） | ✓ |
| 生成正确的 NACK 请求 | ✓ |
| 未到重试间隔不重复请求 | ✓ |
| 重传到达后标记恢复 | ✓ |
| 超时未到的包被放弃（交给 PLC） | ✓ |

### 7.2 FEC 冗余（XOR，k=4 m=1）

| 场景 | 结果 |
|---|---|
| 组满生成冗余包 | ✓（4 源包 × 10B → 1 冗余包 18B） |
| **丢 1 包** | ✓ 恢复，内容与原始包**完全一致** |
| **丢 2 包** | ✗ 无法恢复（**如实体现单 XOR FEC 的能力边界**） |

### 7.3 拥塞控制（延迟梯度 + AIMD）

| 场景 | 码率变化 | 状态 |
|---|---|---|
| 良好网络 | 16000 → **27417** bps | NORMAL |
| 链路排队 | 27417 → **13136** bps | OVERUSE |
| 高丢包 15% | 16000 → **7099** bps | （丢包兜底） |
| 网络恢复 | 9826 → **45774** bps | 加性增 |

统计：增 8 次 / 降 5 次 / overuse 5 次

### 7.4 组合恢复率（NACK + FEC）

| 丢包率 | 实际丢失 | NACK 恢复 | FEC 恢复 | 仍缺失 | **综合恢复率** |
|---|---|---|---|---|---|
| 2% | 11 | 1 | 11 | 0 | **100.0%** |
| 5% | 28 | 1 | 24 | 3 | **89.3%** |
| 10% | 54 | 3 | 39 | 12 | **77.8%** |
| 20% | 89 | 3 | 43 | 43 | **51.7%** |

**曲线符合预期**：丢包率越高，单 XOR FEC（只能防单点丢包）的占比越低，
剩余未恢复的包由抖动缓冲的 PLC 兜底。

### 7.5 拥塞控制闭环（决策 → 执行）

```
测量(延迟梯度+丢包) → 决策(GCC AIMD) → 执行(codec.set_bitrate) → 再测量
```

| 场景 | 目标码率 | 编码器实际码率 |
|---|---|---|
| 无丢包 | 16000 → **48000** bps | 48000 bps |
| 20% 丢包 | 16000 → **6000** bps | 6000 bps |

**动态 FEC 冗余率**（按丢包率调整）：
| 丢包率 | k | 开销 |
|---|---|---|
| <5% | 8 | 12.5% |
| 5~15% | 4 | 25% |
| >15% | 2 | 50% |

---

## 8. 端到端集成（Phase 6）与指标统计

### 8.1 完整链路

```
采集(虚拟设备) → Opus 编码 → 打包(RTP 兼容头) → FEC 冗余
  → SRTP 加密 → ICE 通道(UDP) ──真实网络──▶
  → ICE 接收 → SRTP 解密 → NACK/FEC 恢复 → 抖动缓冲
  → Opus 解码 → 播放(虚拟设备)
```

### 8.2 丢包矩阵

| 丢包率 | 模拟丢弃 | **检测丢包** | FEC 恢复 | PLC | SRTP 加/解 | 结果 |
|---|---|---|---|---|---|---|
| 0% | 0 | **0** | 0 | 0 | 62 / 62 | ✅ |
| 10%（实测 6%） | 3 | **3** ✅ | 2 | 9 | 56 / 56 | ✅ |
| 25%（实测 18%） | 6 | **6** ✅ | 5 | 18 | 61 / 61 | ✅ |

**检测丢包与实际丢弃逐一吻合**；SRTP 无一次解密失败；
PLC 随丢包率上升（符合设计）。

> **关于"末尾丢包"的算法边界**（如实记录）：
> 抖动缓冲的丢包检测原理是 `已接收序号跨度 − 独立到达包数`，
> 它**依赖"更晚的包"到来**才能发现中间的序号空洞。
> 若**最后几个包**丢了，没有更晚的序号来揭示它们，这些"末尾丢包"
> 在通话结束前无法被统计到。
>
> 实测特征（25% 丢包）：
> ```
> 6 轮 × 6 秒（共 138 丢包）  → 累计偏差 0
> 2 秒 × 5 次                → 吻合 5/5，偏差 0
> 4 秒 × 5 次                → 吻合 1/5，累计偏差 4
> ```
> 偏差量级为个位数，长时间运行趋于 0，属**算法固有边界**而非统计错误。
> 这与坑 11 的"虚报 6 倍"性质完全不同（那是口径错误）。

### 8.3 统一指标报告（getStats 等价）

```
======== 通话指标（2136 ms）========
  [发送侧]
    采集帧数        : 101
    编码成功/错误   : 101 / 0  (成功率 100.0%)
    RTP 包/FEC 包   : 33 / 4
    载荷/网络字节   : 7590 / 9393
    编码码率/实际码率: 28426 / 35179 bps  (开销比 1.24)
    NACK 请求/重传  : 0 / 0
  [接收侧]
    接收包数        : 37
    检测丢包        : 0  (丢包率 0.00%)
    恢复(NACK/FEC)  : 0 / 0  (恢复率 100.0%)
  [抖动缓冲]
    接收/播放帧     : 99 / 96
    PLC 补偿        : 0  (播放连续性 100.0%)
    迟到丢弃        : 0
    目标延迟/抖动   : 80.0 ms / 26.19 ms
  [编解码]
    解码成功/错误   : 96 / 0
  [加密]
    SRTP 加密/解密  : 37 / 37  (失败 0)
  [网络自适应]
    目标码率        : 48000 bps
  [ICE]
    通道收发/状态   : 64 / 64  (已连接)
====================================
```

### 8.4 跨层自洽性校验（最有价值的验证）

```
[PASS] SRTP 加密次数 == 解密次数（37 == 37，逐包对应）
[PASS] 发出的媒体+FEC 包数 == SRTP 加密次数（33+4 == 37）
[PASS] 接收包数 == 发送包数（无丢包时）
[PASS] 恢复率落在 [0,1] 区间
[PASS] 播放连续性 > 50%
[PASS] 拥塞控制目标码率落在 [6000, 48000] 内
[PASS] 指标里的目标码率 == 拥塞控制的输出（闭环已接通）
```

**三层数据互相印证**，而不是孤立统计各自汇报——这才是可信的指标。

### 8.5 编码码率与网络码率的区分

```
载荷码率: 15872 bps（9920 载荷字节 / 5000 ms）      ← 编码效率
网络码率: 20832 bps（13020 网络字节，开销比 1.31）  ← 实际占用
```

**为什么要分开**（坑 33）：RTC 链路里至少有四层字节：
```
PCM 样本  →  Opus 载荷  →  RTP/SRTP 整包  →  UDP/IP 帧
```
混在一起算"码率"就没有物理意义。本项目早期版本正是这样做的，
导致"实测码率 82933 bps"比目标 48000 还高 73%。

---

## 9. 关于"无声卡"的处理与验证边界

### 事实

WSL 环境的 `/dev/snd` 只有 `timer`，**没有真实音频设备**：

```bash
$ ls /dev/snd/
timer        # 仅此一项
```

### 应对

实现了 `VirtualAudioDevice`，提供与真实设备一致的接口语义：
- 采集：按采样率定时产出 PCM（文件回放或正弦波合成）
- 播放：接收 PCM，**有独立消费线程模拟 DMA**，带容量上限模拟背压

### 已验证

```
500ms 内采集到 25 帧 / 8000 样本
[PASS] 采集节拍接近实时（约 25 帧/500ms）
```

采集节拍**精确匹配** 20ms/帧（25 帧 × 20ms = 500ms），无累积漂移
（修复坑 14：条件变量固定超时导致的帧率损失）。

### 诚实的边界声明

| 已验证 | 未验证（受环境限制） | 原因 |
|---|---|---|
| ✅ Opus 编解码质量（SNR 29.8dB） | ❌ 真实声卡采样/回放质量 | WSL 无 `/dev/snd` |
| ✅ 抖动缓冲算法行为 | ❌ 真实麦克风的 3A（AEC/ANS/AGC） | 无真实回采信号 |
| ✅ 包格式与 CRC | ❌ 跨公网 NAT 穿透 | 无第二台公网主机 |
| ✅ 虚拟设备采集节拍 | ❌ 硬件时钟漂移 | 虚拟设备时钟理想 |
| ✅ ICE/STUN 本机穿透（srflx×3） | ❌ TURN 中继 | 无 TURN 服务器 |
| ✅ DTLS-SRTP 加密 | ❌ libwebrtc 互操作 | 环境限制（见 README） |

**无法验证的部分在文档中明确标注，不夸大也不隐藏。**

---

## 10. 复现步骤（完整验收）

```bash
# ---- 环境准备 ----
sudo apt-get install -y libopus-dev libsrtp2-dev libnice-dev \
  libwebsockets-dev libglib2.0-dev libssl-dev pkg-config
python3 -m pip install --user "cmake>=3.28"
export PATH=$HOME/.local/bin:$PATH

# ---- 克隆 ----
git clone https://github.com/SimpleStar666/rtc-1v1-audio.git
cd rtc-1v1-audio

# ---- 编译 ----
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build . -j32

# ---- 全量回归（一键）----
cd ..
bash scripts/full_regression.sh

# ---- 或逐项运行 ----
./build/selftest                                          # 媒体层 6 组
./build/weaknet_test --loss 8 --jitter 30 --delay 60 --seconds 10
./build/peer --loopback --seconds 5                       # 端到端自环
./build/ice_test --seconds 3                              # ICE 穿透 + Trickle
./build/ice_test --stun stun.l.google.com --stun-port 19302 --seconds 2
./build/dtls_srtp_test                                    # DTLS-SRTP 21 项
./build/loss_protection_test                              # NACK/FEC/拥塞控制
./build/metrics_test                                      # 指标统计
./build/e2e_test --seconds 4                              # 完整链路
./build/e2e_test --loss 0.25 --seconds 4                  # 弱网下的完整链路

# ---- 信令配对（需三个终端）----
./build/signal_server --port 8899
./build/peer --signal ws://127.0.0.1:8899 --room demo --name A --peer-test
./build/peer --signal ws://127.0.0.1:8899 --room demo --name B --peer-test
```

### 最终验收实测输出

```
=== 1. 克隆 ===
  提交总数: 11    文件总数: 100    含 build/: 0
  源码模块数: 9（app/common/crypto/ice/media/metrics/net/protocol/signal）
  测试文件数: 7    文档数: 4

=== 2. 编译 ===
  ✅ 编译成功（无 error）
  产物: selftest weaknet_test peer signal_server ice_test
        dtls_srtp_test loss_protection_test metrics_test e2e_test

=== 3. 全部测试 ===
  selftest              全部通过（失败 0 项）
  weaknet_test          rc=0
  loopback              rc=0
  ice_test（含 Trickle） 全部通过（失败 0 项）
  dtls_srtp_test        全部通过（失败 0 项）
  loss_protection       全部通过（失败 0 项）
  metrics_test          全部通过（失败 0 项）
  e2e_test(0%)          全部通过（失败 0 项）
  e2e_test(10%)         全部通过（失败 0 项）
  失败项数: 0
```
