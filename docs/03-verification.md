# 验证数据与实测结果

> 所有数据均为**实测产出**，命令可复现。
> 环境：Ubuntu 20.04 (WSL2)，g++ 9.4.0，CMake 4.4.4，32 核，7.8GB RAM

---

## 1. 组件可用性验证（Phase 0）

### 命令

```bash
# 五库联合编译链接 + 真实调用
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

**结论**：apt 提供的 5 个组件全部可用，且 Opus 真实编解码往返成功。

---

## 2. 依赖版本（实测）

| 组件 | 版本 | 用途 |
|---|---|---|
| libopus | 1.3.1 | 音频编解码 |
| libsrtp2 | 2.3.0 | 媒体加密 |
| libnice | 0.1.16 | ICE / STUN |
| libwebsockets | 3.2.0 | 信令通道 |
| glib-2.0 | 2.64.6 | libnice 依赖 |
| openssl | 1.1.1f | DTLS |
| CMake | **4.4.4** | pip 安装（系统自带 3.16 不足）|
| g++ | 9.4.0 | 编译器 |

---

## 3. 自检结果（6 项全通过）

### 命令

```bash
cd rtc-1v1-audio && ./scripts/build.sh && ./build/selftest
```

### 实测输出

```
=== 测试 1：Opus 编解码往返 ===
  [PASS] codec init
    最佳对齐偏移=430 样本(26.9 ms), RMSE=257.5, SNR=29.8 dB, 平均帧=40.0 B, 实测码率=16.0 kbps
  [PASS] 对齐后失真合理 (RMSE < 800)
  [PASS] 对齐后 SNR > 15dB（Opus 语音质量合格）
  [PASS] 实测码率在 10~30kbps 之间

=== 测试 2：多帧拼包 ===
  [PASS] pack_frames 成功
  [PASS] 解包消费字节数一致
  [PASS] 帧数一致
  [PASS] 每帧内容一致

=== 测试 3：包序列化 / CRC 校验 ===
  [PASS] 序列化长度正确
  [PASS] 解析成功
  [PASS] session_id 一致
  [PASS] seq 一致
  [PASS] timestamp 一致
  [PASS] trans_id 一致
  [PASS] payload 一致
  [PASS] 损坏包头被 CRC 拒绝

=== 测试 4：抖动缓冲 ===
  [PASS] 收到 10 个包
  [PASS] 无丢包
  [PASS] 乱序包被接受（抖动缓冲的核心职责）
  [PASS] 重复包被拒绝
  [PASS] 检测到丢包
    统计: recv=13 played=0 lost=14 plc=0 dup=1 late=0

=== 测试 5：信令 JSON 往返 ===
  [PASS] 解析成功 / type 一致 / room 一致 / candidate 一致 / peer_id 一致
  [PASS] offer 解析 / 编解码参数一致

=== 测试 6：虚拟音频设备 ===
  [PASS] 设备打开
    500ms 内采集到 25 帧 / 8000 样本
  [PASS] 采集节拍接近实时（约 25 帧/500ms）

################ 结果：全部通过（失败 0 项）################
```

### 关键数据解读

| 指标 | 数值 | 说明 |
|---|---|---|
| Opus SNR | **29.8 dB** | 16kbps 语音编码的正常质量水平 |
| Opus 算法延迟 | **26.9 ms** | 430 样本 @16kHz，编码器 lookahead |
| 实测码率 | **16.0 kbps** | 与配置的 16000bps 精确一致 |
| 采集节拍 | **25 帧 / 500ms** | 20ms/帧，精确匹配（无漂移）|

---

## 4. 弱网测试矩阵（抖动缓冲行为）

### 命令

```bash
./build/weaknet_test --loss <pct> --jitter <ms> --delay <ms> --seconds 10 --jb <ms>
```

### 实测结果（修复后）

| 用例 | 丢包设定 | 抖动 | 发送 | **实际丢包** | **检测丢包** | 接受 | 播放 | PLC | 目标延迟 | 连续性 |
|---|---|---|---|---|---|---|---|---|---|---|
| **A** 理想 | 0% | 0ms | 500 | 0 | **0** ✅ | 500 | 500 | 45 | 20ms | 91.7% |
| **B** 轻度 | 2% | ±10ms | 494 | 6 | **6** ✅ | 494 | 494 | 48 | 50ms | 91.1% |
| **C** 中度 | 8% | ±30ms | 462 | 38 | **38** ✅ | 462 | 462 | 78 | 84ms | 85.6% |
| **D** 重度 | 15% | ±60ms | 433 | 67 | **67** ✅ | 432 | 432 | 106 | 108ms | 80.3% |
| **E** 大抖动 | 0% | ±80ms | 500 | 0 | **0** ✅ | 499 | 499 | 42 | 105ms | 92.2% |

### 三项重要验证

**(1) 丢包统计精确性**：5 个用例的"检测丢包"与"实际丢包"**逐一吻合**
- B: 6 = 6，C: 38 = 38，D: 67 = 67
- E: 0% 丢包的网络正确报 0（修复前误报 245）

**(2) 自适应目标延迟**：随网络质量合理变化
```
理想网络   → 20ms   （最小化延迟）
轻度丢包   → 50ms
中度丢包   → 84ms
重度丢包   → 108ms  （加深以吸收抖动）
大抖动     → 105ms  （从初始 40ms 主动加深）
```
**趋势正确**：网络越差，缓冲越深——这正是抖动缓冲的核心行为。

**(3) 播放连续性**：负载越重越低，但保持可用
```
理想 91.7%  →  轻度 91.1%  →  中度 85.6%  →  重度 80.3%
```

### 修复前后的关键对比

| 用例 | 指标 | 修复前 | **修复后** | 提升 |
|---|---|---|---|---|
| D 重度丢包 | 接受包数 | 239 | **432** | +81% |
| | PLC 次数 | 305 | **106** | -65% |
| E 大抖动 | 接受包数 | 215 | **499** | **+132%** |
| | PLC 次数 | 333 | **42** | **-87%** |

**E 用例（±80ms 抖动、0% 丢包）最能说明问题**：
修复前 500 个包只接受了 215 个（43%），修复后 499 个（99.8%）。

---

## 5. 编译产物

```
selftest       2.2 MB   自检（6 组测试）
signal_server  1.7 MB   WebSocket 信令服务器
peer           1.8 MB   通话端点
weaknet_test   0.77 MB  弱网测试工具
```

编译耗时：约 15 秒（32 核并行）

---

## 6. 关于"无声卡"的处理与验证边界

### 事实

WSL 环境的 `/dev/snd` 只有 `timer`，**没有真实音频设备**：

```bash
$ ls /dev/snd/
timer        # 仅此一项
```

### 应对

实现了 `VirtualAudioDevice`，提供与真实设备一致的接口语义：
- 采集：按采样率定时产出 PCM（文件回放或正弦波合成）
- 播放：接收 PCM，带容量上限模拟背压

### 已验证

```
=== 测试 6：虚拟音频设备 ===
  [PASS] 设备打开
    500ms 内采集到 25 帧 / 8000 样本
  [PASS] 采集节拍接近实时（约 25 帧/500ms）
```

采集节拍**精确匹配** 20ms/帧（25 帧 × 20ms = 500ms），无累积漂移。

### 诚实的边界声明

| 已验证 | 未验证（受环境限制） |
|---|---|
| ✅ Opus 编解码质量（SNR 29.8dB） | ❌ 真实声卡采样/回放质量 |
| ✅ 抖动缓冲算法行为 | ❌ 真实麦克风的 3A 效果（AEC/ANS/AGC） |
| ✅ 包格式与 CRC | ❌ 真实网络环境下的 NAT 穿透 |
| ✅ 虚拟设备采集节拍 | ❌ 硬件时钟漂移（真实设备才有）|

**这些边界会在后续阶段（网络接入）时进一步补齐，无法验证的部分会在文档中明确标注。**

---

## 7. 复现步骤

```bash
# 1. 安装依赖
sudo apt-get install -y libopus-dev libsrtp2-dev libnice-dev \
  libwebsockets-dev libglib2.0-dev libssl-dev pkg-config ninja-build

# 2. 安装新版 CMake（系统自带 3.16 不足）
python3 -m pip install --user "cmake>=3.28"
export PATH=$HOME/.local/bin:$PATH

# 3. 构建
cd rtc-1v1-audio
./scripts/build.sh

# 4. 运行自检
./build/selftest

# 5. 弱网测试
./build/weaknet_test --loss 8 --jitter 30 --delay 60 --seconds 10

# 6. 本地媒体链路自环
./build/peer --loopback --seconds 5
```
