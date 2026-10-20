#include "media/video_device.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/logging.h"
#include "common/util.h"

namespace rtc {

namespace {
constexpr const char* kTag = "vdev";
constexpr size_t kMaxCaptureQueue = 3;   // 采集队列上限（防止消费者太慢时堆积）
constexpr size_t kMaxPlayQueue    = 8;   // 播放队列上限
}  // namespace

VirtualVideoDevice::VirtualVideoDevice(const Config& cfg) : cfg_(cfg) {}

VirtualVideoDevice::~VirtualVideoDevice() { close(); }

bool VirtualVideoDevice::open() {
    if (opened_) return true;
    if (cfg_.width <= 0 || cfg_.height <= 0 || cfg_.fps <= 0) {
        LOGE(kTag, "invalid config: %dx%d @%dfps", cfg_.width, cfg_.height, cfg_.fps);
        return false;
    }
    // YUV420 要求宽高为偶数（色度平面是 1/2 采样）
    if (cfg_.width % 2 || cfg_.height % 2) {
        LOGE(kTag, "width/height must be even for YUV420 (%dx%d)",
             cfg_.width, cfg_.height);
        return false;
    }
    opened_ = true;
    LOGI(kTag, "device opened: %dx%d @%dfps, source=%s, sink=%s",
         cfg_.width, cfg_.height, cfg_.fps, cfg_.source.c_str(),
         cfg_.sink_dir.empty() ? "(none)" : cfg_.sink_dir.c_str());
    return true;
}

void VirtualVideoDevice::close() {
    stop_capture();
    stop_playout();
    opened_ = false;
    LOGI(kTag, "device closed: captured=%llu played=%llu",
         (unsigned long long)stats_.captured_frames,
         (unsigned long long)stats_.played_frames);
}

// ================= 采集 =================
void VirtualVideoDevice::start_capture() {
    if (capture_running_) return;
    capture_running_ = true;
    cap_start_ms_ = now_ms();
    cap_frame_no_ = 0;
    capture_thread_ = std::thread([this] { capture_loop(); });
    LOGI(kTag, "capture started");
}

void VirtualVideoDevice::stop_capture() {
    if (!capture_running_) return;
    capture_running_ = false;
    cap_cv_.notify_all();
    if (capture_thread_.joinable()) capture_thread_.join();
    LOGI(kTag, "capture stopped (%llu frames)",
         (unsigned long long)stats_.captured_frames);
}

void VirtualVideoDevice::capture_loop() {
    // 按绝对时间点产生帧，误差不累积（与音频侧同一个原则，见坑 14）
    const int64_t frame_ms = 1000 / cfg_.fps;
    int64_t next = cap_start_ms_;

    while (capture_running_) {
        YuvFrame f;
        generate_frame(&f, cap_frame_no_++);

        {
            std::lock_guard<std::mutex> lk(cap_mutex_);
            // 队列满就丢最旧的（模拟真实摄像头"来不及处理就丢帧"）
            while (cap_queue_.size() >= kMaxCaptureQueue) {
                cap_queue_.pop_front();
            }
            cap_queue_.push_back(std::move(f));
            stats_.captured_frames++;
            stats_.capture_fps = double(stats_.captured_frames) * 1000.0 /
                                 double(now_ms() - cap_start_ms_ + 1);
        }
        cap_cv_.notify_all();

        next += frame_ms;
        const int64_t now = now_ms();
        if (next > now) {
            std::this_thread::sleep_for(std::chrono::milliseconds(next - now));
        } else {
            next = now;   // 落后太多就重新对齐，避免追赶式连发
        }
    }
}

bool VirtualVideoDevice::capture(YuvFrame* out) {
    std::unique_lock<std::mutex> lk(cap_mutex_);
    cap_cv_.wait_for(lk, std::chrono::milliseconds(200), [this] {
        return !cap_queue_.empty() || !capture_running_;
    });
    if (cap_queue_.empty()) return false;
    *out = std::move(cap_queue_.front());
    cap_queue_.pop_front();
    return true;
}

// 生成测试图案
void VirtualVideoDevice::generate_frame(YuvFrame* f, int64_t n) {
    const int w = cfg_.width, h = cfg_.height;
    f->allocate(w, h);

    if (cfg_.source == "test:bars") {
        // 彩条：8 条竖直色带（标准测试图，边缘锐利，考验编码器）
        static const uint8_t kLuma[8] = {235, 210, 170, 145, 106, 81, 41, 16};
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++) {
                f->y[size_t(r) * w + c] = kLuma[(c * 8) / w];
            }
        }
        for (size_t i = 0; i < f->u.size(); i++) {
            const int band = int((i % size_t(w / 2)) * 8 / size_t(w / 2));
            f->u[i] = uint8_t(128 + (band % 2 ? 40 : -40));
            f->v[i] = uint8_t(128 + (band % 3 ? 30 : -30));
        }
    } else if (cfg_.source == "test:noise") {
        // 噪声：最难压缩（几乎没有空间相关性），压力测试
        uint32_t seed = uint32_t(n * 2654435761u + 1);
        for (size_t i = 0; i < f->y.size(); i++) {
            seed = seed * 1664525u + 1013904223u;
            f->y[i] = uint8_t(seed >> 24);
        }
        for (size_t i = 0; i < f->u.size(); i++) {
            seed = seed * 1664525u + 1013904223u;
            f->u[i] = uint8_t(seed >> 24);
            seed = seed * 1664525u + 1013904223u;
            f->v[i] = uint8_t(seed >> 24);
        }
    } else {
        // test:motion（默认）：渐变背景 + 移动方块 + 移动圆
        // 既有大面积平坦区（好压缩），又有运动（产生 P 帧差异），
        // 是最能反映真实视频编码行为的图案
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++) {
                f->y[size_t(r) * w + c] = uint8_t((c * 160 / w + r * 60 / h + 30) & 0xFF);
            }
        }
        // 移动方块（每帧右移 4 像素）
        const int bx = int((n * 4) % (w + 60)) - 30;
        const int by = h / 4;
        for (int r = by; r < by + h / 3 && r < h; r++) {
            for (int c = bx; c < bx + h / 3 && c < w; c++) {
                if (c >= 0) f->y[size_t(r) * w + c] = 240;
            }
        }
        // 移动圆（反向移动）
        const int cx = w - int((n * 3) % (w + 40));
        const int cy = h * 2 / 3;
        const int rad = h / 8;
        for (int r = cy - rad; r <= cy + rad; r++) {
            for (int c = cx - rad; c <= cx + rad; c++) {
                if (r < 0 || r >= h || c < 0 || c >= w) continue;
                const int dx = c - cx, dy = r - cy;
                if (dx * dx + dy * dy <= rad * rad) f->y[size_t(r) * w + c] = 40;
            }
        }
        // 色度随时间变化（测试色度通道的帧间差异）
        for (size_t i = 0; i < f->u.size(); i++) {
            f->u[i] = uint8_t(128 + 50 * sin(double(n) * 0.1 + double(i) * 0.01));
            f->v[i] = uint8_t(128 + 50 * cos(double(n) * 0.1 + double(i) * 0.01));
        }
    }
}

// ================= 播放 =================
void VirtualVideoDevice::start_playout() {
    if (playout_running_) return;
    playout_running_ = true;
    playout_thread_ = std::thread([this] { playout_loop(); });
    LOGI(kTag, "playout started");
}

void VirtualVideoDevice::stop_playout() {
    if (!playout_running_) return;
    playout_running_ = false;
    if (playout_thread_.joinable()) playout_thread_.join();
    LOGI(kTag, "playout stopped (%llu frames)",
         (unsigned long long)stats_.played_frames);
}

void VirtualVideoDevice::play(const YuvFrame& frame) {
    std::lock_guard<std::mutex> lk(play_mutex_);
    // 队列满就丢最旧的：视频宁可丢帧也不要无限延迟
    // （与音频不同——音频丢帧会断音，视频丢帧只是卡一下）
    while (play_queue_.size() >= kMaxPlayQueue) {
        play_queue_.pop_front();
    }
    play_queue_.push_back(frame);
}

void VirtualVideoDevice::playout_loop() {
    // 播放侧：把帧"消费"掉（落盘 PPM 或仅统计）
    // 真实渲染器这里是显示到窗口；虚拟设备落盘便于人工查看
    int64_t last_report_ms = now_ms();
    uint64_t last_frames = 0;

    while (playout_running_) {
        YuvFrame f;
        bool got = false;
        {
            std::lock_guard<std::mutex> lk(play_mutex_);
            if (!play_queue_.empty()) {
                f = std::move(play_queue_.front());
                play_queue_.pop_front();
                got = true;
            }
        }
        if (got) {
            if (!cfg_.sink_dir.empty() &&
                (cfg_.max_sink_frames == 0 ||
                 stats_.sink_frames < uint64_t(cfg_.max_sink_frames))) {
                char name[512];
                snprintf(name, sizeof(name), "%s/out_%04llu.ppm",
                         cfg_.sink_dir.c_str(),
                         (unsigned long long)stats_.sink_frames);
                if (write_ppm(f, name)) {
                    std::lock_guard<std::mutex> lk(play_mutex_);
                    stats_.sink_frames++;
                }
            }
            std::lock_guard<std::mutex> lk(play_mutex_);
            stats_.played_frames++;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        // 每秒更新一次播放 fps
        const int64_t now = now_ms();
        if (now - last_report_ms >= 1000) {
            std::lock_guard<std::mutex> lk(play_mutex_);
            stats_.playout_fps = double(stats_.played_frames - last_frames) * 1000.0 /
                                 double(now - last_report_ms);
            last_frames = stats_.played_frames;
            last_report_ms = now;
        }
    }
}

// 写 PPM（P6 二进制格式）—— 无需任何图像库，Windows 自带"照片"可打开
bool VirtualVideoDevice::write_ppm(const YuvFrame& f, const std::string& path) {
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) return false;
    fprintf(fp, "P6\n%d %d\n255\n", f.width, f.height);
    // YUV420 -> RGB 转换（BT.601 标准系数）
    for (int r = 0; r < f.height; r++) {
        for (int c = 0; c < f.width; c++) {
            const int Y = f.y[size_t(r) * f.width + c];
            const int U = f.u[size_t(r / 2) * (f.width / 2) + c / 2] - 128;
            const int V = f.v[size_t(r / 2) * (f.width / 2) + c / 2] - 128;
            const int R = int(1.164 * (Y - 16) + 1.596 * V);
            const int G = int(1.164 * (Y - 16) - 0.392 * U - 0.813 * V);
            const int B = int(1.164 * (Y - 16) + 2.017 * U);
            const uint8_t px[3] = {
                uint8_t(R < 0 ? 0 : (R > 255 ? 255 : R)),
                uint8_t(G < 0 ? 0 : (G > 255 ? 255 : G)),
                uint8_t(B < 0 ? 0 : (B > 255 ? 255 : B))};
            fwrite(px, 1, 3, fp);
        }
    }
    fclose(fp);
    return true;
}

VirtualVideoDevice::Stats VirtualVideoDevice::stats() const {
    std::lock_guard<std::mutex> lk1(cap_mutex_);
    std::lock_guard<std::mutex> lk2(play_mutex_);
    Stats s = stats_;
    s.play_queue_depth = play_queue_.size();
    return s;
}

}  // namespace rtc
