#include "media/audio_pipeline.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>

#include "common/logging.h"
#include "common/util.h"

namespace rtc {

namespace {
constexpr const char* kTag = "pipeline";
}

AudioPipeline::~AudioPipeline() {
    stop_sending();
    stop_playout();
}

bool AudioPipeline::init(const Config& cfg, VirtualAudioDevice* device) {
    cfg_ = cfg;
    device_ = device;

    if (!codec_.init(cfg.bitrate_bps, /*complexity=*/0, /*vbr=*/false, cfg.use_inband_fec)) {
        LOGE(kTag, "codec init failed");
        return false;
    }

    JitterBuffer::Config jcfg;
    jcfg.target_delay_ms = cfg.jb_target_ms;
    jcfg.frame_ms = cfg.frame_ms;
    // 注意：不能写 `jb_ = JitterBuffer(jcfg)` —— JitterBuffer 内含 std::mutex，
    // 而 mutex 删除了拷贝/移动，所以 JitterBuffer 是**不可赋值**类型。
    // jb_ 是成员、早已构造，配置又要等到 init() 才知道，
    // 所以改用 set_config() 在**原有对象上**重新配置（不重建对象）。
    // 见 docs/02-pitfalls.md 坑 39。
    jb_.set_config(jcfg);
    jb_.reset();

    LOGI(kTag, "init: session=%d, bitrate=%dbps, frame=%dms, aggregate=%d frames, jb_target=%dms",
         cfg.session_id, cfg.bitrate_bps, cfg.frame_ms, cfg.aggregate_frames, cfg.jb_target_ms);
    return true;
}

void AudioPipeline::set_send_fn(SendFn fn) { send_fn_ = std::move(fn); }

// ---------------- 发送方向 ----------------
void AudioPipeline::start_sending() {
    if (sending_) return;
    sending_ = true;
    send_thread_ = std::thread([this] { send_loop(); });
    LOGI(kTag, "sending started");
}

void AudioPipeline::stop_sending() {
    if (!sending_) return;
    sending_ = false;
    send_cv_.notify_all();      // 唤醒可能阻塞在 wait 上的发送线程
    if (send_thread_.joinable()) send_thread_.join();
    LOGI(kTag, "sending stopped");
}

void AudioPipeline::send_loop() {
    const int frame_samples = OpusCodec::kFrameSamples;

    // 注册采集回调
    device_->start_capture([this, frame_samples](const int16_t* data, int samples, int64_t /*ts*/) {
        if (!sending_) return;
        if (samples != frame_samples) return;

        std::vector<uint8_t> enc(OpusCodec::kMaxFrameBytes);
        int n = codec_.encode(data, samples, enc.data(), int(enc.size()));
        if (n < 0) {
            std::lock_guard<std::mutex> lk(stat_mutex_);
            report_.encode_errors++;
            return;
        }
        enc.resize(size_t(n));

        std::lock_guard<std::mutex> lk(send_mutex_);
        pending_frames_.push_back(std::move(enc));
        send_cv_.notify_one();
    });

    // 主循环：攒够 aggregate_frames 就发一个包。
    //
    // 注意：不能把"固定超时"当成节拍依据。
    // 早期版本用 `cv.wait_for(50ms)` 轮询，且"还没攒够"时直接 continue，
    // 结果每帧都要等一次 50ms 超时，累积约 2% 帧率损失（5 秒实测 245 帧而非 250 帧）。
    // 正确做法：精确等待"帧数达到聚合数"这一事件。见 docs/02-pitfalls.md 坑 14。
    while (sending_) {
        std::vector<std::vector<uint8_t>> batch;
        {
            std::unique_lock<std::mutex> lk(send_mutex_);
            send_cv_.wait(lk, [this] {
                return pending_frames_.size() >= size_t(cfg_.aggregate_frames) || !sending_;
            });
            if (pending_frames_.empty()) continue;
            if (pending_frames_.size() >= size_t(cfg_.aggregate_frames)) {
                batch.assign(pending_frames_.begin(),
                             pending_frames_.begin() + cfg_.aggregate_frames);
                pending_frames_.erase(pending_frames_.begin(),
                                      pending_frames_.begin() + cfg_.aggregate_frames);
            } else {
                batch.swap(pending_frames_);   // 退出时把剩余的发出去
            }
        }
        if (batch.empty()) continue;

        // 拼包：多帧进一个载荷（与服务器 60ms 帧长对齐）
        std::vector<uint8_t> payload(OpusCodec::kMaxPacketBytes);
        int plen = OpusCodec::pack_frames(batch, payload.data(), int(payload.size()));
        if (plen < 0) {
            LOGE(kTag, "pack_frames failed");
            continue;
        }
        payload.resize(size_t(plen));

        PacketHeader h;
        h.session_id  = uint16_t(cfg_.session_id);
        h.seq         = seq_++;
        h.timestamp   = timestamp_;
        h.phase       = Phase::kBody;
        h.flags       = kFlagNone;
        h.trans_id    = cur_trans_id_;
        h.payload_len = uint16_t(payload.size());

        // 时间戳按"本包实际包含的样本数"推进
        timestamp_ += uint32_t(frame_samples * batch.size());

        if (send_fn_) send_fn_(h, payload);

        {
            std::lock_guard<std::mutex> lk(stat_mutex_);
            report_.sent_packets++;
            report_.sent_bytes += payload.size() + PacketHeader::kSize;
        }
        LOGD(kTag, "sent: seq=%u ts=%u frames=%zu bytes=%zu trans_id=%u",
             h.seq, h.timestamp, batch.size(), payload.size(), h.trans_id);
    }

    device_->stop_capture();
}

// ---------------- 接收方向 ----------------
void AudioPipeline::on_media_packet(const PacketHeader& h, const std::vector<uint8_t>& payload) {
    // 打断过滤：被打断的事务，其后续在途包必须丢弃，否则会串音。
    // 对应简历中 "处理打断后丢弃旧流" 的实现。
    if (has_interrupt_ && h.trans_id == interrupted_trans_id_) {
        if (!(h.flags & kFlagUninterruptible)) {
            std::lock_guard<std::mutex> lk(stat_mutex_);
            report_.interrupted_drops++;
            LOGD(kTag, "drop interrupted packet: seq=%u trans_id=%u", h.seq, h.trans_id);
            return;
        }
        LOGD(kTag, "uninterruptible packet allowed: seq=%u", h.seq);
    }

    {
        std::lock_guard<std::mutex> lk(stat_mutex_);
        report_.recv_packets++;
        report_.recv_bytes += payload.size() + PacketHeader::kSize;
    }

    // 拆包：一个包里可能有多帧
    std::vector<std::vector<uint8_t>> frames;
    if (OpusCodec::unpack_frames(payload.data(), int(payload.size()), &frames) < 0) {
        LOGW(kTag, "unpack_frames failed: len=%zu", payload.size());
        return;
    }

    // 每帧作为一个独立的"播放单位"入抖动缓冲（简化：用递增 seq 标识每帧）
    for (size_t i = 0; i < frames.size(); i++) {
        const uint16_t fseq = uint16_t(h.seq * frames.size() + i);
        jb_.push(fseq, frames[i].data(), frames[i].size());
    }
}

void AudioPipeline::start_playout() {
    if (playout_) return;
    playout_ = true;
    playout_thread_ = std::thread([this] { playout_loop(); });
    LOGI(kTag, "playout started");
}

void AudioPipeline::stop_playout() {
    if (!playout_) return;
    playout_ = false;
    if (playout_thread_.joinable()) playout_thread_.join();
    LOGI(kTag, "playout stopped");
}

void AudioPipeline::playout_loop() {
    const int frame_samples = OpusCodec::kFrameSamples;
    std::vector<int16_t> pcm(frame_samples);
    std::vector<uint8_t> payload;

    auto next_tick = std::chrono::steady_clock::now();
    const auto frame_dur = std::chrono::milliseconds(cfg_.frame_ms);

    while (playout_) {
        // 只有达到目标水位才开始播（避免一开始就 underflow）
        if (jb_.ready_to_play(now_ms())) {
            payload.clear();
            if (jb_.pop(&payload, now_ms())) {
                int n;
                if (payload.empty()) {
                    // PLC：用解码器的丢包隐藏补一帧，保持播放连续
                    n = codec_.decode(nullptr, 0, pcm.data(), frame_samples);
                    if (n < 0) {
                        std::lock_guard<std::mutex> lk(stat_mutex_);
                        report_.decode_errors++;
                        n = frame_samples;
                        std::fill(pcm.begin(), pcm.end(), 0);
                    }
                } else {
                    n = codec_.decode(payload.data(), int(payload.size()), pcm.data(), frame_samples);
                    if (n < 0) {
                        std::lock_guard<std::mutex> lk(stat_mutex_);
                        report_.decode_errors++;
                        continue;
                    }
                }
                if (device_) device_->play(pcm.data(), n);
            }
        }

        // 按播放时钟推进（模拟声卡的固定消费节奏）
        next_tick += frame_dur;
        std::this_thread::sleep_until(next_tick);
    }
}

// ---------------- 打断 ----------------
void AudioPipeline::interrupt(uint16_t trans_id) {
    interrupted_trans_id_ = trans_id;
    has_interrupt_ = true;
    LOGI(kTag, "interrupt set: trans_id=%u (in-flight data will be dropped)", trans_id);
}

void AudioPipeline::clear_interrupt() {
    has_interrupt_ = false;
    interrupted_trans_id_ = 0;
    LOGI(kTag, "interrupt cleared");
}

bool AudioPipeline::is_interrupted(uint16_t trans_id) const {
    return has_interrupt_ && trans_id == interrupted_trans_id_;
}

// ---------------- 统计 ----------------
int AudioPipeline::bitrate_kbps_locked() const {
    // 调用方必须已持有 stat_mutex_（或明确知道不需要保护）
    const double bytes_per_frame = codec_.avg_encoded_bytes();
    const double frames_per_sec = 1000.0 / cfg_.frame_ms;
    return int(bytes_per_frame * frames_per_sec * 8.0 / 1000.0);
}

int AudioPipeline::bitrate_kbps() const {
    std::lock_guard<std::mutex> lk(stat_mutex_);
    return bitrate_kbps_locked();
}

void AudioPipeline::set_target_bitrate_bps(int bps) {
    // ★ 曾经这里只打日志、不真正生效（"简化：仅记录"）。
    //   那等于拥塞控制算出了目标码率却没人执行 —— 自适应成了空转。
    //   Opus 支持运行时改码率（OPUS_SET_BITRATE_REQUEST），直接调即可，
    //   不需要重建编码器。
    //   见 docs/02-pitfalls.md 坑 41。
    if (codec_.set_bitrate(bps)) {
        LOGI(kTag, "target bitrate -> %d bps (applied to encoder)", bps);
    } else {
        LOGW(kTag, "set_target_bitrate_bps(%d) failed", bps);
    }
}

AudioPipeline::Report AudioPipeline::report() const {
    std::lock_guard<std::mutex> lk(stat_mutex_);
    Report r = report_;
    r.jb = jb_.snapshot();
    r.cur_bitrate_kbps = bitrate_kbps_locked();   // 用不加锁版本，避免自死锁
    return r;
}

}  // namespace rtc
