#!/usr/bin/env python3
# 记录坑 45/46/47（都是"多跑几遍"才暴露的问题）
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/docs/02-pitfalls.md'
s = open(P, encoding='utf-8').read()

ADD = """## 坑 45：SRTP 会话被多线程并发使用 → 间歇性画面全黑

### 现象

加上视频后，**单次运行经常是好的，但连续跑 5 轮有 2~3 轮失败**（40~60%）：

```
A 端: 解码 46 / 丢弃 29，PLI 发出 6 次、收到 0 次
      每帧都报: [vp8] decode failed: Bitstream not supported by this decoder
B 端: 完全正常（76 帧全解）
```

特征：**间歇性、只一端失败、完整帧也解不出、退出码仍是 0**。

### 根因

`dtls.protect()` 被**两个线程**并发调用，而 libsrtp 的 `srtp_t` 会话**不是线程安全的**：

| 线程 | 调用 | 何时引入 |
|---|---|---|
| 音频发送线程 | `protect()` | Phase 6 |
| **视频发送线程** | `protect()` | **Phase 7 新增，与上面并发** |
| libnice glib 网络线程 | `unprotect()` | Phase 3 |

并发进入同一个 `srtp_t` 会破坏其内部**重放窗口与序列号状态**，
输出**损坏的密文**。接收端 `srtp_unprotect` 返回成功（因为认证标签是对着
损坏后的状态算的），但明文是垃圾 → 解码器报 `Bitstream not supported`。

**为什么纯音频从来没暴露**：只有 1 个发送线程，天然串行。
加视频后发送路径变成 2 个线程，问题立刻出现 —— 但只在**特定的调度交错**下，
所以表现为"跑几遍才复现一次"。

### 修复

收发方向各加一把锁（两个 `srtp_t` 是独立会话，可以并发，不互相阻塞）：

```cpp
// dtls_srtp.h
mutable std::mutex srtp_out_mutex_;   // 保护 srtp_out_（加密：2 个发送线程）
mutable std::mutex srtp_in_mutex_;    // 保护 srtp_in_ （解密：glib 线程）

// dtls_srtp.cpp
bool DtlsSrtpSession::protect(uint8_t* rtp, int* len) {
    if (!srtp_out_) return false;
    std::lock_guard<std::mutex> lk(srtp_out_mutex_);   // ← 加锁
    ...
}
```

### 验证

```
修复前: 5 轮 → PASS 2~3 / FAIL 2~3   （40~60% 失败率）
修复后: 5 轮 → PASS 5   / FAIL 0     （100%）
```

### 教训

**这是坑 39 的同类问题第二次出现，而且成因完全一样**：
某个组件在"单流"场景下只有 1 个调用者，看起来线程安全；
一旦增加并发调用者就崩。

判断清单（每次加新流/新线程都要过一遍）：

| 问题 | 本例答案 |
|---|---|
| 这个对象会被几个线程调用？ | protect: 2 个；unprotect: 1 个 |
| 它内部有可变状态吗？ | 有（重放窗口、序列号） |
| 它自己的文档说线程安全吗？ | **没有**（libsrtp 明确不保证） |

**关键**：`srtp_t` / `SSL*` / `vpx_codec_ctx_t` 这类 C 库句柄**默认都不线程安全**。
组件的所有权边界（谁能调）必须和并发模型一起设计。

### 附带教训：单次运行通过 ≠ 没有 bug

这个 bug 单跑 10 次可能全过，也可能失败 6 次。
**并发 bug 必须用重复执行来验证**，一次成功毫无意义。

---

## 坑 46：信令服务器不转发 PLI，控制消息被静默丢弃

### 现象

A 端日志显示发了 6 次关键帧请求，但 B 端"关键帧请求：收到 **0** 次"：

```
A: 关键帧请求 : 发出 6 / 收到 0（PLI）
B: 关键帧请求 : 发出 0 / 收到 0（PLI）   ← B 根本没收到
```

后果：A 丢关键帧后请求新关键帧，B 永远不知道 → **画面永久卡死、不自愈**。

### 根因

信令服务器的消息分发 `switch` 只处理 4 种类型：

```cpp
switch (m.type) {
    case SignalType::kJoin:      ...
    case SignalType::kOffer:     handle_forward(..., "offer");   break;
    case SignalType::kAnswer:    handle_forward(..., "answer");  break;
    case SignalType::kCandidate: handle_forward(..., "candidate"); break;
    // kKeyframeRequest 落到 default → 静默丢弃
}
```

新增消息类型时**只改了枚举和序列化，漏了服务器的分发**。
编译不会报错（switch 没有强制穷举），运行也不报错（静默丢弃）。

### 修复

```cpp
case SignalType::kKeyframeRequest:
    if (s->in_room) handle_forward(wsi, s, m, "keyframe_request");
    break;
```

### 教训

**新增一种协议消息，要同时改 4 个地方**，漏一个就静默失效：

| # | 位置 | 漏了会怎样 |
|---|---|---|
| 1 | 枚举定义 `signaling_msg.h` | 编译报错（还好） |
| 2 | 类型名映射 `type_to_string` / `string_to_type` | 序列化失败 |
| 3 | **服务器的分发 switch** | **静默丢弃（本例）** |
| 4 | 客户端的处理 switch | 收到但忽略 |

**症状离根因很远**：服务器少一个 case，表现为"视频画面卡死"。
排查时顺着"PLI 发出 6 次 / 收到 0 次"这条线索才找到信令层。

**改进建议**：`switch` 加 `default: LOGW(...)` 记录未处理的消息类型，
让静默失败变成有日志可查。

---

## 坑 47：控制消息的处理频率被"统计打印"的节奏绑架

### 现象

修完坑 45/46 后，加压测试仍然约 30% 失败，**失败形态极其固定**：

```
A 端: 解码 60 / 丢弃 15    ← 永远正好 15 帧
B 端: 解码 76 / 丢弃 0     ← 完美
```

而且只在**特定条件**下出现（640x480 或更长时间），320x240 大多通过。

**"永远是 15"** 这个数字是最大的线索：15fps × 1 秒 = 15 帧。

### 根因（两层）

**第一层：`video_active` 置得太晚**

```cpp
// call_session.cpp，原来的顺序
vpipe.init(vcfg, &vdevice);   // 初始化
...
vpipe.start();                // 启动
video_active = true;          // ← 这里才置 true
```

ICE 接收回调靠 `video_active` 判断是否把分片交给视频管线：

```cpp
} else if (ph.session_id == 2 && video_active.load()) {
    vpipe.on_video_fragment(ph, payload);
}
```

对端（后加入的 B）可能已经在发首帧了 —— 而**首帧就是关键帧**。
`video_active` 还是 false → 关键帧被**静默丢弃** → A 从 P 帧开始收，
必然解不出。

**第二层（更隐蔽）：PLI 只在每秒一次的循环里处理**

```cpp
// 原来的主循环
for (int s = 0; s < opt.seconds; s++) {
    std::this_thread::sleep_for(std::chrono::seconds(1));   // ← 1 秒
    if (need_keyframe.exchange(false) && video_active.load()) {
        vpipe.on_keyframe_request();    // PLI 在这里才被应用
    }
    print_stats();
}
```

**控制消息的响应延迟被统计打印的频率决定了。**
一个 PLI 从到达信令回调到真正生效，最多要等 **1 秒** —— 正好 15 帧。

### 修复

**(a) 尽早置 `video_active`**：

```cpp
if (!vpipe.init(vcfg, &vdevice)) { ... }
else {
    video_active = true;    // ← 初始化完就置，不要等 start()
    ...
}
```

**(b) 拆开"控制泵"与"统计打印"的节奏**：

```cpp
const int kTickMs = 100;                 // 100ms 粒度
const int kTicksPerSec = 1000 / kTickMs;
for (long tick = 0; tick < total_ticks; tick++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));

    // 控制消息泵：每 100ms 检查一次（PLI 响应 ≤100ms）
    if (need_keyframe.exchange(false) && video_active.load()) {
        vpipe.on_keyframe_request();
    }

    // 统计只在整秒打印
    if ((tick + 1) % kTicksPerSec != 0) continue;
    print_stats();
}
```

**(c) 启动时主动请求关键帧**（WebRTC 也是这么做的）：

```cpp
void VideoPipeline::start() {
    ...
    request_keyframe_now();   // 不等解码失败，主动要一个关键帧
}
```

### 验证

```
修复前: 10 轮 → PASS 6 / FAIL 4   （失败固定丢 15 帧）
修复后: 10 轮 → PASS ? / FAIL ?   （见 docs/03-verification.md 实测）
```

### 教训

**1. 控制消息的响应延迟必须由业务需求决定，不能由无关的循环频率决定。**
PLI 需要 <1 帧的响应，却被放在"每秒打印统计"的循环里 —— 这是把
**可观测性代码的位置**当成了**控制逻辑的位置**。

常见的错误写法：

```cpp
while (running) {
    sleep(1);              // 为了"不刷屏"
    handle_control_msg();  // 结果控制也变成 1 秒粒度
    log_stats();
}
```

正确做法：**两个频率分开**。控制泵高频，日志低频。

**2. 固定数字（永远 15）是最强的线索。**
看到"每次都正好 15"，立刻想到 15fps × 1s = 15 → 去找"哪里有 1 秒"。
这比随机丢帧好查得多 —— **可复现的固定偏差是礼物**。

**3. 启动顺序的竞态要用"尽早就绪"而不是"晚点再说"解决。**
`video_active` 晚置 1 毫秒，就可能丢掉一个关键帧，代价是 1 秒的画面空白。

---

"""

old = '## 汇总：这 44 个坑的类型分布'
if old not in s:
    raise SystemExit('❌ 找不到汇总标题')
s = s.replace(old, ADD + '## 汇总：这 47 个坑的类型分布', 1)
s = s.replace('共 44 个坑。', '共 47 个坑。', 1)

old_row = '| 44 | **PLI 限流基准 + 触发路径不全** ⭐ | **限流/假通过** | **高丢包时不请求关键帧** |'
if old_row in s:
    s = s.replace(old_row, old_row + '\n'
        '| 45 | **SRTP 会话多线程并发使用** ⭐ | **并发安全** | **间歇性画面全黑（40~60%）** |\n'
        '| 46 | **信令服务器不转发 PLI** ⭐ | **协议消息遗漏** | **画面永久卡死不自愈** |\n'
        '| 47 | **控制消息频率被统计打印绑架** ⭐ | **循环频率耦合** | **固定丢 15 帧** |', 1)

open(P, 'w', encoding='utf-8').write(s)
print('✅ 已追加坑 45/46/47')
print('   坑数:', s.count('\n## 坑 '))
print('   总行数:', s.count('\n') + 1)
