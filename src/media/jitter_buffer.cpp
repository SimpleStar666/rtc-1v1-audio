#include "media/jitter_buffer.h"

#include <algorithm>
#include <cmath>
#include "common/logging.h"

namespace rtc {

namespace {
constexpr const char* kTag = "jitter";

// 抖动估计的指数平滑系数（RFC 3550 用 1/16）
constexpr double kJitterAlpha = 1.0 / 16.0;
// 目标延迟的收敛步长
constexpr int kAdaptStepMs = 20;
// 近期最大偏差的统计窗口（帧数）
constexpr int kMaxDevWindow = 50;
}  // namespace

JitterBuffer::JitterBuffer() : JitterBuffer(Config{}) {}

JitterBuffer::JitterBuffer(const Config& cfg) : cfg_(cfg) {
    cfg_.target_delay_ms = std::max(cfg_.min_delay_ms, std::min(cfg_.max_delay_ms, cfg_.target_delay_ms));
    stats_.cur_target_ms = cfg_.target_delay_ms;
}

// 计算序列号差值，处理 16bit 回绕
int16_t JitterBuffer::seq_diff(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(a - b);
}

void JitterBuffer::update_jitter(int64_t arrive_ms, uint16_t seq) {
    if (have_last_) {
        // 抖动定义：本包实际到达间隔 与 期望间隔（帧长）之差
        const int64_t transit = arrive_ms - last_arrive_ms_;
        const double  d = std::fabs(double(transit) - double(cfg_.frame_ms));

        // 平滑值：反映长期趋势（RFC 3550 用 1/16 系数）
        jitter_ms_ += (d - jitter_ms_) * kJitterAlpha;
        stats_.avg_jitter_ms = jitter_ms_;
        if (jitter_ms_ > stats_.max_jitter_ms) stats_.max_jitter_ms = jitter_ms_;

        // 近期最大值：反映"最坏情况"，窗口约 50 帧（1 秒 @20ms）
        // 目标延迟必须覆盖它，否则会出现"包还在路上就被判迟到"。
        if (d > recent_max_dev_ms_) recent_max_dev_ms_ = d;
        if (++recent_count_ >= kMaxDevWindow) {
            recent_count_ = 0;
            // 窗口滚动：让最大值能随网络改善而下降
            recent_max_dev_ms_ *= 0.7;
        }
    }
    last_arrive_ms_ = arrive_ms;
    last_seq_ = seq;
    have_last_ = true;
}

void JitterBuffer::adapt_target_delay() {
    // 目标延迟的设定原则：**覆盖最坏抖动，而不是平均抖动**。
    //
    // 早期版本只用平滑抖动（jitter_ms_）的 3 倍作目标，结果：
    //   抖动 ±60ms 的场景下，目标仅 ~48ms，
    //   于是大量包在"该播的时刻"还没到达，被判为丢失并触发 PLC
    //   （实测：接受 215/500、PLC 333 次）。见 docs/02-pitfalls.md 坑 10。
    //
    // 修正：用"近期最大偏差"作为下界，保证目标延迟覆盖突发抖动。
    const double need_ms = std::max(jitter_ms_ * 2.0, recent_max_dev_ms_ * 1.5);
    int want = int(need_ms) + cfg_.frame_ms;

    // 上调要快（避免断音），下调要慢（避免抖动回来时又不够）
    if (want > cfg_.target_delay_ms) {
        cfg_.target_delay_ms = want;              // 立即上调
    } else if (want < cfg_.target_delay_ms && queue_.size() <= 2) {
        // 仅在网络明显改善且缓冲浅时才缓慢收窄
        cfg_.target_delay_ms = std::max(want, cfg_.target_delay_ms - kAdaptStepMs);
    }

    cfg_.target_delay_ms = std::max(cfg_.min_delay_ms,
                                    std::min(cfg_.max_delay_ms, cfg_.target_delay_ms));
    stats_.cur_target_ms = cfg_.target_delay_ms;

    // 起播门槛随目标延迟一起调整（见 ready_to_play）
    min_playout_frames_ = std::max(1, cfg_.target_delay_ms / cfg_.frame_ms / 2);
}

bool JitterBuffer::push(uint16_t seq, const uint8_t* payload, size_t len, int64_t arrive_ms) {
    if (arrive_ms < 0) arrive_ms = now_ms();

    // ---- 重复检测 ----
    for (const auto& e : queue_) {
        if (e.seq == seq) {
            stats_.dup++;
            LOGD(kTag, "duplicate seq=%u, dropped", seq);
            return false;
        }
    }

    // ---- 到达统计（重复包不计入）----
    arrived_distinct_++;
    if (!have_highest_) {
        highest_seq_ = lowest_seq_ = seq;
        have_highest_ = true;
    } else {
        if (seq_diff(seq, highest_seq_) > 0) highest_seq_ = seq;
        if (seq_diff(seq, lowest_seq_) < 0)  lowest_seq_ = seq;
    }

    // ---- 迟到判定 ----
    // 关键点：抖动缓冲的职责是"吸收乱序"，所以不能简单拒绝所有 seq < expected 的包。
    // 只有两种包该丢：
    //   1) 已经被播放过的序号（seq < play_seq_）——已经没有用
    //   2) 重复包（上面已处理）
    // 早期版本按 next_expected_seq_ 判断，导致先到的高序号包会"吃掉"
    // 后到的低序号包（乱序被误判为迟到），见 docs/02-pitfalls.md 坑 1。
    if (have_seq_ && seq_diff(seq, play_seq_) < 0) {
        stats_.late++;
        LOGD(kTag, "too late seq=%u (already played up to %u), dropped", seq, play_seq_);
        return false;
    }

    // ---- 首个包到达：初始化播放时钟 ----
    if (!have_seq_) {
        play_seq_ = seq;
        have_seq_ = true;
        // 等到目标延迟过去再开始播，避免一上来就 underflow
        play_deadline_ms_ = arrive_ms + cfg_.target_delay_ms;
    }

    // ---- 入队，按 seq 升序插入 ----
    Entry e{seq, arrive_ms, std::vector<uint8_t>(payload, payload + len)};
    auto it = std::upper_bound(queue_.begin(), queue_.end(), e,
                               [](const Entry& a, const Entry& b) {
                                   return seq_diff(a.seq, b.seq) < 0;
                               });
    queue_.insert(it, std::move(e));

    stats_.received++;
    update_jitter(arrive_ms, seq);
    adapt_target_delay();
    stats_.queue_depth = queue_.size();
    return true;
}

bool JitterBuffer::ready_to_play(int64_t now) const {
    if (playing_) return true;
    if (queue_.size() < min_playout_frames_) return false;   // 先攒够帧
    const int64_t t = (now < 0) ? now_ms() : now;
    // 且目标延迟已过（首帧到达后等待 target_delay，避免开始就 underflow）
    return t >= play_deadline_ms_;
}

bool JitterBuffer::pop(std::vector<uint8_t>* payload, int64_t now) {
    payload->clear();

    if (!have_seq_) return false;

    const int64_t t = (now < 0) ? now_ms() : now;

    // 还没到该播的时刻，等
    if (!playing_ && t < play_deadline_ms_) return false;

    // ---- 找 play_seq_ 对应的包 ----
    auto it = std::find_if(queue_.begin(), queue_.end(),
                           [this](const Entry& e) { return e.seq == play_seq_; });

    if (it == queue_.end()) {
        // 该序号不在队列里。判断是"还没到"还是"确定丢了"：
        //
        // 关键：不能一发现序号空洞就立刻判丢——那个包可能还在网络上。
        // 只有当"已经过了足够长时间"（超过目标延迟 + 一个帧长）才认定丢失。
        // 早期版本用过短的容忍窗口，把在途包误判为丢包，导致 PLC 激增。
        const int64_t wait_until = play_deadline_ms_ + cfg_.target_delay_ms + cfg_.frame_ms;

        if (t >= wait_until) {
            stats_.plc++;
            play_seq_ = uint16_t(play_seq_ + 1);
            play_deadline_ms_ += cfg_.frame_ms;
            playing_ = true;
            LOGD(kTag, "PLC for seq=%u (queue=%zu, waited %lld ms over deadline)",
                 uint16_t(play_seq_ - 1), queue_.size(), (long long)(t - play_deadline_ms_));
            return true;  // 空 payload = 要求调用方执行 PLC
        }
        return false;
    }

    // ---- 命中：取出并播放 ----
    playing_ = true;
    *payload = std::move(it->payload);
    queue_.erase(it);

    stats_.played++;
    stats_.queue_depth = queue_.size();
    play_seq_ = uint16_t(play_seq_ + 1);
    play_deadline_ms_ += cfg_.frame_ms;
    return true;
}

void JitterBuffer::advance_play_clock() {
    play_deadline_ms_ += cfg_.frame_ms;
}

void JitterBuffer::reset() {
    queue_.clear();
    have_seq_ = false;
    have_last_ = false;
    playing_ = false;
    jitter_ms_ = 0;
    recent_max_dev_ms_ = 0;
    recent_count_ = 0;
    play_seq_ = 0;
    play_deadline_ms_ = 0;
    highest_seq_ = lowest_seq_ = 0;
    have_highest_ = false;
    arrived_distinct_ = 0;
    stats_ = Stats{};
    stats_.cur_target_ms = cfg_.target_delay_ms;
}

JitterBuffer::Stats JitterBuffer::snapshot() const {
    Stats s = stats_;
    s.queue_depth = queue_.size();
    s.cur_target_ms = cfg_.target_delay_ms;

    // 丢包统计在快照时统一计算：
    //   序号跨度 = highest - lowest + 1
    //   丢包数   = 跨度 − 独立到达数（重复包已被排除）
    // 这样乱序到达不会被误算成丢包——乱序只是次序不同，不是丢失。
    if (have_highest_) {
        const uint64_t span = uint64_t(seq_diff(highest_seq_, lowest_seq_)) + 1;
        s.lost = (span > arrived_distinct_) ? (span - arrived_distinct_) : 0;
    } else {
        s.lost = 0;
    }
    return s;
}

}  // namespace rtc
