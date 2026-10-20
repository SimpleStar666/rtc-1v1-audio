#include "media/video_pipeline.h"

#include <algorithm>
#include <cstring>

#include "common/logging.h"
#include "common/util.h"

namespace rtc {

namespace {
constexpr const char* kTag = "vpipe";

// 关键帧请求的最小间隔。
// 为什么需要限流：如果每个坏帧都发一次请求，网络差时会形成"请求风暴"，
// 反而把带宽挤占掉（WebRTC 里 PLI 也有限流，通常 500ms~1s）。
constexpr int64_t kMinKeyframeReqIntervalMs = 300;

// "屏幕还黑着"时用的紧急间隔。比常规间隔激进，但仍有下限，
// 避免对端还没就绪时把自己刷成请求风暴。
constexpr int64_t kUrgentKeyframeReqIntervalMs = 100;

// 重组超时：一帧的分片如果这么久还没凑齐，就放弃（后续帧已到，没必要等）
constexpr int64_t kReassembleTimeoutMs = 300;
}  // namespace

// 统一的"请求关键帧"入口（带限流）。
//
// 为什么要有这个函数：触发关键帧请求的场景有两个 ——
//   (a) 解码失败（参考帧缺失）
//   (b) 关键帧重组超时/缺片，整帧被丢弃
// 两条路径都要请求，但都必须限流。
//
// ★ 限流基准的初始化是个陷阱：
//   last_kf_req_ms_ 若初始为 0，而 now_ms() 是系统运行时长（可能几十万），
//   则 `now - 0 >= 300` 恒成立 → **限流首次就失效**。
//   这与坑 28（拥塞控制节流基准）是同一类错误：**初始化基准不能是"零值"**。
//   正确做法：第一次请求时用 0 作为哨兵，或把基准初始化到"很久以前"。
void VideoPipeline::request_keyframe_throttled() {
    if (!kf_req_fn_) return;
    const int64_t now = now_ms();

    // 间隔是动态的：
    //   - 还没解出过任何关键帧 → 100ms（紧急：屏幕是黑的，每帧都亏）
    //   - 已经在正常播放     → 300ms（够用，避免请求风暴）
    const int64_t interval = first_keyframe_decoded_
                                 ? kMinKeyframeReqIntervalMs
                                 : kUrgentKeyframeReqIntervalMs;

    // last_kf_req_ms_ == 0 表示"从未请求过"，此时直接放行并建立基准
    if (last_kf_req_ms_ != 0 && now - last_kf_req_ms_ < interval) {
        return;   // 限流中
    }
    last_kf_req_ms_ = now;
    {
        std::lock_guard<std::mutex> lk(stat_mutex_);
        rep_.keyframe_requests++;
    }
    LOGI(kTag, "requesting keyframe from peer (PLI)");
    kf_req_fn_();
}

VideoPipeline::VideoPipeline() = default;

VideoPipeline::~VideoPipeline() { stop(); }

bool VideoPipeline::init(const Config& cfg, VirtualVideoDevice* device) {
    cfg_ = cfg;
    device_ = device;
    if (!device_) {
        LOGE(kTag, "null device");
        return false;
    }

    Vp8Codec::Config vcfg;
    vcfg.width  = cfg_.width;
    vcfg.height = cfg_.height;
    vcfg.fps    = cfg_.fps;
    vcfg.bitrate_kbps = cfg_.bitrate_kbps;
    vcfg.keyframe_interval = cfg_.keyframe_interval;

    if (!codec_.init_encoder(vcfg)) {
        LOGE(kTag, "encoder init failed");
        return false;
    }
    if (!decoder_.init_decoder()) {
        LOGE(kTag, "decoder init failed");
        return false;
    }

    rep_ = Report{};
    LOGI(kTag, "init: %dx%d @%dfps, %dkbps, kf_interval=%d, max_frag_payload=%zu, session=%d",
         cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrate_kbps,
         cfg_.keyframe_interval, cfg_.max_fragment_payload, cfg_.session_id);
    return true;
}

void VideoPipeline::start() {
    if (running_) return;
    running_ = true;
    send_start_ms_ = now_ms();
    kbps_window_ms_ = send_start_ms_;
    send_thread_ = std::thread([this] { send_loop(); });

    // ★ 启动时主动请求一次关键帧（不等解码失败）。
    //   否则接收端要等到"下一个周期性关键帧"才能出画面
    //   （本实现 kf_max_dist = fps*2，即最多等 2 秒）。
    //   实测：不加这一行，A 端会固定丢约 15 帧（1 秒）。
    request_keyframe_now();

    LOGI(kTag, "started");
}

void VideoPipeline::request_keyframe_now() {
    // 绕过限流：启动时的一次请求必然要发出去。
    // 用 last_kf_req_ms_ = 0 作为"未请求过"的哨兵（与 throttled 版本一致）。
    if (!kf_req_fn_) return;
    last_kf_req_ms_ = 0;      // 清掉限流基准，确保这次一定发出
    request_keyframe_throttled();
}

void VideoPipeline::stop() {
    if (!running_) return;
    running_ = false;
    if (send_thread_.joinable()) send_thread_.join();
    LOGI(kTag, "stopped");
}

// ================= 发送：采集 → 编码 → 分片 → 发送 =================
void VideoPipeline::send_loop() {
    const int64_t frame_ms = 1000 / cfg_.fps;
    int64_t next = now_ms();

    while (running_) {
        YuvFrame frame;
        if (!device_->capture(&frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(stat_mutex_);
            rep_.frames_captured++;
        }

        EncodedFrame ef;
        if (!codec_.encode(frame, &ef) || ef.data.empty()) {
            continue;
        }

        const uint16_t frame_id = next_frame_id_++;
        const size_t total = ef.data.size();
        const size_t max_payload = cfg_.max_fragment_payload;
        const uint16_t frag_count =
            uint16_t((total + max_payload - 1) / max_payload);

        {
            std::lock_guard<std::mutex> lk(stat_mutex_);
            rep_.frames_encoded++;
            if (ef.is_keyframe) rep_.keyframes_sent++;
        }

        // 逐片封装并发出
        for (uint16_t i = 0; i < frag_count; i++) {
            const size_t off = size_t(i) * max_payload;
            const size_t len = std::min(max_payload, total - off);

            // 载荷 = 视频分片头(8B) + 分片数据
            std::vector<uint8_t> payload(VideoFragmentHeader::kSize + len);
            VideoFragmentHeader fh;
            fh.frame_id    = frame_id;
            fh.frag_index  = i;
            fh.frag_count  = frag_count;
            fh.is_keyframe = ef.is_keyframe;

            put_u16_be(payload.data() + 0, fh.frame_id);
            put_u16_be(payload.data() + 2, fh.frag_index);
            put_u16_be(payload.data() + 4, fh.frag_count);
            payload[6] = fh.is_keyframe ? 1 : 0;
            payload[7] = 0;
            memcpy(payload.data() + VideoFragmentHeader::kSize,
                   ef.data.data() + off, len);

            PacketHeader h;
            h.session_id = uint16_t(cfg_.session_id);
            h.seq        = uint16_t(frame_id * 1000 + i);   // 简化：保证唯一即可
            h.timestamp  = ef.timestamp;
            h.phase      = Phase::kBody;
            h.flags      = ef.is_keyframe ? kFlagNone : kFlagNone;
            h.trans_id   = 0;
            h.payload_len = uint16_t(payload.size());
            h.marker     = (i == frag_count - 1);   // RTP M 位：帧的最后一个分片

            if (send_fn_) send_fn_(h, payload);

            std::lock_guard<std::mutex> lk(stat_mutex_);
            rep_.fragments_sent++;
            rep_.bytes_sent += payload.size() + PacketHeader::kSize;
            kbps_window_bytes_ += payload.size() + PacketHeader::kSize;
        }

        // 每 500ms 更新一次实测码率
        const int64_t now = now_ms();
        {
            std::lock_guard<std::mutex> lk(stat_mutex_);
            if (now - kbps_window_ms_ >= 500) {
                const double secs = double(now - kbps_window_ms_) / 1000.0;
                rep_.actual_kbps = double(kbps_window_bytes_) * 8.0 / secs / 1000.0;
                rep_.fps_out = double(rep_.frames_encoded) * 1000.0 /
                               double(now - send_start_ms_ + 1);
                kbps_window_bytes_ = 0;
                kbps_window_ms_ = now;
            }
        }

        next += frame_ms;
        const int64_t t = now_ms();
        if (next > t) std::this_thread::sleep_for(std::chrono::milliseconds(next - t));
        else next = t;
    }
}

// ================= 接收：分片重组 → 解码 =================
void VideoPipeline::on_video_fragment(const PacketHeader& h,
                                      const std::vector<uint8_t>& payload) {
    if (payload.size() < VideoFragmentHeader::kSize) return;

    const uint16_t frame_id   = get_u16_be(payload.data() + 0);
    const uint16_t frag_index = get_u16_be(payload.data() + 2);
    const uint16_t frag_count = get_u16_be(payload.data() + 4);
    const bool     is_key     = payload[6] != 0;

    if (frag_count == 0 || frag_index >= frag_count) {
        LOGW(kTag, "bad fragment: idx=%u/%u frame=%u", frag_index, frag_count, frame_id);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(stat_mutex_);
        rep_.fragments_recv++;
    }

    bool complete = false;
    bool need_kf_req = false;
    {
        std::lock_guard<std::mutex> lk(rx_mutex_);

        // 清理超时的重组帧（对端已经不发了，等下去没意义）
        const int64_t now = now_ms();
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (now - it->second.first_ms > kReassembleTimeoutMs) {
                // 超时未凑齐：如果这帧是关键帧，说明关键帧丢了 → 后续都解不了
                if (it->second.is_keyframe) {
                    LOGW(kTag, "keyframe %u incomplete (%zu/%u), requesting new one",
                         it->first, it->second.got.size(), it->second.frag_count);
                    lost_keyframe_until_.insert(it->first);
                    need_kf_req = true;   // 关键帧缺片 → 必须请求
                }
                std::lock_guard<std::mutex> lk2(stat_mutex_);
                rep_.frames_dropped++;
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }

        auto& pf = pending_[frame_id];
        if (pf.frag_count == 0) {
            pf.frag_count = frag_count;
            pf.is_keyframe = is_key;
            pf.first_ms = now;
            pf.frag_len.assign(frag_count, 0);
        }

        // 重复分片直接忽略
        if (pf.got.count(frag_index)) {
            // 已收到过
        } else {
            pf.got.insert(frag_index);
            const uint32_t flen = uint32_t(payload.size() - VideoFragmentHeader::kSize);
            pf.frag_len[frag_index] = flen;

            // ★ 关键修复：必须按 frag_index 写到"该片应该在的位置"，
            //   不能用 `insert(end(), ...)` 按到达顺序追加。
            //
            //   为什么：UDP 不保证分片按序到达。若第 2 片比第 1 片先到，
            //   按到达顺序追加就会把第 2 片的字节放在第 1 片前面，
            //   拼出来的码流字节顺序错乱 → 解码器报
            //   "Bitstream not supported by this decoder"。
            //
            //   这是本项目实测到的真 bug：5 轮通话里有 2 轮 A 端丢 29 帧，
            //   原因正是乱序分片被拼错。见 docs/02-pitfalls.md 坑 45。
            //
            //   正确做法与 RTP 一致：按 offset 定位写入（等价于 RTP 的
            //   fragment offset 语义），片内偏移 = frag_index × max_payload。
            const size_t need = size_t(frag_index) * cfg_.max_fragment_payload + flen;
            if (pf.data.size() < need) pf.data.resize(need, 0);
            memcpy(pf.data.data() + size_t(frag_index) * cfg_.max_fragment_payload,
                   payload.data() + VideoFragmentHeader::kSize, flen);
        }

        if (pf.got.size() == pf.frag_count) complete = true;
    }

    // 关键帧缺片 → 请求新关键帧（在锁外调用，避免持锁回调）
    if (need_kf_req) request_keyframe_throttled();

    if (complete) assemble_and_decode(frame_id);
}

void VideoPipeline::assemble_and_decode(uint16_t frame_id) {
    std::vector<uint8_t> data;
    bool is_key = false;
    {
        std::lock_guard<std::mutex> lk(rx_mutex_);
        auto it = pending_.find(frame_id);
        if (it == pending_.end()) return;
        data = std::move(it->second.data);

        // 按最后一片的实际长度截断：因为按 offset 定位写入时，
        // 缓冲区是按"片索引 × 固定片长"撑开的，末片通常不满，
        // 后面会留一段补零的空白，必须裁掉，否则码流尾部有多余字节。
        const uint16_t last_idx = uint16_t(it->second.frag_count - 1);
        const size_t real_len =
            size_t(last_idx) * cfg_.max_fragment_payload + it->second.frag_len[last_idx];
        if (data.size() > real_len) data.resize(real_len);

        is_key = it->second.is_keyframe;
        pending_.erase(it);

        {
            std::lock_guard<std::mutex> lk2(stat_mutex_);
            rep_.frames_recv++;
            if (is_key) rep_.keyframes_recv++;
        }

        // ★ 视频特有的判断：如果之前丢过关键帧，且当前帧不是关键帧，
        //   那么这一帧必然解不出来（参考帧链断了）。
        //   直接丢弃，并请求关键帧 —— 而不是浪费 CPU 去解一个必然失败的帧。
        if (!is_key && !lost_keyframe_until_.empty()) {
            const uint16_t last_lost = *lost_keyframe_until_.rbegin();
            if (int16_t(frame_id - last_lost) > 0) {
                std::lock_guard<std::mutex> lk2(stat_mutex_);
                rep_.frames_dropped++;
                // 落在这里说明还在等关键帧，本次不解码
                // （关键帧请求由 on_video_fragment 的超时路径或解码失败路径触发）
                return;
            }
        }
    }

    YuvFrame out;
    if (!decoder_.decode(data.data(), data.size(), &out)) {
        {
            std::lock_guard<std::mutex> lk(stat_mutex_);
            rep_.frames_dropped++;
        }
        // 解码失败：极可能参考帧缺失 → 请求关键帧（统一入口，自带限流）
        request_keyframe_throttled();
        return;
    }

    {
        std::lock_guard<std::mutex> lk(stat_mutex_);
        rep_.frames_decoded++;
        rep_.fps_in = double(rep_.frames_decoded) * 1000.0 /
                      double(now_ms() - send_start_ms_ + 1);
    }

    // 成功解出关键帧后：清掉"等待关键帧"标记，并记录"已经有画面了"
    if (is_key) {
        first_keyframe_decoded_ = true;
        std::lock_guard<std::mutex> lk(rx_mutex_);
        lost_keyframe_until_.clear();
    }

    if (device_) device_->play(out);
}

void VideoPipeline::on_keyframe_request() {
    // 对端丢帧了，要求我们下一帧发关键帧（对应 RTCP PLI 的处理）
    LOGI(kTag, "peer requested keyframe (PLI)");
    codec_.request_keyframe();
}

void VideoPipeline::set_target_bitrate_kbps(int kbps) {
    if (codec_.set_bitrate(kbps)) {
        std::lock_guard<std::mutex> lk(stat_mutex_);
        cfg_.bitrate_kbps = kbps;
    }
}

VideoPipeline::Report VideoPipeline::report() const {
    std::lock_guard<std::mutex> lk1(stat_mutex_);
    std::lock_guard<std::mutex> lk2(rx_mutex_);
    Report r = rep_;
    r.pending_frames = pending_.size();
    return r;
}

}  // namespace rtc
