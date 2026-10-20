#include "net/loss_protection.h"

#include <algorithm>
#include <cstring>

#include "common/logging.h"

namespace rtc {

namespace {
constexpr const char* kTag = "loss";
constexpr const char* kTagFec = "fec";
constexpr const char* kTagCc = "gcc";
}  // namespace

// ==================================================================
// NackRequester
// ==================================================================
NackRequester::NackRequester() : NackRequester(Config{}) {}
NackRequester::NackRequester(const Config& cfg) : cfg_(cfg) {}

bool NackRequester::on_packet(uint16_t seq, int64_t now) {
    if (now < 0) now = now_ms();

    stats_.received++;

    // 重复包：已收到过
    if (received_set_.count(seq)) return false;

    // 太旧：超出窗口（说明这是个迟到很久的包，或序号回绕处理有误）
    if (have_seq_) {
        const int16_t diff = int16_t(seq - highest_seq_);
        if (diff < -cfg_.max_packets_in_window) {
            stats_.too_old++;
            return false;
        }
    }

    received_set_.insert(seq);

    if (!have_seq_) {
        have_seq_ = true;
        highest_seq_ = seq;
        span_ = 1;
        arrived_ = 1;
        return true;
    }

    const int16_t diff = int16_t(seq - highest_seq_);
    if (diff > 0) {
        // 序号前进：中间可能缺包 → 登记为 pending
        for (int16_t s = int16_t(highest_seq_ + 1); s != seq; s++) {
            Pending p;
            p.seq = uint16_t(s);
            p.first_missing_ms = now;
            p.last_request_ms = 0;
            p.retries = 0;
            if (!received_set_.count(p.seq)) pending_[p.seq] = p;
        }
        highest_seq_ = seq;
        span_ += uint64_t(diff);
        arrived_++;
    } else {
        // 乱序到达：这个包之前在 pending 里，现在补上了
        auto it = pending_.find(seq);
        if (it != pending_.end()) {
            pending_.erase(it);
            stats_.recovered++;   // 通过乱序到达"恢复"
            LOGD(kTag, "late-but-useful seq=%u recovered by reordering", seq);
        }
    }

    // 清理太旧的 pending
    // 注意：map 的 key 就是序号，Pending::seq 只是副本 —— 这里用 key 更直接。
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now - it->second.first_missing_ms > cfg_.max_nack_age_ms) {
            stats_.given_up++;
            LOGD(kTag, "give up seq=%u (age %lld ms), rely on PLC",
                 it->first, (long long)(now - it->second.first_missing_ms));
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }

    stats_.lost_detected = uint64_t(pending_.size()) +
                           stats_.recovered + stats_.given_up;
    if (span_ > 0) {
        const uint64_t lost_now = (span_ > arrived_) ? (span_ - arrived_) : 0;
        stats_.loss_rate = double(lost_now) / double(span_);
    }
    return true;
}

std::vector<uint16_t> NackRequester::take_nack_list(int64_t now) {
    if (now < 0) now = now_ms();
    std::vector<uint16_t> out;

    for (auto& kv : pending_) {
        Pending& p = kv.second;
        if (p.retries >= cfg_.max_nack_retries) continue;
        if (p.last_request_ms != 0 &&
            (now - p.last_request_ms) < cfg_.retry_interval_ms) {
            continue;   // 还没到重试间隔
        }
        out.push_back(p.seq);
        p.last_request_ms = now;
        p.retries++;
    }

    if (!out.empty()) {
        stats_.nack_sent += out.size();
        LOGD(kTag, "NACK request: %zu seq(s)", out.size());
    }
    return out;
}

NackRequester::Stats NackRequester::snapshot() const {
    Stats s = stats_;
    const uint64_t lost_now = (span_ > arrived_) ? (span_ - arrived_) : 0;
    s.loss_rate = span_ ? double(lost_now) / double(span_) : 0;
    return s;
}

void NackRequester::reset() {
    have_seq_ = false;
    highest_seq_ = 0;
    arrived_ = 0;
    span_ = 0;
    pending_.clear();
    received_set_.clear();
    stats_ = Stats{};
}

// ==================================================================
// FecEncoder —— XOR FEC（(k, 1) 方案）
// ==================================================================
FecEncoder::FecEncoder() : FecEncoder(Config{}) {}
FecEncoder::FecEncoder(const Config& cfg) : cfg_(cfg) {
    if (cfg_.group_size < 1) cfg_.group_size = 1;
    group_payloads_.reserve(size_t(cfg_.group_size));
}

void FecEncoder::set_group_size(int k) {
    if (k < 1) k = 1;
    if (k > 16) k = 16;
    cfg_.group_size = k;
}

bool FecEncoder::add_source(uint16_t seq, const uint8_t* payload, size_t len,
                            std::vector<uint8_t>* fec_out) {
    if (count_in_group_ == 0) group_base_seq_ = seq;

    group_payloads_.emplace_back(payload, payload + len);
    count_in_group_++;

    if (count_in_group_ < cfg_.group_size) return false;

    // 组满：生成 XOR 冗余包
    size_t max_len = 0;
    for (const auto& p : group_payloads_) max_len = std::max(max_len, p.size());

    // 格式：[u16 base_seq][u16 count][u16 各源包长度...][XOR payload]
    //
    // ★ 长度字段必须是 u16，不能是 u8。
    //   60ms 聚合包在 48kbps 时用户载荷约 360 字节 > 255，
    //   用 u8 会被**静默截断**，接收端按错误长度切分 → FEC 恢复出垃圾数据。
    //   低码率（16kbps 约 120 字节）下不会暴露，所以早期测试没抓到。
    //   见 docs/02-pitfalls.md 坑 40。
    const size_t hdr = 2 + 2 + 2 * size_t(count_in_group_);
    std::vector<uint8_t> xored(max_len, 0);
    for (size_t i = 0; i < group_payloads_.size(); i++) {
        for (size_t b = 0; b < group_payloads_[i].size(); b++) {
            xored[b] ^= group_payloads_[i][b];
        }
    }

    fec_out->resize(hdr + max_len);
    put_u16_le(fec_out->data(), group_base_seq_);
    put_u16_le(fec_out->data() + 2, uint16_t(count_in_group_));
    for (size_t i = 0; i < group_payloads_.size(); i++) {
        put_u16_le(fec_out->data() + 4 + 2 * i, uint16_t(group_payloads_[i].size()));
    }
    memcpy(fec_out->data() + hdr, xored.data(), max_len);

    LOGD(kTagFec, "generated FEC: base=%u count=%zu payload=%zu bytes",
         group_base_seq_, group_payloads_.size(), max_len);

    // 复位，准备下一组
    group_payloads_.clear();
    count_in_group_ = 0;
    return true;
}

// ==================================================================
// FecDecoder
// ==================================================================
void FecDecoder::on_source(uint16_t seq, const uint8_t* payload, size_t len) {
    src_[seq].assign(payload, payload + len);
    recovered_flags_.erase(seq);
}

void FecDecoder::on_fec(const uint8_t* fec_pkt, size_t len,
                        std::vector<std::pair<uint16_t, std::vector<uint8_t>>>* recovered_out) {
    if (len < 4) { stats_.unusable++; return; }
    stats_.fec_received++;

    const uint16_t base_seq = get_u16_le(fec_pkt);
    const uint16_t count    = get_u16_le(fec_pkt + 2);
    if (count == 0 || count > 16 || len < 4 + size_t(count)) {
        stats_.unusable++;
        return;
    }

    // 读出各源包长度，算出 XOR 载荷长度
    size_t max_len = 0;
    for (size_t i = 0; i < count; i++) {
        max_len = std::max<size_t>(max_len, get_u16_le(fec_pkt + 4 + 2 * i));
    }
    const size_t hdr = 4 + 2 * size_t(count);
    if (len < hdr + max_len) { stats_.unusable++; return; }

    // 统计组内已有哪些源包、缺哪些
    std::vector<uint16_t> missing;
    for (size_t i = 0; i < count; i++) {
        const uint16_t s = uint16_t(base_seq + i);
        if (src_.count(s) == 0) missing.push_back(s);
    }

    // 只缺 1 个 → 可以恢复（XOR 可逆）
    if (missing.size() == 1) {
        std::vector<uint8_t> rec(fec_pkt + hdr, fec_pkt + hdr + max_len);
        // 把组内其他已知包异或回去
        for (size_t i = 0; i < count; i++) {
            const uint16_t s = uint16_t(base_seq + i);
            if (s == missing[0]) continue;
            auto it = src_.find(s);
            if (it == src_.end()) continue;
            for (size_t b = 0; b < it->second.size(); b++) rec[b] ^= it->second[b];
        }
        // 按该包原始长度截断
        const uint16_t need_len = get_u16_le(fec_pkt + 4 + 2 * size_t(missing[0] - base_seq));
        if (rec.size() > need_len) rec.resize(need_len);

        src_[missing[0]] = rec;
        recovered_flags_.insert(missing[0]);
        stats_.recovered++;
        recovered_out->emplace_back(missing[0], rec);
        LOGI(kTagFec, "recovered seq=%u via XOR FEC (%zu bytes)",
             missing[0], rec.size());
    } else if (missing.empty()) {
        LOGD(kTagFec, "FEC not needed (all %u source packets present)", count);
    } else {
        // 缺 ≥2 个：单个 XOR 包无法恢复
        stats_.unusable++;
        LOGD(kTagFec, "FEC cannot recover: %zu packets missing (XOR only handles 1)",
             missing.size());
    }
}

bool FecDecoder::was_recovered(uint16_t seq) const {
    return recovered_flags_.count(seq) != 0;
}

void FecDecoder::clear_recovered(uint16_t seq) {
    recovered_flags_.erase(seq);
}

void FecDecoder::reset() {
    src_.clear();
    recovered_flags_.clear();
    stats_ = Stats{};
}

// ==================================================================
// CongestionController —— 延迟梯度 + AIMD
// ==================================================================
CongestionController::CongestionController() : CongestionController(Config{}) {}
CongestionController::CongestionController(const Config& cfg) : cfg_(cfg) {
    target_bps_ = cfg_.start_bitrate_bps;
    // 注意：这里**不能**用 now_ms() 初始化 last_update_ms_。
    // 保持 0，由第一次 update_bitrate(now) 惰性设置基准，
    // 这样真实时钟与逻辑时间两种调用方式都能正确工作。见坑 28。
    last_update_ms_ = 0;
    stats_.target_bps = target_bps_;
}

const char* CongestionController::state_name(State s) {
    switch (s) {
        case State::kNormal:  return "NORMAL";
        case State::kOveruse: return "OVERUSE";
        case State::kUnderuse:return "UNDERUSE";
    }
    return "?";
}

void CongestionController::on_arrival(int64_t send_ms, int64_t recv_ms) {
    // 延迟梯度 d(i) = (recv_i - recv_0) - (send_i - send_0)
    // 物理含义：这段时间内累积的排队延迟。
    //   > 0 且持续增长 → 链路在排队 → 有拥塞风险（overuse）
    //   < 0             → 队列在清空（underuse）
    if (!have_prev_) {
        base_send_ms_ = send_ms;
        base_recv_ms_ = recv_ms;
        have_prev_ = true;
        prev_send_ms_ = send_ms;
        prev_recv_ms_ = recv_ms;
        return;
    }

    // 相对到达时间 - 相对发送时间
    const int64_t rel_recv = recv_ms - base_recv_ms_;
    const int64_t rel_send = send_ms - base_send_ms_;
    const double d = double(rel_recv - rel_send);

    // 指数平滑，抑制单包噪声
    trend_ = 0.9 * trend_ + 0.1 * d;

    prev_send_ms_ = send_ms;
    prev_recv_ms_ = recv_ms;
    stats_.delay_gradient_ms = trend_;
}

void CongestionController::on_loss_rate(double loss) {
    loss_rate_ = loss;
    stats_.loss_rate = loss;
}

int CongestionController::update_bitrate(int64_t now) {
    if (now < 0) now = now_ms();

    // ★ 惰性初始化节流基准，而**不是**在构造函数里用 now_ms()。
    //
    // 早期版本在构造函数里写 `last_update_ms_ = now_ms();`，
    // 于是基准是真实时钟（如 500000），而测试用逻辑时间（0、20、40…）驱动，
    // `now - last_update_ms_` 恒为负 → 永远 < 100ms → **所有更新都被节流掉**。
    // 表现为"延迟梯度已累积到 320ms，但码率纹丝不动"。
    // 这是坑 9 / 坑 28 的变体：**部分**依赖真实时钟比完全依赖更难发现。
    // 见 docs/02-pitfalls.md 坑 28。
    if (last_update_ms_ == 0) {
        last_update_ms_ = now;
        stats_.target_bps = target_bps_;
        return target_bps_;
    }

    // 每 ~100ms 更新一次（近似一个 RTT 的反馈周期）
    if (now - last_update_ms_ < 100) return target_bps_;
    last_update_ms_ = now;

    // ---- 判定链路状态 ----
    // 阈值：延迟梯度超过 20ms 认为在排队（经验值，可按实测调整）
    const double kOveruseThresholdMs = 20.0;
    State new_state = State::kNormal;
    if (trend_ > kOveruseThresholdMs) {
        new_state = State::kOveruse;
    } else if (trend_ < -kOveruseThresholdMs) {
        new_state = State::kUnderuse;
    }

    if (new_state != state_) {
        LOGI(kTagCc, "state %s -> %s (gradient=%.1fms, loss=%.1f%%)",
             state_name(state_), state_name(new_state),
             trend_, loss_rate_ * 100.0);
        state_ = new_state;
    }

    // ---- AIMD 调节 ----
    int    new_bps = target_bps_;
    const bool loss_trigger = (loss_rate_ > 0.10);   // 丢包 10% 作为兜底信号

    if (state_ == State::kOveruse || loss_trigger) {
        // 乘性降（快速退让）
        new_bps = int(double(target_bps_) * cfg_.decrease_factor);
        if (state_ == State::kOveruse) stats_.overuse_events++;
        stats_.decrease_events++;
        // 降完之后把梯度基准重置，避免持续触发
        trend_ = 0;
        LOGI(kTagCc, "bitrate DOWN %d -> %d bps (gradient=%.1fms, loss=%.1f%%)",
             target_bps_, new_bps, stats_.delay_gradient_ms, loss_rate_ * 100.0);
    } else if (state_ == State::kUnderuse) {
        // 链路空闲：不立即升（避免抖动回来时又过载）
        stats_.underuse_events++;
    } else {
        // 正常：加性升（缓慢试探）
        new_bps = target_bps_ + int(double(target_bps_) * cfg_.increase_factor);
        stats_.increase_events++;
        LOGD(kTagCc, "bitrate UP %d -> %d bps (gradient=%.1fms)",
             target_bps_, new_bps, trend_);
    }

    // 限幅
    new_bps = std::max(cfg_.min_bitrate_bps, std::min(cfg_.max_bitrate_bps, new_bps));
    target_bps_ = new_bps;
    stats_.target_bps = new_bps;
    return target_bps_;
}

CongestionController::Stats CongestionController::snapshot() const {
    Stats s = stats_;
    s.target_bps = target_bps_;
    s.state = state_;
    s.delay_gradient_ms = trend_;
    s.loss_rate = loss_rate_;
    return s;
}

void CongestionController::reset() {
    target_bps_ = cfg_.start_bitrate_bps;
    trend_ = 0;
    have_prev_ = false;
    loss_rate_ = 0;
    state_ = State::kNormal;
    last_update_ms_ = 0;   // 与构造函数一致：留待首次调用惰性设置
    stats_ = Stats{};
    stats_.target_bps = target_bps_;
}

}  // namespace rtc
