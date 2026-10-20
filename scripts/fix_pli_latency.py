#!/usr/bin/env python3
# 修 PLI 响应延迟：主循环从 1 秒粒度改为 100ms 粒度
# 症状：PLI 最多要等 1 秒才生效，正好丢 15 帧（15fps × 1s）
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/app/call_session.cpp'
s = open(P, encoding='utf-8').read()

old = """    // 实时进度：每秒打印一行
    for (int s = 0; s < opt.seconds; s++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // 处理对端的关键帧请求（在通话主循环里做，不占信令回调线程）
        if (need_keyframe.exchange(false) && video_active.load()) {
            vpipe.on_keyframe_request();
        }
"""

new = """    // 实时进度：每秒打印一行。
    //
    // ★ 循环粒度是 100ms，不是 1s —— 这一点很关键。
    //
    //   这里除了打印统计，还兼任"控制消息泵"：处理对端发来的
    //   关键帧请求（PLI）。如果循环是 1 秒一次，一个 PLI 最多要等
    //   1 秒才被应用，期间接收端持续丢帧 —— 实测正好丢 15 帧
    //   （15fps × 1s），表现为"主叫端固定丢 15 帧，被叫端 0 丢"。
    //
    //   控制消息的响应延迟应该由它的业务需要决定（PLI 希望 <1 帧），
    //   而不是被"统计打印的频率"绑架。见 docs/02-pitfalls.md 坑 47。
    const int kTickMs = 100;
    const int kTicksPerSec = 1000 / kTickMs;   // 10
    const long total_ticks = long(opt.seconds) * kTicksPerSec;

    for (long tick = 0; tick < total_ticks; tick++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));

        // 控制消息泵（每 100ms 检查一次）：处理对端的关键帧请求
        if (need_keyframe.exchange(false) && video_active.load()) {
            vpipe.on_keyframe_request();
        }

        // 只在整秒时打印统计
        if ((tick + 1) % kTicksPerSec != 0) continue;
        const int s = int((tick + 1) / kTicksPerSec) - 1;
"""

if old not in s:
    raise SystemExit('❌ 找不到主循环锚点')
s = s.replace(old, new, 1)
open(P, 'w', encoding='utf-8').write(s)
print('✅ 主循环改为 100ms 粒度，PLI 响应延迟从 ≤1s 降到 ≤100ms')
