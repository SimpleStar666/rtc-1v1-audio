// VP8 编解码自检：验证编码/解码/关键帧/PSNR/码率
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/logging.h"
#include "media/vp8_codec.h"

using namespace rtc;

static int g_fail = 0;
#define CHECK(c, m)                                                        \
    do {                                                                   \
        const bool _ok = (c);                                              \
        printf("  [%s] %s\n", _ok ? "PASS" : "FAIL", m);                    \
        if (!_ok) g_fail++;                                                \
    } while (0)

// 生成测试图案：移动的方块 + 渐变背景
// 为什么不用纯色：纯色帧几乎不占码率，测不出编码器真实行为
static void make_test_frame(YuvFrame* f, int w, int h, int frame_no) {
    f->allocate(w, h);
    // 背景：水平渐变
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            f->y[size_t(r) * w + c] = uint8_t((c * 255 / w + r / 4) & 0xFF);
        }
    }
    // 移动方块（每帧右移 3 像素，循环）
    const int bx = (frame_no * 3) % (w - 40);
    const int by = h / 3;
    for (int r = by; r < by + 40 && r < h; r++) {
        for (int c = bx; c < bx + 40 && c < w; c++) {
            f->y[size_t(r) * w + c] = 235;
        }
    }
    // 色度：随帧变化，测试色度通道
    for (size_t i = 0; i < f->u.size(); i++) {
        f->u[i] = uint8_t(128 + 40 * sin(double(frame_no + int(i)) * 0.05));
        f->v[i] = uint8_t(128 + 40 * cos(double(frame_no + int(i)) * 0.05));
    }
}

// PSNR（峰值信噪比）—— 视频质量的标准度量
// 语音用 SNR，视频用 PSNR（单位 dB，>30 一般算不错，>35 很好）
static double psnr(const YuvFrame& a, const YuvFrame& b) {
    if (a.y.size() != b.y.size()) return 0;
    double mse = 0;
    for (size_t i = 0; i < a.y.size(); i++) {
        const double d = double(a.y[i]) - double(b.y[i]);
        mse += d * d;
    }
    mse /= double(a.y.size());
    if (mse <= 1e-10) return 99.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose")) set_log_level(LogLevel::kDebug);
    }

    printf("################ VP8 编解码测试 ################\n\n");

    const int W = 320, H = 240, FPS = 15, NFRAMES = 30;

    Vp8Codec::Config cfg;
    cfg.width = W;
    cfg.height = H;
    cfg.fps = FPS;
    cfg.bitrate_kbps = 400;
    cfg.keyframe_interval = 15;   // 每 1 秒一个关键帧

    // ================= 1. 初始化 =================
    printf("=== 1. 初始化 ===\n");
    Vp8Codec enc, dec;
    CHECK(enc.init_encoder(cfg), "编码器初始化");
    CHECK(dec.init_decoder(), "解码器初始化");

    // ================= 2. 编解码往返 + PSNR =================
    printf("\n=== 2. 编解码往返（%d 帧，%dx%d @%dfps）===\n", NFRAMES, W, H, FPS);

    int ok_frames = 0, keyframes = 0;
    size_t total_bytes = 0, keyframe_bytes = 0;
    double psnr_sum = 0;
    int psnr_count = 0;
    size_t first_key_bytes = 0, avg_inter_bytes = 0;
    int inter_count = 0;

    for (int i = 0; i < NFRAMES; i++) {
        YuvFrame src, dst;
        make_test_frame(&src, W, H, i);

        EncodedFrame ef;
        if (!enc.encode(src, &ef)) continue;
        total_bytes += ef.data.size();
        if (ef.is_keyframe) { keyframes++; keyframe_bytes += ef.data.size();
                              if (first_key_bytes == 0) first_key_bytes = ef.data.size(); }
        else { avg_inter_bytes += ef.data.size(); inter_count++; }

        if (dec.decode(ef.data.data(), ef.data.size(), &dst)) {
            ok_frames++;
            const double p = psnr(src, dst);
            if (p < 90) { psnr_sum += p; psnr_count++; }
        }
    }

    printf("    编码 %d 帧，解码成功 %d 帧\n", NFRAMES, ok_frames);
    printf("    关键帧: %d 个，关键帧平均 %zu 字节\n",
           keyframes, keyframes ? keyframe_bytes / size_t(keyframes) : 0);
    printf("    非关键帧平均: %zu 字节\n", inter_count ? avg_inter_bytes / size_t(inter_count) : 0);
    printf("    总码流: %zu 字节，平均码率 %.1f kbps\n",
           total_bytes, double(total_bytes) * 8 * FPS / double(NFRAMES) / 1000.0);
    if (psnr_count) printf("    平均 PSNR: %.2f dB\n", psnr_sum / psnr_count);

    CHECK(ok_frames == NFRAMES, "所有帧都能正确解码");
    CHECK(keyframes >= 2, "产生了多个关键帧（周期性关键帧生效）");
    CHECK(psnr_count > 0 && psnr_sum / psnr_count > 30.0,
          "PSNR > 30dB（视频质量合格）");
    CHECK(total_bytes > 0 && total_bytes < size_t(W) * H * NFRAMES,
          "压缩有效（码流远小于原始 YUV）");

    // ================= 3. 关键帧请求 =================
    printf("\n=== 3. 关键帧请求（模拟收到 PLI）===\n");
    enc.request_keyframe();
    YuvFrame src3, dst3;
    make_test_frame(&src3, W, H, 100);
    EncodedFrame ef3;
    CHECK(enc.encode(src3, &ef3) && ef3.is_keyframe,
          "request_keyframe() 后下一帧确实是关键帧");

    // ================= 4. 丢关键帧后的行为（这是视频特有的坑）=================
    printf("\n=== 4. 丢关键帧后的解码行为（视频特有）===\n");
    Vp8Codec enc2, dec2;
    enc2.init_encoder(cfg);
    dec2.init_decoder();

    // 先编码一组，其中第一帧是关键帧
    std::vector<EncodedFrame> seq;
    for (int i = 0; i < 6; i++) {
        YuvFrame f;
        make_test_frame(&f, W, H, 200 + i);
        EncodedFrame e;
        if (enc2.encode(f, &e)) seq.push_back(e);
    }
    printf("    生成 %zu 帧，第 1 帧是关键帧: %s\n",
           seq.size(), (!seq.empty() && seq[0].is_keyframe) ? "是" : "否");

    // 场景 A：正常解码（从关键帧开始）
    int a_ok = 0;
    for (auto& e : seq) { YuvFrame d; if (dec2.decode(e.data.data(), e.data.size(), &d)) a_ok++; }
    printf("    从关键帧开始解码: %d/%zu 成功\n", a_ok, seq.size());
    CHECK(a_ok >= int(seq.size()) - 1, "有参考帧时能正常解码");

    // 场景 B：丢掉关键帧，只解后续 P 帧
    Vp8Codec dec3;
    dec3.init_decoder();
    int b_ok = 0;
    for (size_t i = 1; i < seq.size(); i++) {
        YuvFrame d;
        if (dec3.decode(seq[i].data.data(), seq[i].data.size(), &d)) b_ok++;
    }
    printf("    丢弃关键帧后解码 P 帧: %d/%zu 成功\n", b_ok, seq.size() - 1);
    CHECK(b_ok < int(seq.size()) - 1,
          "丢失关键帧后无法正常解码（→ 必须实现关键帧请求）");
    printf("    → 这就是视频必须做 PLI/FIR（关键帧请求）的原因\n");

    // ================= 5. 运行时码率调整 =================
    printf("\n=== 5. 运行时码率调整 ===\n");
    Vp8Codec enc5;
    Vp8Codec::Config c5 = cfg;
    c5.bitrate_kbps = 200;
    enc5.init_encoder(c5);

    auto measure = [&](Vp8Codec& e, int kbps, const char* label) {
        if (kbps > 0) e.set_bitrate(kbps);
        size_t bytes = 0;
        for (int i = 0; i < 30; i++) {
            YuvFrame f; make_test_frame(&f, W, H, 300 + i);
            EncodedFrame ef;
            if (e.encode(f, &ef)) bytes += ef.data.size();
        }
        const double got = double(bytes) * 8 * FPS / 30.0 / 1000.0;
        printf("    %-16s 目标 %4d kbps -> 实测 %6.1f kbps\n", label, kbps, got);
        return got;
    };
    const double b200 = measure(enc5, 0,   "初始");
    const double b800 = measure(enc5, 800, "上调后");
    const double b100 = measure(enc5, 100, "下调后");
    CHECK(b800 > b200 * 1.5, "上调码率后实际码流变大");
    CHECK(b100 < b200 * 0.8, "下调码率后实际码流变小");

    // ================= 6. 统计 =================
    printf("\n=== 6. 统计 ===\n");
    auto st = enc.stats();
    printf("    编码帧=%llu 关键帧=%llu 字节=%llu 平均帧=%0.1f 平均关键帧=%0.1f\n",
           (unsigned long long)st.frames_encoded, (unsigned long long)st.keyframes,
           (unsigned long long)st.bytes_out, st.avg_frame_bytes, st.avg_keyframe_bytes);
    auto dst_stats = dec.stats();
    printf("    解码帧=%llu 解码错误=%llu\n",
           (unsigned long long)dst_stats.frames_decoded,
           (unsigned long long)dst_stats.decode_errors);
    CHECK(st.frames_encoded > 0 && dst_stats.frames_decoded > 0, "编解码统计正常");

    printf("\n################ 结果：%s（失败 %d 项）################\n",
           g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
