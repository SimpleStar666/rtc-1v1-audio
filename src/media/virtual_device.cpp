#include "media/virtual_device.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include "common/logging.h"
#include "common/util.h"

namespace rtc {

namespace {
constexpr const char* kTag = "adm";
// 合成正弦波的频率（源文件缺失时的兜底音频）
constexpr double kSineHz = 440.0;
}

struct VirtualAudioDevice::Impl {
    Config cfg;
    bool   opened = false;

    // ---- 采集侧 ----
    std::vector<int16_t> source;       // 文件读入的 PCM
    size_t               src_pos = 0;
    std::thread          cap_thread;
    std::atomic<bool>    cap_running{false};
    CaptureCallback      cap_cb;
    uint64_t             captured = 0;

    // ---- 播放侧 ----
    std::mutex           play_mutex;
    std::vector<int16_t> play_buf;
    size_t               play_cap = 16000 * 2;  // 2 秒容量
    uint64_t             played   = 0;          // 累计消费的样本数
    uint64_t             underruns = 0;
    std::vector<int16_t> play_sink;             // 落盘累积
    int64_t              play_clock_ms = 0;     // 播放时钟（模拟设备消耗）

    // 播放消费线程（模拟 DMA 按采样率取走数据）
    std::thread          play_thread;
    std::atomic<bool>    play_running{false};

    ~Impl() {
        stop_capture();
        stop_playout();
    }

    void stop_capture() {
        cap_running = false;
        if (cap_thread.joinable()) cap_thread.join();
    }

    void stop_playout() {
        play_running = false;
        if (play_thread.joinable()) play_thread.join();
    }
};

VirtualAudioDevice::VirtualAudioDevice(const Config& cfg) : impl_(new Impl()) {
    impl_->cfg = cfg;
}

VirtualAudioDevice::~VirtualAudioDevice() {
    close();
    delete impl_;
}

bool VirtualAudioDevice::open() {
    if (impl_->opened) return true;

    if (!impl_->cfg.source_path.empty()) {
        if (!read_pcm_s16le(impl_->cfg.source_path, &impl_->source)) {
            LOGW(kTag, "cannot read source '%s', fallback to sine wave",
                 impl_->cfg.source_path.c_str());
        } else {
            LOGI(kTag, "loaded source '%s': %zu samples (%.1f s)",
                 impl_->cfg.source_path.c_str(), impl_->source.size(),
                 double(impl_->source.size()) / impl_->cfg.sample_rate);
        }
    }

    impl_->play_buf.resize(impl_->play_cap, 0);
    impl_->opened = true;

    LOGI(kTag, "device opened: %dHz/%dch, %dms frame, source=%s sink=%s",
         impl_->cfg.sample_rate, impl_->cfg.channels, impl_->cfg.frame_ms,
         impl_->source.empty() ? "<sine>" : "file",
         impl_->cfg.sink_path.empty() ? "<none>" : impl_->cfg.sink_path.c_str());
    return true;
}

void VirtualAudioDevice::close() {
    if (!impl_->opened) return;
    stop_capture();
    stop_playout();   // 先停消费线程，再落盘，避免数据竞争

    // 播放数据落盘，便于离线分析（波形对比 / FFT）
    if (!impl_->cfg.sink_path.empty() && !impl_->play_sink.empty()) {
        if (write_pcm_s16le(impl_->cfg.sink_path, impl_->play_sink.data(), impl_->play_sink.size())) {
            LOGI(kTag, "playback written to '%s': %zu samples (%.1f s)",
                 impl_->cfg.sink_path.c_str(), impl_->play_sink.size(),
                 double(impl_->play_sink.size()) / impl_->cfg.sample_rate);
        }
    }
    impl_->opened = false;
}

void VirtualAudioDevice::start_capture(CaptureCallback cb) {
    if (!impl_->opened) {
        LOGE(kTag, "start_capture before open()");
        return;
    }
    impl_->cap_cb = std::move(cb);
    impl_->cap_running = true;

    const int frame_samples = impl_->cfg.sample_rate * impl_->cfg.frame_ms / 1000;
    const int frame_us = impl_->cfg.frame_ms * 1000;

    impl_->cap_thread = std::thread([this, frame_samples, frame_us]() {
        Impl* d = impl_;
        std::vector<int16_t> frame(frame_samples);
        auto next_tick = std::chrono::steady_clock::now();

        while (d->cap_running) {
            // ---- 产出这一帧 ----
            if (!d->source.empty()) {
                // 文件回放：读满一帧；到文件尾则补零（静音）
                for (int i = 0; i < frame_samples; i++) {
                    frame[i] = (d->src_pos < d->source.size()) ? d->source[d->src_pos++] : 0;
                }
            } else {
                // 正弦波合成：相位连续，避免爆音
                static double phase = 0.0;
                const double step = 2.0 * M_PI * kSineHz / d->cfg.sample_rate;
                for (int i = 0; i < frame_samples; i++) {
                    frame[i] = int16_t(std::sin(phase) * 8000);
                    phase += step;
                    if (phase > 2 * M_PI) phase -= 2 * M_PI;
                }
            }

            if (d->cap_cb) d->cap_cb(frame.data(), frame_samples, now_ms());
            d->captured += frame_samples;

            // ---- 按真实采样率节拍推进（关键：模拟硬件时钟）----
            // 早期用 sleep_for(20ms) 会累积漂移，导致实验室里的抖动统计失真，见 docs/02-pitfalls.md
            next_tick += std::chrono::microseconds(frame_us);
            std::this_thread::sleep_until(next_tick);
        }
    });
}

void VirtualAudioDevice::stop_capture() {
    impl_->stop_capture();
}

void VirtualAudioDevice::start_playout() {
    if (!impl_->opened) {
        LOGE(kTag, "start_playout before open()");
        return;
    }
    if (impl_->play_running) return;
    impl_->play_running = true;

    // 模拟 DMA：按采样率从缓冲里"取走"数据。
    // 消费粒度用 10ms（160 样本 @16kHz），贴近真实声卡的 period 大小。
    const int chunk = impl_->cfg.sample_rate / 100;   // 10ms
    const int chunk_us = 10 * 1000;

    impl_->play_thread = std::thread([this, chunk, chunk_us]() {
        Impl* d = impl_;
        auto next_tick = std::chrono::steady_clock::now();

        while (d->play_running) {
            {
                std::lock_guard<std::mutex> lk(d->play_mutex);
                if (d->play_buf.size() >= size_t(chunk)) {
                    // 真正"消费"掉——从队列移除，模拟硬件取走
                    d->play_buf.erase(d->play_buf.begin(), d->play_buf.begin() + chunk);
                    d->played += chunk;
                    d->play_clock_ms += int64_t(chunk) * 1000 / d->cfg.sample_rate;
                } else if (!d->play_buf.empty()) {
                    // 不足一个 period：全部消费（真实设备会有 underrun，这里简化）
                    d->played += d->play_buf.size();
                    d->play_buf.clear();
                } else {
                    d->underruns++;
                }
            }
            next_tick += std::chrono::microseconds(chunk_us);
            std::this_thread::sleep_until(next_tick);
        }
    });

    LOGI(kTag, "playout consumer started (chunk=%d samples)", chunk);
}

void VirtualAudioDevice::stop_playout() {
    impl_->stop_playout();
}

bool VirtualAudioDevice::play(const int16_t* pcm, int samples) {
    if (!impl_->opened || samples <= 0) return false;

    std::lock_guard<std::mutex> lk(impl_->play_mutex);
    // 模拟播放设备按固定速率消费：这里用"缓冲水位"表达
    // 真实声卡是 DMA 按采样率取走，虚拟设备用容量上限模拟背压
    if (impl_->play_buf.size() + size_t(samples) > impl_->play_cap) {
        // 缓冲满 —— 真实场景等价于"设备消费不过来"，此处丢弃并记录
        LOGW(kTag, "play buffer full, drop %d samples (played=%llu)",
             samples, (unsigned long long)impl_->played);
        return false;
    }
    impl_->play_buf.insert(impl_->play_buf.end(), pcm, pcm + samples);
    impl_->played += samples;
    impl_->play_clock_ms += int64_t(samples) * 1000 / impl_->cfg.sample_rate;

    if (!impl_->cfg.sink_path.empty()) {
        impl_->play_sink.insert(impl_->play_sink.end(), pcm, pcm + samples);
    }
    return true;
}

VirtualAudioDevice::PlayStats VirtualAudioDevice::play_stats() const {
    std::lock_guard<std::mutex> lk(impl_->play_mutex);
    PlayStats s;
    s.played_samples = impl_->played;
    s.buffer_level   = impl_->play_buf.size();
    s.fill_level     = impl_->play_cap ? double(s.buffer_level) / double(impl_->play_cap) : 0;
    s.underruns      = impl_->underruns;
    return s;
}

void VirtualAudioDevice::set_play_buffer_capacity(size_t samples) {
    std::lock_guard<std::mutex> lk(impl_->play_mutex);
    impl_->play_cap = samples;
    if (impl_->play_buf.size() > samples) impl_->play_buf.resize(samples);
}

bool VirtualAudioDevice::capture_exhausted() const {
    return !impl_->source.empty() && impl_->src_pos >= impl_->source.size();
}

uint64_t VirtualAudioDevice::captured_samples() const {
    return impl_->captured;
}

}  // namespace rtc
