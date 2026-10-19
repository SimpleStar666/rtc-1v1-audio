// 弱网测试：用可控的丢包/抖动/延迟模型驱动抖动缓冲，输出质量指标
//
// 这是"弱网对抗"能力的量化验证工具。
// 用法：./weaknet_test --loss 5 --jitter 30 --delay 100 --seconds 10
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "common/util.h"
#include "media/jitter_buffer.h"
#include "media/opus_codec.h"

using namespace rtc;

struct Args {
    double loss_pct = 0;
    int    jitter_ms = 0;
    int    delay_ms = 0;
    int    seconds = 10;
    int    jb_target = 60;
};

static Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        if (i + 1 >= argc) break;
        if (k == "--loss")   a.loss_pct = atof(argv[++i]);
        else if (k == "--jitter") a.jitter_ms = atoi(argv[++i]);
        else if (k == "--delay")  a.delay_ms = atoi(argv[++i]);
        else if (k == "--seconds") a.seconds = atoi(argv[++i]);
        else if (k == "--jb")     a.jb_target = atoi(argv[++i]);
    }
    return a;
}

int main(int argc, char** argv) {
    Args args = parse(argc, argv);
    set_log_level(LogLevel::kWarn);

    printf("=== 弱网测试 ===\n");
    printf("  丢包率=%.1f%%  抖动=±%dms  基础延迟=%dms  时长=%ds  目标缓冲=%dms\n",
           args.loss_pct, args.jitter_ms, args.delay_ms, args.seconds, args.jb_target);

    JitterBuffer::Config jcfg;
    jcfg.target_delay_ms = args.jb_target;
    jcfg.frame_ms = OpusCodec::kFrameMs;
    JitterBuffer jb(jcfg);
    OpusCodec codec;
    codec.init(16000, 0, false, false);

    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::uniform_int_distribution<int> jit(-args.jitter_ms, args.jitter_ms);

    const int frame_ms = OpusCodec::kFrameMs;
    const int total_frames = args.seconds * 1000 / frame_ms;

    // 生成一帧音频并编码，作为发送载荷
    std::vector<int16_t> pcm(OpusCodec::kFrameSamples);
    for (int i = 0; i < OpusCodec::kFrameSamples; i++) {
        pcm[i] = int16_t(8000 * sin(2 * M_PI * 440.0 * i / 16000.0));
    }
    uint8_t enc[OpusCodec::kMaxFrameBytes];
    int enc_len = codec.encode(pcm.data(), OpusCodec::kFrameSamples, enc, sizeof(enc));

    int sent = 0, dropped = 0, received = 0, played = 0, plc = 0;
    std::vector<double> latencies;

    // 用"逻辑时间"驱动整个仿真：
    //   发送时刻 / 到达时刻 / 播放时刻都是逻辑时间轴上的点，
    //   不需要真的 sleep（跑 10 秒仿真只花几十毫秒真实时间）。
    //   这就是为什么 pop() 要接受 now 参数——见 docs/02-pitfalls.md 坑 9。
    const int64_t t0 = 0;

    // 为了模拟真实时序，必须按"到达时间顺序"而不是"发送顺序"灌包，
    // 否则丢包/抖动模型就失去意义了。
    struct Arrival {
        int64_t  arrive_ms;
        int      seq;
    };
    std::vector<Arrival> arrivals;

    for (int f = 0; f < total_frames; f++) {
        const int64_t send_time = t0 + int64_t(f) * frame_ms;

        // 模拟丢包
        if (uni(rng) * 100.0 < args.loss_pct) {
            dropped++;
            continue;
        }
        sent++;

        // 模拟延迟 + 抖动
        int64_t arrive = send_time + args.delay_ms + (args.jitter_ms ? jit(rng) : 0);
        if (arrive < send_time) arrive = send_time;
        arrivals.push_back({arrive, f});
        latencies.push_back(double(arrive - send_time));
    }

    // 按到达时间排序 —— 这样"乱序到达"就自然发生了
    std::sort(arrivals.begin(), arrivals.end(),
              [](const Arrival& a, const Arrival& b) { return a.arrive_ms < b.arrive_ms; });

    // ---- 仿真主循环：以 1ms 为步进推进逻辑时间 ----
    const int64_t end_ms = t0 + int64_t(total_frames) * frame_ms + 1000;
    size_t next_arrival = 0;
    std::vector<uint8_t> payload;

    for (int64_t now = t0; now <= end_ms; now++) {
        // 推进所有"已到达"的包
        while (next_arrival < arrivals.size() && arrivals[next_arrival].arrive_ms <= now) {
            const Arrival& a = arrivals[next_arrival++];
            if (jb.push(uint16_t(a.seq), enc, size_t(enc_len), a.arrive_ms)) received++;
        }
        // 按播放时钟取帧
        while (jb.pop(&payload, now)) {
            if (payload.empty()) plc++;
            else played++;
        }
    }

    auto st = jb.snapshot();
    Stats lat;
    for (double v : latencies) lat.add(v);

    printf("\n=== 结果 ===\n");
    printf("  发送帧数      : %d\n", total_frames - dropped);
    printf("  丢包帧数      : %d (%.1f%%)\n", dropped, 100.0 * dropped / total_frames);
    printf("  抖动缓冲接受  : %llu\n", (unsigned long long)st.received);
    printf("  检测丢包      : %llu\n", (unsigned long long)st.lost);
    printf("  重复/迟到丢弃 : %llu / %llu\n",
           (unsigned long long)st.dup, (unsigned long long)st.late);
    printf("  播放帧数      : %llu\n", (unsigned long long)st.played);
    printf("  PLC 补偿次数  : %llu\n", (unsigned long long)st.plc);
    printf("  目标延迟      : %.0f ms (初始 %d ms)\n", st.cur_target_ms, args.jb_target);
    printf("  平均抖动      : %.2f ms (最大 %.2f ms)\n", st.avg_jitter_ms, st.max_jitter_ms);
    printf("  到达延迟      : 均值 %.1f ms, 标准差 %.1f ms, 最大 %lld ms\n",
           lat.mean(), lat.stddev(), (long long)lat.max_v);

    // 连续性指标：播放帧 / (播放帧 + PLC帧)
    double continuity = played + plc > 0 ? 100.0 * played / (played + plc) : 0;
    printf("  播放连续性    : %.1f%% （越接近 100%% 说明丢包补偿越不明显）\n", continuity);
    return 0;
}
