# 怎么跑、怎么玩

> 面向"想实际跑起来"的人。所有命令都在 WSL 中实测通过，输出可直接对照。
>
> **TL;DR** —— 如果你想最快看到效果，只有一条命令：
> ```bash
> ./scripts/run.sh
> ```
> 菜单里选 **"完整演示"**，或直接 `./scripts/run.sh demo`。

---

## 目录

1. [30 秒上手](#1-30-秒上手)
2. [环境准备（第一次跑之前）](#2-环境准备第一次跑之前)
3. [一键菜单（推荐）](#3-一键菜单推荐)
4. [逐个命令详解](#4-逐个命令详解)
5. [单独跑各组件](#5-单独跑各组件)
6. [**听到声音**（最直观的验证）](#6-听到声音最直观的验证)
7. [双端真实通话演示](#7-双端真实通话演示)
8. [弱网对抗演示](#8-弱网对抗演示)
9. [看懂输出](#9-看懂输出)
10. [给面试现场用的演示脚本](#10-给面试现场用的演示脚本)
11. [常见问题](#11-常见问题)
12. [命令速查表](#12-命令速查表)

---

## 1. 30 秒上手

如果你已经编译过（`build/` 存在）：

```bash
cd rtc-1v1-audio

# ① 全部测试（约 30 秒）
./scripts/run.sh test

# ② 本地自环，并且**会播放声音**给你听
./scripts/run.sh loopback

# ③ 双端真实通话（自动起服务器 + A/B 配对）
./scripts/run.sh call
```

**没编译过就先跑 `./scripts/run.sh build`，或直接 `./scripts/run.sh demo` 走完整流程。**

---

## 2. 环境准备（第一次跑之前）

本项目在 **Ubuntu 20.04 (WSL2)** 下开发验证。

```bash
# ---- 1. 开发库（五件套 + glib）----
sudo apt-get update
sudo apt-get install -y \
  libopus-dev libsrtp2-dev libnice-dev libwebsockets-dev \
  libglib2.0-dev libssl-dev pkg-config build-essential

# ---- 2. 新版 CMake（系统自带 3.16 不满足要求）----
python3 -m pip install --user "cmake>=3.28"
export PATH=$HOME/.local/bin:$PATH        # 每次新开终端都要加

# ---- 3. 克隆 ----
git clone https://github.com/SimpleStar666/rtc-1v1-audio.git
cd rtc-1v1-audio

# ---- 4. 自检环境（确认依赖齐全）----
./scripts/run.sh env
```

### 期望输出

```
=== 环境自检 ===
  ✓ 环境正常（可编译运行）
```

如果报 `✗ 缺少开发库: xxx`，就 `apt-get install` 对应的 `libxxx-dev`。

> **注意**：`pkg-config` 里 SRTP 的名字是 **`libsrtp2`**（不是 `srtp2`）。
> 手动编译时用 `pkg-config --libs libsrtp2`。

### 关于 CMake 路径

`pip install --user` 装的 cmake 在 `~/.local/bin`。
本项目所有脚本都会自动把这个路径加进 `PATH`，所以**手动跑脚本时不用管**；
但如果你自己敲 `cmake`，记得先 `export PATH=$HOME/.local/bin:$PATH`。

---

## 3. 一键菜单（推荐）

```bash
./scripts/run.sh
```

会打印帮助并给出菜单：

```
请选择要执行的操作:
1) 完整演示
2) 编译
3) 全部测试
4) 本地自环（听声音）
5) 双端通话
6) 弱网矩阵
7) 生成音频
8) 环境自检
9) 退出
```

**推荐第一次选 `1) 完整演示`** —— 它会依次做：
生成测试音频 → 编译 → 跑全部 9 项测试 → 本地自环（**播放声音**）→ 双端通话。

---

## 4. 逐个命令详解

| 命令 | 作用 | 耗时 | 需要先编译 |
|---|---|---|---|
| `./scripts/run.sh demo` | 完整演示（编译+测试+自环+通话） | ~2 分钟 | 否（会自动编译） |
| `./scripts/run.sh build` | 只编译 | ~40 秒 | — |
| `./scripts/run.sh test` | 跑全部 9 项测试并汇总 | ~40 秒 | 是（会自动编译） |
| `./scripts/run.sh loopback` | 本地自环，**并播放解码后的音频** | ~15 秒 | 是 |
| `./scripts/run.sh call` | 双端真实通话（自动起服务器+A/B） | ~20 秒 | 是 |
| `./scripts/run.sh weaknet` | 弱网矩阵（丢包 0/2/5/8/15%） | ~60 秒 | 是 |
| `./scripts/run.sh audio` | 生成测试音频（PCM + WAV） | ~2 秒 | 否 |
| `./scripts/run.sh env` | 环境自检 | <1 秒 | 否 |
| `./scripts/run.sh help` | 帮助 | 即时 | 否 |

---

## 5. 单独跑各组件

编译后所有可执行文件都在 `build/`。**这是最灵活的方式**，
面试时想针对性展示某一层就用这些：

```bash
cd build

# ---- 媒体层（Phase 1）----
./selftest                                   # 6 组自检：包格式/Opus/抖动缓冲/虚拟设备
./selftest 2>&1 | grep -E "SNR|对齐|码率"     # 只看 Opus 质量数据

# ---- 弱网矩阵（抖动缓冲在不同丢包/抖动下的表现）----
./weaknet_test --loss 8 --jitter 30 --delay 60 --seconds 10
./weaknet_test --loss 0 --jitter 80 --delay 0  --seconds 10   # 只看抖动

# ---- 本地自环（不经网络，验证媒体链路）----
./peer --loopback --seconds 3

# ---- ICE 穿透 + Trickle ICE（Phase 3）----
./ice_test --seconds 3                        # host 候选
./ice_test --stun stun.l.google.com --stun-port 19302 --seconds 2   # 拿 srflx 公网映射

# ---- DTLS-SRTP 加密（Phase 4）----
./dtls_srtp_test                              # 21 项：握手/指纹/密钥派生/加解密/防篡改

# ---- 弱网对抗（Phase 5）----
./loss_protection_test                        # NACK / FEC / 拥塞控制

# ---- 指标统计 ----
./metrics_test                                # 分层指标 + 派生指标 + JSON 导出

# ---- 端到端集成（Phase 6，最完整的验证）----
./e2e_test --seconds 4                        # 音频经 ICE + DTLS-SRTP 完整链路
./e2e_test --loss 0.25 --seconds 4            # 25% 丢包下的完整链路

# ---- 看端到端的指标报告 ----
./e2e_test --seconds 3 2>/dev/null | grep -A24 "通话指标"
```

### 各程序支持的参数

**`peer`（通话端点）**
```
--signal <url>      信令服务器地址（默认 ws://127.0.0.1:8888）
--room <name>       房间名（默认 demo）
--name <id>         本端标识（默认 peer）
--audio-file <pcm>  采集源 PCM 文件（默认正弦波合成）
--out <pcm>         播放落盘文件（默认 /tmp/peer_out.pcm）
--seconds <n>       运行时长（默认 5）
--bitrate <bps>     Opus 码率（默认 16000）
--jb <ms>           抖动缓冲目标延迟（默认 60）
--loopback          本地自环验证（不经网络）
--peer-test         双端信令配对测试
--verbose           打开调试日志
```

**`signal_server`**
```
--port <n>          监听端口（默认 8888）
--verbose           调试日志
```

**`weaknet_test`**
```
--loss <pct>        丢包率百分比（0~100）
--jitter <ms>       抖动 ±ms
--delay <ms>        基础延迟
--jb <ms>           抖动缓冲目标延迟
--seconds <n>       时长
```

**`ice_test`**
```
--stun <host>       STUN 服务器（不填则只有 host 候选）
--stun-port <n>     STUN 端口（默认 19302；本地 coturn 是 3478）
--seconds <n>       传输时长
--verbose           调试日志
```

**`e2e_test`**
```
--loss <0~1>        丢包率（小数，如 0.10 表示 10%）
--seconds <n>       时长
--verbose           调试日志
```

---

## 6. **听到声音**（最直观的验证）

这是本项目**最有说服力的演示**——能证明音频真的经过"编码→打包→(网络)→抖动缓冲→解码"走通了。

### 一条命令

```bash
./scripts/run.sh loopback
```

它会：
1. 采集 PCM → Opus 编码 → 打包 → 回灌 → 抖动缓冲 → 解码 → 落盘
2. 把 PCM 转成 WAV
3. **调用 `paplay` 播放**（WSLg 会把音频转发到你的 Windows 扬声器）

期望看到：

```
=== 自环结果（3 秒）===
  发送包数      : 51
  接收包数      : 51
  编码错误      : 0
  解码错误      : 0
  播放帧数      : 147
  PLC 补偿      : 0
  实测码率      : 16.0 kbps
✅ 自环验证通过：媒体链路端到端可用

=== 试听结果 ===
OK: /tmp/peer_out.pcm -> /tmp/peer_out.wav (16000Hz/mono/16bit)
  ! 即将在 WSL 播放解码后的音频（WSLg 会转发到 Windows 扬声器）...
  ✓ 播放完成——你应该听到了扫频声
```

**你听到的是扫频声**（200Hz 逐渐升到 3000Hz）——这是 Opus 解码出来的真实音频。

### 手工版本（想自己控制）

```bash
cd build

# ① 生成测试音频（会同时产出 PCM 和 WAV）
bash ../scripts/make_audio.sh /tmp/rtc_demo

# ② 用扫频作为采集源做自环
./peer --loopback --seconds 3 --audio-file /tmp/rtc_demo/sweep.pcm

# ③ 转换并播放
python3 /tmp/rtc_demo/pcm2wav.py /tmp/peer_out.pcm /tmp/peer_out.wav 16000
paplay /tmp/peer_out.wav
```

### 可用的测试音频

`scripts/make_audio.sh` 会生成 6 种（每种都有 `.pcm` 和 `.wav`）：

| 文件 | 听感 | 用途 |
|---|---|---|
| `sine440.pcm` | 单音"嘟——" | 最基础，验证链路通 |
| `chord.pcm` | 和声（440+880Hz） | 验证多频率能同时还原 |
| **`sweep.pcm`** | **频率由低到高扫过** | **最推荐**，频率变化最容易听出来 |
| `beeps.pcm` | 变化的"滴滴"声 | 接近人耳熟悉的提示音 |
| `noise.pcm` | "沙沙"白噪声 | 测抖动缓冲/PLC 效果 |
| `silence.pcm` | 静音 | 测 DTX/PLC |

### 如果听不到声音

| 情况 | 解决办法 |
|---|---|
| WSL 无 `paplay` | `sudo apt-get install -y pulseaudio-utils` |
| 播放失败但文件已生成 | 直接把 WAV 拷到 Windows 双击播放：<br>`cp /tmp/rtc_demo/*.wav /mnt/c/Users/小陶/Desktop/` |
| 完全无声 | 检查 Windows 音量；确认 WSLg 在运行（`ls /mnt/wslg/PulseServer`） |

`scripts/make_listenable.sh` 会自动把 WAV 复制到 `C:\Users\小陶\Desktop\rtc-demo-audio\`。

---

## 7. 双端真实通话演示

```bash
./scripts/run.sh call
```

**背后做的事**（想手工复现就开三个终端）：

```bash
# 终端 1：信令服务器
cd build && ./signal_server --port 8899

# 终端 2：端点 A（主叫）
cd build && ./peer --signal ws://127.0.0.1:8899 --room demo --name A \
  --peer-test --seconds 8 --audio-file /tmp/rtc_demo/sweep.pcm

# 终端 3：端点 B（被叫）
cd build && ./peer --signal ws://127.0.0.1:8899 --room demo --name B \
  --peer-test --seconds 8 --audio-file /tmp/rtc_demo/sweep.pcm
```

### 期望输出

**端点 A（主叫）**
```
[A] 信令连接建立
[A] 已入房（成员数 1）
[A] 对端就绪 -> 可以开始协商
[A] 我发 offer
[A] 收到 answer: codec=opus;rate=16000;ch=1;frame=20;ptime=60
[A] 收到 candidate: 127.0.0.1:50001 (mline=0)
  入房成功    : 是
  对端就绪    : 是
  收到 answer : 是
  收到候选    : 是
  信令面结果  : ✅ 通过（本端为主叫，收到 answer）
```

**端点 B（被叫）**
```
[B] 已入房（成员数 2）
[B] 收到 offer: codec=opus;rate=16000;ch=1;frame=20;ptime=60
[B] 已回 answer
  信令面结果  : ✅ 通过（本端为被叫，收到 offer）
```

**服务器转发记录**
```
peer#1 joined room 'demo' (1/2 members)
peer#2 joined room 'demo' (2/2 members)
room 'demo' paired: both peers ready
forwarded offer      peer#1 -> peer#2  (169 bytes)
forwarded answer     peer#2 -> peer#1  (170 bytes)
forwarded candidate  peer#2 -> peer#1  (88 bytes)
forwarded candidate  peer#1 -> peer#2  (88 bytes)
```

### 怎么读这个结果

- **主叫收到 answer、被叫收到 offer 是正常的** —— 两端不可能同时收到两者
- **`forwarded ... (169 bytes)` 就是 SDP** —— offer/answer 里包含 codec 参数
  （`codec=opus;rate=16000;ch=1;frame=20;ptime=60`）
- **candidate 88 字节** 就是 ICE 候选（Trickle ICE 逐个发送）

**媒体落盘**：`/tmp/peer_A.pcm`、`/tmp/peer_B.pcm`
```bash
python3 /tmp/rtc_demo/pcm2wav.py /tmp/peer_A.pcm /tmp/peer_A.wav 16000
paplay /tmp/peer_A.wav
```

---

## 8. 弱网对抗演示

```bash
./scripts/run.sh weaknet
```

会跑两组对照：

### ① 抖动缓冲：丢包率递增

实测数据（`--jitter 20 --delay 50 --seconds 8`）：

| 丢包率 | 实际丢失 | **检测丢包** | 播放帧数 | PLC 补偿 |
|---|---|---|---|---|
| 0% | 0 | **0** | 400 | 41 |
| 2% | 5 | **5** | 395 | 46 |
| 5% | 21 | **21** | 379 | 62 |
| 8% | 30 | **30** | 370 | 72 |
| 15% | 54 | **54** | 346 | 94 |

**看点**：
1. **"检测丢包"逐档精确等于"实际丢失"（0/5/21/30/54）** —— 这是丢包统计算法正确的直接证据。
   我曾经按"到达顺序累加"来统计，结果把乱序算成丢包，**虚报了 6 倍**（坑 11）。
2. **PLC 补偿随丢包率单调上升**（41 → 94）—— 丢得越多，补得越多，符合设计。
3. 播放帧数随丢包下降（400 → 346），说明丢的包没有被凭空造出来。

### ② 端到端（含 SRTP 加密 + ICE 通道）

```
  丢包率 0     : 检测丢包 : 0  FEC 恢复 : 0  SRTP 加密/解密 : 62 / 62
  丢包率 0.10  : 检测丢包 : 3  FEC 恢复 : 2  SRTP 加密/解密 : 56 / 56
  丢包率 0.25  : 检测丢包 : 6  FEC 恢复 : 5  SRTP 加密/解密 : 61 / 61
```

> 注：`--loss` 是小数概率，且每轮实际丢弃数有随机波动，
> 所以"检测丢包"与 `实际丢弃` 吻合即可（每次运行数值会略有不同）。

**看点**：
- `SRTP 加密/解密` 次数**完全对应** → 加密链路无一次失败
- `检测丢包` 与模拟的丢弃数吻合 → 跨模块统计口径一致
- `FEC 恢复` 随丢包率上升而增多 → 冗余机制在起作用

### ③ 拥塞控制（单独跑）

```bash
cd build && ./loss_protection_test 2>&1 | grep -E "bitrate (UP|DOWN)"
```

能看到码率随网络状况**实时调整**：

```
[gcc] bitrate UP   16000 -> 17280 bps (gradient=0.0ms)
[gcc] state NORMAL -> OVERUSE (gradient=26.7ms)
[gcc] bitrate DOWN 29610 -> 25168 bps (gradient=26.7ms)
[gcc] bitrate DOWN 25168 -> 21392 bps (gradient=56.6ms)
```

**看点**：延迟梯度上升（链路排队）→ 立即乘性降码率。
这就是 GCC 的核心思想在日志里的实时体现。

---

## 9. 看懂输出

### 测试结果的统一格式

所有测试程序最后都会打印一行汇总：

```
################ 结果：全部通过（失败 0 项）################
```

- `[PASS]` / `[FAIL]` 是单项结果
- 最后一行是汇总，**这一行才决定成败**

一键回归会汇总成表格：

```
======== 全量回归 ========
  selftest              结果：全部通过（失败 0 项）
  weaknet_test          (无汇总行) rc=0
  loopback              (无汇总行) rc=0
  ice_test              结果：全部通过（失败 0 项）
  dtls_srtp_test        结果：全部通过（失败 0 项）
  loss_protection       结果：全部通过（失败 0 项）
  e2e_test(0%)          结果：全部通过（失败 0 项）
  e2e_test(10%)         结果：全部通过（失败 0 项）
失败项数: 0
```

### 端到端指标报告（最值得看的部分）

```bash
cd build && ./e2e_test --seconds 3 2>/dev/null | grep -A24 "通话指标"
```

```
======== 通话指标（3177 ms）========
  [发送侧]
    采集帧数        : 151
    编码成功/错误   : 151 / 0  (成功率 100.0%)
    RTP 包/FEC 包   : 50 / 12
    载荷/网络字节   : 7590 / 9393
    编码码率/实际码率: 28426 / 35179 bps  (开销比 1.24)
  [接收侧]
    检测丢包        : 0  (丢包率 0.00%)
    恢复(NACK/FEC)  : 0 / 0  (恢复率 100.0%)
  [抖动缓冲]
    接收/播放帧     : 150 / 146
    PLC 补偿        : 0  (播放连续性 100.0%)
    目标延迟/抖动   : 80.0 ms / 26.24 ms
  [加密]
    SRTP 加密/解密  : 62 / 62  (失败 0)
  [ICE]
    通道收发/状态   : 64 / 64  (已连接)
====================================
```

### 关键指标怎么读

| 指标 | 含义 | 健康值 |
|---|---|---|
| **载荷/网络字节** | 编码输出 vs 实际发包（含头与 FEC） | 网络 > 载荷（有开销） |
| **编码码率** | Opus 编码码率 | 应与目标码率接近 |
| **开销比** | 网络字节 / 载荷字节 | 1.2~1.4 属正常 |
| **检测丢包** | 抖动缓冲算出的丢包 | **应等于实际丢弃数** |
| **恢复率** | (NACK+FEC恢复) / 总丢包 | 丢包少时应接近 100% |
| **播放连续性** | 播放 / (播放+PLC) | 越接近 100% 越好 |
| **PLC 补偿** | 丢包时用算法补的帧数 | 随丢包率上升属正常 |
| **SRTP 加密/解密** | 加解密次数 | **两者应相等** |
| **目标码率** | 拥塞控制输出 | 应落在 [6000, 48000] |

**跨层自洽才可信**：`SRTP加密 == SRTP解密`、`RTP包 + FEC包 == SRTP加密`、
`接收包 == 发送包` —— 三层数据互相印证。

---

## 10. 给面试现场用的演示脚本

如果要在面试时演示，建议按这个顺序（**从"能听到"到"有数据"**）：

### 第 1 步：让它发出声音（30 秒，最有冲击力）

```bash
./scripts/run.sh loopback
```
> 说："这是一个真实的 1v1 语音通话链路——采集、Opus 编码、打包、抖动缓冲、
> 解码全部是我自己实现的。你现在听到的是经过完整链路解码出来的音频。"

### 第 2 步：展示端到端指标（10 秒）

```bash
cd build && ./e2e_test --seconds 3 2>/dev/null | grep -A24 "通话指标"
```
> 指着 `SRTP 加密/解密 : 62 / 62` 说："**加密次数等于解密次数**，
> 而且编码码率和网络码率分开统计——载荷 28426、网络 35179，
> 开销比 1.24，这些数字能互相印证。"

### 第 3 步：弱网下的表现（20 秒）

```bash
cd build && ./e2e_test --loss 0.25 --seconds 4 2>/dev/null | grep -E "检测丢包|FEC 恢复|SRTP"
```
> 说："模拟 25% 丢包。检测丢包和实际丢弃数吻合，
> 说明我的丢包统计口径是对的——这一点我踩过坑，
> 最初按到达顺序累加，把乱序算成了丢包，虚报了 6 倍。"

### 第 4 步（可选）：STUN 打洞

```bash
cd build && ./ice_test --stun stun.l.google.com --stun-port 19302 --seconds 2 \
  2>&1 | grep -E "srflx|结果"
```
> 说："`srflx×3` 就是通过 STUN 拿到的 3 个公网映射地址。"

### 第 5 步（可选）：打开踩坑文档

```bash
# 挑一个最难的讲
grep -A40 "坑 23" docs/02-pitfalls.md | head -45
```
> 坑 23（STUN 收不到响应）我是用 **tcpdump 抓包**定位的：
> 请求发出去了、响应也回来了，但 libnice 说没收到——
> 最后发现是 **agent 和主循环用了两个不同的 glib 上下文**。

### 面试官可能追问的点（提前准备）

| 追问 | 你可以打开 |
|---|---|
| "抖动缓冲的目标延迟怎么定？" | `src/media/jitter_buffer.cpp` + 坑 10（接受率 43%→99.8%） |
| "NACK 和 FEC 什么时候用哪个？" | `src/net/loss_protection.cpp` 的配置表 + 恢复率曲线 |
| "DTLS 和 SRTP 什么关系？" | `src/crypto/dtls_srtp.cpp`（DTLS 协商密钥 → SRTP 加密媒体） |
| "怎么防止中间人？" | `handshake()` 里的指纹比对 + 坑 26 |
| "Trickle ICE 是什么？" | `ice_transport.h` 的 trickle 接口 + 坑 31（凭证必须单独交换） |
| "你怎么验证的？" | `docs/03-verification.md`（含全新克隆的验收输出） |

---

## 11. 常见问题

### Q0（最常见）：`build.log: Permission denied` 编译失败

**完整报错长这样**：

```
ts@DESKTOP:~/rtc-1v1-audio$ ./scripts/run.sh build

=== 编译 ===
./scripts/run.sh: line 54: build.log: Permission denied
  ✗ 编译失败，见 build/build.log
```

**原因**：`build/` 目录（或整个仓库）**不属于你**，通常是因为之前有人用
`sudo` 或 `root` 身份在这个目录里编译过，留下的文件属于 root。

**一条命令修好**：

```bash
# 在仓库根目录执行
sudo rm -rf build
sudo chown -R $(id -un):$(id -gn) .
```

然后重新跑 `./scripts/run.sh build`。

**自己确认一下**（修复前后对比）：

```bash
ls -ld build          # 看所有者是不是你
# 修复前: drwxr-xr-x 3 root root  .../build     ← root 的，你不能写
# 修复后: drwxrwxr-x 3 ts   ts    .../build     ← 你的
```

> 新版 `run.sh` 已内置权限自检，会**直接告诉你所有者是谁、怎么修**，
> 不会再抛含糊的 `Permission denied`。

---

### Q1：`cmake` 版本太低（3.16）

Ubuntu 20.04 自带 cmake 3.16，而本项目要求 ≥ 3.21。

```bash
python3 -m pip install --user "cmake>=3.28"
export PATH="$HOME/.local/bin:$PATH"

# 验证
which cmake        # 应该是 /home/<你>/.local/bin/cmake
cmake --version    # 应该是 4.x
```

**关键**：`pip install --user` 装到**当前用户**的 `~/.local/bin`。
如果你之前用 `sudo` 装过，那只有 root 能用，你的 `~/.local/bin` 还是空的。

**注意 `which -a cmake`** —— 如果它列出多个，确认排在最前的是新版：

```bash
$ which -a cmake
/home/ts/.local/bin/cmake     ← 希望是这个（新版）
/usr/bin/cmake                ← 系统自带 3.16
```

本项目 `run.sh` 会自动把 `~/.local/bin` 加到 PATH 前面，
并且会**先检查版本**，不满足就直接告诉你缺什么。

> 新版 `run.sh` 已内置版本自检，会打印 `✗ cmake 版本过低：3.16.3（需要 >= 3.21）`
> 和修复命令，不会让你对着 `CMake Error` 自己猜。

---

### Q2：编译报 `-DCMAKE_POLICY_VERSION_MINIMUM` 相关错误

新版 CMake 对旧版 `cmake_minimum_required` 更严格。本项目脚本已自动加上：

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

### Q3：`pkg-config` 找不到 srtp

名字是 **`libsrtp2`**：
```bash
pkg-config --exists libsrtp2 && echo OK
sudo apt-get install -y libsrtp2-dev
```

### Q4：STUN 拿不到 srflx 候选

本项目代码已处理两个已知陷阱（见坑 24）：
- libnice 0.1.16 的 `stun-server` **只接受 IP**（代码里自动做 hostname→IPv4 解析）
- 必须强制 IPv4（`getaddrinfo` 默认可能返回 IPv6，很多环境 IPv6 不通）

如果仍然拿不到，检查 UDP 出网：
```bash
python3 -c "
import socket
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(5)
s.sendto(b'\x00\x01\x00\x00\x21\x12\xA4\x42'+b'\x00'*12,('stun.l.google.com',19302))
print('UDP 出网可用', len(s.recvfrom(1024)[0]), '字节')
"
```

用本地 STUN 服务器也可以（`sudo apt-get install -y coturn`）：
```bash
turnserver -c /tmp/turn.conf &      # listening-port=3478
cd build && ./ice_test --stun 127.0.0.1 --stun-port 3478 --seconds 2
```

### Q5：`signal_server` 端口被占用

```bash
pkill -f signal_server
./scripts/run.sh call              # 脚本会自动清理旧进程
# 或手改端口
./build/signal_server --port 9001
```

### Q6：跑测试时很慢 / 卡住

大部分测试有 `timeout` 保护（脚本里已设）。如果 `ice_test` 卡住，
可能是 STUN 在等超时（默认 8 秒收集窗口）——不加 `--stun` 时很快。

### Q7：没有声音

见 [第 6 节「如果听不到声音」](#如果听不到声音)。

### Q8：想在 Windows 里直接编译（不用 WSL）

**不建议** —— 本项目用的是 Linux 下的 `libnice`/`libwebsockets` 等
apt 包，Windows 上需要 vcpkg 或 MSYS2 自行编译依赖。
WSL 是经过验证的路径（README 里有环境说明）。

---

## 12. 命令速查表

### 一键脚本

```bash
./scripts/run.sh                # 交互菜单（推荐）
./scripts/run.sh demo           # 完整演示
./scripts/run.sh build          # 编译
./scripts/run.sh test           # 全部测试
./scripts/run.sh loopback       # 自环 + 播放声音
./scripts/run.sh call           # 双端通话
./scripts/run.sh weaknet        # 弱网矩阵
./scripts/run.sh audio          # 生成音频
./scripts/run.sh env            # 环境自检
./scripts/run.sh help           # 帮助
```

### 辅助脚本

```bash
bash scripts/make_audio.sh /tmp/rtc_demo     # 生成 6 种测试音频（PCM+WAV）
bash scripts/make_listenable.sh              # 生成音频并复制到 Windows 桌面
bash scripts/full_regression.sh              # 全量回归（与 run.sh test 等价）
bash scripts/acceptance.sh                   # 从 GitHub 全新克隆后完整验收
bash scripts/goal_check.sh                   # 逐条核对功能完成度
bash scripts/verify_opus_bitrate.sh          # 验证 Opus 运行时码率遵循性
bash scripts/e2e_loss_matrix.sh              # 端到端多档丢包对比
bash scripts/check_docs.sh                   # 文档完整性检查
bash scripts/check_pitfalls.sh               # 踩坑编号连续性检查
```

### 编译产物

```bash
./build/selftest                     # 媒体层 6 组自检
./build/weaknet_test                # 弱网矩阵（抖动缓冲）
./build/peer                        # 通话端点（--loopback / --peer-test）
./build/signal_server               # WebSocket 信令服务器
./build/ice_test                    # ICE 穿透 + Trickle ICE
./build/dtls_srtp_test              # DTLS-SRTP 加密
./build/loss_protection_test        # NACK / FEC / 拥塞控制
./build/metrics_test                # 指标统计
./build/e2e_test                    # 端到端完整链路
```

### 试听

```bash
python3 /tmp/rtc_demo/pcm2wav.py 输入.pcm 输出.wav 16000    # PCM → WAV
paplay /tmp/rtc_demo/sweep.wav                              # WSL 内播放
```

---

## 附：目录结构

```
rtc-1v1-audio/
├── scripts/
│   ├── run.sh              ← 一键入口（本文档的主角）
│   ├── make_audio.sh       ← 生成测试音频
│   ├── make_listenable.sh  ← 生成音频并复制到 Windows
│   ├── full_regression.sh  ← 全量回归
│   ├── acceptance.sh       ← 全新克隆验收
│   └── ...                 ← 诊断/验证脚本（50+ 个，多为开发过程留档）
├── src/
│   ├── common/     日志、字节序、CRC32
│   ├── media/      Opus / 抖动缓冲(NetEq) / 虚拟设备(ADM) / 音频管线
│   ├── protocol/   RTP 兼容包格式 / 信令消息
│   ├── signal/     WebSocket 信令服务器 + 客户端
│   ├── ice/        ICE / STUN / Trickle ICE
│   ├── crypto/     DTLS-SRTP
│   ├── net/        NACK / FEC / 拥塞控制
│   ├── metrics/    统一指标统计
│   └── app/        通话端点
├── tests/          9 个测试程序
├── docs/
│   ├── 00-architecture.md   架构
│   ├── 01-progress-log.md   进度与设计决策
│   ├── 02-pitfalls.md       33 个踩坑
│   ├── 03-verification.md   实测数据
│   └── 04-how-to-run.md     ← 本文档
└── build/          编译产物（已 gitignore）
```
