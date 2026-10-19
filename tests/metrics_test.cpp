// 指标统计测试：验证统一指标采集与派生指标计算
#include <cstdio>
#include <cstring>

#include "common/logging.h"
#include "metrics/metrics.h"

using namespace rtc;

static int g_fail = 0;
#define CHECK(c, m)                                                        \
    do {                                                                   \
        const bool _ok = (c);                                              \
        printf("  [%s] %s\n", _ok ? "PASS" : "FAIL", m);                    \
        if (!_ok) g_fail++;                                                \
    } while (0)

// 浮点近似比较
static bool near(double a, double b, double eps = 0.01) {
    return (a > b ? a - b : b - a) < eps;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose")) set_log_level(LogLevel::kDebug);
    }

    printf("################ 指标统计测试 ################\n");

    // ================= 1. 各层上报 =================
    printf("\n=== 1. 各层指标上报 ===\n");
    // 用一个固定的逻辑时间点模拟 5 秒通话（250 帧 × 20ms = 5000ms）。
    // ★ 用逻辑时间：mark_start(0) 与 snapshot(kLogicalNow) 配对，
    //   避免 duration 取到真实时钟差而变成荒谬值（实测出现过 3699778ms）。
    //   见 docs/02-pitfalls.md 坑 28 与 32。
    const int64_t kLogicalNow = 5000;
    MetricsCollector mc;
    mc.mark_start(0);

    // 模拟 5 秒通话：250 帧（20ms 一帧）、每 3 帧一个包
    for (int i = 0; i < 250; i++) mc.on_frame_captured();
    for (int i = 0; i < 250; i++) {
        const bool ok = (i % 100) != 99;   // 模拟 1% 编码失败
        mc.on_frame_encoded(ok, 40);
    }
    // 84 个 RTP 包（250/3 ≈ 83）+ 21 个 FEC 包
    for (int i = 0; i < 83; i++) mc.on_packet_sent(140, false);
    for (int i = 0; i < 21; i++) mc.on_packet_sent(60, true);

    mc.on_packet_received(83);
    mc.on_packets_lost(8);
    mc.on_recovered_nack(5);
    mc.on_recovered_fec(3);

    mc.on_jb_stats(250, 240, 10, 2, 60.0);
    mc.on_jitter(12.5);

    for (int i = 0; i < 240; i++) mc.on_frame_decoded(true);
    for (int i = 0; i < 10; i++)  mc.on_frame_decoded(false);

    for (int i = 0; i < 104; i++) mc.on_srtp(true, true);
    mc.on_srtp(true, false);   // 1 次加密失败

    mc.on_congestion(16000, 3.5);
    mc.on_rtt(45.0);
    mc.on_ice(104, 104, true);
    mc.on_nack_request(8);
    mc.on_retransmit(140);

    auto s = mc.snapshot(kLogicalNow);

    // ---- 校验原始计数 + duration ----
    printf("  采集帧=%llu 编码成功=%llu 错误=%llu\n",
           (unsigned long long)s.frames_captured,
           (unsigned long long)s.frames_encoded,
           (unsigned long long)s.encode_errors);
    printf("  通话时长=%lld ms（逻辑时间）\n", (long long)s.duration_ms);
    CHECK(s.duration_ms == kLogicalNow, "通话时长按逻辑时间计算正确");
    CHECK(s.frames_captured == 250, "采集帧数记录正确");
    CHECK(s.frames_encoded == 248, "编码成功数正确（250 - 2 次失败）");
    CHECK(s.encode_errors == 2, "编码错误数正确");
    CHECK(s.packets_sent == 83, "RTP 包数正确");
    CHECK(s.fec_packets_sent == 21, "FEC 包数正确");
    CHECK(s.nack_requests == 8, "NACK 请求数正确");
    CHECK(s.retransmitted_packets == 1, "重传数正确");
    CHECK(s.srtp_protected == 104, "SRTP 加密成功数正确");
    CHECK(s.srtp_failures == 1, "SRTP 失败数正确");
    CHECK(s.ice_connected, "ICE 连接状态记录正确");

    // ---- 校验派生指标 ----
    printf("\n=== 2. 派生指标计算 ===\n");
    const double enc_rate = s.encode_success_rate();
    printf("  编码成功率: %.2f%%\n", enc_rate * 100.0);
    CHECK(near(enc_rate, 248.0 / 250.0), "编码成功率 = 248/250");

    const double rec_rate = s.recovery_rate();
    printf("  恢复率: %.2f%%（丢包 8，恢复 5+3=8，仍缺失 %llu）\n",
           rec_rate * 100.0, (unsigned long long)s.still_missing());
    CHECK(near(rec_rate, 1.0), "恢复率 = 8/8 = 100%");
    CHECK(s.still_missing() == 0, "仍缺失包数 = 0");

    const double cont = s.playout_continuity();
    printf("  播放连续性: %.2f%%（播放 240，PLC 10）\n", cont * 100.0);
    CHECK(near(cont, 240.0 / 250.0), "播放连续性 = 240/(240+10)");

    // ================= 3. 恢复率的部分场景 =================
    printf("\n=== 3. 部分恢复场景 ===\n");
    {
        MetricsCollector m2;
        m2.mark_start(0);
        m2.on_packets_lost(10);
        m2.on_recovered_nack(4);
        m2.on_recovered_fec(2);
        auto s2 = m2.snapshot(1000);
        printf("  丢包 10，恢复 4+2=6，恢复率 %.1f%%，仍缺失 %llu\n",
               s2.recovery_rate() * 100.0, (unsigned long long)s2.still_missing());
        CHECK(near(s2.recovery_rate(), 0.6), "恢复率 = 6/10 = 60%");
        CHECK(s2.still_missing() == 4, "仍缺失 4 个包（10 - 6）");
    }

    // ================= 4. 无丢包时应为 100% =================
    printf("\n=== 4. 边界：无丢包 ===\n");
    {
        MetricsCollector m3;
        m3.mark_start(0);
        auto s3 = m3.snapshot(1000);
        CHECK(near(s3.recovery_rate(), 1.0), "无丢包时恢复率 = 100%（不是 0）");
        CHECK(near(s3.playout_continuity(), 1.0), "无播放时连续性 = 100%（不是 NaN）");
        CHECK(near(s3.encode_success_rate(), 1.0), "无编码时成功率 = 100%（不是 NaN）");
        printf("  → 避免除零产生 NaN/0 的误报（这是监控指标最常见的坑）\n");
    }

    // ================= 5. 快照与 JSON 输出 =================
    printf("\n=== 5. 输出格式 ===\n");
    const std::string js = s.to_json();
    CHECK(js.find("\"frames_captured\":250") != std::string::npos,
          "JSON 含 frames_captured=250");
    CHECK(js.find("\"ice_connected\":true") != std::string::npos,
          "JSON 含 ice_connected=true");
    CHECK(js.front() == '{' && js.back() == '}', "JSON 首尾括号正确");
    // 简单括号配对检查
    int depth = 0; bool balanced = true;
    for (char ch : js) {
        if (ch == '{') depth++;
        else if (ch == '}') { depth--; if (depth < 0) { balanced = false; break; } }
    }
    CHECK(balanced && depth == 0, "JSON 括号配对正确");
    printf("  JSON 长度: %zu 字节\n", js.size());

    // ================= 6. reset =================
    printf("\n=== 6. 重置 ===\n");
    mc.reset();
    auto s4 = mc.snapshot();
    CHECK(s4.frames_captured == 0 && s4.packets_sent == 0, "reset 后计数归零");

    // ================= 7. 完整报告 =================
    printf("\n=== 7. 完整指标报告（示例）===\n");
    printf("%s\n", s.to_string().c_str());

    // 字节码率检验：22940 字节 / 5000ms = 36.7 kbps
    const int br = s.actual_bitrate_bps();
    printf("  派生码率: %d bps（%llu 字节 / %lld ms）\n",
           br, (unsigned long long)s.bytes_sent, (long long)s.duration_ms);
    CHECK(near(double(br), 22940.0 * 8 * 1000 / 5000.0, 1.0),
          "实测码率按字节/时长正确计算");

    printf("\n################ 结果：%s（失败 %d 项）################\n",
           g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
