#include "media/vp8_codec.h"

#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>
#include <vpx/vpx_image.h>

#include <cstring>

#include "common/logging.h"

namespace rtc {

namespace {
constexpr const char* kTag = "vp8";
}  // namespace

// ---------------- YuvFrame ----------------
void YuvFrame::allocate(int w, int h) {
    width  = w;
    height = h;
    y.assign(size_t(w) * h, 0);
    u.assign(size_t(w / 2) * (h / 2), 128);   // 色度中性值 128
    v.assign(size_t(w / 2) * (h / 2), 128);
}

size_t YuvFrame::size_bytes() const {
    if (empty()) return 0;
    return size_t(width) * height + 2 * size_t(width / 2) * (height / 2);
}

// ---------------- Vp8Codec ----------------
Vp8Codec::Vp8Codec() = default;

Vp8Codec::~Vp8Codec() {
    if (enc_) {
        vpx_codec_destroy(static_cast<vpx_codec_ctx_t*>(enc_));
        delete static_cast<vpx_codec_ctx_t*>(enc_);
        enc_ = nullptr;
    }
    if (dec_) {
        vpx_codec_destroy(static_cast<vpx_codec_ctx_t*>(dec_));
        delete static_cast<vpx_codec_ctx_t*>(dec_);
        dec_ = nullptr;
    }
}

// ================= 编码器 =================
bool Vp8Codec::init_encoder(const Config& cfg) {
    cfg_ = cfg;

    auto* ctx = new vpx_codec_ctx_t();
    vpx_codec_enc_cfg_t ecfg;
    if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &ecfg, 0) != VPX_CODEC_OK) {
        LOGE(kTag, "enc_config_default failed");
        delete ctx;
        return false;
    }

    ecfg.g_w = unsigned(cfg_.width);
    ecfg.g_h = unsigned(cfg_.height);
    ecfg.g_timebase.num = 1;
    ecfg.g_timebase.den = cfg_.fps;
    ecfg.rc_target_bitrate = unsigned(cfg_.bitrate_kbps);
    ecfg.g_threads = unsigned(cfg_.threads);

    // CBR：与音频一致，码率可预测，便于拥塞控制建模
    ecfg.rc_end_usage = VPX_CBR;
    ecfg.rc_min_quantizer = unsigned(cfg_.min_quantizer);
    ecfg.rc_max_quantizer = unsigned(cfg_.max_quantizer);

    // 关键帧策略：自动 + 限制间隔。
    // 视频必须有周期性关键帧 —— 否则中途加入或丢包后永远无法恢复。
    ecfg.kf_mode = VPX_KF_AUTO;
    ecfg.kf_min_dist = 0;
    ecfg.kf_max_dist = unsigned(cfg_.keyframe_interval);

    if (vpx_codec_enc_init(ctx, vpx_codec_vp8_cx(), &ecfg, 0) != VPX_CODEC_OK) {
        LOGE(kTag, "enc_init failed: %s", vpx_codec_error(ctx));
        delete ctx;
        return false;
    }

    // 实时编码优化：0 = 最快（质量换速度），适合 RTC
    vpx_codec_control(ctx, VP8E_SET_CPUUSED, 8);
    // 允许帧内刷新（丢包时用 INTRA 模式恢复，而不是整帧关键帧）
    vpx_codec_control(ctx, VP8E_SET_NOISE_SENSITIVITY, 0);
    vpx_codec_control(ctx, VP8E_SET_STATIC_THRESHOLD, 0);

    enc_ = ctx;
    enc_inited_ = true;
    LOGI(kTag, "encoder init ok: %dx%d @%dfps, %dkbps, kf_max=%d, threads=%d",
         cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrate_kbps,
         cfg_.keyframe_interval, cfg_.threads);
    return true;
}

void Vp8Codec::request_keyframe() {
    // 对应 WebRTC 的 PLI（Picture Loss Indication）响应：
    // 收到对端"我丢帧了"的请求后，下一帧强制编码为关键帧。
    force_keyframe_ = true;
    LOGI(kTag, "keyframe requested (next frame will be key)");
}

bool Vp8Codec::encode(const YuvFrame& frame, EncodedFrame* out) {
    if (!enc_inited_ || !enc_) return false;
    if (frame.empty()) return false;

    auto* ctx = static_cast<vpx_codec_ctx_t*>(enc_);

    // 包装成 vpx_image_t（不拷贝数据，直接引用 YuvFrame 的内存）
    vpx_image_t img;
    if (!vpx_img_wrap(&img, VPX_IMG_FMT_I420,
                      unsigned(frame.width), unsigned(frame.height), 1,
                      const_cast<uint8_t*>(frame.y.data()))) {
        LOGE(kTag, "vpx_img_wrap failed");
        return false;
    }
    // wrap 只设置了 Y 平面，需要手动把 U/V 指过去
    img.planes[VPX_PLANE_U] = const_cast<uint8_t*>(frame.u.data());
    img.planes[VPX_PLANE_V] = const_cast<uint8_t*>(frame.v.data());
    img.stride[VPX_PLANE_U] = frame.width / 2;
    img.stride[VPX_PLANE_V] = frame.width / 2;

    const vpx_enc_frame_flags_t flags = force_keyframe_ ? VPX_EFLAG_FORCE_KF : 0;
    force_keyframe_ = false;

    const vpx_codec_err_t rc = vpx_codec_encode(
        ctx, &img, frame_index_++, 1, flags, VPX_DL_REALTIME);
    if (rc != VPX_CODEC_OK) {
        LOGE(kTag, "encode failed: %s", vpx_codec_error(ctx));
        return false;
    }

    // 取出码流
    vpx_codec_iter_t iter = nullptr;
    const vpx_codec_cx_pkt_t* pkt;
    bool got = false;
    while ((pkt = vpx_codec_get_cx_data(ctx, &iter)) != nullptr) {
        if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;

        out->data.assign(static_cast<uint8_t*>(pkt->data.frame.buf),
                         static_cast<uint8_t*>(pkt->data.frame.buf) + pkt->data.frame.sz);
        out->is_keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
        out->timestamp = uint32_t(pkt->data.frame.pts);
        got = true;

        stats_.frames_encoded++;
        stats_.bytes_out += pkt->data.frame.sz;
        if (out->is_keyframe) {
            stats_.keyframes++;
            stats_.avg_keyframe_bytes =
                0.9 * stats_.avg_keyframe_bytes + 0.1 * double(pkt->data.frame.sz);
        }
        stats_.avg_frame_bytes =
            0.9 * stats_.avg_frame_bytes + 0.1 * double(pkt->data.frame.sz);
        break;   // 单层编码，一帧只有一个包
    }
    return got;
}

bool Vp8Codec::set_bitrate(int kbps) {
    if (!enc_inited_ || !enc_) return false;
    if (kbps < 50)   kbps = 50;
    if (kbps > 4000) kbps = 4000;
    auto* ctx = static_cast<vpx_codec_ctx_t*>(enc_);
    if (vpx_codec_control(ctx, VP8E_SET_CQ_LEVEL, 0) != VPX_CODEC_OK) {
        // CQ 控制失败不算致命，继续改码率
    }
    // libvpx 改码率要重新提交 rc_target_bitrate
    vpx_codec_enc_cfg_t ecfg;
    if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &ecfg, 0) != VPX_CODEC_OK) {
        return false;
    }
    ecfg.g_w = unsigned(cfg_.width);
    ecfg.g_h = unsigned(cfg_.height);
    ecfg.g_timebase.num = 1;
    ecfg.g_timebase.den = cfg_.fps;
    ecfg.rc_target_bitrate = unsigned(kbps);
    ecfg.rc_end_usage = VPX_CBR;
    ecfg.rc_min_quantizer = unsigned(cfg_.min_quantizer);
    ecfg.rc_max_quantizer = unsigned(cfg_.max_quantizer);
    ecfg.kf_mode = VPX_KF_AUTO;
    ecfg.kf_min_dist = 0;
    ecfg.kf_max_dist = unsigned(cfg_.keyframe_interval);

    if (vpx_codec_enc_config_set(ctx, &ecfg) != VPX_CODEC_OK) {
        LOGE(kTag, "enc_config_set(bitrate=%d) failed", kbps);
        return false;
    }
    cfg_.bitrate_kbps = kbps;
    LOGD(kTag, "bitrate adapted to %d kbps", kbps);
    return true;
}

// ================= 解码器 =================
bool Vp8Codec::init_decoder() {
    auto* ctx = new vpx_codec_ctx_t();
    vpx_codec_dec_cfg_t dcfg;
    memset(&dcfg, 0, sizeof(dcfg));
    dcfg.w = unsigned(cfg_.width);
    dcfg.h = unsigned(cfg_.height);
    dcfg.threads = unsigned(cfg_.threads);

    if (vpx_codec_dec_init(ctx, vpx_codec_vp8_dx(), &dcfg, 0) != VPX_CODEC_OK) {
        LOGE(kTag, "dec_init failed: %s", vpx_codec_error(ctx));
        delete ctx;
        return false;
    }
    dec_ = ctx;
    dec_inited_ = true;
    LOGI(kTag, "decoder init ok: %dx%d", cfg_.width, cfg_.height);
    return true;
}

bool Vp8Codec::decode(const uint8_t* data, size_t len, YuvFrame* out) {
    if (!dec_inited_ || !dec_ || !data || len == 0) return false;

    auto* ctx = static_cast<vpx_codec_ctx_t*>(dec_);
    if (vpx_codec_decode(ctx, data, unsigned(len), nullptr, 0) != VPX_CODEC_OK) {
        stats_.decode_errors++;
        LOGD(kTag, "decode failed: %s", vpx_codec_error(ctx));
        return false;
    }

    vpx_codec_iter_t iter = nullptr;
    vpx_image_t* img = vpx_codec_get_frame(ctx, &iter);
    if (!img) {
        // 不是错误：可能是不可见帧（invisible frame），只更新参考帧不产出图像
        return false;
    }

    const int w = int(img->d_w);
    const int h = int(img->d_h);
    out->allocate(w, h);
    // 按 stride 逐行拷贝（stride 可能大于宽度，不能整块 memcpy）
    for (int r = 0; r < h; r++) {
        memcpy(out->y.data() + size_t(r) * w,
               img->planes[VPX_PLANE_Y] + size_t(r) * img->stride[VPX_PLANE_Y], size_t(w));
    }
    for (int r = 0; r < h / 2; r++) {
        memcpy(out->u.data() + size_t(r) * (w / 2),
               img->planes[VPX_PLANE_U] + size_t(r) * img->stride[VPX_PLANE_U], size_t(w / 2));
        memcpy(out->v.data() + size_t(r) * (w / 2),
               img->planes[VPX_PLANE_V] + size_t(r) * img->stride[VPX_PLANE_V], size_t(w / 2));
    }

    stats_.frames_decoded++;
    return true;
}

void Vp8Codec::reset_stats() { stats_ = Stats{}; }

}  // namespace rtc
