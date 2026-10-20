#!/usr/bin/env python3
# 给 JitterBuffer 的各公开方法加锁（修数据竞争）
P = '/home/ts/rtc-1v1-audio/src/media/jitter_buffer.cpp'
s = open(P, encoding='utf-8').read()

def patch(old, new, label):
    global s
    if old not in s:
        raise SystemExit(f'❌ 没找到: {label}')
    s = s.replace(old, new, 1)
    print(f'  ✓ {label}')

# push
patch(
"""bool JitterBuffer::push(uint16_t seq, const uint8_t* payload, size_t len, int64_t arrive_ms) {
    if (arrive_ms < 0) arrive_ms = now_ms();""",
"""bool JitterBuffer::push(uint16_t seq, const uint8_t* payload, size_t len, int64_t arrive_ms) {
    // 锁：push 来自 ICE 接收回调线程，与播放线程的 pop 并发
    std::lock_guard<std::mutex> lk(mutex_);
    if (arrive_ms < 0) arrive_ms = now_ms();""",
'push() 加锁')

# ready_to_play
patch(
"""bool JitterBuffer::ready_to_play(int64_t now) const {
    if (playing_) return true;""",
"""bool JitterBuffer::ready_to_play(int64_t now) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (playing_) return true;""",
'ready_to_play() 加锁')

# pop
patch(
"""bool JitterBuffer::pop(std::vector<uint8_t>* payload, int64_t now) {
    payload->clear();""",
"""bool JitterBuffer::pop(std::vector<uint8_t>* payload, int64_t now) {
    // 锁：pop 在播放线程，与网络回调线程的 push 并发
    std::lock_guard<std::mutex> lk(mutex_);
    payload->clear();""",
'pop() 加锁')

# advance_play_clock
patch(
"""void JitterBuffer::advance_play_clock() {
    play_deadline_ms_ += cfg_.frame_ms;
}""",
"""void JitterBuffer::advance_play_clock() {
    std::lock_guard<std::mutex> lk(mutex_);
    play_deadline_ms_ += cfg_.frame_ms;
}""",
'advance_play_clock() 加锁')

# reset
patch(
"""void JitterBuffer::reset() {
    queue_.clear();""",
"""void JitterBuffer::reset() {
    std::lock_guard<std::mutex> lk(mutex_);
    queue_.clear();""",
'reset() 加锁')

# snapshot
patch(
"""JitterBuffer::Stats JitterBuffer::snapshot() const {
    Stats s = stats_;""",
"""JitterBuffer::Stats JitterBuffer::snapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    Stats s = stats_;""",
'snapshot() 加锁')

# 新增 stats() 和 depth() 的实现（原来在头文件里 inline）
anchor = 'void JitterBuffer::reset() {'
if anchor not in s:
    raise SystemExit('❌ 没找到 reset 位置')
s = s.replace(anchor,
"""// stats() 按值返回：避免调用方在锁外读到正在被修改的 stats_
JitterBuffer::Stats JitterBuffer::stats() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return stats_;
}

size_t JitterBuffer::depth() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size();
}

""" + anchor, 1)
print('  ✓ 新增 stats() / depth() 实现')

open(P, 'w', encoding='utf-8').write(s)
print('✅ jitter_buffer.cpp 加锁完成')
