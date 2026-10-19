#pragma once
// 弱网对抗模块 —— 对应 WebRTC 的 NACK 重传与 ULPFEC 冗余
//
// 为什么要两种机制（面试高频问题）：
//   NACK（ARQ 重传）：接收端发现序号空洞就请求重传。
//     优点：省带宽（只在真丢包时才重传）
//     缺点：要等一个 RTT，低延迟通话可能已过期
//   FEC（前向纠错）：发送端主动多发冗余包。
//     优点：无额外延迟（不等 RTT）
//     缺点：即使没丢包也占带宽
//
// 工程上的选择：**按丢包率动态调 FEC 冗余率**，而不是全开或全关。
//   <5%   丢包：主要靠 NACK（FEC 冗余降到很低）
//   5~15% 丢包：开 FEC，冗余率随丢包率上升
//   >20%  丢包：冗余和重传都赶不上，应降码率/降质量
//
// 本模块实现：
//   1. NackRequester    —— 接收端：检测丢包、生成 NACK 请求、处理重传、超时放弃
//   2. FecEncoder/Decoder —— 发送端：异或冗余（XOR FEC，ULPFEC 的简化版）
//   3. 丢包统计          —— 供拥塞控制与指标上报使用
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "common/util.h"

namespace rtc {

// ------------------------------------------------------------------
// 接收端：NACK 重传请求
// ------------------------------------------------------------------
class NackRequester {
public:
    struct Config {
        int  max_packets_in_window = 2048;  // 序号窗口（判断"太旧"）
        int  max_nack_retries      = 10;    // 同一包最多请求几次
        int  retry_interval_ms     = 40;    // 重试间隔（约 1 个 RTT）
        int  max_nack_age_ms       = 300;   // 超过此时限放弃（已经太晚，不如 PLC）
    };

    struct Stats {
        uint64_t received = 0;
        uint64_t lost_detected = 0;      // 检测到的丢包总数
        uint64_t nack_sent = 0;          // 发出的 NACK 请求数
        uint64_t recovered = 0;          // 通过重传恢复的包数
        uint64_t given_up = 0;           // 放弃等待的包数
        uint64_t too_old = 0;            // 到达太晚被丢弃的包
        double   loss_rate = 0;          // 丢包率
    };

    explicit NackRequester(const Config& cfg);
    NackRequester();   // 使用默认 Config

    // 收到一个媒体包。seq 为序列号，now 为当前时间（-1 用真实时钟）。
    // 返回 true 表示这是一个"新"包（可用于统计），false 表示重复/太旧。
    bool on_packet(uint16_t seq, int64_t now = -1);

    // 取出当前应该发送的 NACK 请求（需要重传的序号列表）。
    // 调用方负责把这份列表发给发送端。清空后再次调用会返回下一批（到重试间隔时）。
    std::vector<uint16_t> take_nack_list(int64_t now = -1);

    Stats snapshot() const;
    void  reset();

private:
    struct Pending {
        uint16_t seq;
        int64_t  first_missing_ms;
        int64_t  last_request_ms;
        int      retries;
    };

    Config cfg_;
    bool     have_seq_ = false;
    uint16_t highest_seq_ = 0;
    uint64_t arrived_ = 0;           // 独立到达包数
    uint64_t span_ = 0;              // 序号跨度

    std::map<uint16_t, Pending> pending_;   // 缺失中的包
    std::set<uint16_t>          received_set_;   // 收到的序号（用于辨识重复）

    Stats stats_;
};

// ------------------------------------------------------------------
// 发送端：XOR FEC（前向纠错）
//
// 原理：把 k 个源包做异或，生成 m 个冗余包。
// 接收端丢了其中任意 ≤ m 个，都能通过解异或方程恢复。
//
// 本实现用最经典的 (k=4, m=1)：每 4 个源包生成 1 个冗余包，
// 能恢复任意 1 个丢失。冗余率 25%——这是"防单点丢包"的常见配置。
//
// 与 WebRTC 的 ULPFEC 对比：
//   ULPFEC 用更复杂的矩阵（不规则保护级别），能应对连续丢包；
//   本实现是"均匀 XOR FEC"，实现简单、能防单点丢包，是它的简化版。
// ------------------------------------------------------------------
class FecEncoder {
public:
    struct Config {
        int group_size = 4;    // k：每多少个源包一组
        int overhead   = 1;    // m：每组生成多少个冗余包
    };

    explicit FecEncoder(const Config& cfg);
    FecEncoder();   // 使用默认 Config

    // 提交一个源包。若达到 group_size，返回 true 并生成一个冗余包到 fec_out。
    // 冗余包格式：[u16 group_base_seq][u16 group_count][payload]
    // payload 为组内各源包载荷的异或结果（长度取组内最大值，短包补零）
    bool add_source(uint16_t seq, const uint8_t* payload, size_t len,
                    std::vector<uint8_t>* fec_out);

    int group_size() const { return cfg_.group_size; }

    // 动态调整冗余率（拥塞控制可调用）
    void set_group_size(int k);

private:
    Config cfg_;
    uint16_t            group_base_seq_ = 0;
    std::vector<std::vector<uint8_t>> group_payloads_;
    int                 count_in_group_ = 0;
};

// 接收端：FEC 恢复
class FecDecoder {
public:
    struct Stats {
        uint64_t fec_received = 0;   // 收到的冗余包
        uint64_t recovered = 0;      // 通过 FEC 恢复的源包
        uint64_t unusable = 0;       // 无法使用的冗余包（缺太多）
    };

    // 记录收到的源包（用于后续恢复）
    void on_source(uint16_t seq, const uint8_t* payload, size_t len);

    // 处理一个 FEC 冗余包。若能恢复，把恢复出的源包放入 recovered_out。
    // recovered_out 中每项为 pair<seq, payload>。
    void on_fec(const uint8_t* fec_pkt, size_t len,
                std::vector<std::pair<uint16_t, std::vector<uint8_t>>>* recovered_out);

    // 判断某序号是否已通过 FEC 恢复（供上层避免重复处理）
    bool was_recovered(uint16_t seq) const;
    void clear_recovered(uint16_t seq);

    const Stats& stats() const { return stats_; }
    void reset();

private:
    std::map<uint16_t, std::vector<uint8_t>> src_;   // 已知源包
    std::set<uint16_t>                       recovered_flags_;
    Stats stats_;
};

// ------------------------------------------------------------------
// 拥塞控制：延迟梯度 + AIMD（对应 WebRTC 的 GCC）
//
// 原理（必背）：
//   接收端统计"到达间隔的延迟梯度"（排队延迟的变化趋势），
//   发送端据此判断链路是否过载，用 AIMD 调节目标码率：
//     overuse    -> 乘性降（×0.85）
//     underuse   -> 不立即升
//     normal     -> 加性升（+8%/RTT）
// 另用丢包率做兜底（事后信号）。
// ------------------------------------------------------------------
class CongestionController {
public:
    struct Config {
        int start_bitrate_bps = 16000;
        int min_bitrate_bps   = 6000;
        int max_bitrate_bps   = 64000;
        double decrease_factor = 0.85;   // 乘性降
        double increase_factor = 0.08;   // 加性升
    };

    enum class State { kNormal, kOveruse, kUnderuse };

    struct Stats {
        int    target_bps = 0;
        double delay_gradient_ms = 0;   // 延迟梯度（正=排队在累积）
        double loss_rate = 0;
        State  state = State::kNormal;
        uint64_t overuse_events = 0;
        uint64_t underuse_events = 0;
        uint64_t increase_events = 0;
        uint64_t decrease_events = 0;
    };

    explicit CongestionController(const Config& cfg);
    CongestionController();   // 使用默认 Config

    // 上报一个到达包（用于延迟梯度估计）。
    // send_ms：发送时刻（或发送端时间戳）
    // recv_ms：接收时刻
    // 注意：延迟梯度用"到达间隔 - 发送间隔"的变化来算，
    //       正值说明排队延迟在增长（链路开始拥塞）。
    void on_arrival(int64_t send_ms, int64_t recv_ms);

    // 上报丢包率（0~1），用于兜底
    void on_loss_rate(double loss);

    // 到达反馈周期（一个 RTT）时调用，产出新的目标码率。
    // now 参数语义同 NackRequester：-1 用真实时钟，>=0 用调用方给的逻辑时间。
    //
    // ★ 为什么必须有这个参数：早期版本内部用 now_ms() 做 100ms 节流，
    //   结果仿真测试用逻辑时间驱动时，该节流永远不满足 → 码率永不更新。
    //   这与 Phase 1 抖动缓冲的坑 9 是同一类问题（组件耦合真实时钟 ⇒ 不可测）。
    //   见 docs/02-pitfalls.md 坑 9 与 28。
    int update_bitrate(int64_t now = -1);

    int   target_bitrate() const { return target_bps_; }
    Stats snapshot() const;
    void  reset();

    static const char* state_name(State s);

private:
    Config cfg_;
    int    target_bps_ = 0;

    // 延迟梯度估计（最小二乘趋势线，对应 WebRTC 的 Trendline）
    double trend_ = 0;              // 累积的延迟梯度
    bool   have_prev_ = false;
    int64_t prev_send_ms_ = 0;
    int64_t prev_recv_ms_ = 0;
    int64_t base_send_ms_ = 0;
    int64_t base_recv_ms_ = 0;

    double loss_rate_ = 0;
    State  state_ = State::kNormal;
    int64_t last_update_ms_ = 0;
    Stats  stats_;
};

}  // namespace rtc
