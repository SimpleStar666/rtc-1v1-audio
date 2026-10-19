#include "metrics/metrics.h"

#include <cstdio>

#include "common/logging.h"

namespace rtc {

namespace {
constexpr const char* kTag = "metrics";
}

// ---------------- 派生指标 ----------------
int CallMetrics::actual_bitrate_bps() const {
    if (duration_ms <= 0) return 0;
    return int(double(bytes_sent) * 8.0 * 1000.0 / double(duration_ms));
}

double CallMetrics::recovery_rate() const {
    // 恢复率 = 已恢复的包数 / 检测到的丢包总数
    //
    // ★ 分母**只**用 packets_lost（原始检测丢包），不能再加上 recovered。
    //   早期实现写成 `lost + recovered`，导致"8 丢包 8 恢复"算出 50%：
    //     8 / (8 + 8) = 0.5     ← 错误
    //   正确应为 8 / 8 = 1.0。
    //   这是"统计口径"类错误的又一例（对比坑 11）。
    //   见 docs/02-pitfalls.md 坑 32。
    if (packets_lost == 0) return 1.0;   // 无丢包视为 100%
    return double(packets_recovered_nack + packets_recovered_fec) / double(packets_lost);
}

uint64_t CallMetrics::still_missing() const {
    const uint64_t recovered = packets_recovered_nack + packets_recovered_fec;
    return packets_lost > recovered ? packets_lost - recovered : 0;
}

double CallMetrics::playout_continuity() const {
    if (jb_played + jb_plc == 0) return 1.0;
    return double(jb_played) / double(jb_played + jb_plc);
}

double CallMetrics::encode_success_rate() const {
    const uint64_t total = frames_encoded + encode_errors;
    if (total == 0) return 1.0;
    return double(frames_encoded) / double(total);
}

std::string CallMetrics::to_string() const {
    char buf[3072];
    snprintf(buf, sizeof(buf),
        "======== 通话指标（%lld ms）========\n"
        "  [发送侧]\n"
        "    采集帧数        : %llu\n"
        "    编码成功/错误   : %llu / %llu  (成功率 %.1f%%)\n"
        "    RTP 包/FEC 包   : %llu / %llu\n"
        "    发送字节        : %llu  (实测码率 %d bps)\n"
        "    NACK 请求/重传  : %llu / %llu\n"
        "  [接收侧]\n"
        "    接收包数        : %llu\n"
        "    检测丢包        : %llu  (丢包率 %.2f%%)\n"
        "    恢复(NACK/FEC)  : %llu / %llu  (恢复率 %.1f%%)\n"
        "  [抖动缓冲]\n"
        "    接收/播放帧     : %llu / %llu\n"
        "    PLC 补偿        : %llu  (播放连续性 %.1f%%)\n"
        "    迟到丢弃        : %llu\n"
        "    目标延迟/抖动   : %.1f ms / %.2f ms\n"
        "  [编解码]\n"
        "    解码成功/错误   : %llu / %llu\n"
        "  [加密]\n"
        "    SRTP 加密/解密  : %llu / %llu  (失败 %llu)\n"
        "  [网络自适应]\n"
        "    目标码率        : %d bps\n"
        "    延迟梯度/RTT    : %.1f ms / %.1f ms\n"
        "  [ICE]\n"
        "    通道收发/状态   : %llu / %llu  (%s)\n"
        "====================================\n",
        (long long)duration_ms,
        (unsigned long long)frames_captured,
        (unsigned long long)frames_encoded, (unsigned long long)encode_errors,
        encode_success_rate() * 100.0,
        (unsigned long long)packets_sent, (unsigned long long)fec_packets_sent,
        (unsigned long long)bytes_sent, actual_bitrate_bps(),
        (unsigned long long)nack_requests, (unsigned long long)retransmitted_packets,
        (unsigned long long)packets_received,
        (unsigned long long)packets_lost, loss_rate * 100.0,
        (unsigned long long)packets_recovered_nack,
        (unsigned long long)packets_recovered_fec,
        recovery_rate() * 100.0,
        (unsigned long long)jb_received, (unsigned long long)jb_played,
        (unsigned long long)jb_plc, playout_continuity() * 100.0,
        (unsigned long long)jb_dropped_late,
        jb_target_delay_ms, jitter_ms,
        (unsigned long long)frames_decoded, (unsigned long long)decode_errors,
        (unsigned long long)srtp_protected, (unsigned long long)srtp_unprotected,
        (unsigned long long)srtp_failures,
        target_bitrate_bps, delay_gradient_ms, rtt_ms,
        (unsigned long long)ice_tx, (unsigned long long)ice_rx,
        ice_connected ? "已连接" : "未连接");
    return std::string(buf);
}

std::string CallMetrics::to_json() const {
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{\"duration_ms\":%lld,"
        "\"frames_captured\":%llu,\"frames_encoded\":%llu,\"encode_errors\":%llu,"
        "\"packets_sent\":%llu,\"fec_packets_sent\":%llu,\"bytes_sent\":%llu,"
        "\"bitrate_bps\":%d,\"nack_requests\":%llu,\"retransmitted\":%llu,"
        "\"packets_received\":%llu,\"packets_lost\":%llu,\"loss_rate\":%.4f,"
        "\"recovered_nack\":%llu,\"recovered_fec\":%llu,\"recovery_rate\":%.4f,"
        "\"jb_received\":%llu,\"jb_played\":%llu,\"jb_plc\":%llu,"
        "\"jb_dropped_late\":%llu,\"jb_target_delay_ms\":%.1f,\"jitter_ms\":%.2f,"
        "\"playout_continuity\":%.4f,"
        "\"frames_decoded\":%llu,\"decode_errors\":%llu,"
        "\"srtp_protected\":%llu,\"srtp_unprotected\":%llu,\"srtp_failures\":%llu,"
        "\"target_bitrate_bps\":%d,\"delay_gradient_ms\":%.1f,\"rtt_ms\":%.1f,"
        "\"ice_tx\":%llu,\"ice_rx\":%llu,\"ice_connected\":%s}",
        (long long)duration_ms,
        (unsigned long long)frames_captured,
        (unsigned long long)frames_encoded,
        (unsigned long long)encode_errors,
        (unsigned long long)packets_sent,
        (unsigned long long)fec_packets_sent,
        (unsigned long long)bytes_sent,
        actual_bitrate_bps(),
        (unsigned long long)nack_requests,
        (unsigned long long)retransmitted_packets,
        (unsigned long long)packets_received,
        (unsigned long long)packets_lost,
        loss_rate,
        (unsigned long long)packets_recovered_nack,
        (unsigned long long)packets_recovered_fec,
        recovery_rate(),
        (unsigned long long)jb_received,
        (unsigned long long)jb_played,
        (unsigned long long)jb_plc,
        (unsigned long long)jb_dropped_late,
        jb_target_delay_ms, jitter_ms,
        playout_continuity(),
        (unsigned long long)frames_decoded,
        (unsigned long long)decode_errors,
        (unsigned long long)srtp_protected,
        (unsigned long long)srtp_unprotected,
        (unsigned long long)srtp_failures,
        target_bitrate_bps, delay_gradient_ms, rtt_ms,
        (unsigned long long)ice_tx, (unsigned long long)ice_rx,
        ice_connected ? "true" : "false");
    return std::string(buf);
}

// ---------------- 采集器 ----------------
MetricsCollector::MetricsCollector() { start_ms_ = now_ms(); }

void MetricsCollector::mark_start(int64_t now) {
    std::lock_guard<std::mutex> lk(m_);
    start_ms_ = (now < 0) ? now_ms() : now;
}

int64_t MetricsCollector::elapsed_ms(int64_t now) const {
    std::lock_guard<std::mutex> lk(m_);
    if (now < 0) now = now_ms();
    return now - start_ms_;
}

void MetricsCollector::on_frame_captured() {
    std::lock_guard<std::mutex> lk(m_);
    c_.frames_captured++;
}

void MetricsCollector::on_frame_encoded(bool ok, size_t bytes) {
    std::lock_guard<std::mutex> lk(m_);
    if (ok) { c_.frames_encoded++; c_.bytes_sent += bytes; }
    else    { c_.encode_errors++; }
}

void MetricsCollector::on_packet_sent(size_t bytes, bool is_fec) {
    std::lock_guard<std::mutex> lk(m_);
    if (is_fec) { c_.fec_packets_sent++; }
    else        { c_.packets_sent++; }
    c_.bytes_sent += bytes;
}

void MetricsCollector::on_retransmit(size_t bytes) {
    std::lock_guard<std::mutex> lk(m_);
    c_.retransmitted_packets++;
    c_.bytes_sent += bytes;
}

void MetricsCollector::on_nack_request(size_t count) {
    std::lock_guard<std::mutex> lk(m_);
    c_.nack_requests += count;
}

void MetricsCollector::on_packet_received(size_t) {
    std::lock_guard<std::mutex> lk(m_);
    c_.packets_received++;
}

void MetricsCollector::on_packets_lost(uint64_t n) {
    std::lock_guard<std::mutex> lk(m_);
    c_.packets_lost = n;
}

void MetricsCollector::on_recovered_nack(uint64_t n) {
    std::lock_guard<std::mutex> lk(m_);
    c_.packets_recovered_nack = n;
}

void MetricsCollector::on_recovered_fec(uint64_t n) {
    std::lock_guard<std::mutex> lk(m_);
    c_.packets_recovered_fec = n;
}

void MetricsCollector::on_jb_stats(uint64_t received, uint64_t played, uint64_t plc,
                                   uint64_t dropped_late, double target_ms) {
    std::lock_guard<std::mutex> lk(m_);
    c_.jb_received = received;
    c_.jb_played = played;
    c_.jb_plc = plc;
    c_.jb_dropped_late = dropped_late;
    c_.jb_target_delay_ms = target_ms;
    // 丢包率按抖动缓冲的口径计算（它才知道序号跨度）
    const uint64_t span = received + plc;   // 近似：到达 + 补偿 = 应有
    if (span > 0) c_.loss_rate = double(plc) / double(span);
}

void MetricsCollector::on_jitter(double ms) {
    std::lock_guard<std::mutex> lk(m_);
    c_.jitter_ms = ms;
}

void MetricsCollector::on_frame_decoded(bool ok) {
    std::lock_guard<std::mutex> lk(m_);
    if (ok) c_.frames_decoded++;
    else    c_.decode_errors++;
}

void MetricsCollector::on_srtp(bool protect, bool ok) {
    std::lock_guard<std::mutex> lk(m_);
    if (ok) {
        if (protect) c_.srtp_protected++;
        else         c_.srtp_unprotected++;
    } else {
        c_.srtp_failures++;
    }
}

void MetricsCollector::on_congestion(int target_bps, double gradient_ms) {
    std::lock_guard<std::mutex> lk(m_);
    c_.target_bitrate_bps = target_bps;
    c_.delay_gradient_ms = gradient_ms;
}

void MetricsCollector::on_rtt(double ms) {
    std::lock_guard<std::mutex> lk(m_);
    c_.rtt_ms = ms;
}

void MetricsCollector::on_ice(uint64_t tx, uint64_t rx, bool connected) {
    std::lock_guard<std::mutex> lk(m_);
    c_.ice_tx = tx;
    c_.ice_rx = rx;
    c_.ice_connected = connected;
}

CallMetrics MetricsCollector::snapshot(int64_t now) const {
    std::lock_guard<std::mutex> lk(m_);
    CallMetrics s = c_;
    if (now < 0) now = now_ms();
    s.duration_ms = now - start_ms_;
    return s;
}

void MetricsCollector::reset() {
    std::lock_guard<std::mutex> lk(m_);
    c_ = CallMetrics{};
    start_ms_ = now_ms();
}

std::string MetricsCollector::report(int64_t now) const {
    return snapshot(now).to_string();
}

}  // namespace rtc
