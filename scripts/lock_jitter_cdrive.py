#!/usr/bin/env python3
# 直接对 C 盘源码文件加锁 + 补 set_config（避免 WSL/C 盘来回覆盖丢失改动）
import sys

C_PP = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/media/jitter_buffer.cpp'
C_H  = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/media/jitter_buffer.h'

s = open(C_PP, encoding='utf-8').read()
changed = 0

def patch(old, new, label):
    global s, changed
    if old in s:
        s = s.replace(old, new, 1)
        print(f'  ✓ {label}')
        changed += 1
    elif new.strip().split('\n')[0] in s:
        print(f'  - {label}（已存在，跳过）')
    else:
        print(f'  ✗ {label}：没找到锚点')

# ---- 1) 构造函数后补 set_config ----
ctor = """JitterBuffer::JitterBuffer(const Config& cfg) : cfg_(cfg) {
    cfg_.target_delay_ms = std::max(cfg_.min_delay_ms, std::min(cfg_.max_delay_ms, cfg_.target_delay_ms));
    stats_.cur_target_ms = cfg_.target_delay_ms;
}
"""
setcfg = ctor + """
// 运行时重新配置（不重建对象）。
// 为什么需要它：JitterBuffer 含 std::mutex，mutex 删除了拷贝/移动，
// 导致整个类不可赋值。而 AudioPipeline 的 jb_ 是成员（构造时用默认配置），
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
patch(ctor, setcfg, 'set_config() 实现')

# ---- 2) 各方法加锁 ----
patch(
"""bool JitterBuffer::push(uint16_t seq, const uint8_t* payload, size_t len, int64_t arrive_ms) {
    if (arrive_ms < 0) arrive_ms = now_ms();""",
"""bool JitterBuffer::push(uint16_t seq, const uint8_t* payload, size_t len, int64_t arrive_ms) {
    // 锁：push 来自 ICE 接收回调线程，与播放线程的 pop 并发（坑 39）
    std::lock_guard<std::mutex> lk(mutex_);
    if (arrive_ms < 0) arrive_ms = now_ms();""",
'push() 加锁')

patch(
"""bool JitterBuffer::ready_to_play(int64_t now) const {
    if (playing_) return true;""",
"""bool JitterBuffer::ready_to_play(int64_t now) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (playing_) return true;""",
'ready_to_play() 加锁')

patch(
"""bool JitterBuffer::pop(std::vector<uint8_t>* payload, int64_t now) {
    payload->clear();""",
"""bool JitterBuffer::pop(std::vector<uint8_t>* payload, int64_t now) {
    // 锁：pop 在播放线程，与网络回调线程的 push 并发（坑 39）
    std::lock_guard<std::mutex> lk(mutex_);
    payload->clear();""",
'pop() 加锁')

patch(
"""void JitterBuffer::advance_play_clock() {
    play_deadline_ms_ += cfg_.frame_ms;
}""",
"""void JitterBuffer::advance_play_clock() {
    std::lock_guard<std::mutex> lk(mutex_);
    play_deadline_ms_ += cfg_.frame_ms;
}""",
'advance_play_clock() 加锁')

patch(
"""void JitterBuffer::reset() {
    queue_.clear();""",
"""// stats() 按值返回：避免调用方在锁外读到正在被修改的 stats_
JitterBuffer::Stats JitterBuffer::stats() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return stats_;
}

size_t JitterBuffer::depth() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size();
}

void JitterBuffer::reset() {
    std::lock_guard<std::mutex> lk(mutex_);
    queue_.clear();""",
'reset() 加锁 + stats/depth 实现')

patch(
"""JitterBuffer::Stats JitterBuffer::snapshot() const {
    Stats s = stats_;""",
"""JitterBuffer::Stats JitterBuffer::snapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    Stats s = stats_;""",
'snapshot() 加锁')

open(C_PP, 'w', encoding='utf-8').write(s)
print(f'\n✅ jitter_buffer.cpp 共应用 {changed} 处改动')

# ---- 校验 ----
chk = open(C_PP, encoding='utf-8').read()
print('  锁出现次数:', chk.count('lock_guard<std::mutex>'))
print('  set_config 实现:', chk.count('JitterBuffer::set_config'))
