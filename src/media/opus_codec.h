#pragma once
// Opus 编解码封装
//
// 对应 WebRTC 音频栈中的 Opus 编码器/解码器。
// 关键设计决策见 docs/01-progress-log.md：
//   - 16kHz 单声道（语音优化，带宽友好）
//   - 20ms 帧长（Opus 在延迟与压缩率之间的经典折中点）
//   - VOIP 模式（针对语音而非音乐优化）
//   - 单包支持多帧拼接（60ms = 3×20ms），与服务器帧长对齐
#include <cstdint>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace rtc {

class OpusCodec {
public:
    static constexpr int kSampleRate = 16000;   // 16kHz
    static constexpr int kChannels   = 1;       // 单声道
    static constexpr int kFrameMs    = 20;      // 20ms 一帧
    static constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;  // 320 样本
    static constexpr int kMaxFrameBytes = 1275; // Opus 单帧上限
    static constexpr int kMaxPacketBytes = kMaxFrameBytes * 3;  // 支持 60ms 拼包

    OpusCodec();
    ~OpusCodec();

    bool init(int bitrate_bps = 16000, int complexity = 0,
              bool use_vbr = false, bool use_inband_fec = false);

    // 编码单帧 20ms。返回编码字节数，<0 表示失败。
    int encode(const int16_t* pcm, int samples, uint8_t* out, int out_cap);

    // 解码。opus_data 为空(NULL) 且 len<=0 时执行 PLC（丢包隐藏）。
    // 返回解码出的样本数，<0 表示失败。
    int decode(const uint8_t* opus_data, int len, int16_t* pcm, int pcm_cap);

    // 多帧拼接：把 N 个 20ms 帧拼进一个包（例如 3 帧 = 60ms）
    // 格式：[u8 frame_count][u16 len_0][payload_0][u16 len_1][payload_1]...
    static int pack_frames(const std::vector<std::vector<uint8_t>>& frames,
                           uint8_t* out, int out_cap);
    static int unpack_frames(const uint8_t* in, int len,
                             std::vector<std::vector<uint8_t>>* out_frames);

    int  bitrate() const { return bitrate_; }

    // 运行时调整目标码率 —— 拥塞控制闭环的关键接口。
    //
    // 为什么必须支持运行时调整：
    //   拥塞控制（GCC）要"测量 -> 决策 -> 生效"闭环，生效环节就是改编码码率。
    //   如果码率只能在 init 时设定，自适应就只是"算出目标值但无作用"。
    //
    // 实现要点：Opus 用 OPUS_SET_BITRATE_REQUEST 控制项 + 独立的值参数，
    // 不能写成 OPUS_SET_BITRATE(x)（宏展开为两个参数，见坑 3）。
    bool set_bitrate(int bps);
    bool inband_fec() const { return inband_fec_; }

    // 编码统计（用于指标上报）
    uint64_t encoded_frames() const { return encoded_frames_; }
    uint64_t decoded_frames() const { return decoded_frames_; }
    uint64_t plc_frames() const { return plc_frames_; }
    // 最近 50 帧的编码字节数均值（实时码率估算）
    double   avg_encoded_bytes() const;

private:
    OpusEncoder* enc_ = nullptr;
    OpusDecoder* dec_ = nullptr;
    int  bitrate_ = 16000;
    int  complexity_ = 0;
    bool vbr_ = false;
    bool inband_fec_ = false;

    uint64_t encoded_frames_ = 0;
    uint64_t decoded_frames_ = 0;
    uint64_t plc_frames_ = 0;

    static constexpr int kHist = 50;
    int      enc_bytes_hist_[kHist] = {0};
    int      enc_hist_pos_ = 0;
    int      enc_hist_cnt_ = 0;
    int      enc_hist_sum_ = 0;
};

}  // namespace rtc
