#include "media/opus_codec.h"

#include <opus/opus.h>

#include <cstring>

#include "common/logging.h"
#include "common/util.h"   // put_u16_le / get_u16_le（拼包用）

namespace rtc {

namespace {
constexpr const char* kTag = "opus";
}

OpusCodec::OpusCodec() = default;

OpusCodec::~OpusCodec() {
    if (enc_) { opus_encoder_destroy(enc_); enc_ = nullptr; }
    if (dec_) { opus_decoder_destroy(dec_); dec_ = nullptr; }
}

bool OpusCodec::init(int bitrate_bps, int complexity, bool use_vbr, bool use_inband_fec) {
    int err = OPUS_OK;

    enc_ = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &err);
    if (!enc_ || err != OPUS_OK) {
        LOGE(kTag, "encoder create failed: %s", opus_strerror(err));
        return false;
    }
    dec_ = opus_decoder_create(kSampleRate, kChannels, &err);
    if (!dec_ || err != OPUS_OK) {
        LOGE(kTag, "decoder create failed: %s", opus_strerror(err));
        return false;
    }

    bitrate_    = bitrate_bps;
    complexity_ = complexity;
    vbr_        = use_vbr;
    inband_fec_ = use_inband_fec;

    // 设置编码参数。
    // 注意：Opus 的 OPUS_SET_XXX(x) 宏会展开成 "REQUEST 常量 + 值检查" 两个参数，
    // 因此不能直接把它塞进函数调用（会变成多一个实参）。这里统一用 *_REQUEST 常量。
    auto set = [&](int req, opus_int32 val, const char* name) -> bool {
        int r = opus_encoder_ctl(enc_, req, val);
        if (r != OPUS_OK) {
            LOGE(kTag, "set %s failed: %s", name, opus_strerror(r));
            return false;
        }
        return true;
    };

    bool ok = true;
    ok &= set(OPUS_SET_BITRATE_REQUEST, opus_int32(bitrate_), "bitrate");
    ok &= set(OPUS_SET_COMPLEXITY_REQUEST, opus_int32(complexity_), "complexity");
    ok &= set(OPUS_SET_VBR_REQUEST, use_vbr ? 1 : 0, "vbr");
    ok &= set(OPUS_SET_INBAND_FEC_REQUEST, use_inband_fec ? 1 : 0, "inband_fec");
    if (use_inband_fec) {
        // 期望丢包率提示：告诉编码器给 FEC 预留多少冗余
        set(OPUS_SET_PACKET_LOSS_PERC_REQUEST, 10, "packet_loss_perc");
    }
    // DTX（不连续传输）：静音期不发包，节省带宽
    set(OPUS_SET_DTX_REQUEST, 0, "dtx");
    ok &= set(OPUS_SET_SIGNAL_REQUEST, opus_int32(OPUS_SIGNAL_VOICE), "signal");

    if (!ok) return false;

    LOGI(kTag, "init ok: %dHz/%dch, %dms/frame(%d samples), bitrate=%dbps, "
               "complexity=%d, vbr=%d, inband_fec=%d",
         kSampleRate, kChannels, kFrameMs, kFrameSamples, bitrate_,
         complexity_, vbr_, inband_fec_);
    return true;
}

// 运行时调整目标码率 —— 让拥塞控制真正生效（闭环的最后一环）。
//
// 注意 OPUS_SET_BITRATE 系列宏展开为两个参数（控制项 + 值），
// 不能写成 OPUS_SET_BITRATE(x)。见 docs/02-pitfalls.md 坑 3。
bool OpusCodec::set_bitrate(int bps) {
    if (!enc_) return false;
    // Opus 的合法码率范围：6~510 kbps（单声道 16kHz 下 6~64 已足够）
    if (bps < 6000)   bps = 6000;
    if (bps > 128000) bps = 128000;

    const int rc = opus_encoder_ctl(enc_, OPUS_SET_BITRATE_REQUEST, opus_int32(bps));
    if (rc != OPUS_OK) {
        LOGE(kTag, "set_bitrate(%d) failed: %d", bps, rc);
        return false;
    }
    bitrate_ = bps;
    LOGD(kTag, "bitrate adapted to %d bps", bps);
    return true;
}

int OpusCodec::encode(const int16_t* pcm, int samples, uint8_t* out, int out_cap) {
    if (!enc_) return -1;
    if (samples != kFrameSamples) {
        // 本项目固定 20ms 帧；变长帧会让抖动缓冲的水位计算复杂化
        LOGW(kTag, "unexpected frame samples: %d (want %d)", samples, kFrameSamples);
    }
    int n = opus_encode(enc_, pcm, samples, out, out_cap);
    if (n < 0) {
        LOGE(kTag, "encode failed: %s", opus_strerror(n));
        return n;
    }
    encoded_frames_++;

    // 滑动窗口统计编码后大小，用于实时码率估算
    enc_hist_sum_ -= enc_bytes_hist_[enc_hist_pos_];
    enc_bytes_hist_[enc_hist_pos_] = n;
    enc_hist_sum_ += n;
    enc_hist_pos_ = (enc_hist_pos_ + 1) % kHist;
    if (enc_hist_cnt_ < kHist) enc_hist_cnt_++;

    return n;
}

int OpusCodec::decode(const uint8_t* opus_data, int len, int16_t* pcm, int pcm_cap) {
    if (!dec_) return -1;

    const bool is_plc = (opus_data == nullptr || len <= 0);
    int n = opus_decode(dec_, opus_data, len, pcm, pcm_cap, is_plc ? 1 : 0);
    if (n < 0) {
        LOGE(kTag, "decode failed: %s (plc=%d, len=%d)", opus_strerror(n), int(is_plc), len);
        return n;
    }
    if (is_plc) plc_frames_++;
    else         decoded_frames_++;
    return n;
}

double OpusCodec::avg_encoded_bytes() const {
    if (enc_hist_cnt_ == 0) return 0;
    return double(enc_hist_sum_) / enc_hist_cnt_;
}

// ---------------- 多帧拼包 ----------------
// 目的：把 3 个 20ms 包合成一个 60ms 包发给服务器，减少包头开销。
// 格式（全部小端）：
//   [0]      u8  frame_count
//   [1..2]   u16 len_0
//   [3..]    payload_0
//            u16 len_1, payload_1, ...
int OpusCodec::pack_frames(const std::vector<std::vector<uint8_t>>& frames,
                           uint8_t* out, int out_cap) {
    if (frames.empty() || frames.size() > 255) return -1;

    int need = 1 + 2 * int(frames.size());
    for (const auto& f : frames) need += int(f.size());
    if (need > out_cap) {
        LOGE(kTag, "pack_frames overflow: need=%d cap=%d", need, out_cap);
        return -1;
    }

    uint8_t* p = out;
    *p++ = uint8_t(frames.size());
    for (const auto& f : frames) {
        put_u16_le(p, uint16_t(f.size())); p += 2;
        memcpy(p, f.data(), f.size());     p += f.size();
    }
    return int(p - out);
}

int OpusCodec::unpack_frames(const uint8_t* in, int len,
                             std::vector<std::vector<uint8_t>>* out_frames) {
    if (!in || len < 1) return -1;
    out_frames->clear();

    const uint8_t* p   = in;
    const uint8_t* end = in + len;

    int count = *p++;
    for (int i = 0; i < count; i++) {
        if (p + 2 > end) { LOGE(kTag, "unpack: truncated at frame %d header", i); return -1; }
        uint16_t flen = get_u16_le(p); p += 2;
        if (p + flen > end) { LOGE(kTag, "unpack: truncated at frame %d payload", i); return -1; }
        out_frames->emplace_back(p, p + flen);
        p += flen;
    }
    return int(p - in);
}

}  // namespace rtc
