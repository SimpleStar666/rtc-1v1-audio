#pragma once
// VP8 视频编解码 —— 对应 WebRTC 的 VP8 编解码器
//
// 为什么选 VP8 而不是 H.264/H.265：
//   1. **WebRTC 强制实现**（RFC 7742）：VP8 是 WebRTC 的必选视频编解码器，
//      H.264 是可选的。讲"WebRTC 视频"就绕不开 VP8。
//   2. 无专利授权问题（H.264/H.265 有 MPEG-LA 专利池）。
//   3. 有 SVC（可伸缩编码）扩展路径，WebRTC 里叫 VP8 SVC / simulcast。
//   4. libvpx 是纯 C API，容易封装；H.264 的 x264 更偏"离线编码"。
//
// 视频与音频的关键差异（面试高频）：
//   - 音频丢一帧用 PLC 补，几乎听不出来；视频丢一帧会花屏/卡顿，
//     且**丢关键帧会让后续所有帧都无法解码**（参考帧缺失）。
//   - 所以视频必须处理：关键帧请求（PLI/FIR）、参考帧丢失后的恢复、
//     以及"关键帧前的帧先丢弃"。
//   - 视频码率比音频大 1~2 个数量级（音频 16kbps，视频 300~800kbps）。
//
// 本模块只做"编解码器封装"这一件事（与 OpusCodec 对称），
// 抖动缓冲/关键帧策略在 VideoPipeline 里。
#include <cstdint>
#include <string>
#include <vector>

namespace rtc {

// YUV420P 图像（也叫 I420）
//
// 为什么用 I420：
//   libvpx 原生支持，且是视频处理的事实标准格式。
//   内存布局：先 Y 平面（w×h），再 U 平面（w/2×h/2），再 V 平面（w/2×h/2）。
//   亮度 Y 全分辨率，色度 U/V 各降采样 1/2（人眼对色度不敏感）。
struct YuvFrame {
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> y, u, v;

    void allocate(int w, int h);
    bool empty() const { return width <= 0 || height <= 0; }
    // 总字节数（w*h + 2*(w/2)*(h/2) = w*h*3/2）
    size_t size_bytes() const;
};

// 一帧压缩后的视频数据
struct EncodedFrame {
    std::vector<uint8_t> data;
    bool     is_keyframe = false;
    uint32_t timestamp   = 0;    // 毫秒或帧序号
    bool     empty() const { return data.empty(); }
};

class Vp8Codec {
public:
    struct Config {
        int width  = 320;
        int height = 240;
        int fps    = 15;
        int bitrate_kbps = 400;      // 视频码率远高于音频
        int min_quantizer = 4;
        int max_quantizer = 56;
        int keyframe_interval = 30;  // 每 N 帧一个关键帧（15fps → 2 秒）
        int threads = 2;
    };

    Vp8Codec();
    ~Vp8Codec();

    // ---- 编码器 ----
    bool init_encoder(const Config& cfg);
    // 编码一帧。返回 true 表示有输出（写入 out）。
    bool encode(const YuvFrame& frame, EncodedFrame* out);
    // 请求下一帧强制为关键帧（收到对端 PLI 时调用）
    void request_keyframe();

    // ---- 解码器 ----
    bool init_decoder();
    // 解码一帧。成功时填充 out（YUV420）。
    // 返回 false 表示解码失败（常见原因：丢了关键帧 / 参考帧缺失）。
    bool decode(const uint8_t* data, size_t len, YuvFrame* out);

    // ---- 运行时码率调整（供拥塞控制调用）----
    bool set_bitrate(int kbps);

    const Config& config() const { return cfg_; }
    bool encoder_ready() const { return enc_inited_; }
    bool decoder_ready() const { return dec_inited_; }

    // 统计
    struct Stats {
        uint64_t frames_encoded = 0;
        uint64_t keyframes      = 0;
        uint64_t frames_decoded = 0;
        uint64_t decode_errors  = 0;
        uint64_t bytes_out      = 0;
        double   avg_frame_bytes = 0;
        double   avg_keyframe_bytes = 0;
    };
    Stats stats() const { return stats_; }
    void  reset_stats();

private:
    // 用 void* 存 libvpx 的 ctx，避免头文件暴露 vpx 依赖
    // （与 dtls_srtp.h 里 ssl_ctx_st* 同样的思路）
    void*  enc_ = nullptr;
    void*  dec_ = nullptr;
    Config cfg_;
    bool   enc_inited_ = false;
    bool   dec_inited_ = false;
    bool   force_keyframe_ = false;
    int64_t frame_index_ = 0;
    Stats  stats_;
};

}  // namespace rtc
