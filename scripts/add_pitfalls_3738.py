#!/usr/bin/env python3
# 在 docs/02-pitfalls.md 里插入坑 37/38，并更新计数
import io, os

P = '/home/ts/rtc-1v1-audio/docs/02-pitfalls.md'

ADD = """## 坑 37：Trickle 候选重复发送 6 倍

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

"""

s = open(P, encoding='utf-8').read()

old_sum = '## 汇总：这 36 个坑的类型分布'
if old_sum not in s:
    raise SystemExit('❌ 没找到汇总标题')
s = s.replace(old_sum, ADD + '## 汇总：这 38 个坑的类型分布', 1)
s = s.replace('共 36 个坑。', '共 38 个坑。', 1)

# 速览表里补两行
old_row = '| 36 | **新版 cmake 只装在 root 的 home** ⭐ | **工具链作用域** | **cmake 版本过低** |'
if old_row in s:
    s = s.replace(old_row, old_row + '\n'
        '| 37 | **Trickle 候选重复发送 6 倍** | **事件语义误用** | 信令带宽浪费 |\n'
        '| 38 | **Trickle 回调注册晚于候选产生** ⭐ | **接线时序** | **候选丢失，ICE 连不通** |', 1)

open(P, 'w', encoding='utf-8').write(s)
print('✅ 已插入坑 37/38')
print('  总行数:', s.count('\n') + 1)
print('  坑标题数:', s.count('\n## 坑 '))
