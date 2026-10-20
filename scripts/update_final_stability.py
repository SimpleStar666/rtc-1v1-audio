#!/usr/bin/env python3
# 用最终实测数据更新 03-verification 的稳定性章节
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/docs/03-verification.md'
s = open(P, encoding='utf-8').read()

# 找到旧的收敛表并整体替换
start = s.find('**修复收敛过程**（这条曲线本身就是"为什么要多跑几遍"的答案）：')
if start < 0:
    raise SystemExit('未找到收敛过程段')
end = s.find('**如果只跑一遍，这些问题一个都发现不了。**')
if end < 0:
    raise SystemExit('未找到结尾段')
end += len('**如果只跑一遍，这些问题一个都发现不了。**')

NEW = """**修复收敛过程**（这条曲线本身就是"为什么要多跑几遍"的答案）：

| 阶段 | 10 轮成绩 | 失败形态 | 修了什么 |
|---|---|---|---|
| 加视频后 | 6 PASS | A 端全帧解码失败 | — |
| 修坑 45（SRTP 加锁） | 7 PASS | 间歇性全帧失败 | 并发安全 |
| 修坑 46（服务器转发 PLI） | 7 PASS | 画面卡死不自愈 | 协议消息 |
| 修坑 47（主循环 100ms） | 9 PASS | 固定丢 15 帧 | 循环频率解耦 |
| 修首帧紧急请求 | 9 PASS | 固定丢 9 帧 | 动态限流间隔 |
| 修 skip 路径 + flag 误消费 | 9 PASS | 固定丢 4 帧 | 补齐请求路径 |
| **加启动关键帧突发** | **10 PASS** | — | **根治启动窗口** |

**最终 10 轮加压结果（变化分辨率/码率/画面源）**：

```
轮1   5s test:bars   /test:motion  320x240 400kbps : PASS  A=77帧 B=77帧
轮2   5s test:motion /test:bars    320x240 400kbps : PASS  A=72帧 B=76帧
轮3   6s test:motion /test:motion  320x240 400kbps : PASS  A=86帧 B=92帧
轮4   5s test:bars   /test:bars    320x240 200kbps : PASS  A=71帧 B=76帧
轮5   5s test:motion /test:motion  320x240 800kbps : PASS  A=76帧 B=76帧
轮6   5s test:bars   /test:motion  640x480 400kbps : PASS  A=71帧 B=76帧
轮7   5s test:motion /test:bars    640x480 400kbps : PASS  A=77帧 B=77帧
轮8   6s test:noise  /test:noise   320x240 600kbps : PASS  A=92帧 B=92帧
轮9   5s test:bars   /test:motion  320x240 400kbps : PASS  A=76帧 B=76帧
轮10  8s test:motion /test:motion  320x240 400kbps : PASS  A=123帧 B=122帧

>>> PASS=10  FAIL=0
```

**关于"启动咬合损耗"（诚实说明）**

从上表能看到：部分轮次 A 端比 B 端少几帧（如轮4 A=71 / B=76，差 5 帧 ≈ 330ms）。
这**不是缺陷**，而是实时视频的固有代价：

- 接收端必须先收到一个**关键帧**才能开始解码
- 在关键帧到达之前收到的 P 帧没有参考帧，无法解码，只能丢弃
- 所以每次通话建立时都会有一小段"咬合期"

本项目把这个窗口从最初的 **15 帧（1 秒）** 压到了 **5~6 帧（约 400ms）**，
手段是三层叠加：

1. 接收端启动时主动请求关键帧（不等解码失败）
2. 首帧解码前用 100ms 的紧急请求间隔（之后恢复 300ms）
3. 发送端开头 1 秒内每 5 帧强制一个关键帧（"initial keyframe burst"）

WebRTC 同样有这个过程，做法也是 PLI + 启动关键帧突发。
**判定标准因此是"没有持续丢帧"，而不是"绝对零丢帧"** ——
持续丢帧才是 bug，启动期的几帧损耗是物理规律。

**如果只跑一遍，上面这几个问题一个都发现不了。**"""

s = s[:start] + NEW + s[end:]
open(P, 'w', encoding='utf-8').write(s)
print('✅ 03-verification 已用最终数据更新')
print('   现', s.count(chr(10)) + 1, '行')
