#!/usr/bin/env python3
# 给 JitterBuffer 补 set_config() 实现
P = '/home/ts/rtc-1v1-audio/src/media/jitter_buffer.cpp'
s = open(P, encoding='utf-8').read()

old = """JitterBuffer::JitterBuffer(const Config& cfg) : cfg_(cfg) {
    cfg_.target_delay_ms = std::max(cfg_.min_delay_ms, std::min(cfg_.max_delay_ms, cfg_.target_delay_ms));
    stats_.cur_target_ms = cfg_.target_delay_ms;
}
"""
new = """JitterBuffer::JitterBuffer(const Config& cfg) : cfg_(cfg) {
    cfg_.target_delay_ms = std::max(cfg_.min_delay_ms, std::min(cfg_.max_delay_ms, cfg_.target_delay_ms));
    stats_.cur_target_ms = cfg_.target_delay_ms;
}

// 运行时重新配置（不重建对象）。
// 为什么需要它：JitterBuffer 含 std::mutex，mutex 删除了拷贝/移动，
// 导致整个类不可赋值。而 AudioPipeline 的 jb_ 是成员（构造时用的是默认配置），
// 真正配置要等 init() 才知道 —— 只能"在对象上改配置"，不能"换一个对象"。
// 见 docs/02-pitfalls.md 坑 39。
void JitterBuffer::set_config(const Config& cfg) {
    std::lock_guard<std::mutex> lk(mutex_);
    cfg_ = cfg;
    cfg_.target_delay_ms = std::max(cfg_.min_delay_ms,
                          std::min(cfg_.max_delay_ms, cfg_.target_delay_ms));
    stats_.cur_target_ms = cfg_.target_delay_ms;
}
"""
if old not in s:
    raise SystemExit('❌ 没找到构造函数')
s = s.replace(old, new, 1)
open(P, 'w', encoding='utf-8').write(s)
print('✅ set_config() 已添加')
