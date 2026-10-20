#!/usr/bin/env python3
# 更新 README 坑数与代码导读并发说明
B = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio'
n = 0

# ---- README ----
p = f'{B}/README.md'
s = open(p, encoding='utf-8').read()
before = s
s = s.replace('踩坑记录（44 项', '踩坑记录（47 项')
if s != before:
    open(p, 'w', encoding='utf-8').write(s)
    print('  ✓ README 坑数 44 -> 47')
    n += 1
else:
    print('  ✗ README 未找到 44 项')

# ---- 代码导读顶部 ----
p2 = f'{B}/docs/05-code-tour.md'
s2 = open(p2, encoding='utf-8').read()
old = '> **视频已加入**：模块 11 讲 VP8 编解码、分片重组、关键帧请求（PLI）。'
new = ('> **视频已加入**：模块 11 讲 VP8 编解码、分片重组、关键帧请求（PLI）。\n'
       '>\n'
       '> **并发提醒**：坑 39（JitterBuffer）与坑 45（DtlsSrtpSession）是同一类问题 ——\n'
       '> 组件在"单流"场景下只有 1 个调用者、看起来线程安全，一旦加了第二条流就崩。\n'
       '> 阅读模块 8（DTLS-SRTP）和模块 11（视频）时注意各自的线程归属表。')
if old in s2:
    s2 = s2.replace(old, new, 1)
    open(p2, 'w', encoding='utf-8').write(s2)
    print('  ✓ 05-code-tour 顶部加并发提醒')
    n += 1
else:
    print('  ✗ 未找到视频说明行')

print(f'\n✅ 共 {n} 处')
