#pragma once
// 抖动缓冲（Jitter Buffer）—— 对应 WebRTC 的 NetEq
//
// 要解决的问题：
//   网络包到达的时间间隔不均匀（抖动），但播放设备必须按固定节奏消费 PCM。
//   抖动缓冲的作用是"吸收抖动"：把乱序/忽快忽慢到达的包重新排队，
//   按播放时钟匀速吐出。
//
// 核心权衡（本项目的关键设计点）：
//   缓冲太小 -> 包还没到就该播了 -> 断音（underflow / 卡顿）
//   缓冲太大 -> 端到端延迟变大
//   所以需要"自适应"：抖动大就加深缓冲，网络稳定就收窄。
//
// 与 NetEq 的差异（诚实对照）：
//   NetEq 做 sample 级时间拉伸（accelerate / preemptive expand）
//   本项目做帧级粗调（调整目标延迟 + 丢包隐藏 PLC），更简单但同类问题
#include <cstdint>
#include <deque>
#include <vector>

#include "common/util.h"

namespace rtc {

class JitterBuffer {
public:
    struct Config {
        int target_delay_ms = 60;   // 初始目标延迟
        int min_delay_ms    = 20;   // 下限（低于此无法吸收抖动）
        int max_delay_ms    = 200;  // 上限（超过此延迟不可接受）
        int frame_ms        = 20;   // 音频帧长
    };

    JitterBuffer();                      // 使用默认 Config
    explicit JitterBuffer(const Config& cfg);

    struct Stats {
        uint64_t received = 0;      // 收到的包
        uint64_t played = 0;        // 播放的帧
        uint64_t lost = 0;          // 检测到丢失的包
        uint64_t plc = 0;           // 触发 PLC 的次数
        uint64_t late = 0;          // 太晚到达被丢弃的包
        uint64_t dup = 0;           // 重复包
        double   cur_target_ms = 0; // 当前目标延迟
        double   avg_jitter_ms = 0; // 平均抖动
        double   max_jitter_ms = 0;
        size_t   queue_depth = 0;
    };

    // 收到一个包。seq 为 RTP 风格序列号（16bit，可回绕），
    // payload 为 Opus 数据。arrive_ms 为到达时间（默认取 now_ms）。
    // 返回：true=接受入队  false=丢弃（重复/太晚）
    bool push(uint16_t seq, const uint8_t* payload, size_t len, int64_t arrive_ms = -1);

    // 按播放时钟取下一个要解码的帧。
    // - 返回 true 并填充 payload：取到数据
    // - 返回 true 且 payload 为空（len=0）：应该执行 PLC（丢包隐藏）
    // - 返回 false：缓冲为空且未到该播的时刻（还没必要 PLC）
    //
    // now 参数：当前时间（-1 表示取真实 now_ms()）。
    // 之所以把它做成参数，是因为内部直接读真实时钟会让这个组件**无法用逻辑时间测试**——
    // 弱网仿真必须能"快进"时间，否则跑 10 秒仿真就要真的等 10 秒。见 docs/02-pitfalls.md 坑 9。
    bool pop(std::vector<uint8_t>* payload, int64_t now = -1);

    // 当前是否应该开始播放（缓冲达到目标水位）。now 语义同上。
    bool ready_to_play(int64_t now = -1) const;

    // 推进播放时钟（每消费一帧调一次）
    void advance_play_clock();

    const Stats& stats() const { return stats_; }
    void  reset();
    size_t depth() const { return queue_.size(); }

    // 统计数据快照（便于上报）
    Stats snapshot() const;

private:
    // 一个已入队的包
    struct Entry {
        uint16_t             seq;
        int64_t              arrive_ms;
        std::vector<uint8_t> payload;
    };

    void update_jitter(int64_t arrive_ms, uint16_t seq);
    void adapt_target_delay();
    static int16_t seq_diff(uint16_t a, uint16_t b);  // a - b，处理回绕

    Config cfg_;
    std::deque<Entry> queue_;   // 按 seq 排序的待播放队列
    uint16_t play_seq_ = 0;            // 下一个要播放的序号（播放进度）
    bool     have_seq_ = false;

    // 丢包统计：记录"到达过的序号范围"与"到达的独立包数"，
    // 丢包数 = 范围跨度 − 独立到达数，在 snapshot() 时统一计算。
    // 这样不会因乱序到达而误判——**乱序不是丢包**。
    // 早期实现按到达顺序累加 gap，重抖动场景下把大量乱序包算成了丢包
    // （实测 500 个包报出 245 个丢包）。见 docs/02-pitfalls.md 坑 11。
    uint16_t highest_seq_ = 0;
    uint16_t lowest_seq_ = 0;
    bool     have_highest_ = false;
    uint64_t arrived_distinct_ = 0;   // 到达过的独立包数（不含重复包）

    int64_t  play_deadline_ms_ = 0;  // 下一帧应该被消费的时刻
    bool     playing_ = false;
    // 起播门槛：缓冲里至少要攒够这么多帧才开始播。
    // 只判断"目标延迟已过"是不够的——如果包还在途中，依然会立刻 underflow。
    size_t   min_playout_frames_ = 3;

    // 抖动估计（RFC 3550 风格）
    double   jitter_ms_ = 0;   // 指数平滑的抖动
    int64_t  last_arrive_ms_ = 0;
    uint16_t last_seq_ = 0;
    bool     have_last_ = false;

    // 近期"最大"到达间隔偏差 —— 目标延迟必须覆盖最坏情况，
    // 只用平滑抖动会低估突发抖动，导致包在途中就被判迟到。见 docs/02-pitfalls.md 坑 10。
    double   recent_max_dev_ms_ = 0;
    int      recent_count_ = 0;

    Stats    stats_;
};

}  // namespace rtc
