#pragma once
// 统一指标统计 —— 对应 WebRTC 的 getStats() / RTCP SR·RR
//
// 为什么需要（面试要点）：
//   1. **可观测性**：RTC 是长连接，问题（卡顿、回声、码率异常）往往在运行时才暴露，
//      必须能实时看到"哪一层出了问题"。
//   2. **驱动自适应**：拥塞控制需要丢包率/延迟梯度；NACK/FEC 需要丢包统计。
//      没有准确的指标，自适应就是瞎调。
//   3. **end-to-end 归因**：分段统计（采集/编码/网络/抖动缓冲/解码/播放）
//      才能定位"是网络丢包还是本地处理不过来"。
//
// WebRTC 的 getStats() 返回 RTCStatsReport，本模块给出等价的精简版。
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/util.h"

namespace rtc {

// 一次通话的完整指标快照
struct CallMetrics {
    // ---- 发送侧 ----
    uint64_t frames_captured = 0;      // 采集到的帧
    uint64_t frames_encoded = 0;       // 成功编码的帧
    uint64_t encode_errors = 0;
    uint64_t packets_sent = 0;         // 发出的 RTP 包
    uint64_t fec_packets_sent = 0;     // 发出的 FEC 冗余包
    uint64_t nack_requests = 0;        // 发出的 NACK 请求
    uint64_t retransmitted_packets = 0;// 重传的包
    uint64_t bytes_sent = 0;

    // ---- 接收侧 ----
    uint64_t packets_received = 0;
    // packets_lost = **检测到的丢包总数（重传/FEC 恢复之前的原始丢包数）**。
    // 注意语义：不是"最终仍缺失的包数"。
    // 之所以这样定义：恢复率 = 恢复数 / 总丢包数，
    // 如果 packets_lost 是"仍缺失"，分母就错了（实测出现过 8 丢包 8 恢复
    // 却算出 50% 恢复率的 bug）。见 docs/02-pitfalls.md 坑 32。
    uint64_t packets_lost = 0;
    uint64_t packets_recovered_nack = 0;
    uint64_t packets_recovered_fec = 0;
    double   loss_rate = 0;

    // ---- 抖动缓冲 ----
    uint64_t jb_received = 0;
    uint64_t jb_played = 0;
    uint64_t jb_plc = 0;               // PLC 补偿帧数
    uint64_t jb_dropped_late = 0;      // 太晚被丢弃
    double   jb_target_delay_ms = 0;
    double   jitter_ms = 0;

    // ---- 编解码质量 ----
    uint64_t frames_decoded = 0;
    uint64_t decode_errors = 0;

    // ---- 加密 ----
    uint64_t srtp_protected = 0;
    uint64_t srtp_unprotected = 0;
    uint64_t srtp_failures = 0;

    // ---- 网络自适应 ----
    int    target_bitrate_bps = 0;
    double delay_gradient_ms = 0;
    double rtt_ms = 0;

    // ---- ICE ----
    uint64_t ice_tx = 0;
    uint64_t ice_rx = 0;
    bool     ice_connected = false;

    int64_t  duration_ms = 0;

    // 派生指标（自动计算）
    int      actual_bitrate_bps() const;
    double   recovery_rate() const;      // 丢包恢复率 = 恢复数/总丢包数
    uint64_t still_missing() const;      // 最终仍缺失的包数（= 总丢包 - 恢复）
    double   playout_continuity() const; // 播放连续性
    double   encode_success_rate() const;

    std::string to_string() const;       // 人类可读
    std::string to_json() const;         // 机器可读（便于上报）
};

// 指标采集器：各模块在运行时把数据喂进来，导出统一快照。
class MetricsCollector {
public:
    MetricsCollector();

    // ---- 各层上报入口 ----
    void on_frame_captured();
    void on_frame_encoded(bool ok, size_t bytes);
    void on_packet_sent(size_t bytes, bool is_fec);
    void on_retransmit(size_t bytes);
    void on_nack_request(size_t count);

    void on_packet_received(size_t bytes);
    void on_packets_lost(uint64_t n);
    void on_recovered_nack(uint64_t n);
    void on_recovered_fec(uint64_t n);

    void on_jb_stats(uint64_t received, uint64_t played, uint64_t plc,
                     uint64_t dropped_late, double target_ms);
    void on_jitter(double ms);

    void on_frame_decoded(bool ok);
    void on_srtp(bool protect, bool ok);

    void on_congestion(int target_bps, double gradient_ms);
    void on_rtt(double ms);
    void on_ice(uint64_t tx, uint64_t rx, bool connected);

    // ---- 导出 ----
    // snapshot 的 now 参数语义同其他地方：-1 用真实时钟，>=0 用调用方给的逻辑时间。
    // 这样仿真测试能用逻辑时间驱动（否则 duration 会是真实时钟差，
    // 与测试注入的指标完全脱节——实测出现过 duration=3699778ms 的荒谬值）。
    // 见 docs/02-pitfalls.md 坑 28 与 32。
    CallMetrics snapshot(int64_t now = -1) const;
    void        reset();
    int64_t     elapsed_ms(int64_t now = -1) const;
    void        mark_start(int64_t now = -1);

    // 控制台打印（带分层的缩进与派生指标）
    std::string report(int64_t now = -1) const;

private:
    mutable std::mutex m_;
    CallMetrics        c_;
    int64_t            start_ms_ = 0;
};

}  // namespace rtc
