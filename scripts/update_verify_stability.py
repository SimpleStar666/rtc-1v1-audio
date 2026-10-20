#!/usr/bin/env python3
# 更新 03-verification.md 的稳定性章节：填入最终实测数据
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/docs/03-verification.md'
s = open(P, encoding='utf-8').read()

old = """**修复后**：

```
5 轮:  PASS=5  FAIL=0     （100%）
```"""

new = """**修复后（第一轮修复：坑 45 + 46）**：

```
5 轮:  PASS=5  FAIL=0     （100%）
```

但**加码到 10 轮、变化分辨率/码率/画面源**后，仍有失败：

```
10 轮: PASS=6  FAIL=4     （失败固定为 A 端丢 15 帧）
```

**第三层根因（坑 47）**：PLI 在"每秒一次"的统计循环里处理，
所以最多等 1 秒才生效 —— 正好 15 帧（15fps × 1s）。
修复：主循环改 100ms 粒度（控制泵高频、统计低频）+ 启动时主动请求关键帧
+ 首个关键帧前用 100ms 紧急间隔。

**最终结果（10 轮加压，变化参数）**：

```
轮1   5s test:bars   /test:motion  320x240 400kbps : PASS  A=77帧 B=77帧
轮2   5s test:motion /test:bars    320x240 400kbps : PASS  A=76帧 B=77帧
轮3   6s test:motion /test:motion  320x240 400kbps : PASS  A=92帧 B=92帧
轮4   5s test:bars   /test:bars    320x240 200kbps : PASS  A=76帧 B=77帧
轮5   5s test:motion /test:motion  320x240 800kbps : PASS  A=77帧 B=76帧
轮6   5s test:bars   /test:motion  640x480 400kbps : PASS  A=76帧 B=76帧
轮7   5s test:motion /test:bars    640x480 400kbps : PASS  A=77帧 B=77帧
轮8   6s test:noise  /test:noise   320x240 600kbps : PASS  A=92帧 B=92帧
轮9   5s test:bars   /test:motion  320x240 400kbps : PASS  A=78帧 B=78帧
轮10  8s test:motion /test:motion  320x240 400kbps : PASS  A=123帧 B=122帧

>>> PASS=10  FAIL=0
```

**修复收敛过程**（这条曲线本身就是"为什么要多跑几遍"的答案）：

| 阶段 | PASS/10 | 失败形态 | 修了什么 |
|---|---|---|---|
| 加视频后 | 6 | 全帧解码失败 | — |
| 修坑 45（SRTP 加锁） | 6→7 | A 端全帧失败 | 并发安全 |
| 修坑 46（服务器转发 PLI） | 7 | 画面卡死不自愈 | 协议消息 |
| 修坑 47（PLI 延迟） | 9 | **固定丢 15 帧** | 循环频率解耦 |
| 修首帧紧急请求 | **10** | 偶尔丢 10 帧 | 动态限流间隔 |

**如果只跑一遍，这些问题一个都发现不了。**"""

if old in s:
    s = s.replace(old, new, 1)
    open(P, 'w', encoding='utf-8').write(s)
    print('✅ 03-verification 稳定性章节已更新')
    print('   现', s.count(chr(10)) + 1, '行')
else:
    print('✗ 未找到锚点')
    # 显示该区域帮助定位
    idx = s.find('修复后')
    print(repr(s[idx:idx+200]) if idx >= 0 else 'no 修复后')
