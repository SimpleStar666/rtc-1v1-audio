# 踩坑记录

> 真实遇到的报错、误判和修复过程，共 11 个坑。
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

**最值得讲的三个：坑 9、坑 10、坑 11**——都是"发现→定位→量化验证"的完整闭环。

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

## 汇总：这 11 个坑的类型分布

| 类型 | 坑 | 数量 |
|---|---|---|
| **逻辑/算法设计错误** | 坑 1（乱序误判）、坑 10（目标延迟低估）、坑 11（丢包统计算错） | **3** |
| **可测试性设计缺陷** | 坑 9（内部读真实时钟） | 1 |
| **测试方法错误** | 坑 2（Opus SNR 未对齐） | 1 |
| **语言细节** | 坑 3（宏展开）、坑 7（头文件）、坑 8（重复声明） | 3 |
| **环境/配置** | 坑 4（CMake）、坑 5（submodule）、坑 6（CA 证书） | 3 |

### 最值得在面试中讲的三个

1. **坑 1（乱序 vs 迟到）**——暴露"两个语义共用一个变量"的设计问题
2. **坑 10（目标延迟跟平均值）**——暴露"调参看似正确但方向错了"的经典陷阱，
   且修复后指标提升显著（接受率 43% → 99.8%）
3. **坑 11（丢包统计口径）**——暴露"用事件顺序表达状态集合"的思维错误，
   且修复后统计精确吻合真值

这三个都是**"发现问题 → 定位根因 → 量化验证"**的完整闭环，
而不是"报错改错"级别的修复。

