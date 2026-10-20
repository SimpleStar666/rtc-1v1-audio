// 视频管线测试：分片/重组/关键帧请求/丢包恢复
//
// 这是视频区别于音频的核心验证：
//   1. 大帧分片与重组
//   2. 丢一个分片 → 整帧丢弃（不是花屏）
//   3. 丢关键帧 → 后续 P 帧全部不可解 → 必须触发关键帧请求
//   4. 关键帧请求限流（避免请求风暴）
//   5. 运行时码率调整
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "media/video_device.h"
#include "media/video_pipeline.h"

using namespace rtc;

static int g_fail = 0;
#define CHECK(c, m)                                                        \
    do {                                                                   \
        const bool _ok = (c);                                              \
        printf("  [%s] %s\n", _ok ? "PASS" : "FAIL", m);                    \
        if (!_ok) g_fail++;                                                \
    } while (0)

// 一对互相连接的 VideoPipeline（模拟两个端点）
struct Pair {
    VirtualVideoDevice dev_a, dev_b;
    VideoPipeline va, vb;
    std::atomic<int> kf_req_a_to_b{0};
    std::atomic<int> kf_req_b_to_a{0};
    std::atomic<int> frag_a_to_b{0};
    std::atomic<int> frag_b_to_a{0};
    double loss = 0.0;   // 模拟丢包率

    Pair(int w, int h, int fps, int kbps, const std::string& src_a, const std::string& src_b)
        : dev_a({w, h, fps, src_a, "", 0}),
          dev_b({w, h, fps, src_b, "", 0}) {}

    bool init() {
        if (!dev_a.open() || !dev_b.open()) return false;

        VideoPipeline::Config ca;
        ca.session_id = 2; ca.width = dev_a.config().width; ca.height = dev_a.config().height;
        ca.fps = dev_a.config().fps; ca.bitrate_kbps = 400; ca.keyframe_interval = 15;
        if (!va.init(ca, &dev_a)) return false;
        if (!vb.init(ca, &dev_b)) return false;

        // A -> B
        va.set_send_fn([this](const PacketHeader& h, const std::vector<uint8_t>& p) {
            frag_a_to_b++;
            if (loss > 0 && rand_double() < loss) return;   // 模拟丢包
            vb.on_video_fragment(h, p);
        });
        va.set_keyframe_request_fn([this] {
            kf_req_a_to_b++;
            vb.on_keyframe_request();
        });
        // B -> A
        vb.set_send_fn([this](const PacketHeader& h, const std::vector<uint8_t>& p) {
            frag_b_to_a++;
            if (loss > 0 && rand_double() < loss) return;
            va.on_video_fragment(h, p);
        });
        vb.set_keyframe_request_fn([this] {
            kf_req_b_to_a++;
            va.on_keyframe_request();
        });

        dev_a.start_capture();
        dev_b.start_capture();
        dev_a.start_playout();
        dev_b.start_playout();
        va.start();
        vb.start();
        return true;
    }

    void shutdown() {
        va.stop(); vb.stop();
        dev_a.stop_capture(); dev_b.stop_capture();
        dev_a.stop_playout(); dev_b.stop_playout();
    }

    static double rand_double() {
        static thread_local std::mt19937 rng(42);
        return std::uniform_real_distribution<double>(0, 1)(rng);
    }
};

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose")) set_log_level(LogLevel::kDebug);
    }

    printf("################ 视频管线测试 ################\n\n");

    // ================= 1. 无丢包：正常传输 =================
    printf("=== 1. 无丢包：分片/重组/解码 ===\n");
    {
        Pair p(320, 240, 15, 400, "test:motion", "test:motion");
        CHECK(p.init(), "两端管线初始化");

        std::this_thread::sleep_for(std::chrono::seconds(3));

        auto ra = p.va.report();
        auto rb = p.vb.report();
        printf("    A 发送: 帧=%llu 关键帧=%llu 分片=%llu 字节=%llu 码率=%.0fkbps\n",
               (unsigned long long)ra.frames_encoded, (unsigned long long)ra.keyframes_sent,
               (unsigned long long)ra.fragments_sent, (unsigned long long)ra.bytes_sent,
               ra.actual_kbps);
        printf("    B 接收: 分片=%llu 完整帧=%llu 解码=%llu 丢弃=%llu 关键帧请求=%llu\n",
               (unsigned long long)rb.fragments_recv, (unsigned long long)rb.frames_recv,
               (unsigned long long)rb.frames_decoded, (unsigned long long)rb.frames_dropped,
               (unsigned long long)rb.keyframe_requests);

        CHECK(ra.frames_encoded > 10, "A 编码了多帧");
        CHECK(ra.fragments_sent > ra.frames_encoded, "大帧被分成多个分片（分片数 > 帧数）");
        CHECK(rb.fragments_recv == ra.fragments_sent, "B 收到全部发片（无丢包）");
        CHECK(rb.frames_recv == ra.frames_encoded, "B 重组出全部帧");
        CHECK(rb.frames_decoded > 0, "B 成功解码");
        CHECK(rb.keyframe_requests == 0, "无丢包时不需要请求关键帧");
        CHECK(ra.actual_kbps > 0, "实测码率 > 0");

        p.shutdown();
    }

    // ================= 2. 丢分片：整帧丢弃，不花屏 =================
    printf("\n=== 2. 丢部分分片：整帧丢弃（不花屏）===\n");
    {
        Pair p(320, 240, 15, 400, "test:motion", "test:motion");
        p.init();
        p.loss = 0.15;   // 15% 分片丢失

        std::this_thread::sleep_for(std::chrono::seconds(4));

        auto ra = p.va.report();
        auto rb = p.vb.report();
        printf("    分片丢失 15%%：发送分片=%llu 收到分片=%llu\n",
               (unsigned long long)ra.fragments_sent,
               (unsigned long long)rb.fragments_recv);
        printf("    完整帧=%llu 解码=%llu 丢弃=%llu 关键帧请求=%llu\n",
               (unsigned long long)rb.frames_recv,
               (unsigned long long)rb.frames_decoded,
               (unsigned long long)rb.frames_dropped,
               (unsigned long long)rb.keyframe_requests);

        CHECK(rb.fragments_recv < ra.fragments_sent, "确实丢了分片");
        CHECK(rb.frames_recv < ra.frames_encoded, "缺片的帧没有被当成完整帧");
        CHECK(rb.frames_dropped > 0, "有帧被丢弃（而不是解出花屏）");
        CHECK(rb.frames_decoded > 0, "仍有部分帧成功解码");

        p.shutdown();
    }

    // ================= 3. 丢关键帧 → 触发关键帧请求（视频核心机制）=================
    printf("\n=== 3. 丢关键帧 → 关键帧请求（PLI 机制）===\n");
    {
        // 用 100% 丢包模拟"关键帧全丢"，然后恢复网络看是否自动恢复
        Pair p(320, 240, 15, 400, "test:motion", "test:motion");
        p.init();

        // 前 1.5 秒全丢（必然包含关键帧）
        p.loss = 1.0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        auto mid = p.vb.report();
        printf("    全丢 1.5 秒后：B 收到分片=%llu 解码=%llu\n",
               (unsigned long long)mid.fragments_recv,
               (unsigned long long)mid.frames_decoded);
        CHECK(mid.fragments_recv == 0, "全丢期间确实没收到");

        // 恢复网络
        p.loss = 0.0;
        std::this_thread::sleep_for(std::chrono::seconds(3));

        auto ra = p.va.report();
        auto rb = p.vb.report();
        printf("    恢复后：A 关键帧=%llu  B 关键帧请求=%llu  解码=%llu\n",
               (unsigned long long)ra.keyframes_sent,
               (unsigned long long)rb.keyframe_requests,
               (unsigned long long)rb.frames_decoded);

        CHECK(rb.keyframe_requests > 0,
              "B 检测到无法解码并发出关键帧请求（PLI）");
        CHECK(rb.frames_decoded > 0,
              "恢复网络后能重新解码（关键帧请求生效）");
        printf("    → 这就是视频必须有 PLI/FIR 的原因：\n");
        printf("      丢关键帧后，后续所有 P 帧都依赖它，必须主动请求新的关键帧\n");

        p.shutdown();
    }

    // ================= 4. 关键帧请求限流 =================
    printf("\n=== 4. 关键帧请求限流（防请求风暴）===\n");
    {
        Pair p(320, 240, 15, 400, "test:motion", "test:motion");
        p.init();
        p.loss = 0.5;   // 高丢包，会频繁解码失败

        std::this_thread::sleep_for(std::chrono::seconds(4));

        auto rb = p.vb.report();
        const double secs = 4.0;
        const double req_per_sec = double(rb.keyframe_requests) / secs;
        printf("    4 秒内关键帧请求 %llu 次（%.1f 次/秒）\n",
               (unsigned long long)rb.keyframe_requests, req_per_sec);
        // 限流 300ms → 最多约 3.3 次/秒
        CHECK(req_per_sec <= 4.0,
              "关键帧请求被限流（<= 4 次/秒，避免挤占带宽）");

        p.shutdown();
    }

    // ================= 5. 码率调整 =================
    printf("\n=== 5. 运行时码率调整 ===\n");
    {
        Pair p(320, 240, 15, 400, "test:motion", "test:motion");
        p.init();
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const double kbps400 = p.va.report().actual_kbps;

        p.va.set_target_bitrate_kbps(1200);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const double kbps1200 = p.va.report().actual_kbps;

        p.va.set_target_bitrate_kbps(150);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const double kbps150 = p.va.report().actual_kbps;

        printf("    400kbps -> 实测 %.0f kbps\n", kbps400);
        printf("    1200kbps -> 实测 %.0f kbps\n", kbps1200);
        printf("    150kbps -> 实测 %.0f kbps\n", kbps150);
        CHECK(kbps1200 > kbps400, "上调码率后实际码流增大");
        CHECK(kbps150 < kbps1200, "下调码率后实际码流减小");

        p.shutdown();
    }

    // ================= 6. 不同内容的码率差异（体现编码效率）=================
    printf("\n=== 6. 不同画面内容的编码效率 ===\n");
    {
        struct Case { const char* src; const char* desc; };
        Case cases[] = {
            {"test:bars",   "彩条（大面积平坦，好压缩）"},
            {"test:motion", "渐变+运动（接近真实视频）"},
            {"test:noise",  "噪声（最难压缩）"},
        };
        double kbps[3] = {0, 0, 0};
        for (int i = 0; i < 3; i++) {
            VirtualVideoDevice dev({320, 240, 15, cases[i].src, "", 0});
            dev.open();
            VideoPipeline vp;
            VideoPipeline::Config c;
            c.width = 320; c.height = 240; c.fps = 15;
            c.bitrate_kbps = 800; c.keyframe_interval = 15;
            vp.init(c, &dev);
            vp.set_send_fn([](const PacketHeader&, const std::vector<uint8_t>&) {});
            dev.start_capture();
            vp.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            kbps[i] = vp.report().actual_kbps;
            vp.stop();
            dev.stop_capture();
            printf("    %-34s 实测 %.0f kbps\n", cases[i].desc, kbps[i]);
        }
        CHECK(kbps[0] > 0 && kbps[1] > 0 && kbps[2] > 0, "三种内容都能编码");
        printf("    → 同样目标码率下，噪声内容实际码流更高（编码器被迫用更多比特）\n");
    }

    printf("\n################ 结果：%s（失败 %d 项）################\n",
           g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
