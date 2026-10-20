#!/usr/bin/env python3
# 追加坑 39/40/41（真通话暴露的并发与位数缺陷）
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/docs/02-pitfalls.md'
s = open(P, encoding='utf-8').read()

ADD = """## 坑 39：抖动缓冲没有锁 → 真实通话下的数据竞争

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

"""

old_sum = '## 汇总：这 38 个坑的类型分布'
if old_sum not in s:
    raise SystemExit('❌ 找不到汇总标题')
s = s.replace(old_sum, ADD + '## 汇总：这 41 个坑的类型分布', 1)
s = s.replace('共 38 个坑。', '共 41 个坑。', 1)

# 速览表补三行
old_row = '| 38 | **Trickle 回调注册晚于候选产生** ⭐ | **接线时序** | **候选丢失，ICE 连不通** |'
if old_row in s:
    s = s.replace(old_row, old_row + '\n'
        '| 39 | **抖动缓冲无锁** ⭐ | **并发安全** | **真实通话下数据竞争，可能崩溃** |\n'
        '| 40 | **FEC 长度字段 u8** | **位宽不足** | **高码率下静默截断，恢复出垃圾** |\n'
        '| 41 | **目标码率接口是空实现** ⭐ | **占位未实现** | **拥塞控制空转，决策不执行** |', 1)

open(P, 'w', encoding='utf-8').write(s)
print('✅ 已追加坑 39/40/41')
print('   总行数:', s.count('\n') + 1)
print('   坑数  :', s.count('\n## 坑 '))
