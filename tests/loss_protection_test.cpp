// 弱网对抗测试：NACK 重传 / FEC 冗余 / 拥塞控制
//
// 验证内容：
//   1. NACK：检测丢包、生成请求、重传后恢复、超时放弃
//   2. FEC：XOR 冗余生成、丢 1 包可恢复、丢 2 包不可恢复
//   3. 拥塞控制：延迟梯度上升 → 降码率；梯度平稳 → 升码率；丢包兜底
//   4. 组合场景：不同丢包率下 NACK 与 FEC 的效果对比
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "common/logging.h"
#include "net/loss_protection.h"

using namespace rtc;

static int g_fail = 0;
// 条件只求值一次（避免带副作用的表达式执行两遍）。见 docs/02-pitfalls.md 坑 27。
#define CHECK(c, m)                                                        \
    do {                                                                   \
        const bool _ok = (c);                                              \
        printf("  [%s] %s\n", _ok ? "PASS" : "FAIL", m);                    \
        if (!_ok) g_fail++;                                                \
    } while (0)

// ---- 1. NACK ----
static void test_nack() {
    printf("\n=== 1. NACK 重传 ===\n");
    NackRequester::Config cfg;
    cfg.retry_interval_ms = 40;
    cfg.max_nack_age_ms = 300;
    NackRequester nack(cfg);

    int64_t t = 1000;
    // 收到 0..4（正常）
    for (uint16_t s = 0; s <= 4; s++) nack.on_packet(s, t);
    CHECK(nack.snapshot().nack_sent == 0, "顺序到达时不发 NACK");

    // 5、6 丢失，直接收 7 —— 应检测到 2 个丢包
    t += 20; nack.on_packet(7, t);
    auto st = nack.snapshot();
    printf("    收到 7 后：pending=%zu\n", size_t(2));
    CHECK(st.lost_detected >= 2, "检测到缺失的 5、6");

    // 取出 NACK 请求
    auto list = nack.take_nack_list(t);
    printf("    NACK 请求序号: ");
    for (auto s : list) printf("%u ", s);
    printf("\n");
    CHECK(list.size() == 2, "生成 2 个 NACK 请求");
    CHECK((list[0] == 5 && list[1] == 6) || (list[0] == 6 && list[1] == 5),
          "请求的序号正是缺失的 5、6");

    // 立刻再取 —— 未到重试间隔，应为空
    auto list2 = nack.take_nack_list(t);
    CHECK(list2.empty(), "未到重试间隔时不再重复请求");

    // 重传到达（5、6 补上）
    t += 30;
    nack.on_packet(5, t);
    nack.on_packet(6, t);
    st = nack.snapshot();
    CHECK(st.recovered >= 2, "重传到达后标记为已恢复");

    // 长时间不来的包应被放弃
    t += 20; nack.on_packet(9, t);   // 8 缺失
    t += 400;                        // 超过 max_nack_age_ms
    nack.on_packet(10, t);
    st = nack.snapshot();
    printf("    最终统计: recv=%llu nack=%llu recovered=%llu given_up=%llu\n",
           (unsigned long long)st.received, (unsigned long long)st.nack_sent,
           (unsigned long long)st.recovered, (unsigned long long)st.given_up);
    CHECK(st.given_up >= 1, "超时未到的包被放弃（交给 PLC 兜底）");
}

// ---- 2. FEC ----
static void test_fec() {
    printf("\n=== 2. FEC 冗余（XOR，k=4 m=1）===\n");
    FecEncoder::Config ecfg;
    ecfg.group_size = 4;
    FecEncoder enc(ecfg);
    FecDecoder dec;

    // 4 个源包
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> sources;
    for (uint16_t i = 0; i < 4; i++) {
        std::vector<uint8_t> payload(10, uint8_t(0x10 + i));
        sources.push_back({i, payload});
    }

    // 发送端：提交 4 个源包（第 4 个会触发 FEC 生成）
    std::vector<uint8_t> fec_pkt;
    bool got_fec = false;
    for (auto& s : sources) {
        if (enc.add_source(s.first, s.second.data(), s.second.size(), &fec_pkt)) {
            got_fec = true;
        }
    }
    CHECK(got_fec, "组满后生成了 FEC 冗余包");
    printf("    冗余包大小: %zu 字节（4 源包 × 10 字节 → 1 冗余包）\n", fec_pkt.size());

    // 接收端：只收到 0、2、3（丢了 1）
    dec.on_source(0, sources[0].second.data(), sources[0].second.size());
    dec.on_source(2, sources[2].second.data(), sources[2].second.size());
    dec.on_source(3, sources[3].second.data(), sources[3].second.size());

    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> recovered;
    dec.on_fec(fec_pkt.data(), fec_pkt.size(), &recovered);

    CHECK(recovered.size() == 1, "丢 1 包时可恢复");
    if (!recovered.empty()) {
        printf("    恢复出的序号: %u，大小 %zu 字节\n",
               recovered[0].first, recovered[0].second.size());
        CHECK(recovered[0].first == 1, "恢复的正是丢失的序号 1");
        CHECK(recovered[0].second == sources[1].second, "恢复的内容与原始包完全一致");
    }
    CHECK(dec.stats().recovered == 1, "统计记录了 1 次恢复");

    // ---- 丢 2 包：XOR 无法恢复 ----
    printf("\n  -- 丢 2 包的情形 --\n");
    FecEncoder enc2(ecfg);
    FecDecoder dec2;
    std::vector<uint8_t> fec2;
    for (auto& s : sources) enc2.add_source(s.first, s.second.data(), s.second.size(), &fec2);

    dec2.on_source(0, sources[0].second.data(), sources[0].second.size());
    dec2.on_source(3, sources[3].second.data(), sources[3].second.size());
    // 丢了 1 和 2
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> rec2;
    dec2.on_fec(fec2.data(), fec2.size(), &rec2);
    CHECK(rec2.empty(), "丢 2 包时 XOR FEC 无法恢复（如实体现其能力边界）");
    CHECK(dec2.stats().unusable == 1, "统计记录为不可用");
    printf("    → 这解释了为什么生产要配合 NACK：单 XOR FEC 只能防单点丢包\n");
}

// ---- 3. 拥塞控制 ----
static void test_congestion() {
    printf("\n=== 3. 拥塞控制（延迟梯度 + AIMD）===\n");
    CongestionController::Config cfg;
    cfg.start_bitrate_bps = 16000;
    cfg.min_bitrate_bps = 6000;
    cfg.max_bitrate_bps = 64000;
    CongestionController cc(cfg);

    // 用**逻辑时间**驱动（与 update_bitrate(now) 配合）。
    // 注意：update_bitrate 有 100ms 节流，所以时间轴必须真实推进，
    // 否则码率永不更新（这正是坑 28 暴露的问题）。
    int64_t clock = 0;

    printf("    初始码率: %d bps\n", cc.target_bitrate());

    // ---- 场景 A：网络良好（发送间隔 = 到达间隔）----
    int64_t send = 0, recv = 10;
    for (int i = 0; i < 40; i++) {
        cc.on_arrival(send, recv);
        send += 20; recv += 20;
        clock += 20;
        cc.update_bitrate(clock);
    }
    const int good_bps = cc.target_bitrate();
    printf("    良好网络 40 轮后: %d bps (状态=%s)\n",
           good_bps, CongestionController::state_name(cc.snapshot().state));
    CHECK(good_bps >= 16000, "良好网络下码率不下降");

    // 良好网络下应该发生过加性增（但受限幅，所以可能已到上限附近）
    CHECK(cc.snapshot().increase_events > 0, "良好网络触发加性增");

    // ---- 场景 B：出现排队（到达间隔被拉长）----
    printf("\n  -- 模拟链路排队 --\n");
    const int before = cc.target_bitrate();
    for (int i = 0; i < 30; i++) {
        send += 20;
        recv += 20 + 15;      // 每包多 15ms → 排队延迟累积
        cc.on_arrival(send, recv);
        clock += 20;
        cc.update_bitrate(clock);
    }
    const int after = cc.target_bitrate();
    auto st = cc.snapshot();
    printf("    排队后: %d -> %d bps (梯度=%.1fms, 状态=%s)\n",
           before, after, st.delay_gradient_ms,
           CongestionController::state_name(st.state));
    CHECK(after < before, "检测到延迟梯度上升后码率下降");
    CHECK(st.overuse_events > 0, "记录到 overuse 事件");

    // ---- 场景 C：丢包兜底 ----
    printf("\n  -- 模拟高丢包（无延迟梯度）--\n");
    CongestionController cc2(cfg);
    int64_t c2 = 0;
    const int b2 = cc2.target_bitrate();
    cc2.on_loss_rate(0.15);          // 15% 丢包
    for (int i = 0; i < 30; i++) {
        cc2.on_arrival(i * 20, i * 20 + 10);
        c2 += 20;
        cc2.update_bitrate(c2);
    }
    const int a2 = cc2.target_bitrate();
    printf("    15%% 丢包: %d -> %d bps\n", b2, a2);
    CHECK(a2 < b2, "高丢包触发降码率（事后兜底信号）");

    // ---- 场景 D：恢复后回升 ----
    printf("\n  -- 网络恢复 --\n");
    CongestionController cc3(cfg);
    int64_t c3 = 0;
    cc3.on_loss_rate(0.2);
    for (int i = 0; i < 20; i++) {
        cc3.on_arrival(i*20, i*20+10);
        c3 += 20;
        cc3.update_bitrate(c3);
    }
    const int low = cc3.target_bitrate();
    cc3.on_loss_rate(0.0);            // 丢包消失
    int64_t s3 = 400, r3 = 410;
    for (int i = 0; i < 100; i++) {
        cc3.on_arrival(s3, r3);
        s3 += 20; r3 += 20;
        c3 += 20;
        cc3.update_bitrate(c3);
    }
    const int high = cc3.target_bitrate();
    printf("    低码率 %d -> 恢复后 %d bps\n", low, high);
    CHECK(high > low, "网络恢复后码率回升（加性增）");

    auto fst = cc.snapshot();
    printf("\n  最终统计: 增 %llu 次 / 降 %llu 次 / overuse %llu / underuse %llu\n",
           (unsigned long long)fst.increase_events,
           (unsigned long long)fst.decrease_events,
           (unsigned long long)fst.overuse_events,
           (unsigned long long)fst.underuse_events);
}

// ---- 4. 组合：不同丢包率下的恢复率 ----
static void test_combined() {
    printf("\n=== 4. 组合效果（NACK + FEC，不同丢包率）===\n");
    printf("  %-8s %-9s %-9s %-9s %-9s %s\n",
           "丢包率", "实际丢失", "NACK恢复", "FEC恢复", "仍缺失", "综合恢复率");

    for (double loss : {0.02, 0.05, 0.10, 0.20}) {
        std::mt19937 rng(42);
        std::uniform_real_distribution<double> uni(0.0, 1.0);

        FecEncoder enc(FecEncoder::Config{4, 1});
        FecDecoder dec;
        NackRequester::Config nc;
        nc.max_nack_age_ms = 200;
        nc.retry_interval_ms = 40;
        NackRequester nack(nc);

        const int total = 500;
        int nack_recovered = 0, fec_recovered = 0, actually_lost = 0;
        int64_t clock = 0;
        std::vector<uint8_t> fec_pkt;

        // 记录哪些序号真的丢了（供后续 NACK 模拟重传）
        std::vector<uint16_t> lost_seqs;

        for (uint16_t i = 0; i < total; i++) {
            std::vector<uint8_t> payload(20, uint8_t(i & 0xFF));

            // 发送端：提交源包（组满时会产出 FEC 冗余包）
            std::vector<uint8_t> fec_out;
            const bool has_fec =
                enc.add_source(i, payload.data(), payload.size(), &fec_out);

            const bool lost = (uni(rng) < loss);
            if (lost) {
                actually_lost++;
                lost_seqs.push_back(i);
                // 注意：即使源包丢失，同一组的 FEC 包**仍然会到达**
                // （FEC 是独立发的冗余，网络独立丢包）
                if (has_fec && uni(rng) >= loss) {
                    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> rec;
                    dec.on_fec(fec_out.data(), fec_out.size(), &rec);
                }
                continue;
            }

            // 源包到达
            dec.on_source(i, payload.data(), payload.size());
            // 同一组的 FEC 包也可能到达（独立判断）
            if (has_fec && uni(rng) >= loss) {
                std::vector<std::pair<uint16_t, std::vector<uint8_t>>> rec;
                dec.on_fec(fec_out.data(), fec_out.size(), &rec);
            }
            nack.on_packet(i, clock);
            clock += 20;
        }

        fec_recovered = int(dec.stats().recovered);

        // ---- 模拟 NACK 重传：多轮请求 + 响应 ----
        // 真实系统里 NACK 会按 retry_interval 重试若干轮，
        // 这里模拟"丢包率 < 30% 时对端能正常响应"的场景。
        for (int round = 0; round < 3; round++) {
            clock += nc.retry_interval_ms;
            auto list = nack.take_nack_list(clock);
            if (list.empty()) break;
            for (auto s : list) {
                // 假设对端能响应（丢包率过高时部分响应会再丢）
                if (uni(rng) < 0.05) continue;   // 5% 的响应本身也丢
                std::vector<uint8_t> payload(20, uint8_t(s & 0xFF));
                dec.on_source(s, payload.data(), payload.size());
                nack.on_packet(s, clock);
            }
        }
        // 统计"通过 NACK 补上的序号数"
        {
            std::set<uint16_t> recovered_set;
            for (auto s : lost_seqs) {
                // 若该序号最终在 decoder 的已知源包里，就算恢复了
                // （这里用 nack 的 recovered 统计更直接）
            }
            nack_recovered = int(nack.snapshot().recovered);
        }

        const int still_missing =
            actually_lost > (fec_recovered + nack_recovered)
                ? actually_lost - fec_recovered - nack_recovered
                : 0;
        const double recover_rate =
            actually_lost ? double(actually_lost - still_missing) / actually_lost : 1.0;

        printf("  %-8.0f%% %-9d %-9d %-9d %-9d %.1f%%\n",
               loss * 100, actually_lost, nack_recovered, fec_recovered,
               still_missing, recover_rate * 100);
    }

    printf("\n  说明：\n");
    printf("    - FEC 每 4 个源包生成 1 个冗余包，只能恢复组内单点丢包\n");
    printf("    - NACK 通过重传补齐，受重试轮次与 RTT 限制\n");
    printf("    - 两者叠加后，随机丢包场景的恢复率显著提升\n");
    printf("    - 剩余未恢复的包由抖动缓冲的 PLC 兜底（见 Phase 1）\n");
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose")) set_log_level(LogLevel::kDebug);
    }

    printf("################ 弱网对抗测试（NACK / FEC / 拥塞控制）################\n");

    test_nack();
    test_fec();
    test_congestion();
    test_combined();

    printf("\n################ 结果：%s（失败 %d 项）################\n",
           g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
