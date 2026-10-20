#!/usr/bin/env python3
# 更新代码导读：三处"未实现/有缺陷"的说明改成"已修复"
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/docs/05-code-tour.md'
s = open(P, encoding='utf-8').read()
n = 0

def rep(old, new, label):
    global s, n
    if old in s:
        s = s.replace(old, new)
        print(f'  ✓ {label}')
        n += 1
    else:
        print(f'  - {label}（未找到，跳过）')

# ---- 1) FEC 长度字段 u8 -> u16 ----
rep('[u16 base_seq][u16 count][u8 len...][XOR payload]',
    '[u16 base_seq][u16 count][u16 len...][XOR payload]',
    'FEC 格式说明 u8→u16')

rep('**实现边界**：FEC 头里每个源包长度是 `u8`（`loss_protection.cpp:176`），',
    '**已修复**：FEC 头里每个源包长度原为 `u8`，**已改为 `u16`**（坑 40）。',
    'FEC u8 边界说明')

rep('但码率超过约 **34kbps**（255B/0.06s）后长度截断，恢复出的包长度不对。',
    '原 u8 在码率超过约 34kbps（255B/0.06s）后会静默截断、恢复出垃圾数据；'
    '改为 u16 后 900 字节/包仍能正确恢复（见 `scripts/test_fec_big.sh`）。',
    'FEC 34kbps 截断说明')

# ---- 2) set_target_bitrate_bps 占位 -> 已实现 ----
rep('1. **`set_target_bitrate_bps()` 目前只是日志占位**（`audio_pipeline.cpp:261-264`），',
    '1. **`set_target_bitrate_bps()` 已真正生效**（坑 41）：'
    '原来只是日志占位，现在直接调 `codec_.set_bitrate()`；'
    '原始占位的说明保留如下 ——',
    'set_target_bitrate_bps 已实现')

# ---- 3) JitterBuffer 无锁 -> 已加锁 ----
rep('3. **JitterBuffer 内部不加锁**，而 `on_media_packet()`（网络回调线程）和 `playout_loop()`',
    '3. **JitterBuffer 已内置锁**（坑 39）：曾经内部不加锁，'
    '而 `on_media_packet()`（网络回调线程）和 `playout_loop()`',
    'JitterBuffer 无锁说明（第 1 处）')

rep('| 线程安全 | **内部不加锁**，调用方保证串行 | 集成层必须自己加锁/排队 | 内部加锁会让 `snapshot()`/测试更复杂 |',
    '| 线程安全 | **内部已加 `std::mutex`**（坑 39） | 每次 push/pop 有一次锁开销（可忽略） | '
    '原先不加锁，真实通话下 push/pop 来自不同线程 → 数据竞争 |',
    '线程安全表格行')

rep('    `AudioPipeline` 目前在网络回调线程 push、播放线程 pop，集成层需要补锁/队列。',
    '    `AudioPipeline` 在网络回调线程 push、播放线程 pop —— 这就是为什么 `JitterBuffer` '
    '**必须自己加锁**（坑 39，已修）。',
    '集成层补锁说明')

rep('内部不加锁，要求 push/pop 串行；当前管线在网络回调线程 push、',
    '内部已加 `std::mutex`（坑 39 修复）；此前不加锁、要求 push/pop 串行，',
    'FAQ 线程安全说明 1')

rep('播放线程 pop，集成层需补锁/队列。这是算法纯粹性 vs 线程安全的取舍。',
    '播放线程 pop，于是改为内部加锁 —— **算法纯粹性让位于正确性**。',
    'FAQ 线程安全说明 2')

# ---- 4) 顶部加一句"已修复"提示 ----
rep('> **行号基准**：2026-10-20，当前工作区。',
    '> **已同步修复**：坑 39（JitterBuffer 加锁）/ 40（FEC 长度 u8→u16）/ '
    '41（目标码率接口真正生效）—— 本文档中标注为"边界/未实现"的三处已全部修复。\n'
    '> **行号基准**：2026-10-20，当前工作区。',
    '顶部声明')

open(P, 'w', encoding='utf-8').write(s)
print(f'\n✅ 共更新 {n} 处')
