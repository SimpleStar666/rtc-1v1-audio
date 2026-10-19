#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "common/logging.h"
#include "common/util.h"
#include "media/jitter_buffer.h"
#include "media/opus_codec.h"
#include "media/virtual_device.h"
#include "protocol/packet.h"
#include "protocol/signaling_msg.h"

using namespace rtc;

static int g_fail = 0;
// 条件只求值一次（避免带副作用的表达式被执行两遍）。见 docs/02-pitfalls.md 坑 27。
#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        const bool _ok = (cond);                                           \
        if (_ok) {                                                         \
            printf("  [PASS] %s\n", msg);                                  \
        } else {                                                           \
            printf("  [FAIL] %s\n", msg);                                  \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

// ---- 测试 1：Opus 编解码往返，验证音质 ----
static void test_opus_roundtrip() {
    printf("\n=== 测试 1：Opus 编解码往返 ===\n");
    OpusCodec codec;
    CHECK(codec.init(16000, 0, false, false), "codec init");

    // 生成 1 秒 440Hz 正弦波
    const int total = OpusCodec::kSampleRate;
    std::vector<int16_t> pcm(total);
    for (int i = 0; i < total; i++) {
        pcm[i] = int16_t(8000 * sin(2 * M_PI * 440.0 * i / OpusCodec::kSampleRate));
    }

    // 逐 20ms 帧编码再解码，对比失真。
    //
    // 重要：不能逐样本直接比对——Opus 是有损编解码器，且带 lookahead（前瞻）延迟，
    // 解码输出相对输入存在固定的时间偏移。直接比对会把"时间偏移"算成"失真"，
    // 得到荒谬的 SNR（实测仅 2.8dB）。正确做法是先做互相关找最佳对齐偏移，
    // 再在对齐后计算残差。这是音频测试的经典坑，见 docs/02-pitfalls.md。
    const int frame_samples = OpusCodec::kFrameSamples;
    std::vector<int16_t> decoded_all;
    std::vector<int16_t> original_all;
    int total_bytes = 0;
    int frames = total / frame_samples;

    for (int f = 0; f < frames; f++) {
        uint8_t enc[OpusCodec::kMaxFrameBytes];
        int n = codec.encode(pcm.data() + f * frame_samples, frame_samples, enc, sizeof(enc));
        if (n < 0) { CHECK(false, "encode frame"); return; }
        total_bytes += n;

        int16_t dec[frame_samples];
        int m = codec.decode(enc, n, dec, frame_samples);
        if (m < 0) { CHECK(false, "decode frame"); return; }

        decoded_all.insert(decoded_all.end(), dec, dec + m);
        original_all.insert(original_all.end(),
                            pcm.begin() + f * frame_samples,
                            pcm.begin() + f * frame_samples + m);
    }

    // 互相关找最佳对齐偏移（在 ±2 帧范围内搜索）
    const int max_lag = 2 * frame_samples;
    int   best_lag = 0;
    double best_err = 1e30;
    for (int lag = 0; lag <= max_lag; lag++) {
        double err = 0;
        int    cnt = 0;
        for (size_t i = 0; i + lag < decoded_all.size() && i < original_all.size(); i++) {
            double e = double(decoded_all[i + size_t(lag)]) - double(original_all[i]);
            err += e * e;
            cnt++;
        }
        if (cnt == 0) continue;
        err /= cnt;
        if (err < best_err) { best_err = err; best_lag = lag; }
    }

    double rmse = sqrt(best_err);
    double snr_db = 20 * log10(8000.0 / (rmse + 1e-9));
    double avg_bytes = double(total_bytes) / frames;
    double kbps = avg_bytes * 8.0 * (1000.0 / OpusCodec::kFrameMs) / 1000.0;

    printf("    最佳对齐偏移=%d 样本(%.1f ms), RMSE=%.1f, SNR=%.1f dB, "
           "平均帧=%.1f B, 实测码率=%.1f kbps\n",
           best_lag, best_lag * 1000.0 / OpusCodec::kSampleRate, rmse, snr_db, avg_bytes, kbps);
    CHECK(rmse < 800, "对齐后失真合理 (RMSE < 800)");
    CHECK(snr_db > 15, "对齐后 SNR > 15dB（Opus 语音质量合格）");
    CHECK(kbps > 10 && kbps < 30, "实测码率在 10~30kbps 之间");
}

// ---- 测试 2：多帧拼包 ----
static void test_frame_packing() {
    printf("\n=== 测试 2：多帧拼包（20ms x N）===\n");
    std::vector<std::vector<uint8_t>> frames = {
        {0x11, 0x22, 0x33},
        {0x44},
        {0x55, 0x66},
    };
    uint8_t buf[256];
    int n = OpusCodec::pack_frames(frames, buf, sizeof(buf));
    CHECK(n > 0, "pack_frames 成功");

    std::vector<std::vector<uint8_t>> out;
    int consumed = OpusCodec::unpack_frames(buf, n, &out);
    CHECK(consumed == n, "解包消费字节数一致");
    CHECK(out.size() == frames.size(), "帧数一致");
    bool same = true;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] != frames[i]) same = false;
    }
    CHECK(same, "每帧内容一致");
}

// ---- 测试 3：包序列化 + CRC 校验 ----
static void test_packet_serde() {
    printf("\n=== 测试 3：包序列化 / CRC 校验 ===\n");
    PacketHeader h;
    h.session_id = 0x1234;
    h.seq = 42;
    h.timestamp = 96000;
    h.phase = Phase::kBody;
    h.flags = kFlagNone;
    h.trans_id = 7;
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5};

    uint8_t buf[128];
    int n = Packet::serialize(h, payload.data(), payload.size(), buf, sizeof(buf));
    CHECK(n == int(PacketHeader::kSize + payload.size()), "序列化长度正确");

    PacketHeader ph;
    std::vector<uint8_t> pp;
    CHECK(Packet::parse(buf, n, &ph, &pp), "解析成功");
    CHECK(ph.session_id == h.session_id, "session_id 一致");
    CHECK(ph.seq == h.seq, "seq 一致");
    CHECK(ph.timestamp == h.timestamp, "timestamp 一致");
    CHECK(ph.trans_id == h.trans_id, "trans_id 一致");
    CHECK(pp == payload, "payload 一致");

    // 故意损坏包头，应被 CRC 拒绝
    uint8_t corrupt[128];
    memcpy(corrupt, buf, n);
    corrupt[2] ^= 0xFF;  // 改 seq 高字节
    PacketHeader ch;
    std::vector<uint8_t> cp;
    CHECK(!Packet::parse(corrupt, n, &ch, &cp), "损坏包头被 CRC 拒绝");
}

// ---- 测试 4：抖动缓冲（含乱序、丢包、PLC）----
static void test_jitter_buffer() {
    printf("\n=== 测试 4：抖动缓冲 ===\n");
    JitterBuffer jb;
    std::vector<uint8_t> data(10, 0xAB);

    // 顺序到达 10 个包
    for (int i = 0; i < 10; i++) {
        bool ok = jb.push(uint16_t(i), data.data(), data.size(), now_ms());
        if (!ok) { CHECK(false, "顺序包入队"); return; }
    }
    auto st = jb.snapshot();
    CHECK(st.received == 10, "收到 10 个包");
    CHECK(st.lost == 0, "无丢包");

    // 乱序：先到 20，后到 18 —— 这是抖动缓冲必须吸收的典型场景
    jb.push(20, data.data(), data.size());
    bool ok18 = jb.push(18, data.data(), data.size());
    CHECK(ok18, "乱序包被接受（抖动缓冲的核心职责）");

    // 重复包应被拒绝
    bool dup = jb.push(18, data.data(), data.size());
    CHECK(!dup, "重复包被拒绝");

    // 丢包检测：跳号
    jb.push(25, data.data(), data.size());
    st = jb.snapshot();
    CHECK(st.lost > 0, "检测到丢包");

    // 播放顺序必须是严格递增的（乱序已被重排）
    printf("    统计: recv=%llu played=%llu lost=%llu plc=%llu dup=%llu late=%llu\n",
           (unsigned long long)st.received, (unsigned long long)st.played,
           (unsigned long long)st.lost, (unsigned long long)st.plc,
           (unsigned long long)st.dup, (unsigned long long)st.late);
}

// ---- 测试 5：信令 JSON 往返 ----
static void test_signaling() {
    printf("\n=== 测试 5：信令 JSON 往返 ===\n");
    SignalMsg m = SignalMsg::make_candidate("room-a", "192.168.1.5:50000", 0);
    m.peer_id = "peer-1";
    std::string json = m.to_json();
    printf("    JSON: %s\n", json.c_str());

    SignalMsg out;
    CHECK(SignalMsg::from_json(json, &out), "解析成功");
    CHECK(out.type == SignalType::kCandidate, "type 一致");
    CHECK(out.room == "room-a", "room 一致");
    CHECK(out.candidate == "192.168.1.5:50000", "candidate 一致");
    CHECK(out.peer_id == "peer-1", "peer_id 一致");

    // offer 带编解码参数
    SignalMsg o = SignalMsg::make_offer("r", "v=0...");
    std::string oj = o.to_json();
    SignalMsg oo;
    CHECK(SignalMsg::from_json(oj, &oo), "offer 解析");
    CHECK(oo.sample_rate == 16000 && oo.channels == 1, "编解码参数一致");
}

// ---- 测试 6：虚拟设备采集节拍 ----
static void test_virtual_device() {
    printf("\n=== 测试 6：虚拟音频设备 ===\n");
    VirtualAudioDevice::Config cfg;
    cfg.source_path = "";   // 用正弦波
    cfg.sink_path   = "/tmp/rtc_test_playback.pcm";
    VirtualAudioDevice dev(cfg);
    CHECK(dev.open(), "设备打开");

    std::atomic<int> frames{0};
    std::atomic<int> samples{0};
    dev.start_capture([&](const int16_t* pcm, int n, int64_t) {
        frames++;
        samples += n;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    dev.stop_capture();

    printf("    500ms 内采集到 %d 帧 / %d 样本\n", frames.load(), samples.load());
    // 500ms @20ms/帧 应该约 25 帧；容差放宽到 15~35
    CHECK(frames >= 15 && frames <= 35, "采集节拍接近实时（约 25 帧/500ms）");
    dev.close();
}

int main(int argc, char** argv) {
    set_log_level(argc > 1 ? LogLevel::kDebug : LogLevel::kInfo);
    printf("################ rtc-1v1-audio 自检 ################\n");

    test_opus_roundtrip();
    test_frame_packing();
    test_packet_serde();
    test_jitter_buffer();
    test_signaling();
    test_virtual_device();

    printf("\n################ 结果：%s（失败 %d 项）################\n",
           g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
