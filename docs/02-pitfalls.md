# 踩坑记录

> 真实遇到的报错、误判和修复过程，共 27 个坑。
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

**各阶段最值得讲**：
- Phase 1：坑 **9、10、11**（算法与可测试性）
- Phase 2：坑 **16、17、18、20**（C 库与 C++ 混用）
- Phase 3：坑 **23、24**（事件循环 + 配置语义，定位难度最高）
- Phase 4：坑 **25、26、27**（库 API 细节 + 协议默认行为 + 测试自身缺陷）

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

## 汇总：这 27 个坑的类型分布

| 类型 | 坑号 | 数量 |
|---|---|---|
| **逻辑/算法设计错误** | 1、10、11 | 3 |
| **可测试性设计缺陷** | 9 | 1 |
| **测试代码自身缺陷** | 2、**27** | 2 |
| **消息/并发时序问题** | 19、20 | 2 |
| **C 库与 C++ 混用陷阱** | 15、16、17、18、21、**25** | 6 |
| **事件循环 / 上下文管理** | **23** | 1 |
| **协议默认行为与配置语义** | 22、24、**26** | 3 |
| **语言细节** | 3、7、8 | 3 |
| **环境/配置** | 4、5、6 | 3 |
| **其他实现疏漏** | 12、13、14 | 3 |

### 最值得在面试中讲的

| 坑 | 为什么值得讲 |
|---|---|
| **1** 乱序 vs 迟到 | 两个语义共用一个变量 → 设计问题 |
| **10** 目标延迟跟平均值 | 修复后接受率 43% → 99.8% |
| **11** 丢包统计口径 | 修复后与真值精确吻合 |
| **17** 裸内存里放 std::string | C/C++ 混用经典崩溃，靠 gdb 定位 |
| **20** 覆盖式赋值丢消息 | 只在时序紧时出现，用队列根治 |
| **23** 上下文不一致收不到 STUN ⭐ | **tcpdump 抓包定位的完整案例** |
| **24** stun-server 只接受 IP | 从二进制字符串找版本差异 |
| **26** DTLS 默认不请求对端证书 ⭐ | "握手成功 ≠ 双向身份已验证" |
| **27** 宏二次求值 ⭐ | **"全 PASS 却报失败"——最难自己发现的一类测试缺陷** |

### 定位方法论总结（本项目最有价值的部分）

七个疑难问题的排查路径，都遵循"分层缩小范围"：

```
候选收集失败（坑 23）
  → 裸 socket 验证网络可达（排除网络层）
  → 库内部日志看到"重传"（锁到 STUN 交互层）
  → tcpdump 看到"请求发出+响应回来"（精确定位到"收到但没处理"）
  → 检查事件循环上下文（找到根因）

server 读不到证书（坑 26）
  → 先排除时序（多跑几轮无效）
  → 查协议默认行为（server 不请求 client 证书）
  → 试 FAIL_IF_NO_PEER_CERT（握手被拒）
  → 改 permissive callback（解决）

全 PASS 却报失败（坑 27）
  → 核对 PASS 行数与计数（发现自相矛盾）
  → 检查宏定义（发现条件被求值两次）
```

**共同点：症状与根因的距离都很远。**
真正解决问题的不是"改代码"，而是**先精确描述现象**——
比如坑 27 若只看到"测试失败"就去改被测代码，方向完全错了。




