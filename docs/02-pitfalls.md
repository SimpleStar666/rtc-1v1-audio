# 踩坑记录

> 真实遇到的报错、误判和修复过程，共 44 个坑。
> 这些是最有价值的部分——面试时"你踩过什么坑"考的就是这些。

**速览**：

| # | 坑 | 类型 | 影响 |
|---|---|---|---|
| 1 | 抖动缓冲把"乱序"误判为"迟到" | 逻辑 bug | 乱序包被丢弃 |
| 2 | Opus 逐样本比对得出 2.8dB SNR | 测试方法 | 误判编解码质量 |
| 3 | `OPUS_SET_XXX` 宏不能当函数参数 | 语言细节 | 编译失败 |
| 4 | CMake 3.16 不满足 3.21+ | 环境 | 配置失败 |
| 5 | GitHub tarball 不含 submodule | 环境 | 编译失败 |
| 6 | WSL 缺 CA 证书 / TLS 被拦截 | 环境 | git clone 失败 |
| 7 | 编译期缺头文件连锁报错 | 语言细节 | 编译失败 |
| 8 | 构造函数重复声明 | 语言细节 | 编译失败 |
| 9 | **组件内部读真实时钟，无法测试** | **可测试性设计** | 测试全为 0 |
| 10 | **目标延迟只跟平滑抖动** | **算法设计** | 接受率仅 43% |
| 11 | **丢包统计按到达顺序累加** | **统计口径** | 虚报 6 倍丢包 |
| 12 | `std::mutex` 非递归 → 自死锁 | 并发 | 进程卡死 |
| 13 | 虚拟播放设备只写不读 | 实现疏漏 | 缓冲爆满刷屏 |
| 14 | 条件变量固定超时 → 帧率损失 | 并发 | 5 秒少 5 帧 |
| 15 | lws `opaque_user_data` 未设置 | C/C++ 混用 | 回调拿不到对象 |
| 16 | **subprotocol 名两端不一致** | C/C++ 混用 | 握手被拒 |
| 17 | **裸内存里放 std::string** | C/C++ 混用 | 段错误 |
| 18 | **`lws_write` 未预留 LWS_PRE** | C/C++ 混用 | 堆越界崩溃 |
| 19 | **消息竞态导致误判超时** | 时序 | 偶发失败 |
| 20 | **发送缓冲覆盖式赋值** | 时序 | 消息丢失 |
| 21 | 头文件暴露 glib 类型 | C/C++ 混用 | 编译失败 |
| 22 | libnice 版本 API 差异 | 库版本 | 编译失败 |
| 23 | **agent 与主循环上下文不一致** ⭐ | **事件循环** | **收不到 STUN 响应** |
| 24 | **stun-server 只接受 IP / IPv6 优先** | 库配置语义 | 静默忽略配置 |
| 25 | **libsrtp2 API 三处不符预期** | C/C++ 混用 | 编译失败（含 most vexing parse）|
| 26 | **DTLS 默认不请求对端证书** ⭐ | **协议默认行为** | server 读不到对端证书 |
| 27 | **测试宏二次求值** ⭐ | **测试代码缺陷** | **全 PASS 却报失败** |
| 28 | **节流基准用真实时钟** ⭐ | **可测试性设计** | **仿真中码率永不更新** |
| 29 | **集成接线顺序错** ⭐ | **多组件集成** | **DTLS 握手必然超时** |
| 30 | **自定义头不兼容 RTP 布局** ⭐ | **协议接口约束** | **SRTP 完全不处理** |
| 31 | **Trickle ICE 漏交换 ufrag/pwd** ⭐ | **协议必要信息** | **候选全对但连不上** |
| 32 | **恢复率分母错 + 指标时长耦合真实时钟** ⭐ | **统计口径/可测试性** | **8/8 算成 50%，时长 3.7M ms** |
| 33 | **载荷字节与网络字节混用同一变量** ⭐ | **语义混淆** | **码率比目标高 73%** |
| 34 | **FEC 包污染 NACK 统计** ⭐ | **序号空间混淆** | **丢包数少报** |
| 35 | **root 编译导致普通用户 `Permission denied`** ⭐ | **用户身份/权限** | **用户完全跑不起来** |
| 36 | **新版 cmake 只装在 root 的 home** ⭐ | **工具链作用域** | **cmake 版本过低** |
| 37 | **Trickle 候选重复发送 6 倍** | **事件语义误用** | 信令带宽浪费 |
| 38 | **Trickle 回调注册晚于候选产生** ⭐ | **接线时序** | **候选丢失，ICE 连不通** |
| 39 | **抖动缓冲无锁** ⭐ | **并发安全** | **真实通话下数据竞争，可能崩溃** |
| 40 | **FEC 长度字段 u8** | **位宽不足** | **高码率下静默截断，恢复出垃圾** |
| 41 | **目标码率接口是空实现** ⭐ | **占位未实现** | **拥塞控制空转，决策不执行** |
| 42 | **视频一帧一包超过 MTU** | **协议设计** | **关键帧几乎收不到** |
| 43 | **视频丢关键帧后永久卡住** ⭐ | **帧间预测依赖** | **画面卡死且不自愈** |
| 44 | **PLI 限流基准 + 触发路径不全** ⭐ | **限流/假通过** | **高丢包时不请求关键帧** |

**各阶段最值得讲**：
- Phase 1：坑 **9、10、11**（算法与可测试性）
- Phase 2：坑 **16、17、18、20**（C 库与 C++ 混用）
- Phase 3：坑 **23、24**（事件循环 + 配置语义，定位难度最高）
- Phase 4：坑 **25、26、27**（库 API 细节 + 协议默认行为 + 测试自身缺陷）
- Phase 5：坑 **28**（"部分正常工作"比完全失效更难查）
- Phase 6：坑 **29、30**（集成接线 + 协议接口约束）
- 交付阶段：坑 **35、36**（开发环境 ≠ 用户环境）

> 坑 35/36 是**用户实际反馈**才暴露的：我之前所有验证都用 root 跑，
> 所以完全没发现普通用户 `ts` 根本编译不了。
> 教训写在文档最后：「用用户会用的身份、在用户会用的目录、跑用户会用的命令」。

---

## 坑 1：抖动缓冲把"乱序"误判为"迟到"（逻辑 bug）

### 现象

自检用例 `[FAIL] 乱序包被接受`。测试步骤：顺序 push 0~9，然后 push 20，再 push 18。

**期望**：18 应该被接受（乱序到达是正常现象）
**实际**：18 被判定为 "late" 并丢弃，`late=2`

### 根因

初版实现用**单一水位**判断：

```cpp
const int16_t diff = seq_diff(seq, next_expected_seq_);
if (diff < 0) {
    stats_.late++;   // 丢弃
    return false;
}
```

`next_expected_seq_` 在 push 20 之后变成了 21。此时 push 18：
`seq_diff(18, 21) = -3 < 0` → 被误判为迟到。

**但抖动缓冲的核心职责恰恰是吸收乱序**——网络包乱序到达是常态，
真正该丢的只有两类：
1. 已经被播放过的序号（`seq < play_seq_`）
2. 重复包

### 修复

把"统计水位"和"播放水位"分开：

```cpp
uint16_t next_expected_seq_;  // 见过的最大序号 +1 → 仅用于丢包统计
uint16_t play_seq_;           // 播放进度 → 判断"是否已播过"

// 迟到判定只看 play_seq_
if (have_seq_ && seq_diff(seq, play_seq_) < 0) { drop as late; }
```

同时重写 `pop()`：按 `play_seq_` 精确查找，找不到时分两种情况：
- 队列里最小序号 > play_seq_ → 该包确定丢失 → PLC
- 队列为空且已过播放时刻 → 该包不会来了 → PLC

### 验证

```
[PASS] 乱序包被接受（抖动缓冲的核心职责）
[PASS] 重复包被拒绝
[PASS] 检测到丢包
统计: recv=13 played=0 lost=14 plc=0 dup=1 late=0
```

### 教训

**"丢包检测"和"迟到判定"是两个不同的问题**，共用一个水位必然出错。
设计数据结构时先问：这个变量被谁读、代表什么语义。

---

## 坑 2：Opus 逐样本比对得出荒谬的 SNR（测试方法错误）

### 现象

自检输出：

```
RMSE=5803.2, SNR=2.8 dB
[FAIL] 失真在可接受范围 (RMSE < 800)
```

16kbps 的 Opus 不可能只有 2.8dB SNR。怀疑是测试方法而非编解码问题。

### 根因

Opus 是**带 lookahead（前瞻）延迟的有损编解码器**。
解码输出相对输入存在**固定时间偏移**（编码器需要前瞻若干样本才能确定最优编码）。

逐样本直接比对会把"时间偏移"全部算成"失真"——
正弦波在 440Hz 下，26.9ms 的偏移相当于约 12 个完整周期，
两个信号几乎完全反相 → 误差接近满幅。

### 修复

先做**互相关搜索最佳对齐偏移**，再在对齐后计算残差：

```cpp
for (int lag = 0; lag <= 2 * frame_samples; lag++) {
    // 计算 lag 偏移下的均方误差
    if (err < best_err) { best_err = err; best_lag = lag; }
}
```

### 验证

```
最佳对齐偏移=430 样本(26.9 ms)
RMSE=257.5, SNR=29.8 dB        ← 从 2.8dB 提升到 29.8dB
[PASS] 对齐后 SNR > 15dB（Opus 语音质量合格）
```

**26.9ms 的偏移量本身是有价值的观察**——它正是 Opus 在这个配置下的算法延迟。

### 教训

**测试失败时，先怀疑测试方法，再怀疑被测代码**。
音频测试的经典陷阱：任何有损编解码器都有延迟，直接逐样本比对必然得到错误结论。

---

## 坑 3：Opus 的 `OPUS_SET_XXX` 宏不能直接当函数参数

### 现象

```
error: no match for call to '(lambda(int, opus_int32, const char*)) (int, int, int, const char [8])'
```

传了 4 个参数，lambda 只接受 3 个。

### 根因

Opus 的控制宏设计特殊：

```c
#define OPUS_SET_BITRATE(x) OPUS_SET_BITRATE_REQUEST, __opus_check_int(x)
```

它展开成**两个**逗号分隔的表达式。用在函数调用里：

```cpp
set(OPUS_SET_BITRATE(16000), 16000, "bitrate");
//  展开为 →
set(OPUS_SET_BITRATE_REQUEST, 16000, 16000, "bitrate");   // 4 个实参
```

### 修复

统一使用 `*_REQUEST` 常量，把值作为独立参数传：

```cpp
set(OPUS_SET_BITRATE_REQUEST, opus_int32(bitrate_), "bitrate");
```

### 教训

遇到"参数个数不匹配"且代码看起来正常时，**检查宏展开**。
C 语言里"看起来像函数"的宏是最容易埋雷的地方。

---

## 坑 4：CMake 版本不足（3.16 vs 3.21+）

### 现象

```
CMake Error at CMakeLists.txt:33 (message):
  CMake >= v3.21 is required to build libdatachannel with media support
```

Ubuntu 20.04 自带 CMake 3.16，不满足要求。

### 尝试与结论

| 方案 | 结果 |
|---|---|
| `apt install cmake` | ❌ 仓库里就是 3.16 |
| `pip install cmake` | ✅ **成功，得到 4.4.4** |

### 修复

```bash
python3 -m pip install --user "cmake>=3.28"
export PATH=$HOME/.local/bin:$PATH   # 关键：pip 装的在 ~/.local/bin
```

**注意**：pip 会警告 `~/.local/bin is not on PATH`，必须显式加进 PATH。
构建脚本里已处理这一点。

### 附带发现

CMake 4.x 移除了对 `<3.5` 的兼容策略，老项目需要加：
```bash
-DCMAKE_POLICY_VERSION_MINIMUM=3.5
```
本项目 CMakeLists 已声明 3.21 最低版本，但加上该参数可兼容更多依赖。

---

## 坑 5：GitHub 归档 tarball 不含 submodule

### 现象

下载 libdatachannel tarball 后编译，报：

```
CMake Error: The source directory .../deps/libjuice does not contain a CMakeLists.txt file.
```

### 根因

**GitHub 的 `/archive/refs/tags/*.tar.gz` 打包时不含 submodule 内容**。
`deps/plog`、`deps/libjuice`、`deps/srtp`、`deps/usrsctp`、`deps/json` 全是空目录。

`.gitmodules` 文件存在，但需要 `git clone --recurse-submodules` 才能填充——
而本环境 git 的 HTTPS 被 TLS 中间拦截，clone 失败。

### 绕行方案

改用 **apt 提供的系统库**（libnice 替代 libjuice、libsrtp2、libusrsctp、
nlohmann-json3），并手动下载 plog（header-only）填充：
```bash
cmake -DUSE_NICE=ON -DUSE_SYSTEM_SRTP=ON -DUSE_SYSTEM_USRSCTP=ON -DUSE_SYSTEM_JSON=ON ...
```

### 结论

最终**没有采用 libdatachannel**，而是直接用 apt 的原始组件自建——
更可控，也更符合"理解每一层"的项目目标。

### 教训

下载开源项目源码时，**优先用 `git clone --recurse-submodules`**；
用 tarball 前先检查 `deps/` 是否为空。

---

## 坑 6：WSL 缺 CA 证书导致所有 HTTPS 失败

### 现象

```bash
$ git clone https://github.com/...
fatal: unable to access '...': server certificate verification failed.
       CAfile: none CRLfile: none
```

但 `curl -k`（跳过校验）能通，`apt` 也能通。

### 诊断过程

```bash
$ ls -la /etc/ssl/certs/ca-certificates.crt
-rw-r--r-- 1 root root 219342 ...      # CA 文件存在！

$ openssl s_client -connect github.com:443 < /dev/null | grep "Verify return"
Verify return code: 21 (unable to verify the first certificate)
```

**CA 文件存在但仍校验失败** → 说明 TLS 被中间设备拦截（证书链不完整），
不是缺少根证书。

### 应对

- **Git 操作**：改用 GitHub API（本项目通过 MCP 通道推送，不受影响）
- **下载源码**：用 `curl -sk`（跳过校验）——在有中间拦截的环境里是可接受的取舍

### 教训

`CAfile: none` 不一定代表缺证书。若 CA 文件确实存在却仍失败，
应怀疑**中间人拦截**（企业网络/代理），此时 `openssl s_client` 的
`Verify return code` 能给出决定性证据。

---

## 坑 7：编译期缺少头文件导致的连锁错误

### 现象

```
error: 'put_u16_le' was not declared in this scope
error: 'condition_variable' is not a member of 'std'
error: 'std::this_thread' has not been declared
error: 'frames' was not declared in this scope
```

### 根因

多个文件忘了 include 对应头文件：
| 缺失符号 | 需要的头文件 |
|---|---|
| `put_u16_le` / `get_u16_le` | `common/util.h` |
| `std::condition_variable` | `<condition_variable>` |
| `std::this_thread` / `std::chrono` | `<thread>` / `<chrono>` |
| `std::atomic` | `<atomic>` |
| `std::find_if` | `<algorithm>` |

### 教训

**头文件要"自己满足自己"**：一个 .cpp 用到的每个类型和函数，
都应该能追溯到本文件里显式 include 的头文件，不要依赖其他头文件的传递包含。

（顺带：`util.h` 里的 `Stats::stddev()` 用了 `sqrt` 却没 include `<cmath>`，
属于同类问题，已修复。）

---

## 坑 8：构造函数重复声明

### 现象

```
error: 'JitterBuffer::JitterBuffer(const Config&)' cannot be overloaded with
       'JitterBuffer::JitterBuffer(const Config&)'
```

### 根因

编辑过程中，头文件里同时留下了两处声明：
```cpp
JitterBuffer();
explicit JitterBuffer(const Config& cfg);
...
explicit JitterBuffer(const Config& cfg = {});   // 重复！
```

### 教训

**编辑头文件后要通读一遍**。编译器只能告诉你"重复了"，
不会告诉你"哪一处才是你想要的"。

---

## 坑 9：组件内部直接读真实时钟，导致无法测试（设计缺陷）

### 现象

弱网仿真跑完，所有用例都是 `播放帧数: 0`：

```
=== A. 理想网络 ===
  抖动缓冲接受  : 500
  播放帧数      : 0        ← 收到 500 个包，却一帧都没播
  PLC 补偿次数  : 0
  播放连续性    : 0.0%
```

### 根因

`JitterBuffer::pop()` 内部调用 `now_ms()` 读取**真实系统时钟**，
而弱网测试用**逻辑时间戳**（`arrive_ms`）驱动仿真：

```cpp
// 测试侧：逻辑时间从 0 开始
int64_t arrive = send_time + delay + jitter;   // 例如 50ms

// 组件内部：真实时间是从开机算起的一大坨数
if (!playing_ && now_ms() < play_deadline_ms_) return false;   // 永远为真 → 永远不播
```

真实时间（如 93815ms）永远小于逻辑时间轴上的 `play_deadline_ms_`（如 80ms）之外的东西吗？
不——问题在于两者**根本不在同一个时间基准上**，比较结果毫无意义。

### 修复

把时间作为**可选参数**注入，而不是组件内部直接读：

```cpp
bool pop(std::vector<uint8_t>* payload, int64_t now = -1);
// now < 0 时取真实 now_ms()（生产路径不变）
// now >= 0 时用调用方给的时间（测试路径可以快进）
```

测试侧改为按 **1ms 步进推进逻辑时间**的仿真主循环：

```cpp
for (int64_t now = t0; now <= end_ms; now++) {
    while (有包到达时间 <= now) jb.push(...);
    while (jb.pop(&payload, now)) { ... }
}
```

### 收益

不只是修好测试——**跑 10 秒弱网仿真只花几十毫秒真实时间**。

### 教训

**依赖真实时钟的代码是不可测的**。
设计需要仿真/回放的组件时，把"时间"当作依赖显式传入（依赖注入），
而不是在内部隐式获取。这个原则同样适用于随机数、文件系统、网络。

---

## 坑 10：目标延迟只跟"平滑抖动"，导致大量误判丢包（算法设计错误）

### 现象

修好坑 9 之后，播放正常了，但**丢包统计严重虚高**：

```
=== E. 大抖动无丢包（±80ms，0% 丢包）===
  发送帧数      : 500
  丢包帧数      : 0 (0.0%)      ← 实际一个包都没丢
  抖动缓冲接受  : 215           ← 只接受了 215 个！
  检测丢包      : 294           ← 报出 294 个丢包（假的）
  PLC 补偿次数  : 333
```

"0% 丢包"的网络报出 294 个丢包，显然不对。

### 根因

两个相关缺陷：

**(a) 目标延迟低估突发抖动**

初版用**平滑抖动**的三倍作目标延迟：

```cpp
if (jitter_ms_ * 3 > cfg_.target_delay_ms)
    want = int(jitter_ms_ * 3) + cfg_.frame_ms;
```

但 `jitter_ms_` 是 RFC 3550 风格的**指数平滑值**（α=1/16），它反映的是长期趋势，
**会被突发抖动"平摊"掉**。±80ms 的抖动经平滑后可能只有 ~25ms，
于是目标延迟设为 ~48ms —— 远小于实际需要的 ~120ms。

结果：大量包在"该播的时刻"还没到达，被判为丢失并触发 PLC。

**(b) 起播门槛太低**

只判断"目标延迟已过"就起播。如果包还在途中，起播后立刻 underflow。

### 修复

**(a) 目标延迟必须覆盖"最坏情况"**，而不只是平均值：

```cpp
// 维护近期（约 50 帧窗口）的最大到达偏差
if (d > recent_max_dev_ms_) recent_max_dev_ms_ = d;
if (++recent_count_ >= kMaxDevWindow) {
    recent_count_ = 0;
    recent_max_dev_ms_ *= 0.7;    // 窗口滚动，允许随网络改善而下降
}

// 目标延迟取"平滑值 2 倍"与"近期最大值 1.5 倍"的较大者
const double need_ms = std::max(jitter_ms_ * 2.0, recent_max_dev_ms_ * 1.5);
int want = int(need_ms) + cfg_.frame_ms;

// 上调要快（避免断音），下调要慢（避免抖动回来时又不够）
if (want > cfg_.target_delay_ms)      cfg_.target_delay_ms = want;
else if (want < cfg_.target_delay_ms && queue_.size() <= 2)
    cfg_.target_delay_ms -= kAdaptStepMs;
```

**(b) 起播要等缓冲攒够帧**，而不只是等时间：

```cpp
bool ready_to_play(int64_t now) const {
    if (playing_) return true;
    if (queue_.size() < min_playout_frames_) return false;   // 新增：攒够帧
    return now >= play_deadline_ms_;
}
// min_playout_frames_ 随目标延迟自适应：target_delay / frame_ms / 2
```

**(c) 延长"判定丢失"的容忍窗口**

发现序号空洞时，不能立刻判丢——那个包**可能还在网络上**：

```cpp
// 旧：只要过了播放时刻就判丢
if ((nothing_arrived || ahead) && t >= play_deadline_ms_) → PLC

// 新：额外再等一个目标延迟 + 一个帧长
const int64_t wait_until = play_deadline_ms_ + cfg_.target_delay_ms + cfg_.frame_ms;
if (t >= wait_until) → PLC
```

### 验证（修复前后对比）

| 用例 | 指标 | 修复前 | 修复后 |
|---|---|---|---|
| **A 理想网络** | 播放帧数 | 500 | 500 |
| | PLC | 47 | 45 |
| **D 重度丢包+强抖动** | 接受 | 239 | **432** |
| | PLC | 305 | **106** |
| **E 大抖动无丢包** | 接受 | 215 | **499** |
| | PLC | 333 | **42** |
| | 目标延迟 | 96ms | 105ms（合理加深） |

**"接受 215 → 499"这一项提升最显著**：说明大量原本被误判丢掉的包，实际上是正常到达的。

### 教训

**抖动缓冲的目标延迟必须由"最坏抖动"决定，而不是"平均抖动"。**
平均值会让你在网络突发时不断断音——而这个错误恰恰是"看起来在正确调参"的陷阱。

---

## 坑 11：丢包统计按"到达顺序"累加，把乱序算成丢包（统计口径错误）

### 现象

修好坑 10 后，误判丢包大幅下降，但仍有一处不合理：

```
=== C. 中度丢包（8%）===
  丢包帧数      : 38 (7.6%)     ← 实际丢 38
  检测丢包      : 135           ← 报了 135
```

### 根因

统计逻辑按**到达顺序**累加序号空洞：

```cpp
} else if (seq_diff(seq, next_expected_seq_) > 0) {
    stats_.lost += seq_diff(seq, next_expected_seq_);   // 累加"洞"
}
next_expected_seq_ = seq + 1;                            // 水位跟随到达顺序
```

**乱序到达时这必然出错**。举例：到达顺序是 `10, 13, 11, 12`
- 收到 13：水位从 11 跳到 13 → 累加 2 个"丢包"
- 收到 11、12：水位又回落，但**已累加的 2 不会被扣回**

于是真实丢包 0，统计却报 2。抖动越大、乱序越多，虚高越严重。

### 修复

**不要在到达时累加，改为在快照时统一计算**：

```cpp
// 到达时只记录"序号范围"和"独立到达数"
arrived_distinct_++;
if (seq_diff(seq, highest_seq_) > 0) highest_seq_ = seq;
if (seq_diff(seq, lowest_seq_) < 0)  lowest_seq_ = seq;

// snapshot() 时统一算：
//   序号跨度 = highest − lowest + 1
//   丢包数   = 跨度 − 独立到达数
const uint64_t span = uint64_t(seq_diff(highest_seq_, lowest_seq_)) + 1;
s.lost = (span > arrived_distinct_) ? (span - arrived_distinct_) : 0;
```

原理：**丢包 = 序号空间里的"空洞"数量**，与到达顺序无关。
用集合运算代替顺序累加，天然免疫乱序。

（重复包在入队前已被拒绝，所以不会污染 `arrived_distinct_`。）

### 验证（统计准确性对照）

| 用例 | 实际丢包 | 修复前检测 | **修复后检测** |
|---|---|---|---|
| A 理想网络 | 0 | 0 | **0** ✅ |
| B 轻度 (2%) | 6 | 6 | **6** ✅ |
| C 中度 (8%) | 38 | 135 | **38** ✅ |
| D 重度 (15%) | 67 | 238 | **67** ✅ |
| **E 大抖动 0% 丢包** | 0 | 245 | **0** ✅ |

**修复后 5 个用例的"检测丢包"与实际丢包数逐一精确吻合**。

### 教训

**"乱序"和"丢包"是两个正交的概念**，不能用同一个顺序累加器表达。
凡是统计量，先问清楚：它该由"事件顺序"决定，还是由"状态集合"决定？
选错会导致系统性地高估问题严重性——而这类错误在监控指标上最难被发现。

---

## 坑 12：`std::mutex` 非递归 → 自死锁（并发）

### 现象

loopback 自环测试运行到一半卡死，进程不退出：

```
$ ./peer --loopback --seconds 5
... 卡住，无任何输出 ...
$ gdb -p <pid>
#0  __lll_lock_wait ()                 ← 卡在等锁
#1  std::mutex::lock
#2  rtc::AudioPipeline::bitrate_kbps  ← 试图再次获取同一把锁
#3  rtc::AudioPipeline::report
#4  rtc::AudioPipeline::run_loop
```

### 根因

`report()` 里调用了 `bitrate_kbps()`，而后者又要获取 `send_mutex_`；
但 `report()` 的调用点**已经持有**这把锁：

```cpp
void AudioPipeline::report() {
    std::lock_guard<std::mutex> lk(send_mutex_);   // ① 上锁
    ...
    double br = bitrate_kbps();                    // ② 内部再次 lock
    ...
}

double AudioPipeline::bitrate_kbps() {
    std::lock_guard<std::mutex> lk(send_mutex_);   // ③ 死等 ①
    ...
}
```

`std::mutex` **不是递归锁**——同一线程重复 lock 会永久阻塞。
`futex_wait` 就是它的底层表现。

### 修复

提供"假定已持锁"的版本，加 `_locked` 后缀明确前提：

```cpp
// 公开接口：内部加锁
double bitrate_kbps() {
    std::lock_guard<std::mutex> lk(send_mutex_);
    return bitrate_kbps_locked();
}

// 前提：调用者已持有 send_mutex_
double bitrate_kbps_locked() const { ... }   // 不加锁
```

`report()` 改调 `bitrate_kbps_locked()`。

### 教训

**加锁边界要在设计时确定，不能一层层往里加锁。**
判断规则：**如果一个方法可能被"已持锁"的代码调用，就必须同时提供 `_locked` 版本。**

替代方案（各有代价，理解取舍比记住结论重要）：
| 方案 | 优点 | 缺点 |
|---|---|---|
| `_locked` 后缀约定（本项目采用） | 零开销、意图明确 | 靠命名约定，无编译期保证 |
| `std::recursive_mutex` | 不会自死锁 | 掩盖设计问题；持有时间被拉长 |
| 缩小锁粒度 | 并发性更好 | 改动大，易引入新竞态 |

**面试要点**：`std::recursive_mutex` 不是"更安全的 mutex"——
它常常是**掩盖了应该重构的锁设计**。本项目选择暴露问题而不是绕过它。

---

## 坑 13：虚拟播放设备只写不读 → 缓冲爆满刷屏（实现疏漏）

### 现象

日志被同一条消息刷屏，且播放延迟不断增长：

```
[WARN][adm] play buffer full, drop 320 samples
[WARN][adm] play buffer full, drop 320 samples
... （数千行）
```

### 根因

`VirtualAudioDevice` 的 `play()` 只把数据写进播放缓冲，
但**没有任何消费者把它取走**：

```cpp
void VirtualAudioDevice::play(const int16_t* pcm, int samples) {
    // 只往 play_buffer_ 里塞
    play_buffer_.insert(play_buffer_.end(), pcm, pcm + samples);
    // ← 没有对应的"取出并播放"逻辑
}
```

真实声卡有 DMA 硬件按固定采样率取走数据；虚拟设备必须**自己模拟这个消费者**，
否则缓冲只增不减。

### 修复

加一个独立线程模拟 DMA 消费（每 10ms 取 160 样本）：

```cpp
void VirtualAudioDevice::start_playout() {
    playout_thread_ = std::thread([this] {
        while (running_) {
            // 每 10ms 消费一个 chunk（160 样本 = 10ms @16kHz）
            consume_chunk(160);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
}
```

并统计 `underruns`（缓冲空 = 播放断流）。

### 验证

```
修复前: 缓冲无限增长 + 数千行 WARN
修复后: 播放缓冲稳定在目标水位，无刷屏
        采集 25 帧/500ms（与真实时钟精确一致）
```

### 教训

**"虚拟设备"必须模拟真实设备的完整行为，不能只实现写入端。**
真实音频设备的三个关键行为：
1. **有独立的消费节拍**（DMA 按时钟取数据）—— 漏了它缓冲就爆
2. **有有限缓冲**（不够就 underrun）—— 漏了它就无法暴露供给不足
3. **有时钟漂移**（采集与播放时钟不严格相等）—— 本项目未模拟，已如实声明

**测试替身（test double）的原则**：替代品要保真到"能暴露真实问题"的程度，
否则测试通过但上线就出问题。

---

## 坑 14：条件变量固定超时 → 帧率损失（并发）

### 现象

发送节拍精确性测试显示 5 秒内少了 5 帧（应 250 帧，实际 245 帧，约 2% 损失）：

```
[FAIL] 采集节拍接近实时（约 25 帧/500ms）
       实测 245 帧 / 5000ms
```

### 根因

发送线程用**固定超时**等待条件变量：

```cpp
while (running_) {
    std::unique_lock<std::mutex> lk(send_mutex_);
    send_cv_.wait_for(lk, std::chrono::milliseconds(50));   // ✗ 固定 50ms
    do_send();
}
```

问题在于：`wait_for(50ms)` 的语义是"**最多**等 50ms"——
它可能在 48ms 就因某个通知返回，也可能因为调度延迟变成 52ms。
**每次的误差累积**，几千次后就是明显的帧率漂移。

而实际需求是"按 20ms 节拍精确发送"——这需要**绝对时间基准**，不是相对超时。

### 修复

改成"等到下一个绝对时间点"，并让停止信号主动唤醒：

```cpp
// 唤醒用的通知：只在停止时触发，不干扰节拍
while (running_) {
    const int64_t next = last_send_ms_ + kFrameMs;   // 绝对时间点
    std::unique_lock<std::mutex> lk(send_mutex_);
    if (send_cv_.wait_until(lk, ms_to_timepoint(next), [this]{ return !running_; }))
        break;                                        // 被停止信号唤醒
    lk.unlock();
    do_send();
    last_send_ms_ = next;
}
// 停止时：
running_ = false;
send_cv_.notify_all();     // 主动唤醒，避免等满整个周期
```

关键改动：
1. 用 `wait_until` + **绝对时间点**（误差不累积）
2. 有谓词（`!running_`），停止时能立即退出
3. 停止时主动 `notify_all`（否则要白等一个周期）

### 验证

```
修复前: 245 帧 / 5000ms（2% 损失，误差累积）
修复后: 252 帧 / 3 秒（精确），84 包，0 编解码错误，0 PLC
```

### 教训

**"定时"要用绝对时间基准，不能用相对超时累加。**

| 写法 | 误差行为 |
|---|---|
| `sleep_for(20ms)` / `wait_for(20ms)` 循环 | **累积**（每次的调度误差都留下） |
| 计算绝对时间点 + `sleep_until` / `wait_until` | **不累积**（自动补偿上一次的偏差） |

这在音视频里尤其重要：20ms 节拍累积 2% 误差，
在 1 小时通话里就是 **72 秒**的漂移——足以造成明显的音画不同步或缓冲溢出。

**条件变量还要注意"谓词 + 主动唤醒"**：
没有谓词，停止时要等满整个超时；没有主动唤醒，停止延迟不可控。

---

## 坑 15：libwebsockets 的 opaque_user_data 未设置

### 现象

信令客户端连接后回调里拿不到 C++ 对象，`lws_get_opaque_user_data(wsi)` 返回 NULL。

### 根因

`lws_client_connect_info` 结构体里**漏了设置 `opaque_user_data`**：

```cpp
lws_client_connect_info ci;
memset(&ci, 0, sizeof(ci));
ci.context = ctx;
ci.address = host_.c_str();
// ... 但没有 ci.opaque_user_data = this;
```

### 修复

```cpp
ci.opaque_user_data = this;   // 关键：让回调能找回 C++ 对象
```

### 为什么不用 per-session user data

`connect` 返回前 session 尚未创建，因此无法在连接发起阶段填充 per-session
user data。而 `opaque_user_data` 在整个连接生命周期内都有效。

### 教训

C 库的"回调 + 上下文指针"模式里，**上下文指针必须显式设置**，
不要假定它会自动关联。`memset(&ci, 0, sizeof(ci))` 之后每一项都要检查。

---

## 坑 16：Subprotocol 名称两端不一致，握手失败

### 现象

客户端连接后立刻断开，服务器日志：

```
N: No supported protocol "rtc-signal-cli"
```

### 根因

WebSocket 的 subprotocol 协商要求**两端协议名完全相同**：

| 端 | 协议名 |
|---|---|
| 服务器 | `rtc-signal` |
| 客户端 | `rtc-signal-cli` ← 多了 `-cli` 后缀 |

libwebsockets 找不到匹配的协议就直接拒绝连接。

### 修复

客户端协议名改为与服务器一致：

```cpp
static const lws_protocols protocols[] = {
    {"rtc-signal", client_callback, sizeof(PerSessionData), 4096, 0, nullptr, 0},
    ...
};
```

### 教训

用 libwebsockets（或其他带 subprotocol 协商的库）时，
**协议名应作为共享常量定义**，两端引用同一处，而不是各自硬编码字符串。

---

## 坑 17：在 libwebsockets 的 user data 里放 std::string → 段错误

### 现象

连接建立后立即段错误：

```
Thread 2 received signal SIGSEGV
#0 std::string::_M_set_length (__n=0, this=0x7ffff002b8b8)
#2 client_callback ... signaling_client.cpp:220
```

崩溃在 `data->rx.clear()`，而 `data` 就是 per-session user data。

### 根因

**libwebsockets 的 per-session user data 是它自己用 `malloc` 分配的裸内存**，
不会调用 C++ 构造函数（也不会调析构函数）：

```cpp
struct PerSessionData {
    SignalingClient* client = nullptr;
    std::string      rx;      // ❌ 内部指针是未初始化的野值
};
```

`std::string` 的 `_M_data()` 指针来自构造函数初始化；在裸内存上，
该指针是随机值。第一次 `clear()` 就访问野指针 → 段错误。

### 修复

**user data 里只放 POD 成员**，把 C++ 对象放到类自己的成员里：

```cpp
struct PerSessionData {
    SignalingClient* client = nullptr;   // ✓ 仅指针，lws 会零初始化
};

// 接收缓冲改为 SignalingClient 的成员
std::string& rx = client->rx_buffer();
```

### 对比：什么时候可以放 C++ 容器

服务器端的 `Session` 是**自己 `new`/`delete` 管理的**，不是 lws 的裸内存，
所以那里可以安全地持有 `std::vector` / `std::deque`：

```cpp
// 服务器（安全）
Session* s = new Session();      // 构造被调用
...
delete s;                        // 析构被调用
```

**判断标准**：这块内存是谁分配的、生命周期谁管。
由 C 库 malloc 的裸内存，只能放 POD。

### 教训

**C++ 对象不能放进 C 库管理的裸内存。**
这是 C/C++ 混用最常见的崩溃来源，而且症状（段错误位置）与根因（对象未构造）距离很远，
靠 gdb 回溯到 `std::string::clear()` 才看清。

---

## 坑 18：`lws_write` 未预留 LWS_PRE → 堆越界

### 现象

服务器在处理配对时崩溃：

```
double free or corruption (out)
Aborted (core dumped)
```

### 根因

libwebsockets 要求 `lws_write` 的缓冲区**前面必须预留 `LWS_PRE` 字节**
（供它写 WebSocket 协议头）。早期版本直接传自己的缓冲裸指针：

```cpp
int n = lws_write(wsi, s->tx.data(), s->tx.size(), LWS_WRITE_TEXT);
```

libwebsockets 会往 `data()` 之前的内存写协议头 → **堆元数据被破坏** →
后续 `free` 时报 "double free or corruption"。

### 修复

```cpp
std::vector<uint8_t> outbuf(LWS_PRE + msg.size());
memcpy(outbuf.data() + LWS_PRE, msg.data(), msg.size());
int n = lws_write(wsi, outbuf.data() + LWS_PRE, msg.size(), LWS_WRITE_TEXT);
```

### 教训

**堆越界的报错位置与出错位置通常相隔很远**（这里是在后续 free 时才报）。
所以看到 "double free or corruption" 时，不要只盯着 free 的代码，
要往前找**所有写缓冲区的地方**。

`LWS_PRE` 这个要求在文档里，但容易漏——客户端和服务器两端都要注意。

---

## 坑 19：消息竞态导致一端误判"入房超时"

### 现象

两 peer 配对时，偶发一端报超时：

```
[peer B] 入房超时
```

但服务器日志显示 peer#2 确实入房成功了。

### 根因

服务器在处理 peer#2 的 join 时，**同一个处理流程里连续两次调用了 `queue_text`**：

```cpp
// 给入房者发 joined
queue_text(wsi, joined.to_json());
// 紧接着，发现对端在，再发 peer_ready
if (peer) queue_text(wsi, ready.to_json());   // ← 覆盖了上一条
```

而当时 `queue_text` 用的是**单槽缓冲**（`s->tx.assign(...)`），
第二次调用直接把第一条覆盖掉了。于是 peer#2 只收到 `peer_ready`、收不到 `joined`。

**这是时序相关的**：两条消息间隔足够大时才不丢，所以表现为偶发。

### 修复（两处）

**(a) 服务器：发送缓冲改为队列**（见坑 20）

**(b) 客户端：放宽等待条件**——不要死等某一个消息：

```cpp
// 旧：必须等到 joined
for (...) if (joined) break;
if (!joined) { 报超时; return 1; }

// 新：joined 与 peer_ready 任一到达即视为配对成功（顺序不定）
for (...) if (joined && peer_ready) break;
if (!joined && !peer_ready) { 报超时; return 1; }
```

同时"信令面通过"的判定从"必须有候选"改为"(收到 offer 或 answer) 之一"——
**主叫收 answer、被叫收 offer，两端不可能同时收到两者**。

### 教训

**"等待某条消息"的代码要问：这条消息一定会来吗？顺序确定吗？**
网络消息的顺序通常不保证，严格的单点等待是脆弱设计。
更稳的做法是等待"状态达成"（一组条件的组合），而不是"某个事件到达"。

---

## 坑 20：发送缓冲用覆盖式赋值 → 消息丢失

### 现象

同坑 19：服务器连续发两条消息时，先发的被后发的覆盖。

### 根因

```cpp
struct Session {
    std::vector<uint8_t> tx;   // 单槽
};

int queue_text(lws* wsi, const std::string& msg) {
    s->tx.assign(msg.begin(), msg.end());   // ❌ 覆盖已有内容
    lws_callback_on_writable(wsi);
}
```

`assign()` 的语义是**替换**，不是追加。只要有两条消息在"写入回调尚未执行"
的窗口内入队，第一条必然丢失。

### 修复

改为真正的队列：

```cpp
struct Session {
    std::deque<std::string> tx;   // ✓ 队列
};

int queue_text(lws* wsi, const std::string& msg) {
    s->tx.push_back(msg);          // 追加
    lws_callback_on_writable(wsi);
}

// 可写回调里每次只发队首一条（lws_write 每次处理一条消息）
case LWS_CALLBACK_SERVER_WRITEABLE: {
    const std::string& msg = s->tx.front();
    ... lws_write(...);
    s->tx.pop_front();
    if (!s->tx.empty()) lws_callback_on_writable(wsi);   // 还有就继续
    break;
}
```

### 教训

**区分"赋值/覆盖"与"入队/追加"的语义**。
凡是"待发送/待处理"的缓冲，默认应该是队列——
除非能证明任何时刻最多只有一个元素（这里显然不成立，
因为服务器在配对时会一次推送两条消息）。

这类 bug 的特点是**在快速路径上不出现，只在时序紧的时候丢消息**，
最难排查。用队列是低成本的根治手段。

---

## 坑 21：libnice 回调签名在头文件中引入 glib 类型

### 现象

```
error: 'guint' has not been declared
error: 'gpointer' has not been declared
error: no declaration matches 'void rtc::IceTransport::cb_gathering_done(NiceAgent*, guint, gpointer)'
```

### 根因

头文件里声明的静态回调用了 `guint` / `gchar` / `gpointer`（glib 的 typedef），
但头文件不应引入 glib 依赖（会污染所有包含者）。
另外 `nice_agent.h` 里用的 guint 与头文件里的声明必须 ABI 一致才能匹配。

### 修复

头文件用标准 C 类型（glib 的 `guint` 就是 `unsigned int`，`gchar` 是 `char`，
`gpointer` 是 `void*`——ABI 完全等价）：

```cpp
// 头文件
static void cb_gathering_done(NiceAgent* agent, unsigned int stream_id, void* user_data);
static void cb_new_selected_pair(NiceAgent* agent, unsigned int stream_id,
                                 unsigned int component_id, char* lfoundation,
                                 char* rfoundation, void* user_data);
```

实现文件里 `#include <glib.h>` 后可以用 glib 类型，但为了一致性也用标准类型。

### 教训

**跨模块的头文件不要把第三方库的类型暴露出去**（除非该库是接口的一部分）。
用 ABI 等价的标准类型既避免依赖传播，也不会影响链接。

---

## 坑 22：libnice 版本间 API 差异

### 现象

```
error: 'nice_agent_set_remote_candidates_done' was not declared in this scope
```

### 根因

`nice_agent_set_remote_candidates_done()` 是 **libnice 更新版本**的 API，
Ubuntu 20.04 自带的 **0.1.16 里不存在**。

### 修复

删掉这次调用即可——`nice_agent_parse_remote_sdp()` **本身就完成了
"设置远端候选 + 启动连通性检查"**两件事，不需要额外通知。

### 教训

**对着系统实际安装的头文件写代码**，不要按最新文档写。
查阅版本差异的方式：

```bash
grep -n "nice_agent_set_remote" /usr/include/nice/agent.h
```

更普遍地说：跨版本兼容时，先用 `grep` 确认 API 是否存在再写。

---

## 坑 23：agent 与 glib 主循环的上下文不一致 → STUN 响应收不到（★★★ 最难定位）

### 现象

STUN 配置正确、网络可达，但**永远拿不到 srflx 候选**：

```
A 候选类型: host×6 srflx×0 relay×0
```

而 `gathering` 在 0.2 秒内就"完成"了——远快于 STUN 往返应有的时间。

### 定位过程（一步步排除）

**(1) 确认网络可达**（裸 socket 能拿到 STUN 响应）
```python
s.sendto(stun_binding_request, ("stun.l.google.com", 19302))
# 收到 32 字节响应 → UDP 出网正常
```

**(2) 开启 libnice 内部日志**，看到关键线索：
```
libnice-stun-DEBUG: STUN transaction retransmitted (timeout 397ms).
libnice-stun-DEBUG: STUN transaction retransmitted (timeout 795ms).
...
```
**STUN 请求发出去了，但在不断重传** —— 说明响应"没收到"。

**(3) tcpdump 抓包 —— 这一步给出了决定性证据**：
```
192.168.0.197.45878 > 74.125.250.129.19302: UDP, length 20   ← 请求发出
74.125.250.129.19302 > 192.168.0.197.45878: UDP, length 32   ← 响应回来了！
192.168.0.197.45878 > 74.125.250.129.19302: UDP, length 20   ← 却又重传
```
**响应明明到达了网卡，libnice 却说没收到。**

**(4) 定位根因**：上下文不一致。

```cpp
// 旧代码
agent_ = nice_agent_new(g_main_context_default(), ...);  // agent 挂在 default 上下文
loop_  = g_main_loop_new(NULL, FALSE);                   // 主循环迭代 thread-default
                                                          // ← 不是同一个上下文！
```

libnice 把它的 socket source 挂在 `default` 上下文上，而主循环迭代的是
**thread-default** 上下文。结果：socket 可读事件永远不被分发，
数据包躺在内核缓冲区里，agent 读不到 → 超时重传。

### 修复

**让 agent 与主循环共用一个显式创建的上下文**：

```cpp
ctx_ = g_main_context_new();                          // 自己创建
agent_ = nice_agent_new(ctx_, NICE_COMPATIBILITY_RFC5245);
loop_  = g_main_loop_new(ctx_, FALSE);                // 同一个 ctx
nice_agent_attach_recv(agent_, stream, comp, ctx_, cb, this);   // 也传 ctx_
```

### 验证

```
修复前: A 候选类型: host×6 srflx×0 relay×0
修复后: A 候选类型: host×6 srflx×3 relay×0    ← 拿到 3 个公网映射候选
```

### 教训

**glib 的主上下文是"隐式全局状态"，最容易出错的地方。**
`g_main_context_default()` 与 `g_main_loop_new(NULL, ...)`（thread-default）
**是两个不同的上下文**，名字看着像但其实无关。

涉及 glib 主循环的库（libnice、GStreamer 等）都要注意：
**创建对象时用的上下文，必须与迭代事件用的上下文是同一个**。

定位这类问题的有效手段（按成本从低到高）：
1. 库的内部日志（`G_MESSAGES_DEBUG=all`）—— 看到"现象层"线索
2. **tcpdump 抓包** —— 区分"没发出"和"发出了没收到"
3. 最小复现 —— 排除自身代码复杂度

第 (3) 步的 tcpdump 在这里是关键：它把问题从"STUN 不通"精确定位到
"响应到达但未被处理"，从而把排查范围从网络层缩小到事件循环层。

---

## 坑 24：libnice 的 stun-server 只接受 IP，且必须强制 IPv4

### 现象

配置 `stun.l.google.com` 后仍然没有 srflx 候选。

### 根因（两个叠加）

**(a) libnice 0.1.16 的 stun-server 属性只接受 IP 地址**

从动态库的字符串里能看到属性文档原文：
```bash
$ strings libnice.so.10 | grep -i "STUN server"
"The IP address (not the hostname) of the STUN server to use"
```

传域名会被**静默忽略**——不报错、不警告、不收集 srflx。

**(b) getaddrinfo 默认可能优先返回 IPv6**

```bash
$ getent hosts stun.l.google.com
2001:4860:4864:5:8000::1          # ← 返回的是 IPv6！
```

WSL 的 IPv6 出网不通，于是 STUN 请求发往 IPv6 地址后一直重传超时。

### 修复

自己做一次**强制 IPv4** 的解析：

```cpp
std::string resolve_ipv4(const std::string& host) {
    struct in_addr tmp;
    if (inet_pton(AF_INET, host.c_str(), &tmp) == 1) return host;  // 已是 IP

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          // ★ 只要 IPv4
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return "";

    char buf[INET_ADDRSTRLEN] = {0};
    auto* sa = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    const char* ok = inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
    freeaddrinfo(res);
    return ok ? std::string(buf) : std::string();
}
```

### 验证

```
[INFO][ice] STUN configured: stun.l.google.com (74.125.250.129):19302
A 候选类型: host×6 srflx×3 relay×0     ← 公网映射候选收集成功
```

### 教训

**"配置项传进去了"不等于"配置生效了"。**
这里的属性是"静默忽略非法值"的语义——最难发现的一类问题。

排查思路：**去动态库的字符串里找属性的真实文档**：
```bash
strings libnice.so.10 | grep -i "<属性名>"
```
这比查在线文档更可靠，因为**版本的差异就在二进制里**。

另外：**跨平台网络代码要显式指定地址族**，不要依赖 getaddrinfo 的默认顺序。

---

## 坑 25：libsrtp2 的 API 与"想当然"不符（三处）

### 现象

一连串编译错误：

```
error: conflicting declaration 'typedef srtp_ctx_t* srtp_t'
error: 'srtp_policy_t' has no member named 'keysalt'; did you mean 'keys'?
error: request for member 'data' in 'rtc::out', which is of non-class type 'std::vector<unsigned char>(size_t)'
error: cannot convert 'srtp_t**' to 'srtp_t'
```

### 根因（三个独立问题）

**(a) `srtp_t` 本身就是指针类型**

```c
typedef struct srtp_ctx_t_ srtp_ctx_t;
typedef srtp_ctx_t *srtp_t;        // ← srtp_t 已经是指针
```

所以成员应声明为 `srtp_t srtp_in_;`，**不是** `srtp_t* srtp_in_;`。
而 `srtp_create(&session, &policy)` 的第一个参数是 `srtp_t*`（二级指针）——
因为它要"输出"一个 `srtp_t`。

**(b) 没有 `keysalt` 字段；`key` 必须"key+salt 连续"**

```c
typedef struct srtp_policy_t {
    unsigned char *key;        // ← 只要一个指针
    srtp_master_key_t **keys;
    unsigned long num_master_keys;
    ...
} srtp_policy_t;
```

libsrtp 按套件定义的长度（AES-128：key 16B + salt 14B）**从 `key` 指针连续读取**。
所以必须自己把 key 和 salt 拼成一段连续内存：

```cpp
std::vector<uint8_t> material;
material.insert(material.end(), key, key + 16);
material.insert(material.end(), salt, salt + 14);
policy.key = material.data();
```

**(c) Most vexing parse（最令人烦恼的解析）**

```cpp
std::vector<uint8_t> out(size_t(pending));   // ← 被解析成**函数声明**！
int n = BIO_read(wbio_, out.data(), pending);
// error: request for member 'data' in 'rtc::out',
//        which is of non-class type 'std::vector<unsigned char>(size_t)'
```

`std::vector<uint8_t> out(size_t(pending));` 中 `size_t(pending)` 看起来像参数声明，
于是整行被当作"声明一个名为 out 的函数，接受一个 size_t 参数"。
**报错信息完全看不出这是个解析歧义。**

修复：显式转换，或改用花括号/等号初始化：

```cpp
std::vector<uint8_t> out(static_cast<size_t>(pending));   // ✓
```

### 教训

1. **对着系统头文件写代码**，不要凭记忆或最新文档：
   ```bash
   grep -n -A20 'typedef struct srtp_policy_t' /usr/include/srtp2/srtp.h
   ```
2. **看到 "request for member X in Y, which is of non-class type" 时，
   第一反应应该是 most vexing parse**，而不是怀疑类型写错。
3. 涉及"多段密钥材料"的 API，先确认它是"分开传"还是"连续传"。

---

## 坑 26：DTLS 双向证书交换 —— server 侧读不到对端证书

### 现象

DTLS 握手两侧都返回成功，但：

```
A.peer_fingerprint() = E6:84:0C:D1:...    ← client 能读到对端证书
B.peer_fingerprint() = (空)               ← server 读不到
```

且 `SSL_get_peer_certificate()` 在 server 侧返回 NULL。

### 定位过程

**(1) 先怀疑"握手完成时机不对称"** —— 尝试在 server 侧多跑几轮
"收包 + 推进状态机"，无效。说明不是时序问题。

**(2) 查 OpenSSL 的默认行为** —— 关键认知：

> **OpenSSL 的 DTLS server 默认不向 client 索要证书**
> （不发送 CertificateRequest）。

所以：
- client **总是**会收到 server 的证书（server 必然出示）→ A 能读到 ✓
- server **不会**请求 client 的证书 → client 不出示 → B 读不到 ✗

**这不是库的缺陷，而是默认策略。** 要双向互验证书必须显式开启。

**(3) 尝试一：`SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT`** —— 失败。

握手直接不通过。因为 WebRTC 场景用的是**自签名证书**，
标准校验必然报 "unable to get local issuer certificate"，
`FAIL_IF_NO_PEER_CERT` 就把整个握手拒掉了。

**(4) 尝试二（正确）：`SSL_VERIFY_PEER` + 始终放行的 verify_callback**

```cpp
SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER,
    [](int /*preverify_ok*/, X509_STORE_CTX* /*ctx*/) -> int {
        return 1;   // 跳过 CA 链校验；身份由业务层"指纹比对"保证
    });
```

注意 `SSL_VERIFY_PEER` 的作用是**请求对端证书**，
而 verify_callback 返回 1 保证自签名证书不被拒。
**真正的身份验证由我们在握手后比对的指纹完成**——这正是 WebRTC 的做法
（WebRTC 也不允许 CA 证书，只用 `a=fingerprint` 校验）。

### 验证

```
修复前: A 读到指纹 ✓ / B 读到空 ✗
修复后: A 读到指纹 ✓ / B 读到指纹 ✓   （双向指纹互验均通过）
```

### 教训

**"握手成功"不等于"双向身份都已验证"。**
DTLS/TLS 的默认策略是"服务端出示证书、客户端可选"——
mTLS（双向）需要显式请求。

排查这类问题的方法：**先确认协议默认行为，再怀疑实现**。
本例中"client 正常、server 为空"这个不对称现象本身就是强线索：
它精确指向"谁向谁索要证书"的默认策略差异。

另外：**用自签名证书时，`SSL_VERIFY_FAIL_IF_NO_PEER_CERT` 会直接拒掉握手**——
需要自定义 verify_callback 放行，把身份保证交给上层（指纹比对）。

---

## 坑 27：测试宏二次求值 —— "全 PASS 却报失败"（★ 隐蔽性极高）

### 现象

极其自相矛盾的输出：

```
21 项全部显示 [PASS]
FAIL 行数：0
但汇总行：################ 结果：存在失败（失败 2 项）################
```

**打印全是 PASS，计数器却非零。**

### 根因

测试宏对条件**求值了两次**：

```cpp
#define CHECK(c, m) do {                              \
    printf("  [%s] %s\n", (c) ? "PASS" : "FAIL", m);  \
    /*                    ↑ 第一次求值 */               \
    if (!(c)) g_fail++;                               \
    /*     ↑ 第二次求值 */                             \
} while (0)
```

对**无副作用**的表达式（如 `a.peer_fingerprint() == b_fp`）没问题；
但对**带副作用**的表达式就致命：

```cpp
CHECK(a.protect(rtp.data(), &len), "A 加密成功");
//     ↑ 这个调用会被执行两次
```

执行过程：
1. `printf` 里的第一次调用 `a.protect(...)` → 明文成功加密 → 打印 **PASS**
2. `if` 里的第二次调用 `a.protect(...)` → 这个包**已经加密过了**，
   再加密一次会失败（返回 false）→ `g_fail++`

本项目中共有 2 处这样的表达式（`a.protect` 和 `b.unprotect`），
恰好对应报出的"失败 2 项"。

### 修复

**条件只求值一次**，存入局部变量：

```cpp
#define CHECK(c, m)                                     \
    do {                                                \
        const bool _ok = (c);    /* 只求值一次 */        \
        printf("  [%s] %s\n", _ok ? "PASS" : "FAIL", m); \
        if (!_ok) g_fail++;                             \
    } while (0)
```

### 验证

```
修复前: 21 PASS / 0 FAIL 行 / 汇总"失败 2 项"
修复后: 21 PASS / 0 FAIL 行 / 汇总"全部通过（失败 0 项）"   ✅
```

### 教训

**宏参数必须当作"可能被求值多次"来处理。**
这是 C/C++ 宏的经典陷阱，但在测试代码里尤为危险——
因为它会伪造出"测试失败"的假象，让你去排查**没有问题的被测代码**。

防御手段：
1. 宏体里**先求值到局部变量**，只用变量（本项目采用）
2. 或者把带副作用的调用**先执行、把结果传给宏**：
   ```cpp
   const bool ok = a.protect(...);
   CHECK(ok, "A 加密成功");
   ```
3. 用 `static inline` 模板函数替代宏（C++ 里最彻底的做法）

**这个坑最有价值的地方是它的表现**：
不是崩溃、不是编译错误，而是"输出自相矛盾"——
如果不仔细核对 PASS 数量与汇总计数，很容易误判成被测代码有问题。

---

## 坑 28：拥塞控制节流基准用真实时钟 → 仿真中码率永不更新

### 现象

拥塞控制测试中，延迟梯度已累积到 **320ms**，但码率**纹丝不动**：

```
=== 3. 拥塞控制（延迟梯度 + AIMD）===
    良好网络 40 轮后: 16000 bps (状态=NORMAL)
  [FAIL] 良好网络触发加性增

  -- 模拟链路排队 --
    排队后: 16000 -> 16000 bps (梯度=320.7ms, 状态=NORMAL)   ← 梯度这么大却没降
  [FAIL] 检测到延迟梯度上升后码率下降
```

同时 `overuse 0 / underuse 0` —— 说明**状态判定根本没执行过**。

### 定位过程

**(1) 先修"看起来最像"的问题**：`update_bitrate()` 内部用 `now_ms()`，
于是给接口加上可注入的 `now` 参数（同坑 9 的处理）。**仍失败。**

**(2) 关键线索**：梯度算出来了（320ms），说明 `on_arrival` 正常；
但 `overuse_events == 0`，说明 `update_bitrate` 里的状态判定**从未进入**。

**(3) 检查节流逻辑**：
```cpp
int CongestionController::update_bitrate(int64_t now) {
    if (now - last_update_ms_ < 100) return target_bps_;   // ← 永远命中这里
    ...
}
```

**(4) 根因**：`last_update_ms_` 在**构造函数里用真实时钟初始化**：

```cpp
CongestionController::CongestionController(const Config& cfg) {
    ...
    last_update_ms_ = now_ms();      // ← 真实时钟，例如 500000
}
```

而测试传的逻辑时间是 `0, 20, 40, ...`：
```
now - last_update_ms_ = 20 - 500000 = -499980   →  恒 < 100  →  永远节流
```

**所有更新都被节流掉了。**

### 修复

**节流基准改为惰性初始化**（首次调用时设置），而不是在构造函数里取真实时钟：

```cpp
// 构造函数
last_update_ms_ = 0;    // 不取 now_ms()

// update_bitrate
if (last_update_ms_ == 0) {
    last_update_ms_ = now;     // 首次调用时建立基准
    return target_bps_;
}
if (now - last_update_ms_ < 100) return target_bps_;
last_update_ms_ = now;
```

`reset()` 也保持一致（设为 0）。

### 验证

```
修复前: 16000 -> 16000 bps (梯度=320.7ms)，overuse 0 次
修复后: 良好网络 16000 -> 27417 bps（加性增生效）
        排队后   27417 -> 13136 bps（乘性降生效，状态=OVERUSE）
        高丢包   16000 ->  7099 bps（丢包兜底生效）
        恢复后    9826 -> 45774 bps（回升生效）
        增 8 次 / 降 5 次 / overuse 5 次
```

### 教训

**"部分依赖真实时钟"比"完全依赖"更难发现。**

坑 9 是组件内部**每次都**读真实时钟 —— 表现为测试结果全为 0，很容易发现。
本例是**只在初始化时读一次** —— 大部分逻辑用注入的时间正常工作
（梯度算对了！），只有节流基准是错的。

这类问题的特征是：**"看起来部分正常工作"**。
梯度有值、函数有返回，只有某个分支静默失效。

排查方法：**给关键分支加计数器/日志**。
本例中 `overuse_events == 0` 这个统计值就是决定性线索——
它精确指出"状态判定分支从未进入"，从而把排查范围从"算法错"缩小到"节流逻辑错"。

**通用原则（坑 9 + 坑 28 的共同结论）**：
> 任何需要仿真的组件，**时间必须完全由外部注入**，
> 包括初始化基准。构造函数里出现 `now_ms()` 就是隐患。

---

## 坑 29：端到端接线顺序错 → DTLS 握手必然超时

### 现象

端到端测试中 ICE 全部通过，但 DTLS 握手失败，后续媒体传输 0 包：

```
=== 2. DTLS-SRTP 握手（在 ICE 通道上）===
  [FAIL] 双方 DTLS 握手成功
  [FAIL] SRTP 会话就绪

=== 5. 端到端结果 ===
  发送包数(源)      : 0
  接收并解密包数    : 0
```

### 根因（两个接线错误叠加）

**(a) ICE 接收回调注册在握手之后**

```cpp
// 错误顺序
std::thread th_a([&]{ sa.handshake(&ta, c); });   // 握手先开始
...
ice_b.set_on_recv(on_ice_recv);                    // 回调才注册
```

握手期间到达的 DTLS 记录**无人接收**（回调还没注册），双方都等不到对方报文。

**(b) 只接了 B 侧，A 侧没接**

```cpp
ice_b.set_on_recv(on_ice_recv);              // ✓
ice_a.set_on_recv([](const uint8_t*, size_t){});   // ✗ 空回调，直接丢弃
```

DTLS 是**双向**握手：client 同样需要接收 server 的证书与 Finished。
A 侧回调为空 → A 收不到任何握手报文。

**(c) 传输层 recv 是忙等**

```cpp
int recv(uint8_t* buf, size_t cap, int timeout_ms) override {
    std::lock_guard<std::mutex> lk(mutex_);
    if (queue_.empty()) return 0;    // ← 立即返回，没有等待 timeout_ms
}
```

DTLS 握手循环会反复调 `recv` 等待对端报文。
若 `recv` 立即返回 0，外层就变成**忙等**——在极短的真实时间内把
`handshake_timeout_ms` 的预算耗光，握手直接超时。

### 修复

**(a) 顺序**：先注册 ICE 接收回调，**再**启动握手：

```cpp
ice_b.set_on_recv(on_ice_recv);
ice_a.set_on_recv([&ta](const uint8_t* d, size_t n){ ta.feed(d, n); });   // A 侧也要接
std::thread th_a(...);   // 之后才 start handshake
```

**(b) recv 真正等待**：

```cpp
const int64_t deadline = now_ms() + timeout_ms;
while (now_ms() <= deadline) {
    { /* 有数据就返回 */ }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
return 0;   // 真超时
```

### 教训

**"接线"（wiring）错误和"逻辑"错误一样常见，而且症状更具误导性。**

本例中 ICE 全部通过、编码器正常初始化，只有 DTLS 失败——
很容易让人怀疑 DTLS 本身，实际是回调注册顺序问题。

**通用检查清单（集成多组件时）**：
1. 所有事件回调是否**在触发它们的事件之前**注册？
2. **双向**协议是否两端都接上了回调？（DTLS/TLS/ICE 都需要）
3. 阻塞式接口是否真的"阻塞"？立即返回会造成忙等。

---

## 坑 30：自定义包头不兼容 RTP 布局 → SRTP 无法处理

### 现象

DTLS 握手成功、SRTP 会话就绪后，媒体包**到达了却走错分支**：

```
[DEBUG][e2e] B: ICE recv 153 bytes, first=0x00     ← 首字节 0x00
[DEBUG][e2e] B: ICE recv 153 bytes, first=0x00
...
  发送包数(源)      : 50
  接收并解密包数    : 0        ← 一个都没解析出来
  SRTP 加密/解密    : 62 / 0
```

包确实到达（153 字节），但接收端把它当成了 **DTLS 记录**而不是 SRTP 媒体包。

### 根因

我最初设计的自定义包头把 `session_id` 放在**最前面**（第 0~1 字节）：

```
[0..1]   session_id      ← 值通常是 0 或 1
[2..3]   seq
...
```

而 **SRTP（libsrtp2）按标准 RTP 头解析包**，第一个字节的低 2 位必须是版本号 `2`：

```cpp
// 接收端的分流判断
const bool looks_rtp = (len > 12) && ((data[0] & 0xC0) == 0x80);
```

`session_id = 0` 时首字节是 `0x00` → `0x00 & 0xC0 = 0x00 ≠ 0x80` → 判为非 RTP →
被送进 DTLS 分支 → SRTP 解密从未被调用。

### 修复：包格式与 RTP 头对齐

重新设计为 **前 12 字节完全兼容 RTP 固定头**：

```
--- RTP 兼容段（12 字节）---
[0]      V/P/X/CC = 0x80      ← 版本位 = 2（关键！）
[1]      M/PT（本项目用 96 动态类型）
[2..3]   seq
[4..7]   timestamp
[8..11]  ssrc（承载 session_id）
--- 自定义扩展段（8 字节）---
[12]     phase
[13]     flags
[14..15] trans_id
[16..17] payload_len
[18..19] ext_crc
```

总头长 20 字节。这样 SRTP 能正确解析 RTP 段，
自定义字段（phase / trans_id）作为 RTP 头之后的附加数据保留。

### 修复过程中发现的连带问题

改完之后媒体通了，但出现**两个新问题**，都是同一个根因的衍生：

**(a) 丢包统计虚高到 10032**

FEC 包最初用"源序号 + 10000"区分：

```cpp
fh.seq = uint16_t(10000 + h.seq);   // ✗
```

SRTP 按 SSRC 跟踪序号。同一 SSRC 的序号先跳到 10000 再回落，
被 SRTP 判为重放而拒绝；接收端丢包统计把这 10000 的跳变算成"丢了 1 万个包"。

**修复**：FEC 包用**独立的 session_id（独立 SSRC）**：

```cpp
fh.session_id = 2;    // ✓ 与媒体流（session_id=1）分开的序号空间
```

**(b) "解码出静音"的误判**

检查音频幅度时只看**最后一帧**：

```cpp
for (auto s : last_decoded) max_amp = max(max_amp, abs(s));   // ✗ 只检查了 1 帧
```

而最后一帧可能正好是 PLC 产生的静音帧（舒适噪声，幅度接近 0）。

**修复**：统计**所有**解码帧的最大幅度：

```cpp
for (int i = 0; i < n; i++) { int a = abs(out[i]); if (a > max_decoded_amp) max_decoded_amp = a; }
// 验证结果：max_amp = 8218 ✓
```

### 教训

**引入 SRTP 就相当于承诺了"包的头部必须是合法 RTP"。**

这是"自建协议"与"使用标准协议栈"之间的**接口约束**——
自定义头可以随便设计，但一旦要过 SRTP，前 12 字节就必须符合 RTP 规范。

**三条经验**：
1. **多流分隔必须用独立 SSRC**，不要用"序号偏移"这种 hack——
   会破坏协议栈的序号连续性假设。
2. **判断包类型要看协议定义的特征位**（如 RTP 版本位），
   不要依赖自定义字段的取值。
3. **验证音频内容要看统计量（全局最大幅度），不要只看单帧**——
   单帧可能是 PLC 的静音帧。

---

## 坑 31：Trickle ICE 只交换候选 → 连接建不起来

### 现象

实现 Trickle ICE 后，候选交换**完全成功**，但连接建立失败：

```
=== 4. Trickle ICE（候选增量发送）===
    A 通过 trickle 送出候选: 36 个
    B 通过 trickle 送出候选: 36 个
  [PASS] A 侧产生了 trickle 候选回调
  [PASS] B 侧产生了 trickle 候选回调
    A 逐个添加 B 的候选: 36/36 成功
  [PASS] A 通过 add_remote_candidate_sdp 增量添加远端候选成功
    B 逐个添加 A 的候选: 36/36 成功
  [PASS] B 通过 add_remote_candidate_sdp 增量添加远端候选成功
  [FAIL] Trickle 模式下 A 连接建立      ← 候选都对，就是连不上
  [FAIL] Trickle 模式下 B 连接建立
```

"候选全部加成功"与"连接建立失败"并存 —— 这个组合本身就是强线索。

### 根因

**ICE 连通性检查的 STUN 报文需要用户名和密码**（RFC 8445）：

```
USERNAME = 远端ufrag:本地ufrag
MESSAGE-INTEGRITY = HMAC(密码)
```

而**单个候选片段（`a=candidate:...`）里不含 ufrag 与 pwd** ——
它们原本在 SDP 的 `a=ice-ufrag` / `a=ice-pwd` 行里。

Trickle 模式下我只传了候选，没传凭证 → STUN 报文缺少正确的用户名/密码
→ 对端校验失败 → 连通性检查永远完不成。

### 修复

Trickle ICE 必须**同时**交换凭证：

```cpp
// 本地凭证（对应 SDP 的 a=ice-ufrag / a=ice-pwd）
IceTransport::Credentials local_credentials() const {
    gchar* ufrag = nullptr; gchar* pwd = nullptr;
    nice_agent_get_local_credentials(agent_, stream_id, &ufrag, &pwd);
    ...
}

// 设置远端凭证（Trickle 建链的必要步骤）
void set_remote_credentials(const std::string& ufrag, const std::string& pwd);
```

测试中的调用顺序：
```cpp
const auto ac = ta.local_credentials();
const auto bc = tb.local_credentials();
tb.set_remote_credentials(ac.ufrag, ac.pwd);
ta.set_remote_credentials(bc.ufrag, bc.pwd);
// 之后再逐个 add_remote_candidate_sdp
```

### 验证

```
修复前: 候选 36/36 加成功，但 [FAIL] 连接建立
修复后: 凭证交换 + 候选 36/36 → [PASS] 连接建立
        Trickle 建链后收发: A 收 20 / B 收 20
```

### 教训

**Trickle ICE 不是"把候选换成分开发"这么简单。**
它把原本打包在 SDP 里的三类信息拆开传输：

| 信息 | 作用 | 是否可 trickle |
|---|---|---|
| 候选（a=candidate） | 地址+优先级 | ✅ 可以边收集边发 |
| **凭证（a=ice-ufrag / a=ice-pwd）** | **STUN 认证** | ❌ **必须先交换一次** |
| 角色（controlling/controlled） | 谁提名 | ❌ 随协商确定 |

**容易犯的错误**：只 trickle 了"看起来最像数据"的候选，
漏掉了"看起来像配置"的凭证。

**排查提示**：当出现"所有前置步骤都 PASS，但最终的连接/握手建不起来"时，
要检查**是否遗漏了某个必要的初始化信息**——
本例的凭证、坑 26 的证书请求、坑 29 的回调注册顺序，都属于这一类。

---

## 坑 32：恢复率分母口径错 + 指标时长耦合真实时钟

### 现象（两个 bug 一起暴露）

指标测试中，"8 个丢包全部恢复"却算出 50% 恢复率；同时通话时长荒谬：

```
  恢复率: 50.00%（丢包 8，恢复 5+3=8）
  [FAIL] 恢复率 = 8/8 = 100%

======== 通话指标（3699778 ms）========     ← 3.7 百万毫秒？！
    发送字节        : 22940  (实测码率 49 bps)   ← 码率被同样压低
```

### 根因

**(a) 恢复率的分母重复计算**

```cpp
double CallMetrics::recovery_rate() const {
    const uint64_t recovered = packets_recovered_nack + packets_recovered_fec;
    const uint64_t total_lost = packets_lost + recovered;   // ✗ 加了两次
    return double(recovered) / double(total_lost);
}
// 8 / (8 + 8) = 0.5  ← 错误
```

`packets_lost` 的定义是"检测到的丢包总数"，而 `recovered` 是其中已被恢复的部分——
**recovered 是 lost 的子集，不是补充**。把它加到分母上等于把丢包数翻倍。

**修复**：分母只用 `packets_lost`：

```cpp
if (packets_lost == 0) return 1.0;
return double(recovered) / double(packets_lost);   // 8/8 = 1.0 ✓
```

同时补充 `still_missing()` 明确"最终仍缺失"的语义：

```cpp
uint64_t still_missing() const {
    return packets_lost > recovered ? packets_lost - recovered : 0;
}
```

**这正是坑 11 的同类问题**——"统计口径"错误。区别只是那次是丢包率，这次是恢复率。

**(b) 指标时长用真实时钟**

```cpp
void MetricsCollector::mark_start(int64_t now = -1) {
    start_ms_ = (now < 0) ? now_ms() : now;    // 测试传了 0（逻辑时间）
}
CallMetrics MetricsCollector::snapshot() const {
    s.duration_ms = now_ms() - start_ms_;      // ✗ 这里却用真实时钟
}
```

测试用逻辑时间起点 `0`，而 `snapshot()` 用真实时钟（例如 3699778）——
两者相减得到 3699778ms 的"时长"，进而把码率压到 49 bps。

**修复**：把 `now` 也做成可注入参数（与 `update_bitrate(now)` 一致）：

```cpp
CallMetrics snapshot(int64_t now = -1) const;
```

测试改为 `mc.snapshot(kLogicalNow)`，duration 即为 5000ms。

### 验证

```
修复前: 恢复率 50.00%，时长 3699778ms，码率 49 bps
修复后: 恢复率 100.00%（仍缺失 0），时长 5000ms，码率 36704 bps
        （22940 字节 × 8 / 5 秒 = 36704 bps ✓）
```

### 教训

**1. 定义统计量的"分母"时，要问清楚它代表什么集合。**
```
恢复率 = 恢复数 / 总丢包数      ✓  recovered 是 lost 的子集
       ≠ 恢复数 / (丢包数+恢复数)  ✗  等于把分母翻倍
```
这与坑 11（丢包率口径）、坑 1（乱序 vs 迟到）同源：
**都是"两个语义共用一个变量/口径"导致的统计错误**。

**2. 指标模块同样必须完全接受注入时间。**
坑 28 是拥塞控制，这里是指标统计——**同一个陷阱在项目里出现了两次**。
教训：**凡是涉及"时长/速率"的计算，时间基准都必须是参数，不能内部取 `now_ms()`**。

**3. 派生指标要有"零值语义"。**
```
无丢包  → 恢复率 100%（而不是 0 或 NaN）
无播放  → 连续性 100%（而不是 NaN）
无编码  → 成功率 100%（而不是 NaN）
```
监控指标最常见的坑就是"无数据"被算成"0%"而触发误报警。

---

## 坑 33：载荷字节与网络字节混用同一变量 → 码率凭空翻倍

### 现象

接入拥塞控制闭环后，指标出现自相矛盾：

```
  [拥塞控制] 编码器当前码率: 48000 bps
    目标码率        : 48000 bps
    发送字节        : 43737  (实测码率 82933 bps)   ← 比目标高 73%！
```

编码器明明被限制在 48000 bps，"实测码率"却是 82933 bps。

### 根因

`bytes_sent` 一个变量承载了**两种语义**的字节数：

```cpp
void MetricsCollector::on_frame_encoded(bool ok, size_t bytes) {
    if (ok) { c_.frames_encoded++; c_.bytes_sent += bytes; }   // ① 编码载荷
    ...
}
void MetricsCollector::on_packet_sent(size_t bytes, bool is_fec) {
    ...
    c_.bytes_sent += bytes;                                    // ② 网络整包
}
```

而且 e2e 里传入的 `bytes` 还不是同一层的数据：
- ① 传的是**编码器输出**（Opus 载荷）
- ② 传的是**序列化后的整包**（含 20 字节自定义头）

两者混加后，`bytes_sent` 既不是纯载荷、也不是纯网络字节，
算出的"码率"没有任何物理意义。

### 修复：彻底拆成两个计数器

```cpp
uint64_t payload_bytes = 0;   // 编码器输出的载荷（衡量编码效率）
uint64_t wire_bytes = 0;      // 实际发到网络的整包（含头与 FEC 冗余）
```

并给出两个不同的派生指标：

```cpp
int actual_bitrate_bps() const;   // 按 payload_bytes 计 → 编码码率
int wire_bitrate_bps() const;     // 按 wire_bytes 计 → 含开销的实际占用
double overhead_ratio() const;    // wire / payload → 网络开销比
```

### 验证

```
metrics_test:
  载荷码率: 15872 bps（9920 载荷字节 / 5000 ms）      ← 与目标 16000 吻合
  网络码率: 20832 bps（13020 网络字节，开销比 1.31）  ← 含头与 FEC 开销
  [PASS] 载荷码率按编码字节/时长正确计算
  [PASS] 网络码率 > 载荷码率
  [PASS] 开销比 > 1

e2e_test:
  载荷/网络字节: 19590 / 24147
  编码码率/实际码率: 37772 / 46559 bps（开销比 1.23）
  目标码率: 48000 bps
```

**关于 "载荷 37772 < 目标 48000"**：这不是 bug，而是 AIMD 的正常行为——
码率从 16000 起步，每次加性增 8%，需要约 1.4 秒才爬到 48000；
测试时长 4 秒，平均值自然低于终值。

单独验证 Opus 的运行时码率遵循性（`scripts/verify_opus_bitrate.sh`）：
```
target=16000 bps -> 实测 16000 bps
target=32000 bps -> 实测 32000 bps
target=48000 bps -> 实测 48000 bps      ← 精确遵循，无偏差
```

### 教训

**"字节数"必须标明是哪一层的字节。** RTC 链路里至少有四层字节：
```
PCM 样本  →  Opus 载荷  →  RTP/SRTP 整包  →  UDP/IP 帧
```
把任意两层混加，得到的"码率"就没有意义。

这与坑 1（乱序/迟到）、坑 11（丢包口径）、坑 32（恢复率分母）
是**同一类错误**：**一个变量/口径承载两种语义**。
在本项目中这类问题已出现 **4 次**，是最需要主动防御的设计盲区。

**防御手段**：变量命名必须体现层次（`payload_bytes` / `wire_bytes`），
不要用 `bytes_sent`、`size`、`count` 这类无层次含义的名字。

---

## 坑 34：FEC 包污染 NACK 统计 → 丢包数少报

### 现象

写运行手册时，把端到端的丢包矩阵做成表格，发现数字对不上：

| 丢包率 | 实际丢弃 | 检测丢包 | 吻合 |
|---|---|---|---|
| 0 | 0 | 0 | ✓ |
| 0.10 | 3 | 3 | ✓ |
| 0.25 | **14** | **13** | ✗ |

25% 丢包时**少报了**。而更早的一次运行更明显：

```
  丢包率 0.25 : 模拟丢弃 : 9  检测丢包 : 6      ← 少了 3 个
```

### 根因

`e2e_test.cpp` 里 `nack.on_packet()` 被放在**分支判断之前**，
于是**所有**解密成功的包都进了 NACK 统计——包括 FEC 冗余包：

```cpp
if (Packet::parse(...)) {
    recv_packets++;
    nack.on_packet(ph.seq, -1);          // ✗ 所有包都进来，含 FEC 包
    if (ph.flags & kFlagFecPacket) {
        fec_dec.on_fec(...);
        // 注释里明明写着"FEC 包用独立 SSRC，不能喂给 NACK 统计"
    } else if (ph.session_id == 1) {
        ...
    }
}
```

而 FEC 包用的是**独立 SSRC（session_id=2）**、独立的序号空间。
两个序号空间混在一起后，NACK 看到的序号序列变成这样：

```
媒体流: 0 1 2 3 4 5 ...
FEC流:  0 0 0 0 ...        ← 与媒体流序号重叠
合并后: 0 0 1 0 2 0 3 ...  ← 序号来回跳，空洞判断全乱
```

序号跳来跳去 → "跨度 − 到达数"的计算被扰乱 → 丢包数不准。

### 特别值得注意的一点

**这段代码的注释本身是对的**，它明确警告了不能把 FEC 包喂给 NACK。
但**代码没照着注释写**——`nack.on_packet()` 在注释覆盖的分支之外。

这是"注释与代码不一致"的典型：注释描述的是**应该怎么做**，
代码做的是**另一回事**，而且注释的存在反而让人以为已经处理过了。

### 修复

把 `nack.on_packet()` 移进媒体流分支：

```cpp
if (ph.flags & kFlagFecPacket) {
    // FEC 包：独立 SSRC，只喂 FEC 解码器
    fec_dec.on_fec(...);
} else if (ph.session_id == 1) {
    // 媒体流：这才喂给 NACK 统计
    nack.on_packet(ph.seq, -1);
    fec_dec.on_source(...);
    jb.push(...);
}
```

### 验证

```
修复前: 实际丢弃 9  → 检测丢包 6      （少报 3）
修复后: 实际丢弃 14 → 检测丢包 13     （偏差 1）
        长时运行 6 轮共 138 丢包，偏差 0
```

### 残留的 1 个偏差是算法固有边界（不是 bug）

抖动缓冲的丢包检测原理是：

```
已接收序号跨度(span) − 独立到达包数(arrived) = 丢包数
```

它**依赖"更晚的包"到来**才能发现中间的序号空洞。
如果**最后几个包**丢了，没有更晚的序号来揭示它们——
这些"末尾丢包"在通话结束前无法被统计。

实测这个偏差的特征：
```
时长 2s : 吻合 5/5 次, 累计偏差 0
时长 4s : 吻合 1/5 次, 累计偏差 4
时长 6s : 吻合 5/5 次, 累计偏差 0
6 轮 × 6s（25% 丢包，共 138 丢包）: 偏差 0
```

偏差量级很小（个位数），且长时间运行趋于 0。
**这与坑 11 的"虚报 6 倍"完全不同**——那是统计口径错误，这是算法边界。

识别方法：**看偏差方向与量级**。
- 修复统计口径错误后：偏差变为 0 或极小正数（末尾丢包）
- 若仍在"少报/多报"大量包，说明口径仍有问题

### 教训

**1. 注释不能代替代码。** 注释写了约束，就要确保代码真的在约束内。
更可靠的做法是**让约束在结构上成立**——把 `nack.on_packet()` 放进
`session_id == 1` 分支里，它就不可能被 FEC 包触发。

**2. 同一协议的"多个序号空间"是统计陷阱。**
只要有两种流共用一套统计，就必须在**入口处**分流，而不是在出口处过滤。

**3. 写文档能发现代码 bug。**
这个 bug 是"把数据整理成表格"时暴露的——
因为表格要求"检测丢包"与"实际丢弃"**逐行对齐**，
一旦对齐就立刻看出 14 ≠ 13。
**"把结果整理成人类可读的形式"本身就是一种验证手段。**

---

## 坑 35：用 root 编译过 → 普通用户跑不动（`Permission denied`）

### 现象

用户在自己的 WSL 里跑一键脚本，第一行就失败：

```bash
ts@DESKTOP-R196JSM:~/rtc-1v1-audio$ ./scripts/run.sh build

=== 编译 ===
./scripts/run.sh: line 54: build.log: Permission denied
  ✗ 编译失败，见 build/build.log
```

报错说"见 build/build.log"，但**那个日志本身就是写不进去的那个文件**——
错误信息被重定向吞掉了，用户看到的是一个自相矛盾的提示。

### 根因

**开发过程中我是以 `root` 身份在 `/home/ts/rtc-1v1-audio` 里编译的**
（工具默认以 root 运行），于是整个目录树都变成了 root 所有：

```
drwxr-xr-x 8 root root  /home/ts/rtc-1v1-audio
drwxr-xr-x 3 root root  /home/ts/rtc-1v1-audio/build
非 ts 拥有的条目: 906        ← 906 个文件全是 root 的
```

用户以 `ts` 身份运行时：
- `mkdir -p build` **静默成功**（目录已存在，不需要创建）
- 但 `> build/build.log` 需要写权限 → 被拒绝

**`mkdir -p` 的"成功"是个陷阱**：它只保证"目录存在"，
不保证"我能往里写"。脚本没有检查后者。

### 修复

**(a) 立刻恢复目录归属**

```bash
sudo chown -R $(id -un):$(id -gn) ~/rtc-1v1-audio
sudo rm -rf build        # 删掉 root 留下的构建产物
```

**(b) 脚本增加权限自检**（`scripts/run.sh`）

在动手编译**之前**就检查，并给出可直接复制的修复命令：

```bash
check_writable "$ROOT"  "仓库目录" || return 1
check_writable "$BUILD" "build 目录" || return 1
# 目录可写但旧日志文件可能属于别人 → 还要试能否截断
if [ -e "$BUILD/build.log" ] && ! ( : > "$BUILD/build.log" ) 2>/dev/null; then
  err "build/build.log 不可写（可能属于其他用户）"
  echo "      修复：sudo rm -rf build"
  return 1
fi
```

修复后的提示：

```
=== 编译 ===
  ✗ build 目录 不可写：/home/ts/rtc-1v1-audio/build
      所有者: root:root    当前用户: ts

      修复方法（任选其一）
        sudo rm -rf build
        sudo chown -R ts:ts .

      原因：通常是因为之前用 sudo 或 root 身份编译过，
            留下的文件属于 root，普通用户无法覆盖。
```

### 教训

**1. 永远不要用 root/sudo 编译用户项目。**
一次 `sudo cmake` 就能让后续所有普通用户操作失败，
而且症状延迟出现、难以归因。
需要用 sudo 的只有"安装依赖"这一步，**编译和运行绝不需要 sudo**。

**2. `mkdir -p` 成功 ≠ 目录可写。** 写文件前必须检查写权限。

**3. 把日志写进"可能不可写的目录"是设计问题。**
本项目改为：日志仍写 `build/`，但**写之前先检测可写性**；
不可写就直接给出"为什么 + 怎么修"，而不是让 shell 抛一句 `Permission denied`。

**4. 错误信息不能指向一个自己都写不出来的文件。**
"见 build/build.log" —— 而 build.log 正是写失败的那个，这是自相矛盾的提示。
正确做法：把失败原因直接打印到终端，日志只是附加参考。

---

## 坑 36：新版 cmake 只装在 root 的 home 里 → 普通用户用不到

### 现象

修好权限后继续编译，又失败：

```
  ✗ cmake 配置失败，日志：build/cmake.log
      CMake Error at CMakeLists.txt:1 (cmake_minimum_required):
        CMake 3.21 or higher is required.  You are running version 3.16.3
```

而我开发时一直用得好好的，是 cmake 4.4.4。

### 根因

新版 cmake 是用 `pip install --user` 装的，而**我是 root**：

```
/root/.local/bin/cmake      → cmake 4.4.4   （只有 root 能用）
/usr/bin/cmake              → cmake 3.16.3  （系统自带，所有用户）
/home/ts/.local/bin/        → 空！          （ts 没有新版）
```

`pip install --user` 把包装进**当前用户**的 home。
以 root 装 = 只有 root 能用。用户 `ts` 的 `~/.local/bin` 是空的，
于是落到系统 3.16.3，而本项目要求 ≥ 3.21。

**这是"用 root 做开发"的第二个连带后果**（第一个是坑 35 的权限）。

### 修复

**(a) 给真实用户装一份**

```bash
python3 -m pip install --user "cmake>=3.28"
export PATH="$HOME/.local/bin:$PATH"
```

验证：
```bash
$ which cmake          # → /home/ts/.local/bin/cmake
$ cmake --version      # → cmake version 4.4.4
```

**(b) 试过但不可行的方案（记录避免再走弯路）**

| 方案 | 结果 |
|---|---|
| 软链 `/usr/local/bin/cmake -> /root/.local/bin/cmake` | ❌ `/root` 是 700，`ts` 读不到，报 `Permission denied` |
| `pip install --prefix=/usr/local cmake` | ⚠️ 只生成可执行入口，包数据不完整，仍不可用 |
| 给 ts 装 `--user`（**采用**） | ✅ 干净、独立、升级不影响别人 |

**(c) 脚本增加版本自检**（`scripts/run.sh`）

```bash
check_cmake_version() {
  local min_major=3 min_minor=21
  ver=$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
  # 比较主/次版本号…
  # 不满足则打印修复命令并返回 1
}
```

修复后的提示：

```
=== 编译 ===
  ✗ cmake 版本过低：3.16.3（需要 >= 3.21）
      cmake 路径: /usr/bin/cmake

      修复方法
        python3 -m pip install --user "cmake>=3.28"
        export PATH="$HOME/.local/bin:$PATH"

      说明：Ubuntu 20.04 自带的 cmake 是 3.16，不满足本项目要求。
            装好后 ~/.local/bin/cmake 才是新版，必须让它排在 PATH 前面。
```

**(d) 同时修掉"配置失败留下残骸"的问题**

cmake 配置失败会留下半成品 `CMakeCache.txt`，
下次构建会报一个更莫名其妙的错：

```
Error: could not find CMAKE_PROJECT_NAME in Cache
```

所以配置失败后要主动清理：

```bash
if ! cmake .. > cmake.log 2>&1; then
  tail -20 cmake.log
  rm -f CMakeCache.txt          # 清掉残骸，避免下次误报
  echo "已清理失败的 CMakeCache.txt"
  return 1
fi
```

### 教训

**1. `pip install --user` 的作用域是"当前用户"，不是"系统"。**
用 root 装 = 只有 root 能用。给用户装工具要用用户的身份装。

**2. 依赖安装和项目编译应该用同一个用户身份。**
否则会出现"我这儿明明是好的，你怎么就不行"这类问题。

**3. 工具链版本要在脚本里显式检查，不能假设 PATH 里的是对的。**
`which -a cmake` + `cmake --version` 两行就能避免大量误报。

**4. 探测路径时要注意权限边界。**
软链到 `/root/...` 看起来优雅，实际因目录权限 700 而失效。
跨用户共享工具应装在 `/usr/local`（并确保完整包数据）或各自 `--user` 安装。

---

## 坑 37：Trickle 候选重复发送 6 倍

### 现象

接通话日志，发现候选日志异常多：

```
[ice] trickle candidate #1: a=candidate:1 1 UDP ... 45518 typ host
[ice] trickle candidate #2: a=candidate:2 1 TCP ... 9 typ host tcptype active
...
[ice] trickle candidate #36: a=candidate:6 1 TCP ... 45273 typ host tcptype passive
```

本机只有 6 个候选，却发了 **36 次**。

### 根因

libnice 的 `new-candidate` 信号只告知"**有新候选**"，**不告诉你是哪一个**。
而回调里遍历了 `nice_agent_get_local_candidates()` 的**全部**候选并全部发出：

```cpp
// 错误实现
void cb_new_candidate(...) {
    GSList* cands = nice_agent_get_local_candidates(agent, stream_id, comp);
    for (each c in cands) {
        gchar* sdp = nice_agent_generate_local_candidate_sdp(agent, c);
        on_trickle_(sdp);        // 每次都把"已有的全部"重发一遍
    }
}
```

候选逐个产生：第 1 次回调发 1 个，第 2 次发 2 个，……第 6 次发 6 个
→ 1+2+3+4+5+6 = 21，信号还重复触发则更多（实测 36）。

### 修复

记录已发过的候选 SDP，只发新的：

```cpp
std::set<std::string> sent_candidates_;

for (each candidate c) {
    const std::string frag = sdp_of(c);
    if (sent_candidates_.count(frag)) continue;   // 跳过已发过的
    sent_candidates_.insert(frag);
    on_trickle_(frag);
}
```

### 验证

```
修复前: trickle candidate #1..#36（6 个候选发了 36 次）
修复后: trickle candidate #1..#6 （正好 6 个）
```

### 教训

**"事件通知"往往只说"有变化了"，不告诉你"变化是什么"。**
收到通知后要自己判断"哪些是新的"——
不能把"当前全量状态"当成"本次增量"发出去。

---

## 坑 38：Trickle 回调注册晚于候选产生 → 候选丢失，ICE 连不通

### 现象

修好坑 37（去重）后候选不再重复发，但**对端一个都没收到**：

```
# 本端日志：候选明明产生了
[ice] trickle candidate #1: a=candidate:1 1 UDP 192.168.0.197:46822 typ host
[ice] candidate gathering done (stream=1)

# 但通话卡在：
--- 阶段 4/6：ICE 连通性检查 ---
  已应用 0 个对端候选（后续候选随到随用）
  ✗ ICE 连通超时（候选对未选通）
```

### 根因

`IceTransport::init()` 内部**就已经开始收集候选**：

```cpp
bool IceTransport::init(const Config& cfg) {
    ...
    nice_agent_gather_candidates(agent_, cfg_.stream_id);   // 这里就开始收集
    return true;
}
```

而调用方在 `init()` **之后**才注册回调：

```cpp
ice.init(icfg);                              // 候选已开始产生
ice.set_on_trickle_candidate([&](sdp){      // 回调这时才设上
    sig.send(make_candidate(room, sdp, 0));
});
```

候选毫秒级就产生（gathering 异步且很快），此时 `on_trickle_` 还是空 →
`if (on_trickle_)` 为假 → 候选被**静默丢弃**。

### 修复

缓冲"回调注册之前"产生的候选，注册时补发：

```cpp
// 产生候选时
if (on_trickle_) {
    on_trickle_(frag);
} else {
    early_candidates_.push_back(frag);   // 先攒着，别丢
}

// 注册回调时
void set_on_trickle_candidate(TrickleCallback cb) {
    std::lock_guard<std::mutex> lk(trickle_mutex_);
    on_trickle_ = std::move(cb);
    for (const auto& c : early_candidates_) on_trickle_(c);   // 补发
    early_candidates_.clear();
}
```

### 验证

```
修复前: 已应用 0 个对端候选 → ICE 连通超时
修复后: 已应用 6 个对端候选 → ✓ ICE 通道已连通（选中候选对: 1 <-> 1）
```

### 教训

**"注册回调"和"启动事件源"的顺序，决定了会不会丢事件。**

通用模式：**先注册回调，再启动事件源**。
无法调整顺序时（如 `init()` 既建对象又启动收集），
就让事件源**缓冲**早期事件，等回调就位后补发。

同类问题在坑 29（DTLS 握手前必须接好 ICE 接收回调）也出现过一次 ——
**"事件驱动"的代码，接线顺序永远要反复确认**。

---

## 坑 39：抖动缓冲没有锁 → 真实通话下的数据竞争

### 现象

接上真实双进程通话（`--call`）后，`JitterBuffer` 的两个方法来自**不同线程**：

```
jb_.push()  ←  AudioPipeline::on_media_packet()
            ←  IceTransport 的 on_recv 回调
            ←  libnice 的 glib 主循环线程
            ←  ⚠ 网络线程

jb_.pop()   ←  AudioPipeline::playout_loop()
            ←  ⚠ 播放线程
```

而 `JitterBuffer` 内部**一把锁都没有**（`queue_` 是 `std::deque`，
还有一堆 `uint16_t`/`uint64_t` 水位变量）。

### 根因

**开发路径把问题掩盖了**：

1. 最初只跑 loopback（`--loopback`）：发送回调在发送线程里直接
   `on_media_packet()` → 相当于 push 和 pop 都发生在一个进程的相近节奏里，
   竞争窗口极小，测试全过。
2. `e2e_test` 是单进程两实例，负载低、时序温和，也没暴露。
3. **接上真实 ICE 通道后**，push 变成从 libnice 的 glib 线程进来，
   与播放线程完全并发 —— 这才是标准的多线程场景。

数据竞争的后果：轻则统计错乱（`played`/`lost` 数值乱跳），
重则 `std::deque` 内部指针被破坏 → **段错误或堆损坏**。
而且这类问题**不可复现**（取决于调度），线上偶发极难查。

### 修复

给 `JitterBuffer` 加一把 `mutable std::mutex`，所有公开方法加锁：

```cpp
class JitterBuffer {
public:
    bool push(...);                       // 加锁
    bool pop(...);                        // 加锁
    bool ready_to_play(int64_t) const;    // 加锁
    void reset();                         // 加锁
    Stats snapshot() const;               // 加锁
    Stats stats() const;                  // 加锁（按值返回！）
    size_t depth() const;                 // 加锁
private:
    mutable std::mutex mutex_;
    // 内部私有方法（update_jitter/adapt_target_delay）**不加锁**，
    // 由已持锁的公开方法调用 —— 这是标准的"锁边界"设计（见坑 12）
};
```

**连带修正 1：`stats()` 必须按值返回**

```cpp
// 错误：返回 const 引用 → 调用方在锁外读到正在被改的数据
const Stats& stats() const { return stats_; }

// 正确：加锁拷贝
Stats stats() const { std::lock_guard<std::mutex> lk(mutex_); return stats_; }
```

"返回值"和"返回引用"在线程安全上有本质区别：前者是快照，后者是活指针。

**连带修正 2：`std::mutex` 让类不可赋值**

加锁后编译报错：

```
error: use of deleted function 'JitterBuffer& JitterBuffer::operator=(JitterBuffer&&)'
note:  'std::mutex& std::mutex::operator=(const std::mutex&)' is deleted
```

因为 `std::mutex` 删除了拷贝/移动，含 mutex 的类**自动不可拷贝/不可移动**。
而 `AudioPipeline::init()` 里原来是：

```cpp
jb_ = JitterBuffer(jcfg);   // ✗ 不可赋值
```

`jb_` 是成员（构造时用默认配置），真正配置要等 `init()` 才知道。
改为**在原有对象上重新配置**：

```cpp
jb_.set_config(jcfg);       // ✓ 不重建对象
jb_.reset();
```

### 验证

```
修复后：全量回归 8 项全部通过
        真实双进程通话：A/B 各 84 包收发，SRTP 解密 84/84，解码错误 0，退出码 0
```

### 教训

**"单线程能跑通"不代表"多线程安全"。**

判断一个组件要不要加锁，看的是**它被哪些线程调用**，不是"测试过没问题"：

| 调用场景 | 线程模型 | 需要锁吗 |
|---|---|---|
| `--loopback` | 发送线程 + 播放线程（已并发！） | **需要**（当时就漏了） |
| `e2e_test` | 同上 | 需要 |
| `--call` | libnice glib 线程 + 播放线程 | **必须** |

**关键认知**：`on_media_packet()` 这个"看起来只是个普通函数"的接口，
在 loopback 里是"自己线程调自己"，在真实通话里是"网络线程调过来" ——
**同一个函数，线程语义完全不同**。这就是为什么集成层最难。

同类问题在本项目是第 5 次出现（坑 1/11/32/33/39 都属于
"一个东西承载两种语义/两种上下文"）。

---

## 坑 40：FEC 长度字段用 u8 → 高码率下静默截断

### 现象

FEC 包格式里，每个源包的长度用 **1 字节（u8）** 存：

```
[u16 base_seq][u16 count][u8 各源包长度...][XOR payload]
```

而 60ms 聚合包在 48 kbps 下：
```
48000 bps × 0.06 s / 8 = 360 字节/包  >  255 (u8 上限)
```

→ 长度被**静默截断**（360 → 104），接收端按错误的长度切分 XOR 载荷，
**恢复出垃圾数据**。没有任何报错。

### 根因

低码率下不会触发：

| 码率 | 60ms 包载荷 | 是否溢出 u8 |
|---|---|---|
| 16 kbps（默认） | 120 字节 | 否 |
| 32 kbps | 240 字节 | 否（接近上限） |
| **48 kbps** | **360 字节** | **是** |
| 64 kbps | 480 字节 | 是 |

默认配置是 16 kbps，所以**所有现有测试都没抓到**。
直到接通话并考虑高码率场景（拥塞控制上调到 48 kbps）才暴露。

### 修复

长度字段改为 u16：

```
[u16 base_seq][u16 count][u16 各源包长度...][XOR payload]
```

编码端：
```cpp
const size_t hdr = 2 + 2 + 2 * size_t(count_in_group_);      // 2 字节/长度
put_u16_le(fec_out->data() + 4 + 2 * i, uint16_t(len));       // 写 u16
```
解码端对应改成 `get_u16_le(fec_pkt + 4 + 2*i)`。

### 验证

专门写了大载荷测试（120 / 360 / 900 字节）：

```
   120 字节/包 (FEC包 132 字节): ✅ 恢复成功且内容一致
   360 字节/包 (FEC包 372 字节): ✅ 恢复成功且内容一致
   900 字节/包 (FEC包 912 字节): ✅ 恢复成功且内容一致
```

### 教训

**字段位宽要按"最坏情况"选，不能按"默认配置"选。**

选 u8 时的心理是"120 字节够了，省 1 字节"——
但**省下的字节数微不足道（4 字节/包），风险却是静默数据损坏**。

排查方法：**把所有配置参数推到极值跑一遍**。
本项目默认 16 kbps，测试全绿；一旦按拥塞控制的上限 48 kbps 跑，
这个 bug 立刻现形。

> **"默认配置下能跑"和"所有合法配置下能跑"是两件事。**

---

## 坑 41：拥塞控制的目标码率没有真正生效

### 现象

`AudioPipeline::set_target_bitrate_bps()` 的实现是：

```cpp
void AudioPipeline::set_target_bitrate_bps(int bps) {
    LOGI(kTag, "target bitrate -> %d bps", bps);
    // 简化：重建编码器代价高，这里仅记录；真实实现应支持运行时调整
}
```

**只打日志，什么都不做。**

### 根因

当时以为"改码率需要重建 Opus 编码器"（代价高），
于是留了个占位没实现。

实际上 Opus 支持**运行时**改码率：

```cpp
opus_encoder_ctl(enc_, OPUS_SET_BITRATE_REQUEST, opus_int32(bps));
```

不需要重建。而本项目在 `OpusCodec::set_bitrate()` 里**早就实现了**这个能力
（Phase 6 的 e2e 测试就是直接调它做闭环验证的），
只是管线这一层的接口没接上。

### 后果

**拥塞控制成了空转**：
GCC 算出 `target_bps`，日志打印"target bitrate -> 48000 bps"，
但编码器仍在 16000 bps 编码 —— **决策和执行断开**。

这类"假实现"比崩溃更危险：它让**指标看起来正常**，
只有对比"目标码率"和"实测码率"才能发现对不上。

### 修复

```cpp
void AudioPipeline::set_target_bitrate_bps(int bps) {
    if (codec_.set_bitrate(bps)) {
        LOGI(kTag, "target bitrate -> %d bps (applied to encoder)", bps);
    } else {
        LOGW(kTag, "set_target_bitrate_bps(%d) failed", bps);
    }
}
```

### 教训

**占位实现（stub）必须显式标记，而且要有测试守住。**

三种处理方式的取舍：

| 做法 | 优点 | 缺点 |
|---|---|---|
| 留空 + 注释"简化"（本项目原做法） | 编译通过、进度快 | **静默失效，最难发现** |
| `assert(false)` / 返回错误码 | 一调用就暴露 | 需要调用方处理 |
| 直接实现（最终做法） | 无隐患 | 需要一点时间 |

**关键检查点**：每个"看起来应该有作用"的接口，都要问一句
**"调了它之后，有什么可观测的变化？"**
如果答案是"只有日志"，那它就是没实现。

---

## 坑 42：视频必须分片，而分片丢一个就整帧作废

### 现象

加上视频后，第一版实现"一帧一个包"直接发。结果：

```
vpx_codec_encode 成功，码流 7127 字节（关键帧）
Packet::serialize 时 payload_len 是 u16，能放下
但发到网络上：单个 UDP 包 7127 字节
→ 超过 MTU（1500），IP 层被迫分片
→ 任何一个 IP 分片丢失，整个 UDP 包在内核层就被丢弃
```

实测现象：**关键帧几乎永远收不到**，接收端一直请求关键帧。

### 根因

**视频和音频在"包大小"上有本质差异**：

| | 音频（Opus） | 视频（VP8） |
|---|---|---|
| 单帧大小 | 40~120 字节 | **2000~8000 字节** |
| 能否一帧一包 | 能（3 帧聚合也才 360 字节） | **不能**（关键帧远超 MTU） |
| 丢包后果 | PLC 补一帧，几乎无感 | **整帧作废**（缺一个分片就拼不出完整帧） |
| 丢关键帧后果 | 无特殊影响 | **后续所有 P 帧都无法解码** |

MTU 约束：以太网 1500 字节，减去 IP 头 20 + UDP 头 8 + RTP/SRTP 头约 20~
→ **实用载荷上限约 1200 字节**，留安全余量取 1100。

### 修复

**发送端分片**：把一帧切成 N 个 ≤1100 字节的片，每片加 8 字节分片头：

```
[u16 frame_id][u16 frag_index][u16 frag_count][u8 flags][u8 reserved]
```

**接收端重组**：按 frame_id 聚合分片，凑齐 frag_count 才拼成一帧。
**任一分片缺失 → 整帧丢弃**（不是花屏，是明确丢弃）。

实测（320x240 @15fps，400kbps）：
```
A 发送: 帧=46  关键帧=4  分片=138    ← 平均每帧 3 个分片
B 接收: 分片=138  完整帧=46  解码=46  丢弃=0
```

### 教训

**"能不能一帧一个包"取决于帧的大小与 MTU 的关系，而不是习惯。**

音频帧小，一帧一包没问题；视频帧大，必须分片。
更关键的是**丢包的语义完全不同**：

- 音频：丢一包 = 丢一小段波形 → PLC 补出来，听感可接受
- 视频：丢一分片 = 整帧残缺 → 不能"猜"，必须整帧丢弃

**这决定了抗损策略完全不同**：
音频可以用 FEC/PLC 硬扛；视频必须靠**关键帧请求**来恢复。

---

## 坑 43：视频丢关键帧后，后续帧全部不可解码（必须实现 PLI）

### 现象

网络抖动导致一个关键帧丢失后，接收端**画面永久卡住**：

```
A 发送: 关键帧 #0 发出（丢了）
B 收到: P 帧 #1 #2 #3 #4 ... 全部解码失败
B 画面: 一直停在最后一帧（或黑屏）
```

而且**不会自己恢复** —— 因为后续 P 帧都依赖那个丢失的关键帧作为参考。

### 根因

视频编码是**帧间预测**的：

```
I 帧（关键帧）：独立编码，可单独解码
P 帧：只编码"与参考帧的差异"，必须知道参考帧才能解码
      → P 帧依赖前一个帧，形成一条链
      → 链头（关键帧）断了，整条链都废
```

实测验证（`vp8_test` 里专门测了这个）：

```
从关键帧开始解码: 6/6 成功
丢弃关键帧后解码 P 帧: 0/5 成功      ← 全部失败
```

### 修复：关键帧请求（PLI / FIR）

接收端一旦发现"解码失败且无法恢复"，就向发送端请求新的关键帧：

```cpp
// 接收端：解码失败 → 请求
if (!decoder_.decode(...)) {
    rep_.frames_dropped++;
    request_keyframe_throttled();     // 带限流
}

// 发送端：收到请求 → 下一帧强制关键帧
void Vp8Codec::request_keyframe() {
    force_keyframe_ = true;           // 下一帧用 VPX_EFLAG_FORCE_KF
}
```

**关键帧请求必须走可靠通道**：本项目用信令（WebSocket/TCP）传
`kKeyframeRequest` 消息。真实 WebRTC 用 RTCP PLI（与 RTP 同通道但有独立可靠性）。

**必须限流**：如果每个坏帧都请求，网络差时会形成"请求风暴"，反而挤占带宽。
本项目限流 300ms（约 3.3 次/秒），WebRTC 的 PLI 限流通常是 500ms~1s。

### 验证

```
全丢 1.5 秒后：B 收到分片=0 解码=0
恢复网络后：A 关键帧=5  B 关键帧请求=1  解码=45
[PASS] B 检测到无法解码并发出关键帧请求（PLI）
[PASS] 恢复网络后能重新解码（关键帧请求生效）
```

### 教训

**"丢包恢复"在音频和视频里是两套完全不同的机制。**

| | 音频 | 视频 |
|---|---|---|
| 丢包恢复手段 | PLC（本地生成） | **PLI 请求关键帧（要往返）** |
| 恢复延迟 | 立即（不需要对端配合） | **至少 1 个 RTT** |
| 能否本地弥补 | 能（波形连续，可以猜） | **不能**（不能凭空生成画面） |
| 周期性保护 | 不需要 | **必须周期性发关键帧**（本实现 2 秒一个） |

面试时这条很好用：**"视频为什么必须有 PLI？"**
答：因为 P 帧依赖参考帧，关键帧丢了整条依赖链断裂，
本地无法恢复（不像音频能 PLC 猜），只能请对端重发一个关键帧。

---

## 坑 44：关键帧请求的限流基准初始化错 → 限流完全失效

### 现象

写限流逻辑时用了很自然的写法：

```cpp
int64_t last_kf_req_ms_ = 0;          // 成员初始值

void request_keyframe() {
    const int64_t now = now_ms();
    if (now - last_kf_req_ms_ >= kMinIntervalMs) {   // 300ms
        last_kf_req_ms_ = now;
        send_request();
    }
}
```

看起来没问题，但**第一次请求时 `now - 0` 是一个巨大的值**（`now_ms()` 是系统
运行时长，可能几十万毫秒），所以 `>= 300` 恒成立。

更糟的是：**这个"恒成立"其实不影响限流本身**（第一次放行是对的），
但**测试里暴露了另一个问题** —— 我在测试中用了"50% 丢包 4 秒"来验证限流，
结果请求数是 **0**，`CHECK(req_per_sec <= 4.0)` 恒真，**假通过**。

### 根因（两个问题叠加）

**(a) 限流基准用 0 做初始值** —— 这是"哨兵值"与"真实时间"混用。
虽然本例恰好没造成限流失效，但它是**脆弱的**：
如果 `now_ms()` 在启动初期返回小值（<300），行为就会变。

**(b) 更严重：触发路径不全**。
限流测试用 50% 丢包，此时帧是"**缺片被丢弃**"，
**根本没有走到解码失败那条路径**，所以一次请求都没发。

```cpp
// 只有解码失败才请求 —— 缺片丢弃的路径漏了
if (!decoder_.decode(...)) { request_keyframe(); }
```

高丢包时帧根本到不了解码器（缺片在重组阶段就被丢了）→ 永远不请求
→ **画面永久卡住，而且不会自愈**。

### 修复

**(a) 统一成一个带限流的入口，用 0 作"从未请求"哨兵**：

```cpp
void VideoPipeline::request_keyframe_throttled() {
    if (!kf_req_fn_) return;
    const int64_t now = now_ms();
    // last_kf_req_ms_ == 0 表示从未请求过 → 直接放行并建立基准
    if (last_kf_req_ms_ != 0 && now - last_kf_req_ms_ < kMinIntervalMs) return;
    last_kf_req_ms_ = now;
    rep_.keyframe_requests++;
    kf_req_fn_();
}
```

**(b) 把"缺片丢弃"也纳入触发条件**：

```cpp
// 重组超时且这帧是关键帧 → 关键帧丢了，必须请求
if (it->second.is_keyframe) {
    lost_keyframe_until_.insert(it->first);
    need_kf_req = true;
}
...
if (need_kf_req) request_keyframe_throttled();   // 在锁外调用
```

### 验证

```
修复前: 4 秒内关键帧请求 0 次（假通过：CHECK 恒真）
修复后: 4 秒内关键帧请求 7 次（1.8 次/秒）
[PASS] 关键帧请求被限流（<= 4 次/秒，避免挤占带宽）
```

### 教训

**1. 这是坑 28 的第三次复发**（节流基准初始化）。
"时间基准用 0 初始化"在本项目已经坑了三次（坑 28 拥塞控制、坑 32 指标时长、坑 44）。
**统一规则：时间类基准用"哨兵值"（0 或 -1）表示"未初始化"，并在使用时显式判断。**

**2. "假通过"比"失败"更危险。**
测试 4 的 `CHECK(req_per_sec <= 4.0)` 在请求数为 0 时**必然通过** ——
它验证的是"不超过上限"，而 0 次当然不超过。
**断言要能区分"通过"和"没测到"**，例如加一条 `CHECK(requests > 0)` 前置条件。

这条经验对所有"限流/上限"类测试都适用：
**先断言"确实发生了"，再断言"没有超限"。**

---

## 汇总：这 44 个坑的类型分布

| 类型 | 坑号 | 数量 |
|---|---|---|
| **逻辑/算法设计错误** | 1、10、11 | 3 |
| **统计口径 / 语义混淆** | 1、11、32(a)、33、34 | 5 |
| **可测试性设计缺陷** | 9、28、32(b) | 3 |
| **测试代码自身缺陷** | 2、27、30(b)、34 | 4 |
| **消息/并发时序问题** | 19、20 | 2 |
| **C 库与 C++ 混用陷阱** | 15、16、17、18、21、25 | 6 |
| **事件循环 / 上下文管理** | 23 | 1 |
| **协议默认行为与配置语义** | 22、24、26 | 3 |
| **多组件集成接线错误** | 29 | 1 |
| **协议接口约束（RTP/SRTP/ICE）** | 30、31 | 2 |
| **语言细节** | 3、7、8 | 3 |
| **环境 / 用户身份 / 工具链** | 4、5、6、**35**、**36** | 5 |
| **其他实现疏漏** | 12、13、14 | 3 |

### 最值得在面试中讲的

| 坑 | 为什么值得讲 |
|---|---|
| **1** 乱序 vs 迟到 | 两个语义共用一个变量 → 设计问题 |
| **10** 目标延迟跟平均值 | 修复后接受率 43% → 99.8% |
| **11** 丢包统计口径 | 统计口径错误，虚报 6 倍 |
| **17** 裸内存里放 std::string | C/C++ 混用经典崩溃，靠 gdb 定位 |
| **20** 覆盖式赋值丢消息 | 只在时序紧时出现，用队列根治 |
| **23** 上下文不一致收不到 STUN ⭐ | tcpdump 抓包定位的完整案例 |
| **26** DTLS 默认不请求对端证书 ⭐ | "握手成功 ≠ 双向身份已验证" |
| **27** 宏二次求值 ⭐ | "全 PASS 却报失败" |
| **28** 节流基准用真实时钟 ⭐ | "部分正常工作"比完全失效更难查 |
| **29** 集成接线顺序错 ⭐ | 症状与根因距离远 |
| **30** 自定义头不兼容 RTP ⭐ | 自建协议 vs 标准协议栈的接口约束 |
| **31** Trickle 漏交换凭证 ⭐ | "前置全 PASS，最终建链失败"的典型 |
| **32** 恢复率分母 + 指标时长 ⭐ | 统计口径错误在一个项目里复发 |
| **33** 载荷/网络字节混用 ⭐ | 同一类语义混淆反复出现 |
| **34** FEC 包污染 NACK 统计 ⭐ | 注释写了约束但代码没遵守；写文档时才发现 |
| **35** root 编译导致用户跑不动 ⭐ | **"开发环境 ≠ 用户环境"的经典教训** |
| **36** 工具装在 root home | 同上，root 开发的第二个连带后果 |

### 定位方法论总结（本项目最有价值的部分）

```
候选收集失败（坑 23）         ← tcpdump 抓包
server 读不到证书（坑 26）     ← 查协议默认行为
全 PASS 却报失败（坑 27）      ← 核对 PASS 行数与计数
码率纹丝不动（坑 28）          ← 看统计值（overuse==0 指出分支未进入）
握手必然超时（坑 29）          ← 检查回调注册顺序与阻塞语义
媒体到了却不解密（坑 30）      ← 打日志看首字节（0x00 vs 0x80）
候选都对却连不上（坑 31）      ← 检查是否漏传必要初始化信息
恢复率算成 50%（坑 32）        ← 检查派生指标的分母口径
码率比目标高 73%（坑 33）      ← 检查字节计数是哪一层的
检测丢包少报（坑 34）          ← 把数据整理成表格逐行对齐
Permission denied（坑 35）     ← ls -ld 看所有者
cmake 版本不对（坑 36）        ← which -a cmake 看用的是哪一个
```

**共同点：症状与根因的距离都很远，且都不能靠"看代码"发现。**
真正起作用的是**先精确描述现象**（抓包、打日志、算统计、看首字节、核对计数、做表格、看文件所有者），
再用排除法缩小范围——而不是凭直觉改代码。

**坑 35/36 特别值得记住**：它们不是代码 bug，而是
**"我这儿能跑 ≠ 用户那儿能跑"**——
这是所有交付给别人用的东西都会遇到的经典问题。

### 系统性盲区总结（最值得主动防御的四类）

| 盲区 | 出现次数 | 坑号 | 防御方式 |
|---|---|---|---|
| **一变量/口径承载两种语义** | **5** | 1、11、32(a)、33、34 | 命名体现层次；统计量明确分母；入口处分流 |
| **组件耦合真实时钟** | **3** | 9、28、32(b) | 时间必须完全由外部注入（含初始化基准） |
| **集成遗漏必要信息** | **3** | 26、29、31 | 对照协议要求逐项检查：证书请求/回调/凭证 |
| **开发环境 ≠ 用户环境** | **2** | 35、36 | 用普通用户身份测试；脚本自检权限与工具链版本 |

前三类是**代码设计**层面的盲区，第四类是**交付**层面的盲区。
本项目最后一轮才暴露第四类——因为之前所有验证都是 root 做的，
**这本身就是验证方法的漏洞**。

**最终的验证原则**：
> 用**用户会用的身份**、在**用户会用的目录**、跑**用户会用的命令**。

写代码时就该主动自问：
*这个变量的语义是唯一的吗？这个时间从哪来？协议要求的信息都传了吗？
这两个流的序号空间分开了吗？注释写的约束，代码真的遵守了吗？
换个用户身份还能跑吗？工具链版本对得上吗？*










